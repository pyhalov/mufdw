/*----------------------------------------------------------------------
 *
 * Minimal Usable Foreign Data Wrapper
 *
 * Portions Copyright (c) 2012-2021, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2023 Alexander Pyhalov
 * IDENTIFICATION
 *		  mufdw.c
 *
 *----------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "commands/defrem.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

typedef struct mufdwRelationInfo
{
	char	   *table_name;
	char	   *schema_name;
}			mufdwRelationInfo;

typedef struct mufdwScanState
{
	char	   *query;			/* text of SELECT command */
	char	   *portal_name;
}			mufdwScanState;

PG_FUNCTION_INFO_V1(mufdw_handler);
PG_FUNCTION_INFO_V1(mufdw_validator);

/*
 * mufdw validator - we allow only schema name and table name options
 * for foreign table. Foreign server or mapping can't have any options.
 */
Datum
mufdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	int			opt_cnt = 0;

	ListCell   *lc;

	foreach(lc, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (catalog != ForeignTableRelationId)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("option \"%s\" is not allowed in this context", def->defname)));
		if (strcmp(def->defname, "table_name") != 0 && strcmp(def->defname, "schema_name") != 0)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("option \"%s\" is unknown", def->defname)));

		opt_cnt++;
	}

	if (catalog == ForeignTableRelationId && (opt_cnt != 2))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("both table_name and table_schema options should be set")));


	PG_RETURN_VOID();
}

/*
 * mufdwGetForeignRelSize
 *              Estimate # of rows and width of the result of the scan
 *
 * We should consider the effect of all baserestrictinfo clauses here, but
 * not any join clauses.
 */
static void
mufdwGetForeignRelSize(PlannerInfo *root,
					   RelOptInfo *baserel,
					   Oid foreigntableid)
{
	mufdwRelationInfo *fpinfo;	/* fdw private information */
	ListCell   *lc;
	ForeignTable *table;

	/* construct private fpinfo */
	fpinfo = (mufdwRelationInfo *) palloc0(sizeof(mufdwRelationInfo));

	baserel->fdw_private = fpinfo;

	table = GetForeignTable(foreigntableid);

	/* apply table options */
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "table_name") == 0)
			fpinfo->table_name = defGetString(def);
		else if (strcmp(def->defname, "schema_name") == 0)
			fpinfo->schema_name = defGetString(def);
	}
	/* Now do the actual size estimation */
	if (baserel->tuples < 0)
	{
		/*
		 * When table lacks statistics (and this is always the case for mufdw,
		 * as we haven't implemented stat gathering), reltuples is < 0,
		 * meaning "unknown". We can't do much without consulting the "remote"
		 * server, but we can use a hack similar to plancat.c's treatment of
		 * empty relations: use a minimum size * estimate of 10 pages, and
		 * divide by the column-datatype-based width estimate to get the
		 * corresponding number of tuples.
		 */
		baserel->pages = 10;
		baserel->tuples =
			(10 * BLCKSZ) / (baserel->reltarget->width +
							 MAXALIGN(SizeofHeapTupleHeader));
	}

	/* Estimate baserel size as best we can with local statistics. */
	set_baserel_size_estimates(root, baserel);
}

/*
 * mufdwGetForeignPaths
 *              Create possible scan paths for a scan on the foreign table
 */
static void
mufdwGetForeignPaths(PlannerInfo *root,
					 RelOptInfo *baserel,
					 Oid foreigntableid)
{
	ForeignPath *path;
	Cost		total_cost = 0;


	total_cost += seq_page_cost * baserel->pages;
	total_cost += cpu_tuple_cost * baserel->tuples;

	/*
	 * Create simplest ForeignScan path node and add it to baserel.  This path
	 * corresponds to SeqScan path of regular tables.
	 *
	 * Although this path uses no join clauses, it could still have required
	 * parameterization due to LATERAL refs in its tlist.
	 */
	path = create_foreignscan_path(root, baserel,
								   NULL,	/* default pathtarget */
								   baserel->rows,
								   1.0 /* startup cost */ ,
								   total_cost /* total cost */ ,
								   NIL, /* no pathkeys */
								   baserel->lateral_relids,
								   NULL,	/* no extra plan */
								   NIL);	/* no fdw_private list */

	add_path(baserel, (Path *) path);
}

/*
 * mufdwGetForeignPlan
 *              Create ForeignScan plan node which implements selected best path
 */
static ForeignScan *
mufdwGetForeignPlan(PlannerInfo *root,
					RelOptInfo *foreignrel,
					Oid foreigntableid,
					ForeignPath *best_path,
					List *tlist,
					List *scan_clauses,
					Plan *outer_plan)
{
	mufdwRelationInfo *fpinfo = (mufdwRelationInfo *) foreignrel->fdw_private;
	List	   *fdw_private;
	char	   *query;

	/*
	 * Put all the scan_clauses into the plan node's qual list for the
	 * executor to check.  So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Construct query */
	query = psprintf("SELECT * FROM %s.%s", quote_identifier(fpinfo->schema_name), quote_identifier(fpinfo->table_name));

	/* FDW private is supposed to be a list of nodes. */
	fdw_private = list_make1(makeString(query));

	return make_foreignscan(tlist, scan_clauses, foreignrel->relid, NIL /* no params list */ , fdw_private, NIL /* no custom tlist */ , NIL /* no remote quals */ , outer_plan);
}

static char *
open_new_cursor(char *query)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	SPIPlanPtr	plan;
	Portal		portal;
	char	   *portal_name;

	SPI_connect();

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		elog(ERROR, "SPI_prepare(\"%s\") failed", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

	portal_name = MemoryContextStrdup(oldcontext, portal->name);

	SPI_finish();

	return portal_name;
}

static void
close_old_cursor(mufdwScanState * fsstate)
{
	Portal		portal;

	if (fsstate->portal_name)
	{
		SPI_connect();
		portal = SPI_cursor_find(fsstate->portal_name);
		if (portal)
			SPI_cursor_close(portal);
		SPI_finish();

		pfree(fsstate->portal_name);
	}
}

/*
 * mufdwBeginForeignScan
 * 		 Initiate an executor scan of a foreign table.
 */
static void
mufdwBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	mufdwScanState *fsstate;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* private scan state */
	fsstate = (mufdwScanState *) palloc0(sizeof(mufdwScanState));
	node->fdw_state = (void *) fsstate;
	fsstate->query = strVal(linitial(fsplan->fdw_private));

	fsstate->portal_name = open_new_cursor(fsstate->query);
}

/*
 * mufdwReScanForeignScan
 *              Restart the scan.
 */
static void
mufdwReScanForeignScan(ForeignScanState *node)
{
	mufdwScanState *fsstate = (mufdwScanState *) node->fdw_state;

	close_old_cursor(fsstate);
	fsstate->portal_name = open_new_cursor(fsstate->query);
}

/*
 * mufdwIterateForeignScan
 * 	Iterate foreign scan
 */
static TupleTableSlot *
mufdwIterateForeignScan(ForeignScanState *node)
{
	mufdwScanState *fsstate = (mufdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	Portal		portal;

	ExecClearTuple(slot);
	SPI_connect();
	/* Find a portal for the query */
	portal = SPI_cursor_find(fsstate->portal_name);
	if (portal)
	{
		HeapTuple	tuple;
		TupleDesc	tupdesc;

		/* Fetch one tuple */
		SPI_cursor_fetch(portal, true, 1);
		if (SPI_processed == 0)
		{
			SPI_finish();
			return NULL;
		}
		tupdesc = SPI_tuptable->tupdesc;
		tuple = SPI_tuptable->vals[0];
		/* Store it in the scan slot */
		heap_deform_tuple(tuple, tupdesc, slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		/*
		 * Tuple data is allocated in SPI context, so we have to materialize
		 * slot
		 */
		ExecMaterializeSlot(slot);
	}
	SPI_finish();

	return slot;
}

/*
 * mufdwEndForeignScan
 * 	End foreign scan
 */
static void
mufdwEndForeignScan(ForeignScanState *node)
{
}


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
mufdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* Functions for scanning foreign tables */
	routine->GetForeignRelSize = mufdwGetForeignRelSize;
	routine->GetForeignPaths = mufdwGetForeignPaths;
	routine->GetForeignPlan = mufdwGetForeignPlan;
	routine->BeginForeignScan = mufdwBeginForeignScan;
	routine->IterateForeignScan = mufdwIterateForeignScan;
	routine->ReScanForeignScan = mufdwReScanForeignScan;
	routine->EndForeignScan = mufdwEndForeignScan;

	PG_RETURN_POINTER(routine);
}
