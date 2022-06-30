/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include "dist_copy.h"

#include <postgres.h>
#include <access/tupdesc.h>
#include <catalog/namespace.h>
#include <executor/executor.h>
#include <libpq-fe.h>
#include <miscadmin.h>
#include <parser/parse_type.h>
#include <port/pg_bswap.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>

#include "compat/compat.h"
#include "chunk.h"
#include "copy.h"
#include "data_node.h"
#include "dimension.h"
#include "dimension_slice.h"
#include "guc.h"
#include "hypercube.h"
#include "hypertable.h"
#include "nodes/chunk_dispatch.h"
#include "nodes/chunk_insert_state.h"
#include "partitioning.h"
#include "remote/connection_cache.h"
#include "remote/dist_txn.h"
#include "ts_catalog/chunk_data_node.h"

#define DEFAULT_PG_DELIMITER '\t'
#define DEFAULT_PG_NULL_VALUE "\\N"

/*
 * Maximum number of rows in batch for insert. Note that arrays of this size are
 * also allocated on stack.
 */
#define MAX_BATCH_ROWS 1024

/*
 * Maximum bytes of COPY data in batch. This is also the default size of the
 * output copy data buffer.
 */
#define MAX_BATCH_BYTES 10 * 1024 * 1024

/* This contains the information needed to parse a dimension attribute out of a row of text copy
 * data
 */
typedef struct CopyDimensionInfo
{
	const Dimension *dim;
	int corresponding_copy_field;
	Datum default_value;
	FmgrInfo io_func;
	Oid typioparams;
	int32 atttypmod;
} CopyDimensionInfo;

typedef struct DataNodeConnection
{
	TSConnectionId id;
	TSConnection *connection;
} DataNodeConnection;

/* This contains information about connections currently in use by the copy as well as how to create
 * and end the copy command.
 */
typedef struct CopyConnectionState
{
	/*
	 * Cached connections to data nodes.
	 * Why do we need another layer of caching, when there is dist_txn layer
	 * already? The API it provides is one function that "does everything
	 * automatically", namely it's going to stop the COPY each time we request
	 * the connection. This is not something we want to do for each row when
	 * we're trying to do bulk copy.
	 * We can't use the underlying remote_connection_cache directly, because the
	 * remote chunk creation (chunk_api_create_on_data_nodes) would still use
	 * the dist_txn layer. Chunks are created interleaved with the actual COPY
	 * operation, so we would have to somehow maintain these two layers in sync.
	 */
	List *data_node_connections;

	/*
	 * Connections to which we have written something and have to finalize them.
	 */
	List *connections_in_use;

	bool using_binary;
	const char *outgoing_copy_cmd;
} CopyConnectionState;

/* This contains the state needed by a non-binary copy operation.
 */
typedef struct TextCopyContext
{
	int ndimensions;
	CopyDimensionInfo *dimensions;
	FmgrInfo *out_functions;
	char delimiter;
	char *null_string;
	char **fields;
	int nfields;
} TextCopyContext;

/* This contains the state needed by a binary copy operation.
 */
typedef struct BinaryCopyContext
{
	ExprContext *econtext;
	FmgrInfo *out_functions;
	Datum *values;
	bool *nulls;
} BinaryCopyContext;

/* This is this high level state needed for an in-progress copy command.
 */
typedef struct RemoteCopyContext
{
	/* Operation data */
	CopyConnectionState connection_state;
	Hypertable *ht;
	List *attnums;
	void *data_context; /* TextCopyContext or BinaryCopyContext */
	bool binary_operation;
	MemoryContext mctx; /* MemoryContext that holds the RemoteCopyContext */

	/*
	 * Incoming rows are batched before creating the chunks and sending them to
	 * data nodes. The following fields contain the current batch of rows.
	 */
	StringInfo *batch_rows;
	int current_batch_rows;
	int current_batch_bytes;
	Point **batch_points;
} RemoteCopyContext;

/*
 * This will create and populate a CopyDimensionInfo struct from the passed in
 * dimensions and values.
 */
static CopyDimensionInfo *
generate_copy_dimensions(const Dimension *dims, int ndimensions, const List *attnums,
						 const Hypertable *ht)
{
	CopyDimensionInfo *result = palloc0(ndimensions * sizeof(CopyDimensionInfo));
	int idx;

	for (idx = 0; idx < ndimensions; ++idx)
	{
		const Dimension *d = &dims[idx];
		CopyDimensionInfo *target = &result[idx];
		int i = 0;
		ListCell *lc;

		foreach (lc, attnums)
		{
			if (lfirst_int(lc) == d->column_attno)
				break;
			++i;
		}

		target->dim = d;

		if (i == attnums->length)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unable to use default value for partitioning column \"%s\"",
							NameStr(d->fd.column_name))));
		}
		else
		{
			Relation rel = relation_open(ht->main_table_relid, AccessShareLock);
			TupleDesc rel_desc = RelationGetDescr(rel);
			Form_pg_attribute attribute =
				TupleDescAttr(rel_desc, AttrNumberGetAttrOffset(d->column_attno));
			Oid in_func_oid;

			target->corresponding_copy_field = i;
			getTypeInputInfo(attribute->atttypid, &in_func_oid, &target->typioparams);
			fmgr_info(in_func_oid, &target->io_func);
			target->atttypmod = attribute->atttypmod;

			relation_close(rel, AccessShareLock);
		}
	}

	return result;
}

static Datum
get_copy_dimension_datum(char **fields, CopyDimensionInfo *info)
{
	Datum d;
	if (info->corresponding_copy_field != -1)
	{
		if (fields[info->corresponding_copy_field] == NULL)
		{
			if (info->dim->type == DIMENSION_TYPE_OPEN)
				ereport(ERROR,
						(errcode(ERRCODE_NOT_NULL_VIOLATION),
						 errmsg("NULL value in column \"%s\" violates not-null constraint",
								NameStr(info->dim->fd.column_name)),
						 errhint("Columns used for time partitioning cannot be NULL")));

			return 0;
		}
		d = InputFunctionCall(&info->io_func,
							  fields[info->corresponding_copy_field],
							  info->typioparams,
							  info->atttypmod);
	}
	else
		d = info->default_value;

	return d;
}

static int64
convert_datum_to_dim_idx(Datum datum, const Dimension *d)
{
	Oid dimtype;

	if (d->partitioning)
		datum = ts_partitioning_func_apply(d->partitioning, InvalidOid, datum);

	switch (d->type)
	{
		case DIMENSION_TYPE_OPEN:
			dimtype =
				(d->partitioning == NULL) ? d->fd.column_type : d->partitioning->partfunc.rettype;

			return ts_time_value_to_internal(datum, dimtype);
		case DIMENSION_TYPE_CLOSED:
			return (int64) DatumGetInt32(datum);
		case DIMENSION_TYPE_ANY:
		default:
			elog(ERROR, "invalid dimension type when inserting tuple");
			return -1;
	}
}

static Point *
calculate_hyperspace_point_from_fields(char **data, CopyDimensionInfo *dimensions,
									   int num_dimensions)
{
	Point *p;
	int i;

	p = palloc0(POINT_SIZE(num_dimensions));
	p->cardinality = num_dimensions;
	p->num_coords = num_dimensions;

	for (i = 0; i < num_dimensions; ++i)
	{
		Datum datum = get_copy_dimension_datum(data, &dimensions[i]);
		p->coordinates[i] = convert_datum_to_dim_idx(datum, dimensions[i].dim);
	}

	return p;
}

static const List *
create_connection_list_for_chunk(CopyConnectionState *state, int32 chunk_id,
								 const List *chunk_data_nodes, Oid userid)
{
	List *result = NIL;
	ListCell *lc;
	foreach (lc, chunk_data_nodes)
	{
		TSConnection *connection = NULL;
		ChunkDataNode *cdn = lfirst(lc);
		TSConnectionId required_id = remote_connection_id(cdn->foreign_server_oid, userid);

		ListCell *lc2;
		foreach (lc2, state->data_node_connections)
		{
			DataNodeConnection *entry = (DataNodeConnection *) lfirst(lc2);
			connection = entry->connection;
			if (required_id.server_id == entry->id.server_id &&
				required_id.user_id == entry->id.user_id)
			{
				result = lappend(result, connection);
				break;
			}
		}

		if (lc2 == NULL)
		{
			/*
			 * Did not find a cached connection, create a new one and cache it.
			 */
			connection = remote_dist_txn_get_connection(required_id, REMOTE_TXN_NO_PREP_STMT);

			DataNodeConnection *entry = palloc(sizeof(DataNodeConnection));
			entry->connection = connection;
			entry->id = required_id;

			state->data_node_connections = lappend(state->data_node_connections, entry);
			result = lappend(result, connection);
		}

		if (remote_connection_get_status(connection) == CONN_PROCESSING)
		{
			elog(ERROR,
				 "wrong status CONN_PROCESSING for connection to data node %d when performing "
				 "distributed COPY\n",
				 required_id.server_id);
		}

		if (remote_connection_get_status(connection) == CONN_IDLE)
		{
			TSConnectionError err;
			if (!remote_connection_begin_copy(connection,
											  state->outgoing_copy_cmd,
											  state->using_binary,
											  &err))
			{
				remote_connection_error_elog(&err, ERROR);
			}

			if (!list_member(state->connections_in_use, connection))
			{
				/*
				 * The normal distributed insert path (not dist_copy, but
				 * data_node_copy) doesn't reset the connections when it creates
				 * a new chunk. So the connection status will be idle after we
				 * created a new chunk, but it will still be in the list of
				 * active connections. Don't add duplicates.
				 */
				state->connections_in_use = lappend(state->connections_in_use, connection);
			}
		}
	}

	return result;
}

/*
 * Flush all active data node connections simultaneously, instead of doing this
 * one-by-one in remote_connection_end_copy().
 */
static void
flush_active_connections(const CopyConnectionState *state)
{
	List *to_end_copy = NIL;
	ListCell *active_cell;
	foreach (active_cell, state->connections_in_use)
	{
		TSConnection *conn = lfirst(active_cell);

		TSConnectionStatus status = remote_connection_get_status(conn);
		if (status != CONN_COPY_IN)
		{
			/*
			 * This is also called when terminating with error, so some
			 * connections might be in some error status, not CONN_COPY_IN.
			 */
			continue;
		}

		PGconn *pg_conn = remote_connection_get_pg_conn(conn);
		Assert(PQisnonblocking(pg_conn));

		PGresult *res = PQgetResult(pg_conn);
		if (res == NULL || PQresultStatus(res) != PGRES_COPY_IN)
		{
			/*
			 * No actual COPY on the connection, this is an internal program error.
			 */
			elog(ERROR,
				 "connection marked as CONN_COPY_IN, but no COPY is in progress when flushing data "
				 "nodes");
		}

		to_end_copy = lappend(to_end_copy, conn);

		if (PQputCopyEnd(pg_conn, NULL) != 1)
		{
			ereport(ERROR,
					(errmsg("could not end remote COPY"),
					 errdetail("%s", PQerrorMessage(pg_conn))));
		}

		remote_connection_set_status(conn, CONN_PROCESSING);
	}

	/*
	 * First, concurrently flush the remaining write buffers to the remote
	 * servers. Then, read out the CopyEnd response. It might also be delayed
	 * while the server is processing the received data.
	 */
	List *to_flush = list_copy(to_end_copy);
	/*
	 * The connections that were busy on this step and that we have to flush
	 * again.
	 */
	List *to_flush_next = NIL;
	/*
	 * Parallel list of what we have to wait for (read/write) for each
	 * connection.
	 */
	List *wait_events = NIL;
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		ListCell *to_flush_cell;
		foreach (to_flush_cell, to_flush)
		{
			TSConnection *conn = lfirst(to_flush_cell);
			PGconn *pg_conn = remote_connection_get_pg_conn(conn);
			Assert(PQisnonblocking(pg_conn));

			/* Write out all the pending buffers. */
			int res = PQflush(pg_conn);
			if (res == -1)
			{
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_EXCEPTION),
						errmsg("could not flush COPY data"));
			}
			else if (res == 0)
			{
				/* Flushed. */
			}
			else
			{
				/* Busy. */
				Assert(res == 1);
				to_flush_next = lappend(to_flush_next, conn);
				wait_events = lappend_int(wait_events, WL_SOCKET_WRITEABLE);
				fprintf(stderr, "wait for writable socket!!!!\n");
				continue;
			}

			/* Then, read out the final result. */
			res = PQconsumeInput(pg_conn);
			if (res == 0)
			{
				TSConnectionError err;
				remote_connection_get_error(conn, &err);
				remote_connection_error_elog(&err, ERROR);
			}

			if (PQisBusy(pg_conn))
			{
				/* Busy. */
				to_flush_next = lappend(to_flush_next, conn);
				wait_events = lappend_int(wait_events, WL_SOCKET_READABLE);
				continue;
			}

			/* Hooray, done with this connection. */
		}

		if (list_length(to_flush_next) == 0)
		{
			/* Flushed everything. */
			break;
		}

		/*
		 * Wait for changes on busy connections.
		 * Postgres API doesn't allow to remove a socket from the wait event,
		 * and it's level-triggered, so we have to recreate the set each time.
		 */
		WaitEventSet *set = CreateWaitEventSet(CurrentMemoryContext, list_length(to_flush_next));
		ListCell *set_cell;
		ListCell *events_cell;
		Assert(list_length(to_flush_next) == list_length(wait_events));
		forboth (set_cell, to_flush_next, events_cell, wait_events)
		{
			TSConnection *conn = lfirst(set_cell);
			PGconn *pg_conn = remote_connection_get_pg_conn(conn);
			(void) AddWaitEventToSet(set,
									 /* events = */ lfirst_int(events_cell),
									 PQsocket(pg_conn),
									 /* latch = */ NULL,
									 /* user_data = */ NULL);
		}

		WaitEvent occurred[1];
		int wait_result PG_USED_FOR_ASSERTS_ONLY = WaitEventSetWait(set,
																	/* timeout = */ 1000,
																	occurred,
																	/* nevents = */ 1,
																	WAIT_EVENT_COPY_FILE_WRITE);

		/*
		 * The possible results are:
		 * `0` --  Timeout. Just retry the flush, it will cause errors.
		 * `1` -- We have successfully waited for something, we don't care,
		 * just continue to flush the entire list.
		 */
		Assert(wait_result == 0 || wait_result == 1);

		FreeWaitEventSet(set);

		List *tmp = to_flush_next;
		to_flush_next = to_flush;
		to_flush = tmp;

		to_flush_next = list_truncate(to_flush_next, 0);
		wait_events = list_truncate(wait_events, 0);
	}

	/*
	 * 3. Clean up the connections (this isn't viable! FIXME)
	 */
	ListCell *lc;
	foreach (lc, to_end_copy)
	{
		TSConnection *conn = lfirst(lc);
		PGconn *pg_conn = remote_connection_get_pg_conn(conn);

		/*
		 * Mark connection as idle in any case, to prevent sticky error state.
		 */
		remote_connection_set_status(conn, CONN_IDLE);

		/*
		 * Switch the connection back into blocking mode because that's what the
		 * non-COPY code expects.
		 */
		if (PQsetnonblocking(pg_conn, 0))
		{
			ereport(ERROR,
					(errmsg("failed to switch the connection into blocking mode"),
					 errdetail("%s", PQerrorMessage(pg_conn))));
		}
	}

	/*
	 * 4. Verify the EndCopy result.
	 */
	foreach (lc, to_end_copy)
	{
		TSConnection *conn = lfirst(lc);
		PGconn *pg_conn = remote_connection_get_pg_conn(conn);

		/*
		 * Verify that the copy has ended.
		 */
		PGresult *res = PQgetResult(pg_conn);
		if (res == NULL)
		{
			ereport(ERROR, (errmsg("unexpected NULL result when ending remote COPY")));
		}

		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			TSConnectionError err;
			remote_connection_get_result_error(res, &err);
			remote_connection_error_elog(&err, ERROR);
		}

		res = PQgetResult(pg_conn);
		if (res != NULL)
		{
			ereport(ERROR,
					(errmsg("unexpected non-NULL result %d when ending remote COPY",
							PQresultStatus(res)),
					 errdetail("%s", PQerrorMessage(pg_conn))));
		}
	}

	list_free(to_end_copy);
}

static void
end_copy_on_data_nodes(const CopyConnectionState *state)
{
	/* Exit the copy subprotocol. */
	TSConnectionError err;
	bool failure = false;
	ListCell *lc;
	foreach (lc, state->connections_in_use)
	{
		TSConnection *conn = lfirst(lc);

		if (remote_connection_get_status(conn) == CONN_COPY_IN &&
			!remote_connection_end_copy(conn, &err))
			failure = true;
	}

	if (failure)
		remote_connection_error_elog(&err, ERROR);
}

static const List *
get_connections_for_chunk(RemoteCopyContext *context, int32 chunk_id, const List *chunk_data_nodes,
						  Oid userid)
{
	CopyConnectionState *state = &context->connection_state;
	MemoryContext oldmctx;

	oldmctx = MemoryContextSwitchTo(context->mctx);
	const List *conns = create_connection_list_for_chunk(state, chunk_id, chunk_data_nodes, userid);
	MemoryContextSwitchTo(oldmctx);

	return conns;
}

static bool
copy_should_send_binary()
{
	return ts_guc_enable_connection_binary_data;
}

/*
 * Extract a quoted list of identifiers from a DefElem with arg type T_list.
 */
static char *
name_list_to_string(const DefElem *def)
{
	StringInfoData string;
	ListCell *lc;
	bool first = true;

	initStringInfo(&string);

	foreach (lc, (List *) def->arg)
	{
		Node *name = (Node *) lfirst(lc);

		if (!first)
			appendStringInfo(&string, ", ");
		else
			first = false;

		if (IsA(name, String))
			appendStringInfoString(&string, quote_identifier(strVal(name)));
		else if (IsA(name, A_Star))
			appendStringInfoChar(&string, '*');
		else
			elog(ERROR, "unexpected node type in name list: %d", (int) nodeTag(name));
	}
	return string.data;
}

/*
 * Extract a string value (otherwise uninterpreted) from a DefElem.
 */
static char *
def_get_string(const DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR), errmsg("%s requires a parameter", def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return psprintf("%ld", (long) intVal(def->arg));
		case T_Float:

			/*
			 * T_Float values are kept in string form, so this type cheat
			 * works (and doesn't risk losing precision)
			 */
			return strVal(def->arg);
		case T_String:
			return strVal(def->arg);
		case T_TypeName:
			return TypeNameToString((TypeName *) def->arg);
		case T_List:
			return name_list_to_string(def);
		case T_A_Star:
			return pstrdup("*");
		default:
			elog(ERROR, "unrecognized node type: %d", nodeTag(def->arg));
	}
	return NULL; /* keep compiler quiet */
}

/* These are the only option available for binary copy operations */
static bool
is_supported_binary_option(const char *option)
{
	return strcmp(option, "oids") == 0 || strcmp(option, "freeze") == 0 ||
		   strcmp(option, "encoding") == 0;
}

/* Generate a COPY sql command for sending the data being passed in via 'stmt'
 * to a data node.
 */
static const char *
deparse_copy_cmd(const CopyStmt *stmt, const Hypertable *ht, bool binary)
{
	ListCell *lc;
	StringInfo command = makeStringInfo();

	appendStringInfo(command,
					 "COPY %s ",
					 quote_qualified_identifier(NameStr(ht->fd.schema_name),
												NameStr(ht->fd.table_name)));

	if (stmt->attlist != NULL)
	{
		bool first = true;
		appendStringInfo(command, "(");
		foreach (lc, stmt->attlist)
		{
			if (!first)
				appendStringInfo(command, ", ");
			else
				first = false;

			appendStringInfo(command, "%s", quote_identifier(strVal(lfirst(lc))));
		}
		appendStringInfo(command, ") ");
	}

	appendStringInfo(command, "FROM STDIN");

	if (stmt->options != NIL || binary)
	{
		bool first = true;
		appendStringInfo(command, " WITH (");
		foreach (lc, stmt->options)
		{
			DefElem *defel = lfirst_node(DefElem, lc);
			const char *option = defel->defname;

			/* Ignore text only options for binary copy */
			if (binary && !is_supported_binary_option(option))
				continue;

			if (!first)
				appendStringInfo(command, ", ");
			else
				first = false;

			/* quoted options */
			if (strcmp(option, "delimiter") == 0 || strcmp(option, "null") == 0 ||
				strcmp(option, "quote") == 0 || strcmp(option, "escape") == 0 ||
				strcmp(option, "encoding") == 0)
				appendStringInfo(command, "%s '%s'", option, def_get_string(defel));
			/* options that take columns (note force_quote is only for COPY TO) */
			else if (strcmp(option, "force_not_null") == 0 || strcmp(option, "force_null") == 0)
				appendStringInfo(command, "%s (%s)", option, def_get_string(defel));
			/* boolean options don't require an argument to use default setting */
			else if (defel->arg == NULL &&
					 (strcmp(option, "oids") == 0 || strcmp(option, "freeze") == 0 ||
					  strcmp(option, "header") == 0))
				appendStringInfo(command, "%s", option);
			/* everything else should pass directly through */
			else
				appendStringInfo(command, "%s %s", option, def_get_string(defel));
		}

		if (binary)
			appendStringInfo(command, "%sFORMAT binary", first ? "" : ", ");
		appendStringInfo(command, ")");
	}

	return command->data;
}

/*
 * This function checks the options specified for the copy command and makes
 * sure they're supported.  It also determines what delimiter and null
 * encoding are being specified and will use these values when sending data to
 * the data node as they presumably won't conflict with the values being passed.
 * Note that the CopyBegin call will have such validation as checking for
 * duplicate options, this function just checks added constraints for the
 * distributed copy. This call is only needed when sending data in text format
 * to the data node.
 */
static void
validate_options(List *copy_options, char *delimiter, char **null_string)
{
	ListCell *lc;
	bool delimiter_found = false;

	/* Postgres defaults */
	*delimiter = DEFAULT_PG_DELIMITER;
	*null_string = DEFAULT_PG_NULL_VALUE;

	foreach (lc, copy_options)
	{
		const DefElem *defel = lfirst_node(DefElem, lc);

		if (strcmp(defel->defname, "format") == 0)
		{
			const char *fmt;

			Assert(nodeTag(defel->arg) == T_String);
			fmt = strVal(defel->arg);

			if (strcmp(fmt, "binary") == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("remote copy does not support binary data")));
			else if (strcmp(fmt, "csv") == 0 && !delimiter_found)
				*delimiter = ',';
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			const char *delim_string = def_get_string(defel);

			Assert(strlen(delim_string) == 1);
			*delimiter = delim_string[0];
			delimiter_found = true;
		}
		else if (strcmp(defel->defname, "null") == 0)
			*null_string = def_get_string(defel);
	}
}

/* Populates the passed in pointer with an array of output functions and returns the array size.
 * Note that we size the array to the number of columns in the hypertable for convenience, but only
 * populate the functions for columns used in the copy command.
 */
static int
get_copy_conversion_functions(Oid relid, const List *copy_attnums, FmgrInfo **functions,
							  bool binary)
{
	ListCell *lc;
	Relation rel = relation_open(relid, AccessShareLock);
	TupleDesc tupDesc = RelationGetDescr(rel);

	*functions = palloc0(tupDesc->natts * sizeof(FmgrInfo));
	foreach (lc, copy_attnums)
	{
		int offset = AttrNumberGetAttrOffset(lfirst_int(lc));
		Oid out_func_oid;
		bool isvarlena;
		Form_pg_attribute attr = TupleDescAttr(tupDesc, offset);

		if (binary)
			getTypeBinaryOutputInfo(attr->atttypid, &out_func_oid, &isvarlena);
		else
			getTypeOutputInfo(attr->atttypid, &out_func_oid, &isvarlena);

		fmgr_info(out_func_oid, &((*functions)[offset]));
	}
	relation_close(rel, AccessShareLock);

	return tupDesc->natts;
}

static TextCopyContext *
generate_text_copy_context(const CopyStmt *stmt, const Hypertable *ht, const List *attnums)
{
	TextCopyContext *ctx = palloc0(sizeof(TextCopyContext));

	get_copy_conversion_functions(ht->main_table_relid, attnums, &ctx->out_functions, false);

	ctx->ndimensions = ht->space->num_dimensions;
	validate_options(stmt->options, &ctx->delimiter, &ctx->null_string);
	ctx->dimensions =
		generate_copy_dimensions(ht->space->dimensions, ctx->ndimensions, attnums, ht);
	return ctx;
}

static BinaryCopyContext *
generate_binary_copy_context(ExprContext *econtext, const Hypertable *ht, const List *attnums)
{
	BinaryCopyContext *ctx = palloc0(sizeof(BinaryCopyContext));
	int columns =
		get_copy_conversion_functions(ht->main_table_relid, attnums, &ctx->out_functions, true);

	ctx->econtext = econtext;
	ctx->values = palloc0(columns * sizeof(Datum));
	ctx->nulls = palloc0(columns * sizeof(bool));

	return ctx;
}

RemoteCopyContext *
remote_copy_begin(const CopyStmt *stmt, Hypertable *ht, ExprContext *per_tuple_ctx, List *attnums,
				  bool binary_copy)
{
	MemoryContext mctx =
		AllocSetContextCreate(CurrentMemoryContext, "Remote COPY", ALLOCSET_DEFAULT_SIZES);
	RemoteCopyContext *context;
	MemoryContext oldmctx;

	oldmctx = MemoryContextSwitchTo(mctx);
	context = palloc0(sizeof(RemoteCopyContext));
	context->ht = ht;
	context->attnums = attnums;
	context->mctx = mctx;
	context->binary_operation = binary_copy;
	context->connection_state.connections_in_use = NIL;
	context->connection_state.data_node_connections = NIL;
	context->connection_state.using_binary = binary_copy;
	context->connection_state.outgoing_copy_cmd = deparse_copy_cmd(stmt, ht, binary_copy);

	context->batch_rows = palloc0(sizeof(StringInfo) * MAX_BATCH_ROWS);
	context->batch_points = palloc0(sizeof(Point *) * MAX_BATCH_ROWS);
	context->current_batch_rows = 0;
	context->current_batch_bytes = 0;

	if (binary_copy)
		context->data_context = generate_binary_copy_context(per_tuple_ctx, ht, attnums);
	else
		context->data_context = generate_text_copy_context(stmt, ht, attnums);

	MemoryContextSwitchTo(oldmctx);

	return context;
}

const char *
remote_copy_get_copycmd(RemoteCopyContext *context)
{
	return context->connection_state.outgoing_copy_cmd;
}

static StringInfo
parse_next_text_row(CopyFromState cstate, List *attnums, TextCopyContext *ctx)
{
	StringInfo row_data = makeStringInfo();
	int i;

	bool result = NextCopyFromRawFields(cstate, &ctx->fields, &ctx->nfields);
	if (!result)
		return NULL;

	Assert(ctx->nfields == list_length(attnums));

	for (i = 0; i < ctx->nfields - 1; ++i)
		appendStringInfo(row_data,
						 "%s%c",
						 ctx->fields[i] ? ctx->fields[i] : ctx->null_string,
						 ctx->delimiter);

	appendStringInfo(row_data,
					 "%s\n",
					 ctx->fields[ctx->nfields - 1] ? ctx->fields[ctx->nfields - 1] :
													 ctx->null_string);

	return row_data;
}

static StringInfo
generate_binary_copy_data(Datum *values, bool *nulls, List *attnums, FmgrInfo *out_functions)
{
	StringInfo row_data = makeStringInfo();
	uint16 buf16;
	uint32 buf32;
	ListCell *lc;

	buf16 = pg_hton16((uint16) attnums->length);
	appendBinaryStringInfo(row_data, (char *) &buf16, sizeof(buf16));

	foreach (lc, attnums)
	{
		int offset = AttrNumberGetAttrOffset(lfirst_int(lc));

		if (nulls[offset])
		{
			buf32 = pg_hton32((uint32) -1);
			appendBinaryStringInfo(row_data, (char *) &buf32, sizeof(buf32));
		}
		else
		{
			Datum value = values[offset];
			bytea *outputbytes;
			int output_length;

			outputbytes = SendFunctionCall(&out_functions[offset], value);
			output_length = VARSIZE(outputbytes) - VARHDRSZ;
			buf32 = pg_hton32((uint32) output_length);
			appendBinaryStringInfo(row_data, (char *) &buf32, sizeof(buf32));
			appendBinaryStringInfo(row_data, VARDATA(outputbytes), output_length);
		}
	}

	return row_data;
}

static StringInfo
parse_next_binary_row(CopyFromState cstate, List *attnums, BinaryCopyContext *ctx)
{
	MemoryContext old = MemoryContextSwitchTo(ctx->econtext->ecxt_per_tuple_memory);
	bool result = NextCopyFrom(cstate, ctx->econtext, ctx->values, ctx->nulls);
	MemoryContextSwitchTo(old);

	if (!result)
		return NULL;

	return generate_binary_copy_data(ctx->values, ctx->nulls, attnums, ctx->out_functions);
}

static Point *
get_current_point_for_text_copy(TextCopyContext *ctx)
{
	return calculate_hyperspace_point_from_fields(ctx->fields, ctx->dimensions, ctx->ndimensions);
}

static Point *
calculate_hyperspace_point_from_binary(Datum *values, bool *nulls, const Hyperspace *space)
{
	Point *p;
	int i;

	p = palloc0(POINT_SIZE(space->num_dimensions));
	p->cardinality = space->num_dimensions;
	p->num_coords = space->num_dimensions;

	for (i = 0; i < space->num_dimensions; ++i)
	{
		const Dimension *dim = &space->dimensions[i];
		Datum datum = values[dim->column_attno - 1];

		if (nulls[dim->column_attno - 1])
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("value required for partitioning column %s",
							NameStr(dim->fd.column_name))));
		p->coordinates[i] = convert_datum_to_dim_idx(datum, dim);
	}

	return p;
}

static Point *
get_current_point_for_binary_copy(BinaryCopyContext *ctx, const Hyperspace *hs)
{
	return calculate_hyperspace_point_from_binary(ctx->values, ctx->nulls, hs);
}

static bool
read_next_copy_row(RemoteCopyContext *context, CopyFromState cstate)
{
	Point *point = NULL;
	Hypertable *ht = context->ht;
	StringInfo row_data;

	if (context->binary_operation)
	{
		row_data = parse_next_binary_row(cstate, context->attnums, context->data_context);
	}
	else
	{
		row_data = parse_next_text_row(cstate, context->attnums, context->data_context);
	}

	if (row_data == NULL)
	{
		return false;
	}

	if (context->binary_operation)
	{
		point = get_current_point_for_binary_copy(context->data_context, ht->space);
	}
	else
	{
		point = get_current_point_for_text_copy(context->data_context);
	}

	Assert(context->current_batch_rows < MAX_BATCH_ROWS);
	context->batch_rows[context->current_batch_rows] = row_data;
	context->batch_points[context->current_batch_rows] = point;

	context->current_batch_rows++;
	context->current_batch_bytes += row_data->len;

	return true;
}

static bool
send_copy_data(StringInfo row_data, const List *connections)
{
	ListCell *lc;

	foreach (lc, connections)
	{
		TSConnection *conn = lfirst(lc);
		TSConnectionError err;

		if (!remote_connection_put_copy_data(conn, row_data->data, row_data->len, &err))
			remote_connection_error_elog(&err, ERROR);
	}

	return true;
}

/*
 * Rows for sending to a particular data node.
 */
typedef struct DataNodeRows
{
	int data_node_id;
	Oid server_oid;
	TSConnection *connection;
	int rows_total;

	/* Array of indices into the batch row array. */
	int *row_indices;
} DataNodeRows;

static bool
remote_copy_process_and_send_data(RemoteCopyContext *context)
{
	Hypertable *ht = context->ht;
	const int n = context->current_batch_rows;
	Assert(n <= MAX_BATCH_ROWS);

	/*
	 * This list tracks the per-batch insert states of the data nodes
	 * (DataNodeRows).
	 */
	List *data_nodes = NIL;

	/* For each row, find or create the destination chunk. */
	bool did_flush = false;
	for (int row_in_batch = 0; row_in_batch < n; row_in_batch++)
	{
		Point *point = context->batch_points[row_in_batch];

		Chunk *chunk = ts_hypertable_find_chunk_for_point(ht, point);
		if (chunk == NULL)
		{
			if (!did_flush)
			{
				/*
				 * The data node connections have to be flushed before creating
				 * a new chunk. They might have outstanding COPY data from the
				 * previous batch.
				 */
				flush_active_connections(&context->connection_state);
				did_flush = true;
			}
			chunk = ts_hypertable_create_chunk_for_point(ht, point);
		}

		/*
		 * For remote copy, we don't use chunk insert states on the AN.
		 * So we need to explicitly set the chunk as unordered when copies
		 * are directed to previously compressed chunks.
		 */
		if (ts_chunk_is_compressed(chunk) && (!ts_chunk_is_unordered(chunk)))
			ts_chunk_set_unordered(chunk);

		/*
		 * Schedule the row for sending to the data nodes containing the chunk.
		 */
		ListCell *lc;
		foreach (lc, chunk->data_nodes)
		{
			ChunkDataNode *chunk_data_node = lfirst(lc);
			/* Find the existing insert state for this data node. */
			DataNodeRows *data_node_rows = NULL;
			ListCell *lc2;
			foreach (lc2, data_nodes)
			{
				data_node_rows = lfirst(lc2);
				if (chunk_data_node->foreign_server_oid == data_node_rows->server_oid)
				{
					break;
				}
			}

			if (lc2 == NULL)
			{
				/* No insert state for this data node yet. Create it. */
				data_node_rows = palloc(sizeof(DataNodeRows));
				data_node_rows->server_oid = chunk_data_node->foreign_server_oid;
				data_node_rows->rows_total = 0;
				data_node_rows->row_indices = palloc(sizeof(int) * context->current_batch_rows);
				data_nodes = lappend(data_nodes, data_node_rows);
			}

			Assert(data_node_rows->server_oid == chunk_data_node->foreign_server_oid);

			data_node_rows->row_indices[data_node_rows->rows_total] = row_in_batch;
			data_node_rows->rows_total++;
		}
	}

	/*
	 * Open the connection to each data node with help of the connection cache.
	 */
	MemoryContext old = MemoryContextSwitchTo(context->mctx);
	ListCell *lc;
	foreach (lc, data_nodes)
	{
		DataNodeRows *data_node = lfirst(lc);

		ListCell *lc2;
		foreach (lc2, context->connection_state.data_node_connections)
		{
			DataNodeConnection *entry = (DataNodeConnection *) lfirst(lc2);
			if (data_node->server_oid == entry->id.server_id)
			{
				data_node->connection = entry->connection;
				break;
			}
		}

		if (lc2 == NULL)
		{
			/*
			 * Did not find a cached connection, create a new one and cache it.
			 */
			TSConnectionId required_id = remote_connection_id(data_node->server_oid, GetUserId());
			data_node->connection =
				remote_dist_txn_get_connection(required_id, REMOTE_TXN_NO_PREP_STMT);

			DataNodeConnection *entry = palloc(sizeof(DataNodeConnection));
			entry->connection = data_node->connection;
			entry->id = required_id;

			context->connection_state.data_node_connections =
				lappend(context->connection_state.data_node_connections, entry);
		}

		/*
		 * Begin COPY on the connection if needed.
		 */
		TSConnectionStatus status = remote_connection_get_status(data_node->connection);
		if (status == CONN_IDLE)
		{
			TSConnectionError err;
			if (!remote_connection_begin_copy(data_node->connection,
											  context->connection_state.outgoing_copy_cmd,
											  context->connection_state.using_binary,
											  &err))
			{
				remote_connection_error_elog(&err, ERROR);
			}

			/*
			 * Add the connection to the list of active connections to be
			 * flushed later.
			 * The normal distributed insert path (not dist_copy, but
			 * data_node_copy) doesn't reset the connections when it creates
			 * a new chunk. So the connection status will be idle after we
			 * created a new chunk, but it will still be in the list of
			 * active connections. Don't add duplicates.
			 */
			if (!list_member(context->connection_state.connections_in_use, data_node->connection))
			{
				context->connection_state.connections_in_use =
					lappend(context->connection_state.connections_in_use, data_node->connection);
			}
		}
		else if (status == CONN_COPY_IN)
		{
			/* Ready to use. */
			Assert(
				list_member(context->connection_state.connections_in_use, data_node->connection));
		}
		else
		{
			elog(ERROR,
				 "wrong status %d for connection to data node %d when performing "
				 "distributed COPY\n",
				 status,
				 data_node->server_oid);
		}
	}
	MemoryContextSwitchTo(old);

	/*
	 * Actually send the data to the data nodes. We don't interleave the data
	 * nodes here, because the batches are relatively small.
	 */
	StringInfoData copy_data = { .data = palloc(MAX_BATCH_BYTES), .maxlen = MAX_BATCH_BYTES };
	foreach (lc, data_nodes)
	{
		DataNodeRows *dn = lfirst(lc);
		PGconn *pg_conn = remote_connection_get_pg_conn(dn->connection);

		resetStringInfo(&copy_data);
		for (int row = 0; row < dn->rows_total; row++)
		{
			StringInfo row_data = context->batch_rows[dn->row_indices[row]];
			appendBinaryStringInfo(&copy_data, row_data->data, row_data->len);
		}

		/*
		 * Send the copy data to the remote server.
		 * It can't really return 0 ("would block") until it runs out
		 * of memory. It just grows the buffer and tries to flush in
		 * pqPutMsgEnd().
		 */
		int res = PQputCopyData(pg_conn, copy_data.data, copy_data.len);

		if (res == -1)
		{
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_EXCEPTION),
					 errmsg("could not send COPY data"),
					 errdetail("%s", PQerrorMessage(pg_conn))));
		}

		/*
		 * We don't have to specially flush the data here, because the flush is
		 * attempted after finishing each protocol message (pqPutMsgEnd()).
		 */
	}

	return true;
}

void
remote_copy_end(RemoteCopyContext *context)
{
	flush_active_connections(&context->connection_state);
	end_copy_on_data_nodes(&context->connection_state);
	MemoryContextDelete(context->mctx);
}

uint64
remote_distributed_copy(const CopyStmt *stmt, CopyChunkState *ccstate, List *attnums)
{
	MemoryContext oldmctx = CurrentMemoryContext;
	EState *estate = ccstate->estate;
	Hypertable *ht = ccstate->dispatch->hypertable;
	RemoteCopyContext *context = remote_copy_begin(stmt,
												   ht,
												   GetPerTupleExprContext(estate),
												   attnums,
												   copy_should_send_binary());
	uint64 processed = 0;

	MemoryContext batch_context =
		AllocSetContextCreate(CurrentMemoryContext, "Remote COPY batch", ALLOCSET_DEFAULT_SIZES);

	PG_TRY();
	{
		MemoryContextSwitchTo(batch_context);
		while (true)
		{
			ResetPerTupleExprContext(ccstate->estate);

			CHECK_FOR_INTERRUPTS();

			bool eof = !read_next_copy_row(context, ccstate->cstate);
			if (!eof && context->current_batch_rows < MAX_BATCH_ROWS &&
				context->current_batch_bytes < MAX_BATCH_BYTES)
			{
				/*
				 * Accumulate more rows into the current batch.
				 */
				continue;
			}

			/*
			 * Send out the current batch.
			 */
			remote_copy_process_and_send_data(context);

			processed += context->current_batch_rows;
			context->current_batch_rows = 0;
			context->current_batch_bytes = 0;
			MemoryContextReset(batch_context);

			if (eof)
			{
				break;
			}
		}
	}
	PG_CATCH();
	{
		/* If we hit an error, make sure we end our in-progress COPYs */
		remote_copy_end(context);
		PG_RE_THROW();
	}
	PG_END_TRY();

	remote_copy_end(context);
	MemoryContextSwitchTo(oldmctx);

	return processed;
}

/*
 * Send a tuple/row to data nodes.
 *
 * The slot is serialized in text or binary format, depending on setting. The
 * data is already "routed" to the "right" chunk as indicated by the chunk
 * insert state.
 */
bool
remote_copy_send_slot(RemoteCopyContext *context, TupleTableSlot *slot, const ChunkInsertState *cis)
{
	ListCell *lc;
	bool result;
	StringInfo row_data;

	/* Pre-materialize all attributes since we will access all of them */
	slot_getallattrs(slot);

	if (context->binary_operation)
	{
		BinaryCopyContext *binctx = context->data_context;

		MemSet(binctx->nulls, 0, list_length(context->attnums) * sizeof(bool));

		foreach (lc, context->attnums)
		{
			AttrNumber attnum = lfirst_int(lc);
			int i = AttrNumberGetAttrOffset(attnum);

			binctx->values[i] = slot_getattr(slot, attnum, &binctx->nulls[i]);
		}

		row_data = generate_binary_copy_data(binctx->values,
											 binctx->nulls,
											 context->attnums,
											 binctx->out_functions);
	}
	else
	{
		TextCopyContext *textctx = context->data_context;
		char delim = textctx->delimiter;

		row_data = makeStringInfo();

		foreach (lc, context->attnums)
		{
			AttrNumber attnum = lfirst_int(lc);
			bool isnull;
			Datum value;

			if (lc == list_tail(context->attnums))
				delim = '\n';

			value = slot_getattr(slot, attnum, &isnull);

			if (isnull)
				appendStringInfo(row_data, "%s%c", textctx->null_string, delim);
			else
			{
				int off = AttrNumberGetAttrOffset(attnum);
				const char *output = OutputFunctionCall(&textctx->out_functions[off], value);
				appendStringInfo(row_data, "%s%c", output, delim);
			}
		}
	}

	PG_TRY();
	{
		const List *connections;

		connections =
			get_connections_for_chunk(context, cis->chunk_id, cis->chunk_data_nodes, cis->user_id);
		Assert(list_length(connections) == list_length(cis->chunk_data_nodes));
		Assert(list_length(connections) > 0);
		result = send_copy_data(row_data, connections);
	}
	PG_CATCH();
	{
		/* If we hit an error, make sure we end our in-progress COPYs */
		remote_copy_end(context);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
}
