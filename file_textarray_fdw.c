/*-------------------------------------------------------------------------
 *
 * file_textarray_fdw.c
 *		  foreign-data wrapper for server-side flat files, returned as a
 *        single text array field.        
 *
 * Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  file_textarray_fdw/file_textarray_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/reloptions.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#if PG_VERSION_NUM >= 90200
#include "optimizer/pathnode.h"
#endif
#include "utils/array.h"
#include "utils/builtins.h"
#if PG_VERSION_NUM >= 90200
#include "utils/rel.h"
#endif

PG_MODULE_MAGIC;

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct FileFdwOption
{
	const char *optname;
	Oid			optcontext;		/* Oid of catalog in which option may appear */
};

/*
 * Valid options for file_fdw.
 * These options are based on the options for COPY FROM command.
 *
 * Note: If you are adding new option for user mapping, you need to modify
 * fileGetOptions(), which currently doesn't bother to look at user mappings.
 */
static struct FileFdwOption valid_options[] = {
	/* File options */
	{ "filename",		ForeignTableRelationId },

	/* Format options */
	/* oids option is not supported */
	{ "format",			ForeignTableRelationId },
	{ "header",			ForeignTableRelationId },
	{ "delimiter",		ForeignTableRelationId },
	{ "quote",			ForeignTableRelationId },
	{ "escape",			ForeignTableRelationId },
	{ "null",			ForeignTableRelationId },
	{ "encoding",		ForeignTableRelationId },

	/*
	 * force_quote is not supported by file_fdw because it's for COPY TO.
	 */

	/*
	 * force_not_null is not supported by file_fdw.  It would need a parser
	 * for list of columns, not to mention a way to check the column list
	 * against the table.
	 */

	/* Sentinel */
	{ NULL,			InvalidOid }
};

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */
typedef struct FileFdwExecutionState
{
	char		   *filename;	/* file to read */
	List		   *options;	/* merged COPY options, excluding filename */
	CopyState		cstate;		/* state of reading file */
    /* stash for processing text arrays */
	int             text_array_stash_size;
	Datum          *text_array_values;
	bool           *text_array_nulls;
    /* required for spcial empty line case */
    bool            emptynull;
} FileFdwExecutionState;

#define FILE_FDW_TEXTARRAY_STASH_INIT 64

/*
 * SQL functions
 */
extern Datum file_textarray_fdw_handler(PG_FUNCTION_ARGS);
extern Datum file_textarray_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(file_textarray_fdw_handler);
PG_FUNCTION_INFO_V1(file_textarray_fdw_validator);

/*
 * FDW callback routines
 */
#if PG_VERSION_NUM >= 90200
static void filePlanForeignScan(Oid foreigntableid,
									PlannerInfo *root,
									RelOptInfo *baserel);
#else
static FdwPlan *filePlanForeignScan(Oid foreigntableid,
									PlannerInfo *root,
									RelOptInfo *baserel);
#endif
static void fileExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void fileBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *fileIterateForeignScan(ForeignScanState *node);
static void fileReScanForeignScan(ForeignScanState *node);
static void fileEndForeignScan(ForeignScanState *node);

/* text array support */

static void makeTextArray(FileFdwExecutionState *fdw_private,
						   TupleTableSlot *slot, char **raw_fields, int nfields);
static void check_table_shape(Relation rel);

/*
 * Helper functions
 */
static bool is_valid_option(const char *option, Oid context);
static void fileGetOptions(Oid foreigntableid,
			   char **filename, List **other_options);
static void estimate_costs(PlannerInfo *root, RelOptInfo *baserel,
						   const char *filename,
						   Cost *startup_cost, Cost *total_cost);


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
file_textarray_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	fdwroutine->PlanForeignScan = filePlanForeignScan;
	fdwroutine->ExplainForeignScan = fileExplainForeignScan;
	fdwroutine->BeginForeignScan = fileBeginForeignScan;
	fdwroutine->IterateForeignScan = fileIterateForeignScan;
	fdwroutine->ReScanForeignScan = fileReScanForeignScan;
	fdwroutine->EndForeignScan = fileEndForeignScan;

	PG_RETURN_POINTER(fdwroutine);
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses file_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum
file_textarray_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	char	   *filename = NULL;
	List	   *other_options = NIL;
	ListCell   *cell;

	/*
	 * Only superusers are allowed to set options of a file_fdw foreign table.
	 * This is because the filename is one of those options, and we don't
	 * want non-superusers to be able to determine which file gets read.
	 *
	 * Putting this sort of permissions check in a validator is a bit of a
	 * crock, but there doesn't seem to be any other place that can enforce
	 * the check more cleanly.
	 *
	 * Note that the valid_options[] array disallows setting filename at
	 * any options level other than foreign table --- otherwise there'd
	 * still be a security hole.
	 */
	if (catalog == ForeignTableRelationId && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superuser can change options of a file_fdw foreign table")));

	/*
	 * Check that only options supported by file_fdw, and allowed for the
	 * current object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem	   *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			struct FileFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->optname);
			}

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("Valid options in this context are: %s",
							 buf.data)));
		}

		/* Separate out filename, since ProcessCopyOptions won't allow it */
		if (strcmp(def->defname, "filename") == 0)
		{
			if (filename)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			filename = defGetString(def);
		}
		else
			other_options = lappend(other_options, def);
	}

	/*
	 * Now apply the core COPY code's validation logic for more checks.
	 */
	ProcessCopyOptions(NULL, true, other_options);

	/*
	 * Filename option is required for file__textarray_fdw foreign tables.
	 */
	if (catalog == ForeignTableRelationId && filename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
				 errmsg("filename is required for file_fdw foreign tables")));

	PG_RETURN_VOID();
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
is_valid_option(const char *option, Oid context)
{
	struct FileFdwOption *opt;

	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Fetch the options for a file_fdw foreign table.
 *
 * We have to separate out "filename" from the other options because
 * it must not appear in the options list passed to the core COPY code.
 */
static void
fileGetOptions(Oid foreigntableid,
			   char **filename, List **other_options)
{
	ForeignTable *table;
	ForeignServer *server;
	ForeignDataWrapper *wrapper;
	List	   *options;
	ListCell   *lc,
			   *prev;

	/*
	 * Extract options from FDW objects.  We ignore user mappings because
	 * file_fdw doesn't have any options that can be specified there.
	 *
	 * (XXX Actually, given the current contents of valid_options[], there's
	 * no point in examining anything except the foreign table's own options.
	 * Simplify?)
	 */
	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);

	options = NIL;
	options = list_concat(options, wrapper->options);
	options = list_concat(options, server->options);
	options = list_concat(options, table->options);

	/*
	 * Separate out the filename.
	 */
	*filename = NULL;
	prev = NULL;
	foreach(lc, options)
	{
		DefElem	   *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "filename") == 0)
		{
			*filename = defGetString(def);
			options = list_delete_cell(options, lc, prev);
			break;
		}
		prev = lc;
	}
	if (*filename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_REPLY),
				 errmsg("filename is required for file_fdw foreign tables")));
	*other_options = options;
}

/*
 * filePlanForeignScan
 *    Create possible access paths for a scan on the foreign table
 *
 *    Currently we don't support any push-down feature, so there is only one
 *    possible access path, which simply returns all records in the order in
 *    the data file.
 */

#if PG_VERSION_NUM >= 90200
static void
#else
static FdwPlan *
#endif
filePlanForeignScan(Oid foreigntableid,
					PlannerInfo *root,
					RelOptInfo *baserel)
{
#if PG_VERSION_NUM >= 90200
	Cost    startup_cost;
	Cost    total_cost;
#else
	FdwPlan	   *fdwplan;
#endif
	char	   *filename;
	List	   *options;

	/* Fetch options --- we only need filename at this point */
	fileGetOptions(foreigntableid, &filename, &options);

	/* Construct FdwPlan with cost estimates */
#if PG_VERSION_NUM >= 90200
	estimate_costs(root, baserel, filename,
           &startup_cost, &total_cost);

	/* Create a ForeignPath node and add it as only possible path */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
									 baserel->rows,
									 startup_cost,
									 total_cost,
									 NIL, /* no pathkeys */
									 NULL, /* no outer rel either */									 
									 NIL,
									 NIL)); /* no fdw_private data */
	/*
	 * If data file was sorted, and we knew it somehow, we could insert
	 * appropriate pathkeys into the ForeignPath node to tell the planner that.
	 */
#else
	fdwplan = makeNode(FdwPlan);
	estimate_costs(root, baserel, filename,
				   &fdwplan->startup_cost, &fdwplan->total_cost);
	fdwplan->fdw_private = NIL;				/* not used */

	return fdwplan;
#endif
}

/*
 * fileExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
fileExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	char	   *filename;
	List	   *options;

	/* Fetch options --- we only need filename at this point */
	fileGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				   &filename, &options);

	ExplainPropertyText("Foreign File", filename, es);

	/* Suppress file size if we're not showing cost details */
	if (es->costs)
	{
		struct stat		stat_buf;

		if (stat(filename, &stat_buf) == 0)
			ExplainPropertyLong("Foreign File Size", (long) stat_buf.st_size,
								es);
	}
}

/*
 * fileBeginForeignScan
 *		Initiate access to the file by creating CopyState
 */
static void
fileBeginForeignScan(ForeignScanState *node, int eflags)
{
	char	   *filename;
	List	   *options;
	CopyState	cstate;
	FileFdwExecutionState *festate;
    char       *null_print = NULL;
    bool        is_csv = false;
	ListCell   *lc;

	check_table_shape(node->ss.ss_currentRelation);

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Fetch options of foreign table */
	fileGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				   &filename, &options);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns,
	 * so as to match the expected ScanTupleSlot signature.
	 */
	cstate = BeginCopyFrom(node->ss.ss_currentRelation,
						   filename,
						   NIL,
						   options);

	/*
	 * Save state in node->fdw_state.  We must save enough information to call
	 * BeginCopyFrom() again.
	 */
	festate = (FileFdwExecutionState *) palloc(sizeof(FileFdwExecutionState));
	festate->filename = filename;
	festate->options = options;
	festate->cstate = cstate;

    /* set up the work area we'll use to construct the array */
	festate->text_array_stash_size = FILE_FDW_TEXTARRAY_STASH_INIT;
	festate->text_array_values =
		palloc(FILE_FDW_TEXTARRAY_STASH_INIT * sizeof(Datum));
	festate->text_array_nulls =
		palloc(FILE_FDW_TEXTARRAY_STASH_INIT * sizeof(bool));

	/* calculate whether we're using an empty string for null */
	foreach(lc, options)
	{
		DefElem	   *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "format") == 0)
		{
			char * fname = defGetString(def);
			if (strcmp(fname, "csv") == 0)
				is_csv = true;
		}
		else if (strcmp(def->defname, "null") == 0)
		{
			null_print =  defGetString(def);
		}
	}
	if (null_print == NULL) /* option not set - we're using the default */
		festate->emptynull =  is_csv;
	else 
		festate->emptynull = (strlen(null_print) == 0);

	node->fdw_state = (void *) festate;
}

/*
 * fileIterateForeignScan
 *		Read next record from the data file and store it into the
 *		ScanTupleSlot as a virtual tuple
 */
static TupleTableSlot *
fileIterateForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	bool			found;
	ErrorContextCallback errcontext;
	char          **raw_fields;
	int             nfields;
        

	/* Set up callback to identify error line number. */
	errcontext.callback = CopyFromErrorCallback;
	errcontext.arg = (void *) festate->cstate;
	errcontext.previous = error_context_stack;
	error_context_stack = &errcontext;

	/*
	 * The protocol for loading a virtual tuple into a slot is first
	 * ExecClearTuple, then fill the values/isnull arrays, then
	 * ExecStoreVirtualTuple.  If we don't find another row in the file,
	 * we just skip the last step, leaving the slot empty as required.
	 *
	 * We can pass ExprContext = NULL because we read all columns from the
	 * file, so no need to evaluate default expressions.
	 *
	 * We can also pass tupleOid = NULL because we don't allow oids for
	 * foreign tables.
	 */
	ExecClearTuple(slot);


	found = NextCopyFromRawFields(festate->cstate, &raw_fields, &nfields);

	if (found)
	{
		makeTextArray(festate, slot, raw_fields, nfields);
 		ExecStoreVirtualTuple(slot);
	}

	/* Remove error callback. */
	error_context_stack = errcontext.previous;

	return slot;
}

/*
 * fileEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
fileEndForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;

	/* if festate is NULL, we are in EXPLAIN; nothing to do */
	if (festate)
		EndCopyFrom(festate->cstate);
}

/*
 * fileReScanForeignScan
 *		Rescan table, possibly with new parameters
 */
static void
fileReScanForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;

	EndCopyFrom(festate->cstate);

	festate->cstate = BeginCopyFrom(node->ss.ss_currentRelation,
									festate->filename,
									NIL,
									festate->options);
}

/*
 * Estimate costs of scanning a foreign table.
 */
static void
estimate_costs(PlannerInfo *root, RelOptInfo *baserel,
			   const char *filename,
			   Cost *startup_cost, Cost *total_cost)
{
	struct stat		stat_buf;
	BlockNumber		pages;
	int				tuple_width;
	double			ntuples;
	double			nrows;
	Cost			run_cost = 0;
	Cost			cpu_per_tuple;

	/*
	 * Get size of the file.  It might not be there at plan time, though,
	 * in which case we have to use a default estimate.
	 */
	if (stat(filename, &stat_buf) < 0)
		stat_buf.st_size = 10 * BLCKSZ;

	/*
	 * Convert size to pages for use in I/O cost estimate below.
	 */
	pages = (stat_buf.st_size + (BLCKSZ-1)) / BLCKSZ;
	if (pages < 1)
		pages = 1;

	/*
	 * Estimate the number of tuples in the file.  We back into this estimate
	 * using the planner's idea of the relation width; which is bogus if not
	 * all columns are being read, not to mention that the text representation
	 * of a row probably isn't the same size as its internal representation.
	 * FIXME later.
	 */
	tuple_width = MAXALIGN(baserel->width) + MAXALIGN(sizeof(HeapTupleHeaderData));

	ntuples = clamp_row_est((double) stat_buf.st_size / (double) tuple_width);

	/*
	 * Now estimate the number of rows returned by the scan after applying
	 * the baserestrictinfo quals.  This is pretty bogus too, since the
	 * planner will have no stats about the relation, but it's better than
	 * nothing.
	 */
	nrows = ntuples *
		clauselist_selectivity(root,
							   baserel->baserestrictinfo,
							   0,
							   JOIN_INNER,
							   NULL);

	nrows = clamp_row_est(nrows);

	/* Save the output-rows estimate for the planner */
	baserel->rows = nrows;

	/*
	 * Now estimate costs.  We estimate costs almost the same way as
	 * cost_seqscan(), thus assuming that I/O costs are equivalent to a
	 * regular table file of the same size.  However, we take per-tuple CPU
	 * costs as 10x of a seqscan, to account for the cost of parsing records.
	 */
	run_cost += seq_page_cost * pages;

	*startup_cost = baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost * 10 + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;
	*total_cost = *startup_cost + run_cost;
}

/*
 * Make sure the table is the right shape. i.e. it must have exactly one column,
 * which must be of type text[]
 */

static void
check_table_shape(Relation rel)
{
	TupleDesc       tupDesc;
	Form_pg_attribute *attr; 
	int         attr_count;
	int         i;
	int         elem1 = -1;

	tupDesc = RelationGetDescr(rel);
	attr = tupDesc->attrs;
	attr_count  = tupDesc->natts;

	for (i = 0; i < attr_count; i++)
	{
		if (attr[i]->attisdropped)
			continue;
		if (elem1 > -1)
			ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_REPLY),
				 errmsg("table for file_textarray_fdw foreign tables must have only one column")));
		elem1 = i;
	}
	if (elem1 == -1)
			ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_REPLY),
				 errmsg("table for file_textarray_fdw foreign tables must have one column")));
		;
	if (tupDesc->attrs[elem1]->atttypid != TEXTARRAYOID)
			ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_REPLY),
				 errmsg("table for file_textarray_fdw foreign tables must consist of a text[] column")));

}

/*
 * Construct the text array from the read in data, and stash it in the slot 
 */ 

static void 
makeTextArray(FileFdwExecutionState *fdw_private, TupleTableSlot *slot, char **raw_fields, int nfields)
{
	Datum     *values;
	bool      *nulls;
	int        dims[1];
	int        lbs[1];
	int        fld;
	Datum      result;
	int        fldct = nfields;
	char      *string;

	if (nfields == 1 && 
		raw_fields[0] == NULL  
		&& fdw_private->emptynull
		   )
	{
		/* Treat an empty line as having no fields */
		fldct = 0;
	}	
	else if (nfields > fdw_private->text_array_stash_size)
	{
		/* make sure the workspace is big enough */
		while (fdw_private->text_array_stash_size < nfields)
			fdw_private->text_array_stash_size *= 2;

		fdw_private->text_array_values =repalloc(
			fdw_private->text_array_values,
			fdw_private->text_array_stash_size * sizeof(Datum));
		fdw_private->text_array_nulls =repalloc(
			fdw_private->text_array_nulls,
			fdw_private->text_array_stash_size * sizeof(bool));		
	}

	values = fdw_private->text_array_values;
	nulls = fdw_private->text_array_nulls;

	dims[0] = fldct;
	lbs[0] = 1; /* sql arrays typically start at 1 */

	for (fld=0; fld < fldct; fld++)
	{
		string = raw_fields[fld];

		if (string == NULL)
		{
			values[fld] = PointerGetDatum(NULL);
			nulls[fld] = true;
		}
		else
		{
			nulls[fld] = false;
			values[fld] = PointerGetDatum(
				DirectFunctionCall1(textin, 
									PointerGetDatum(string)));
		}
	}

	result = PointerGetDatum(construct_md_array(
								 values, 
								 nulls,
								 1,
								 dims,
								 lbs,
								 TEXTOID,
								 -1,
								 false,
								 'i'));

	slot->tts_values[0] = result;
	slot->tts_isnull[0] = false;

}
