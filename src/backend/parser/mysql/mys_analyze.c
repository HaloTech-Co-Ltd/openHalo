/*-------------------------------------------------------------------------
 *
 * mys_analyze.c
 *	  MySQL specific analyze
 *
 * 
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 * 
 * 易景科技是Halo Database、Halo Database Management System、羲和数据
 * 库、羲和数据库管理系统（后面简称 Halo ）、openHalo软件的发明人同时也为
 * 知识产权权利人。Halo 软件的知识产权，以及与本软件相关的所有信息内容（包括
 * 但不限于文字、图片、音频、视频、图表、界面设计、版面框架、有关数据或电子文
 * 档等）均受中华人民共和国法律法规和相应的国际条约保护，易景科技享有上述知识
 * 产权，但相关权利人依照法律规定应享有的权利除外。未免疑义，本条所指的“知识
 * 产权”是指任何及所有基于 Halo 软件产生的：（a）版权、商标、商号、域名、与
 * 商标和商号相关的商誉、设计和专利；与创新、技术诀窍、商业秘密、保密技术、非
 * 技术信息相关的权利；（b）人身权、掩模作品权、署名权和发表权；以及（c）在
 * 本协议生效之前已存在或此后出现在世界任何地方的其他工业产权、专有权、与“知
 * 识产权”相关的权利，以及上述权利的所有续期和延长，无论此类权利是否已在相
 * 关法域内的相关机构注册。
 *
 * This software and related documentation are provided under a license
 * agreement containing restrictions on use and disclosure and are 
 * protected by intellectual property laws. Except as expressly permitted
 * in your license agreement or allowed by law, you may not use, copy, 
 * reproduce, translate, broadcast, modify, license, transmit, distribute,
 * exhibit, perform, publish, or display any part, in any form, or by any
 * means. Reverse engineering, disassembly, or decompilation of this 
 * software, unless required by law for interoperability, is prohibited.
 * 
 * This software is developed for general use in a variety of
 * information management applications. It is not developed or intended
 * for use in any inherently dangerous applications, including applications
 * that may create a risk of personal injury. If you use this software or
 * in dangerous applications, then you shall be responsible to take all
 * appropriate fail-safe, backup, redundancy, and other measures to ensure
 * its safe use. Halo Corporation and its affiliates disclaim any 
 * liability for any damages caused by use of this software in dangerous
 * applications.
 * 
 *
 * IDENTIFICATION
 *	  src/backend/parser/mysql/mys_analyze.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/skey.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_proc.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/mysql/mys_parsenodes.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parserapi.h"
#include "parser/mysql/mys_analyze.h"
#include "parser/mysql/mys_parse_agg.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "optimizer/optimizer.h"
#include "adapter/mysql/adapter.h"


typedef struct 
{
    char alias[NAMEDATALEN];   /* hash key must be first item */
    Node *expr;
} AliasExpr;

static Query *transformMysVariableSetStmt(ParseState *pstate, MysVariableSetStmt *stmt);
static Query *transformMysSelectIntoStmt(ParseState *pstate, MysSelectIntoStmt *stmt);
static Query *transformUpdateStmtInternal(ParseState *pstate, UpdateStmt *stmt);
static List *retrieveSetTargets(List *totalTargetList, RangeVar *relation);
static Node *mergeWhereClauseOnClause(Node *whereClause, Node *onClause);
static void rectifyExpr(RangeVar *relation, char *newVal, Node *expr);
static void rectifyColumnRef(RangeVar *relation, char *newVal, ColumnRef *columnRef);
static Query *transformUpdateStmtToCte(ParseState *pstate, UpdateStmt *stmt);
static void determineRecursiveColTypes(ParseState *pstate, Node *larg, List *nrtargetlist);
static void rectifyHavingClause(SelectStmt *stmt);
static void rectifyHavingAlias(HTAB *aliases, Node *expr);


/*
 * mys_transformOptionalSelectInto -
 *	  If SELECT has INTO, convert it to MysSelectIntoStmt.
 *
 * The only thing we do here that we don't do in transformStmt() is to
 * convert SELECT ... INTO into MysSelectIntoStmt.  Since utility statements
 * aren't allowed within larger statements, this is only allowed at the top
 * of the parse tree, and so we only try it before entering the recursive
 * transformStmt() processing.
 */
Query *
mys_transformOptionalSelectInto(ParseState *pstate, Node *parseTree)
{
	if (IsA(parseTree, SelectStmt))
	{
		SelectStmt *stmt = (SelectStmt *) parseTree;

		/* If it's a set-operation tree, drill down to leftmost SelectStmt */
		while (stmt && stmt->op != SETOP_NONE)
			stmt = stmt->larg;
		Assert(stmt && IsA(stmt, SelectStmt) && stmt->larg == NULL);

		if (stmt->intoClause)
		{
			MysSelectIntoStmt *newStmt = makeNode(MysSelectIntoStmt);

            newStmt->target = stmt->intoClause->colNames;
            newStmt->expr = parseTree;

			/*
			 * Remove the intoClause from the SelectStmt.  This makes it safe
			 * for transformSelectStmt to complain if it finds intoClause set
			 * (implying that the INTO appeared in a disallowed place).
			 */
			stmt->intoClause = NULL;

			parseTree = (Node *) newStmt;
		}
	}

	return transformStmt(pstate, parseTree);
}

/*
 * mys_transformStmt -
 *	  recursively transform a Parse tree into a Query tree.
 */
Query *
mys_transformStmt(ParseState *pstate, Node *parseTree)
{
	Query	   *result;

	/*
	 * We apply RAW_EXPRESSION_COVERAGE_TEST testing to basic DML statements;
	 * we can't just run it on everything because raw_expression_tree_walker()
	 * doesn't claim to handle utility statements.
	 */
#ifdef RAW_EXPRESSION_COVERAGE_TEST
	switch (nodeTag(parseTree))
	{
		case T_SelectStmt:
		case T_InsertStmt:
		case T_UpdateStmt:
		case T_DeleteStmt:
			(void) test_raw_expression_coverage(parseTree, NULL);
			break;
		default:
			break;
	}
#endif							/* RAW_EXPRESSION_COVERAGE_TEST */

	/*
	 * Caution: when changing the set of statement types that have non-default
	 * processing here, see also stmt_requires_parse_analysis() and
	 * analyze_requires_snapshot().
	 */
	switch (nodeTag(parseTree))
	{
			/*
			 * Optimizable statements
			 */
		case T_InsertStmt:
			result = transformInsertStmt(pstate, (InsertStmt *) parseTree);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_UpdateStmt:
			result = transformUpdateStmt(pstate, (UpdateStmt *) parseTree);
			break;

		case T_SelectStmt:
			{
				SelectStmt *n = (SelectStmt *) parseTree;

				if (n->valuesLists)
					result = transformValuesClause(pstate, n);
				else if (n->op == SETOP_NONE)
					result = transformSelectStmt(pstate, n);
				else
					result = transformSetOperationStmt(pstate, n);
			}
			break;

		case T_ReturnStmt:
			result = transformReturnStmt(pstate, (ReturnStmt *) parseTree);
			break;

		case T_PLAssignStmt:
			result = transformPLAssignStmt(pstate,
										   (PLAssignStmt *) parseTree);
			break;

			/*
			 * Special cases
			 */
		case T_DeclareCursorStmt:
			result = transformDeclareCursorStmt(pstate,
												(DeclareCursorStmt *) parseTree);
			break;

		case T_ExplainStmt:
			result = transformExplainStmt(pstate,
										  (ExplainStmt *) parseTree);
			break;

		case T_CreateTableAsStmt:
			result = transformCreateTableAsStmt(pstate,
												(CreateTableAsStmt *) parseTree);
			break;

		case T_CallStmt:
			result = transformCallStmt(pstate,
									   (CallStmt *) parseTree);
			break;
        
        case T_MysVariableSetStmt:
            result = transformMysVariableSetStmt(pstate, (MysVariableSetStmt *) parseTree);
            break;

        case T_MysSelectIntoStmt:
            result = transformMysSelectIntoStmt(pstate, (MysSelectIntoStmt *) parseTree);
            break;

		default:

			/*
			 * other statements don't require any transformation; just return
			 * the original parsetree with a Query node plastered on top.
			 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}

	/* Mark as original query until we learn differently */
	result->querySource = QSRC_ORIGINAL;
	result->canSetTag = true;

	return result;
}

static Query *
transformMysVariableSetStmt(ParseState *pstate, MysVariableSetStmt *stmt)
{
    Query *result;

	/* transform contained query(SelectStmt) */
	stmt->varSetStmt = (Node *) transformOptionalSelectInto(pstate, stmt->varSetStmt);

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

/*
 * For select into,
 * stored procedures have their own process, can't reach here
 */
static Query *
transformMysSelectIntoStmt(ParseState *pstate, MysSelectIntoStmt *stmt)
{
    Query *result;
    ListCell *lc;
    int targetNum = list_length(stmt->target);

    foreach(lc, stmt->target)
    {
        if (IsA(lfirst(lc), UserVarRef))
        {
            /* OK */
        }
        else
        {
            elog(ERROR, "Undeclared variable");
        }
    }

	/* transform contained query(SelectStmt) */
	stmt->expr = (Node *) transformStmt(pstate, stmt->expr);

    foreach(lc, ((Query *)(stmt->expr))->targetList)
    {
        TargetEntry *n = (TargetEntry *)lfirst(lc);

        if (n->resjunk == false)
        {
            targetNum--;
        }
    }

    if (targetNum != 0)
    {
        elog(ERROR, "The used SELECT statements have a different number of columns");
    }

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

static Query *
transformUpdateStmtInternal(ParseState *pstate, UpdateStmt *stmt)
{
    Query	   *qry = makeNode(Query);
    ParseNamespaceItem *nsitem;
    Node	   *qual;

    qry->commandType = CMD_UPDATE;
    pstate->p_is_insert = false;

    /* process the WITH clause independently of all else */
    if (stmt->withClause)
    {
        qry->hasRecursive = stmt->withClause->recursive;
        qry->cteList = transformWithClause(pstate, stmt->withClause);
        qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
    }

    qry->resultRelation = setTargetTable(pstate, stmt->relation,
                                         stmt->relation->inh,
                                         true,
                                         ACL_UPDATE);
    nsitem = pstate->p_target_nsitem;

    /* subqueries in FROM cannot access the result relation */
    nsitem->p_lateral_only = true;
    nsitem->p_lateral_ok = false;

    /*
     * the FROM clause is non-standard SQL syntax. We used to be able to do
     * this with REPLACE in POSTQUEL so we keep the feature.
     */
    transformFromClause(pstate, stmt->fromClause);

    /* remaining clauses can reference the result relation normally */
    nsitem->p_lateral_only = false;
    nsitem->p_lateral_ok = true;

    qual = transformWhereClause(pstate, stmt->whereClause,
                                EXPR_KIND_WHERE, "WHERE");

    qry->returningList = transformReturningList(pstate, stmt->returningList);

    /*
     * Now we are done with SELECT-like processing, and can get on with
     * transforming the target list to match the UPDATE target columns.
     */
    qry->targetList = transformUpdateTargetList(pstate, stmt->targetList);

    qry->sortClause = transformSortClause(pstate,
                                          stmt->sortClause,
                                          &qry->targetList,
                                          EXPR_KIND_ORDER_BY,
                                          false /* allow SQL92 rules */ ); 

    /* transform LIMIT */
    qry->limitOffset = transformLimitClause(pstate, 
                                            stmt->limitOffset,                                                                                                     
                                            EXPR_KIND_OFFSET, "OFFSET",
                                            stmt->limitOption);

    qry->limitCount = transformLimitClause(pstate, 
                                           stmt->limitCount,
                                           EXPR_KIND_LIMIT, "LIMIT",
                                           stmt->limitOption);

    qry->limitOption = stmt->limitOption;

    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

    qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
    qry->hasSubLinks = pstate->p_hasSubLinks;

    assign_query_collations(pstate, qry);

    return qry;
}

/*
 * mys_stmt_requires_parse_analysis
 *		Returns true if parse analysis will do anything non-trivial
 *		with the given raw parse tree.
 *
 * Generally, this should return true for any statement type for which
 * transformStmt() does more than wrap a CMD_UTILITY Query around it.
 * When it returns false, the caller can assume that there is no situation
 * in which parse analysis of the raw statement could need to be re-done.
 *
 * Currently, since the rewriter and planner do nothing for CMD_UTILITY
 * Queries, a false result means that the entire parse analysis/rewrite/plan
 * pipeline will never need to be re-done.  If that ever changes, callers
 * will likely need adjustment.
 */
bool
mys_stmt_requires_parse_analysis(RawStmt *parseTree)
{
	bool		result;

	switch (nodeTag(parseTree->stmt))
	{
			/*
			 * Optimizable statements
			 */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_SelectStmt:
		case T_ReturnStmt:
		case T_PLAssignStmt:
			result = true;
			break;

			/*
			 * Special cases
			 */
		case T_DeclareCursorStmt:
		case T_ExplainStmt:
		case T_CreateTableAsStmt:
		case T_CallStmt:
        case T_MysVariableSetStmt:
			result = true;
			break;

		default:
			/* all other statements just get wrapped in a CMD_UTILITY Query */
			result = false;
			break;
	}

	return result;
}

/*
 * mys_analyze_requires_snapshot
 *		Returns true if a snapshot must be set before doing parse analysis
 *		on the given raw parse tree.
 */
bool
mys_analyze_requires_snapshot(RawStmt *parseTree)
{
	return mys_stmt_requires_parse_analysis(parseTree);
}

static void
mys_construct_returningList(ParseState *pstate, InsertStmt *stmt)
{
    Relation resultRelation;
    TupleDesc tupledesc;
    TupleConstr *constr;
    //Oid namespaceId;
    //char seqname[512];
    char *auto_increment_column;
    //int i = 0;

    Assert(stmt->returningList == NULL);

    resultRelation = pstate->p_target_relation;
    tupledesc = resultRelation->rd_att;
    constr = tupledesc->constr;
    //namespaceId = ((Form_pg_class)(resultRelation->rd_rel))->relnamespace;

    if ((constr == NULL) || (constr != NULL && constr->num_defval <= 0))
    {
        return;
    }
    else
    {
        Relation attrdef;
        Relation depRel;
        int i = 0;

        attrdef = relation_open(AttrDefaultRelationId, AccessShareLock);
        depRel = relation_open(DependRelationId, AccessShareLock);
        for (i = 0; i < constr->num_defval; i++)
        {
            bool isAutoIncrement = false;
            AttrDefault attrdefault = constr->defval[i];
            ScanKeyData keys[2];
            SysScanDesc scan;
            HeapTuple attrdefTuple;
            Oid columnDefaultOid;
            HeapTuple depTuple;

            ScanKeyInit(&keys[0],
                        Anum_pg_attrdef_adrelid,
                        BTEqualStrategyNumber,
                        F_OIDEQ,
                        ObjectIdGetDatum(RelationGetRelid(resultRelation)));
            ScanKeyInit(&keys[1],
                        Anum_pg_attrdef_adnum,
                        BTEqualStrategyNumber,
                        F_INT2EQ,
                        Int16GetDatum(attrdefault.adnum));
            scan = systable_beginscan(attrdef, AttrDefaultIndexId, true,
                                      NULL, 2, keys);
            attrdefTuple = systable_getnext(scan);
            columnDefaultOid = ((Form_pg_attrdef)GETSTRUCT(attrdefTuple))->oid;
            systable_endscan(scan);

            ScanKeyInit(&keys[0],
                        Anum_pg_depend_classid,
                        BTEqualStrategyNumber, F_OIDEQ,
                        ObjectIdGetDatum(AttrDefaultRelationId));
	        ScanKeyInit(&keys[1],
                        Anum_pg_depend_objid,
                        BTEqualStrategyNumber, F_OIDEQ,
                        ObjectIdGetDatum(columnDefaultOid));
            
            scan = systable_beginscan(depRel, DependDependerIndexId, true,
							          NULL, 2, keys);
            while (HeapTupleIsValid(depTuple = systable_getnext(scan)))
            {
                Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

                if (foundDep->refclassid == RelationRelationId &&
                    foundDep->objsubid == 0 &&
                    foundDep->refobjsubid == 0 &&
                    foundDep->deptype == DEPENDENCY_NORMAL &&
                    get_rel_relkind(foundDep->refobjid) == RELKIND_SEQUENCE)
                {
                    isAutoIncrement = true;
                }
            }
            systable_endscan(scan);

            if (isAutoIncrement)
            {
                FormData_pg_attribute attr = tupledesc->attrs[attrdefault.adnum - 1];
                ResTarget *restarget = makeNode(ResTarget);
                ColumnRef *columnref = makeNode(ColumnRef);
                List *returning = list_make1(restarget);

                auto_increment_column = pstrdup(NameStr(attr.attname));

                columnref->fields = lcons(makeString(auto_increment_column), NULL);
                restarget->name = NULL;
                restarget->indirection = NIL;
                restarget->val = (Node *)columnref;

                stmt->returningList = returning;

                break;
            }
        }
        relation_close(attrdef, AccessShareLock);
        relation_close(depRel, AccessShareLock);
    }

    // for (i = 0; i < tupledesc->natts; i++)
    // {
    //     Oid seq_oid;
    //     FormData_pg_attribute attr = tupledesc->attrs[i];
    //     sprintf(seqname, "%s_%s_seq", NameStr(((Form_pg_class)(resultRelation->rd_rel))->relname), NameStr(attr.attname));

    //     seq_oid = GetSysCacheOid2(RELNAMENSP, Anum_pg_class_oid,
	// 							 PointerGetDatum(seqname),
	// 							 ObjectIdGetDatum(namespaceId));
        
    //     if (OidIsValid(seq_oid))
    //     {
    //         ResTarget *restarget = makeNode(ResTarget);
    //         ColumnRef *columnref = makeNode(ColumnRef);
    //         List *returning = list_make1(restarget);

    //         auto_increment_column = strdup(NameStr(attr.attname));

    //         columnref->fields = lcons(makeString(auto_increment_column), NULL);
    //         restarget->name = NULL;
    //         restarget->indirection = NIL;
    //         restarget->val = (Node *)columnref;

    //         stmt->returningList = returning;
    //         break;
    //     }
    // }
}

/*
 * mys_transformSelectStmt -
 *	  transforms a Select Statement
 *
 * Note: this covers only cases with no set operations and no VALUES lists;
 * see below for the other cases.
 */

static void
rectifyHavingClause(SelectStmt *stmt)
{
    HTAB *aliases;
    HASHCTL hashctl;
    ListCell *lc;

    hashctl.keysize = NAMEDATALEN;
    hashctl.entrysize = sizeof(AliasExpr);
    aliases = hash_create("Alias-Expre in target list", 
                          2048, 
                          &hashctl, 
                          HASH_ELEM | HASH_STRINGS);

    foreach (lc, stmt->targetList)
    {
        ResTarget *rt = (ResTarget *) lfirst(lc);
        if (rt->name != NULL)
        {
            bool found;
            AliasExpr *alaisExpr;
            alaisExpr = (AliasExpr *)hash_search(aliases, 
                                                 rt->name, 
                                                 HASH_ENTER, 
                                                 &found);
            if (!found)
            {
                alaisExpr->expr = rt->val;
            }
        }
    }

    rectifyHavingAlias(aliases, stmt->havingClause);
}

static void
rectifyHavingAlias(HTAB *aliases, Node *expr)
{
    /* 如果having中用的是自定义函数的别名，则最终结果与MySQl不同 */
    /* MySQL其实不允许这种情况： */
    /* Can't update table 'tab1' in stored function/trigger because it is already used by statement which invoked this stored function/trigger. */
    /* 实际中，having一般不会这么使用。 */
    if (IsA(expr, BoolExpr))
    {
		BoolExpr *blexpr;
        ListCell *lc;
		blexpr = castNode(BoolExpr, expr);
        foreach (lc, blexpr->args)
        {
            Node *node = lfirst(lc);
            rectifyHavingAlias(aliases, node);
        }
    }
    else 
    {
        if (IsA(expr, A_Expr))
        {
            A_Expr *aExpr = castNode(A_Expr, expr);
            Node *lNode = aExpr->lexpr;
            Node *rNode = aExpr->rexpr;
            if ((lNode != NULL) && IsA(lNode, ColumnRef))
            {
                ColumnRef *cr = castNode(ColumnRef, lNode);
                if (list_length(cr->fields) == 1)
                {
                    char *nm;
                    AliasExpr *aliasExpr;
                    bool found;
                    nm = strVal(linitial(cr->fields));
                    aliasExpr = (AliasExpr *)hash_search(aliases, 
                                                         nm,
                                                         HASH_FIND, 
                                                         &found);
                    if (found)
                    {
                        aExpr->lexpr = copyObject(aliasExpr->expr);
                    }
                }
            }
            if ((rNode != NULL) && IsA(rNode, ColumnRef))
            {
                ColumnRef *cr = castNode(ColumnRef, rNode);
                if (list_length(cr->fields) == 1)
                {
                    char *nm;
                    AliasExpr *aliasExpr;
                    bool found;
                    nm = strVal(linitial(cr->fields));
                    aliasExpr = (AliasExpr *)hash_search(aliases, 
                                                         nm,
                                                         HASH_FIND, 
                                                         &found);
                    if (found)
                    {
                        aExpr->rexpr = copyObject(aliasExpr->expr);
                    }
                }
            }
        }
        else if (IsA(expr, NullTest))
        {
            NullTest *nt = castNode(NullTest, expr);
            Node *node = (Node *)(nt->arg);
            if ((node != NULL) && IsA(node, ColumnRef))
            {
                ColumnRef *cr = castNode(ColumnRef, node);
                if (list_length(cr->fields) == 1)
                {
                    char *nm;
                    AliasExpr *aliasExpr;
                    bool found;
                    nm = strVal(linitial(cr->fields));
                    aliasExpr = (AliasExpr *)hash_search(aliases, 
                                                         nm,
                                                         HASH_FIND, 
                                                         &found);
                    if (found)
                    {
                        nt->arg = (Expr *)copyObject(aliasExpr->expr);
                    }
                }
            }
        }
        else if (IsA(expr, BooleanTest))
        {
            BooleanTest *bt = castNode(BooleanTest, expr);
            Node *node = (Node *)(bt->arg);
            if ((node != NULL) && IsA(node, ColumnRef))
            {
                ColumnRef *cr = castNode(ColumnRef, node);
                if (list_length(cr->fields) == 1)
                {
                    char *nm;
                    AliasExpr *aliasExpr;
                    bool found;
                    nm = strVal(linitial(cr->fields));
                    aliasExpr = (AliasExpr *)hash_search(aliases, 
                                                         nm,
                                                         HASH_FIND, 
                                                         &found);
                    if (found)
                    {
                        bt->arg = (Expr *)copyObject(aliasExpr->expr);
                    }
                }
            }
        }
        else if (IsA(expr, SubLink))
        {
            SubLink *sl = castNode(SubLink, expr);
            Node *node = sl->testexpr;
            if ((node != NULL) && IsA(node, ColumnRef))
            {
                ColumnRef *cr = castNode(ColumnRef, node);
                if (list_length(cr->fields) == 1)
                {
                    char *nm;
                    AliasExpr *aliasExpr;
                    bool found;
                    nm = strVal(linitial(cr->fields));
                    aliasExpr = (AliasExpr *)hash_search(aliases, 
                                                         nm,
                                                         HASH_FIND, 
                                                         &found);
                    if (found)
                    {
                        sl->testexpr = copyObject(aliasExpr->expr);
                    }
                }
            }
        }
    }
}

Query *
mys_transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;
	ListCell   *l;

	qry->commandType = CMD_SELECT;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* Complain if we get called from someplace where INTO is not allowed */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
									exprLocation((Node *) stmt->intoClause))));

	/* make FOR UPDATE/FOR SHARE info available to addRangeTableEntry */
	pstate->p_locking_clause = stmt->lockingClause;

	/* make WINDOW info available for window functions, too */
	pstate->p_windowdefs = stmt->windowClause;

	/* process the FROM clause */
    transformFromClause(pstate, stmt->fromClause);

    curSqlTableId = InvalidOid;
    foreach (l, pstate->p_rtable)
    {
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(l); 
        curSqlTableId = rte->relid;
        break;
    }

    /* transform targetlist */
    qry->targetList = transformTargetList(pstate, stmt->targetList,
                                          EXPR_KIND_SELECT_TARGET);

    /* mark column origins */
	markTargetListOrigins(pstate, qry->targetList);


	/* transform WHERE */
	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	/* initial processing of HAVING clause is much like WHERE clause */
    if (stmt->havingClause != NULL)
    {
        rectifyHavingClause(stmt);
    }
	qry->havingQual = transformWhereClause(pstate, stmt->havingClause,
                                           EXPR_KIND_HAVING, "HAVING");

	/*
	 * Transform sorting/grouping stuff.  Do ORDER BY first because both
	 * transformGroupClause and transformDistinctClause need the results. Note
	 * that these functions can also change the targetList, so it's passed to
	 * them by reference.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											&qry->groupingSets,
											&qry->targetList,
											qry->sortClause,
											EXPR_KIND_GROUP_BY,
											false /* allow SQL92 rules */ );
	qry->groupDistinct = stmt->groupDistinct;

	if (stmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(stmt->distinctClause) == NULL)
	{
		/* We had SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* We had SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														stmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* transform LIMIT */
	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	/* transform window clauses after we have seen all window functions */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	/* resolve any still-unresolved output columns as being type text */
	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, stmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* this must be done after collations, for reliable comparison of exprs */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		mys_parseCheckAggregates(pstate, qry);

	return qry;
}


/*
 * standard_transformInsertStmt -
 *	  transform an Insert Statement
 */
Query *
mys_transformInsertStmt(ParseState *pstate, InsertStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *selectStmt = (SelectStmt *) stmt->selectStmt;
	List	   *exprList = NIL;
	bool		isGeneralSelect;
	List	   *sub_rtable;
	List	   *sub_namespace;
	List	   *icolumns;
	List	   *attrnos;
	ParseNamespaceItem *nsitem;
	RangeTblEntry *rte;
	ListCell   *icols;
	ListCell   *attnos;
	ListCell   *lc;
	bool		isOnConflictUpdate;
	AclMode		targetPerms;

	/* There can't be any outer WITH to worry about */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;
    
	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	qry->override = stmt->override;

	isOnConflictUpdate = (stmt->onConflictClause &&
						  stmt->onConflictClause->action == ONCONFLICT_UPDATE);

	/*
	 * We have three cases to deal with: DEFAULT VALUES (selectStmt == NULL),
	 * VALUES list, or general SELECT input.  We special-case VALUES, both for
	 * efficiency and so we can handle DEFAULT specifications.
	 *
	 * The grammar allows attaching ORDER BY, LIMIT, FOR UPDATE, or WITH to a
	 * VALUES clause.  If we have any of those, treat it as a general SELECT;
	 * so it will work, but you can't use DEFAULT items together with those.
	 */
	isGeneralSelect = (selectStmt && (selectStmt->valuesLists == NIL ||
									  selectStmt->sortClause != NIL ||
									  selectStmt->limitOffset != NULL ||
									  selectStmt->limitCount != NULL ||
									  selectStmt->lockingClause != NIL ||
									  selectStmt->withClause != NULL));

	/*
	 * If a non-nil rangetable/namespace was passed in, and we are doing
	 * INSERT/SELECT, arrange to pass the rangetable/namespace down to the
	 * SELECT.  This can only happen if we are inside a CREATE RULE, and in
	 * that case we want the rule's OLD and NEW rtable entries to appear as
	 * part of the SELECT's rtable, not as outer references for it.  (Kluge!)
	 * The SELECT's joinlist is not affected however.  We must do this before
	 * adding the target table to the INSERT's rtable.
	 */
	if (isGeneralSelect)
	{
		sub_rtable = pstate->p_rtable;
		pstate->p_rtable = NIL;
		sub_namespace = pstate->p_namespace;
		pstate->p_namespace = NIL;
	}
	else
	{
		sub_rtable = NIL;		/* not used, but keep compiler quiet */
		sub_namespace = NIL;
	}

	/*
	 * Must get write lock on INSERT target table before scanning SELECT, else
	 * we will grab the wrong kind of initial lock if the target table is also
	 * mentioned in the SELECT part.  Note that the target table is not added
	 * to the joinlist or namespace.
	 */
	targetPerms = ACL_INSERT;
	if (isOnConflictUpdate)
		targetPerms |= ACL_UPDATE;
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 false, false, targetPerms);

    /* for Mysql last_insert_id */
    mys_construct_returningList(pstate, stmt);

	/* Validate stmt->cols list, or build default list if no list given */
	icolumns = checkInsertTargets(pstate, stmt->cols, &attrnos);
	Assert(list_length(icolumns) == list_length(attrnos));

	/*
	 * Determine which variant of INSERT we have.
	 */
	if (selectStmt == NULL)
	{
		/*
		 * We have INSERT ... DEFAULT VALUES.  We can handle this case by
		 * emitting an empty targetlist --- all columns will be defaulted when
		 * the planner expands the targetlist.
		 */
		exprList = NIL;
	}
	else if (isGeneralSelect)
	{
		/*
		 * We make the sub-pstate a child of the outer pstate so that it can
		 * see any Param definitions supplied from above.  Since the outer
		 * pstate's rtable and namespace are presently empty, there are no
		 * side-effects of exposing names the sub-SELECT shouldn't be able to
		 * see.
		 */
		ParseState *sub_pstate = make_parsestate(pstate);
		Query	   *selectQuery;

		/*
		 * Process the source SELECT.
		 *
		 * It is important that this be handled just like a standalone SELECT;
		 * otherwise the behavior of SELECT within INSERT might be different
		 * from a stand-alone SELECT. (Indeed, Postgres up through 6.5 had
		 * bugs of just that nature...)
		 *
		 * The sole exception is that we prevent resolving unknown-type
		 * outputs as TEXT.  This does not change the semantics since if the
		 * column type matters semantically, it would have been resolved to
		 * something else anyway.  Doing this lets us resolve such outputs as
		 * the target column's type, which we handle below.
		 */
		sub_pstate->p_rtable = sub_rtable;
		sub_pstate->p_joinexprs = NIL;	/* sub_rtable has no joins */
		sub_pstate->p_namespace = sub_namespace;
		sub_pstate->p_resolve_unknowns = false;

		selectQuery = transformStmt(sub_pstate, stmt->selectStmt);

		free_parsestate(sub_pstate);

		/* The grammar should have produced a SELECT */
		if (!IsA(selectQuery, Query) ||
			selectQuery->commandType != CMD_SELECT)
			elog(ERROR, "unexpected non-SELECT command in INSERT ... SELECT");

		/*
		 * Make the source be a subquery in the INSERT's rangetable, and add
		 * it to the INSERT's joinlist (but not the namespace).
		 */
		nsitem = addRangeTableEntryForSubquery(pstate,
											   selectQuery,
											   makeAlias("*SELECT*", NIL),
											   false,
											   false);
		addNSItemToQuery(pstate, nsitem, true, false, false);

		/*----------
		 * Generate an expression list for the INSERT that selects all the
		 * non-resjunk columns from the subquery.  (INSERT's tlist must be
		 * separate from the subquery's tlist because we may add columns,
		 * insert datatype coercions, etc.)
		 *
		 * HACK: unknown-type constants and params in the SELECT's targetlist
		 * are copied up as-is rather than being referenced as subquery
		 * outputs.  This is to ensure that when we try to coerce them to
		 * the target column's datatype, the right things happen (see
		 * special cases in coerce_type).  Otherwise, this fails:
		 *		INSERT INTO foo SELECT 'bar', ... FROM baz
		 *----------
		 */
		exprList = NIL;
		foreach(lc, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Expr	   *expr;

			if (tle->resjunk)
				continue;
			if (tle->expr &&
				(IsA(tle->expr, Const) || IsA(tle->expr, Param)) &&
				exprType((Node *) tle->expr) == UNKNOWNOID)
				expr = tle->expr;
			else
			{
				Var		   *var = makeVarFromTargetEntry(nsitem->p_rtindex, tle);

				var->location = exprLocation((Node *) tle->expr);
				expr = (Expr *) var;
			}
			exprList = lappend(exprList, expr);
		}

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}
	else if (list_length(selectStmt->valuesLists) > 1)
	{
		/*
		 * Process INSERT ... VALUES with multiple VALUES sublists. We
		 * generate a VALUES RTE holding the transformed expression lists, and
		 * build up a targetlist containing Vars that reference the VALUES
		 * RTE.
		 */
		List	   *exprsLists = NIL;
		List	   *coltypes = NIL;
		List	   *coltypmods = NIL;
		List	   *colcollations = NIL;
		int			sublist_length = -1;
		bool		lateral = false;

		Assert(selectStmt->intoClause == NULL);

		foreach(lc, selectStmt->valuesLists)
		{
			List	   *sublist = (List *) lfirst(lc);

			/*
			 * Do basic expression transformation (same as a ROW() expr, but
			 * allow SetToDefault at top level)
			 */
			sublist = transformExpressionList(pstate, sublist,
											  EXPR_KIND_VALUES, true);

			/*
			 * All the sublists must be the same length, *after*
			 * transformation (which might expand '*' into multiple items).
			 * The VALUES RTE can't handle anything different.
			 */
			if (sublist_length < 0)
			{
				/* Remember post-transformation length of first sublist */
				sublist_length = list_length(sublist);
			}
			else if (sublist_length != list_length(sublist))
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("VALUES lists must all be the same length"),
						 parser_errposition(pstate,
											exprLocation((Node *) sublist))));
			}

			/*
			 * Prepare row for assignment to target table.  We process any
			 * indirection on the target column specs normally but then strip
			 * off the resulting field/array assignment nodes, since we don't
			 * want the parsed statement to contain copies of those in each
			 * VALUES row.  (It's annoying to have to transform the
			 * indirection specs over and over like this, but avoiding it
			 * would take some really messy refactoring of
			 * transformAssignmentIndirection.)
			 */
			sublist = transformInsertRow(pstate, sublist,
										 stmt->cols,
										 icolumns, attrnos,
										 true);

			/*
			 * We must assign collations now because assign_query_collations
			 * doesn't process rangetable entries.  We just assign all the
			 * collations independently in each row, and don't worry about
			 * whether they are consistent vertically.  The outer INSERT query
			 * isn't going to care about the collations of the VALUES columns,
			 * so it's not worth the effort to identify a common collation for
			 * each one here.  (But note this does have one user-visible
			 * consequence: INSERT ... VALUES won't complain about conflicting
			 * explicit COLLATEs in a column, whereas the same VALUES
			 * construct in another context would complain.)
			 */
			assign_list_collations(pstate, sublist);

			exprsLists = lappend(exprsLists, sublist);
		}

		/*
		 * Construct column type/typmod/collation lists for the VALUES RTE.
		 * Every expression in each column has been coerced to the type/typmod
		 * of the corresponding target column or subfield, so it's sufficient
		 * to look at the exprType/exprTypmod of the first row.  We don't care
		 * about the collation labeling, so just fill in InvalidOid for that.
		 */
		foreach(lc, (List *) linitial(exprsLists))
		{
			Node	   *val = (Node *) lfirst(lc);

			coltypes = lappend_oid(coltypes, exprType(val));
			coltypmods = lappend_int(coltypmods, exprTypmod(val));
			colcollations = lappend_oid(colcollations, InvalidOid);
		}

		/*
		 * Ordinarily there can't be any current-level Vars in the expression
		 * lists, because the namespace was empty ... but if we're inside
		 * CREATE RULE, then NEW/OLD references might appear.  In that case we
		 * have to mark the VALUES RTE as LATERAL.
		 */
		if (list_length(pstate->p_rtable) != 1 &&
			contain_vars_of_level((Node *) exprsLists, 0))
			lateral = true;

		/*
		 * Generate the VALUES RTE
		 */
		nsitem = addRangeTableEntryForValues(pstate, exprsLists,
											 coltypes, coltypmods, colcollations,
											 NULL, lateral, true);
		addNSItemToQuery(pstate, nsitem, true, false, false);

		/*
		 * Generate list of Vars referencing the RTE
		 */
		exprList = expandNSItemVars(nsitem, 0, -1, NULL);

		/*
		 * Re-apply any indirection on the target column specs to the Vars
		 */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}
	else
	{
		/*
		 * Process INSERT ... VALUES with a single VALUES sublist.  We treat
		 * this case separately for efficiency.  The sublist is just computed
		 * directly as the Query's targetlist, with no VALUES RTE.  So it
		 * works just like a SELECT without any FROM.
		 */
		List	   *valuesLists = selectStmt->valuesLists;

		Assert(list_length(valuesLists) == 1);
		Assert(selectStmt->intoClause == NULL);

		/*
		 * Do basic expression transformation (same as a ROW() expr, but allow
		 * SetToDefault at top level)
		 */
		exprList = transformExpressionList(pstate,
										   (List *) linitial(valuesLists),
										   EXPR_KIND_VALUES_SINGLE,
										   true);

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}

	/*
	 * Generate query's target list using the computed list of expressions.
	 * Also, mark all the target columns as needing insert permissions.
	 */
	rte = pstate->p_target_nsitem->p_rte;
	qry->targetList = NIL;
	Assert(list_length(exprList) <= list_length(icolumns));
	forthree(lc, exprList, icols, icolumns, attnos, attrnos)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col = lfirst_node(ResTarget, icols);
		AttrNumber	attr_num = (AttrNumber) lfirst_int(attnos);
		TargetEntry *tle;

		tle = makeTargetEntry(expr,
							  attr_num,
							  col->name,
							  false);
		qry->targetList = lappend(qry->targetList, tle);

		rte->insertedCols = bms_add_member(rte->insertedCols,
										   attr_num - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * If we have any clauses yet to process, set the query namespace to
	 * contain only the target relation, removing any entries added in a
	 * sub-SELECT or VALUES list.
	 */
	if (stmt->onConflictClause || stmt->returningList)
	{
		pstate->p_namespace = NIL;
		addNSItemToQuery(pstate, pstate->p_target_nsitem,
						 false, true, true);
	}

	/* Process ON CONFLICT, if any. */
	if (stmt->onConflictClause)
		qry->onConflict = transformOnConflictClause(pstate,
													stmt->onConflictClause);

	/* Process RETURNING, if any. */
	if (stmt->returningList)
		qry->returningList = transformReturningList(pstate,
													stmt->returningList);

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

Query *
mys_transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	Node	   *qual;

	qry->commandType = CMD_DELETE;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* set up range table with just the result rel */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 true,
										 ACL_DELETE);
	nsitem = pstate->p_target_nsitem;

	/* there's no DISTINCT in DELETE */
	qry->distinctClause = NIL;

	/* subqueries in USING cannot access the result relation */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * The USING clause is non-standard SQL syntax, and is equivalent in
	 * functionality to the FROM list that can be specified for UPDATE. The
	 * USING keyword is used rather than FROM because FROM is already a
	 * keyword in the DELETE syntax.
	 */
	transformFromClause(pstate, stmt->usingClause);

	/* remaining clauses can reference the result relation normally */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

    qry->sortClause = transformSortClause(pstate,
                                          stmt->sortClause,
                                          &qry->targetList,
                                          EXPR_KIND_ORDER_BY,
                                          false /* allow SQL92 rules */ ); 

    /* transform LIMIT */
    qry->limitOffset = transformLimitClause(pstate, 
                                            stmt->limitOffset,                                                                                                     
                                            EXPR_KIND_OFFSET, "OFFSET",
                                            stmt->limitOption);

    qry->limitCount = transformLimitClause(pstate, 
                                           stmt->limitCount,
                                           EXPR_KIND_LIMIT, "LIMIT",
                                           stmt->limitOption);

    qry->limitOption = stmt->limitOption;

    qry->returningList = transformReturningList(pstate, stmt->returningList);

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	assign_query_collations(pstate, qry);

	/* this must be done after collations, for reliable comparison of exprs */
	if (pstate->p_hasAggs)
		mys_parseCheckAggregates(pstate, qry);

	return qry;
}


static List *
retrieveSetTargets(List *totalTargetList, RangeVar *relation)
{
    List *targetList;
    ListCell *lc;
    int index;

    targetList = NIL;
    index = 1;
    foreach (lc, totalTargetList)
    {
        if ((index % 2) == 1)
        {
            char *targetRelName = NULL;
            int targetRelNameLen = 0;
            A_Const *rel;
            Node *node = lfirst(lc);
            // TODO: 还要判断schema
            if (!IsA(node, A_Const))
            {
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("syntax error in table name or alias after set.")));
            }
            rel = castNode(A_Const, node);
            targetRelName = rel->val.val.str;
            targetRelNameLen = strlen(targetRelName);
            if (((targetRelNameLen == strlen(relation->relname)) && 
                 (strncasecmp(targetRelName, relation->relname, targetRelNameLen) == 0)) || 
                ((relation->alias!= NULL) && 
                 (relation->alias->aliasname != NULL) && 
                 (targetRelNameLen == strlen(relation->alias->aliasname)) && 
                 (strncasecmp(targetRelName, relation->alias->aliasname, targetRelNameLen) == 0)))
            {
                ListCell *subLc;
                int subIndex = 1;
                foreach (subLc, totalTargetList)
                {
                    if ((index + 1) == subIndex)
                    {
                        Node* resTarget = lfirst(subLc);
                        targetList = lappend(targetList, resTarget);
                        break;
                    }
                    ++subIndex;
                }
            }
        }
        ++index;
    }

    return targetList;
}


static Node *
mergeWhereClauseOnClause(Node *whereClause, Node *onClause)
{
    Node *newWhereClause;

    if (whereClause != NULL)
    {
        if (IsA(whereClause, BoolExpr))
        {
            BoolExpr *blexpr = (BoolExpr *)whereClause;
            if (blexpr->boolop == AND_EXPR)
            {
                blexpr->args = lappend(blexpr->args, onClause);
                newWhereClause = copyObject((Node *)blexpr);
            }
            else 
            {
                newWhereClause = (Node *)makeBoolExpr(AND_EXPR, 
                                                      list_make2(copyObject(whereClause), 
                                                                 copyObject(onClause)), 
                                                      -1);
            }
        }
        else 
        {
            newWhereClause = (Node *)makeBoolExpr(AND_EXPR, 
                                                  list_make2(copyObject(whereClause), 
                                                             copyObject(onClause)), 
                                                  -1);
        }
    }
    else
    {
        newWhereClause = copyObject(onClause);
    }

    return newWhereClause;
}


static void
rectifyExpr(RangeVar *relation, char *newVal, Node *expr)
{
    if ((expr != NULL) && (!IsA(expr, A_Const))) 
    {
        if (IsA(expr, ColumnRef))
        {
            // t2.name2 = t1.name1
            ColumnRef * columnRef = castNode(ColumnRef, expr);
            rectifyColumnRef(relation, newVal, columnRef);
        }
        else if (IsA(expr, FuncCall))
        {
            // t2.name2 = concat(t1.name1, t2.name2)
            FuncCall *funcCall = castNode(FuncCall, expr);
            ListCell *subLc;
            foreach (subLc, funcCall->args)
            {
                Node *nd = lfirst(subLc);
                rectifyExpr(relation, newVal, nd);
            }
        }
        else if (IsA(expr, A_Expr))
        {
            A_Expr * curExpr = castNode(A_Expr, expr);
            rectifyExpr(relation, newVal, curExpr->lexpr);
            rectifyExpr(relation, newVal, curExpr->rexpr);
        }
        else 
        {
            /* TODO: 可能还有更复杂的情况 */
        }
    }
}

static void
rectifyColumnRef(RangeVar *relation, char *newVal, ColumnRef *columnRef)
{
    // t2.name2 = t1.name1
    if (2 <= list_length(columnRef->fields))
    {
        char *relName = strVal(linitial(columnRef->fields));
        int relNameLen = strlen(relName);
        if (!(((relNameLen == strlen(relation->relname)) && 
               (strncasecmp(relName, relation->relname, relNameLen) == 0)) || 
              ((relation->alias != NULL) && 
               (relation->alias->aliasname != NULL) && 
               (relNameLen == strlen(relation->alias->aliasname)) && 
               (strncasecmp(relName, relation->alias->aliasname, relNameLen) == 0))))
        {
            Value *val = linitial(columnRef->fields);
            val->val.str = pstrdup(newVal);
        }
    }
}


static Query *
transformUpdateStmtToCte(ParseState *pstate, UpdateStmt *stmt)
{
    UpdateStmt *updateStmt;
    WithClause *withClause;
    List *ctes;
    CommonTableExpr *commonTableExpr;

    updateStmt = makeNode(UpdateStmt);
    withClause = makeNode(WithClause);
    ctes = NIL;
    commonTableExpr = makeNode(CommonTableExpr);

    // 修补 targetList
    {
        ListCell *lc;
        int index;
        index = 1;
        foreach (lc, stmt->targetList)
        {
            if ((index % 2) == 1)
            {
                Node *node = lfirst(lc);
                if (node == NULL)
                {
                    A_Const *n = makeNode(A_Const);
                    n->val.type = T_String;
                    if ((stmt->relation->alias != NULL) && 
                        (stmt->relation->alias->aliasname != NULL)) 
                    {
                        n->val.val.str = pstrdup(stmt->relation->alias->aliasname);
                    }
                    else 
                    {
                        n->val.val.str = pstrdup(stmt->relation->relname);
                    }
                    n->location = -1;
                    lfirst(lc) = (Node *)n;
                }
            }
        }
    }

    // 修补 onClause
    {
        A_Expr *aExpr = (A_Expr*)(stmt->onClause);
        ColumnRef *lExpr = (ColumnRef*)(aExpr->lexpr);
        ColumnRef *rExpr = (ColumnRef*)(aExpr->rexpr);
        if (list_length(lExpr->fields) == 1)
        {
            Value *v = makeNode(Value);
            v->type = T_String;
            if ((stmt->relation->alias != NULL) && 
                (stmt->relation->alias->aliasname != NULL)) 
            {
                v->val.str = pstrdup(stmt->relation->alias->aliasname);
            }
            else 
            {
                v->val.str = pstrdup(stmt->relation->relname);
            }
            lExpr->fields = lcons(v, lExpr->fields);
        }
        if (list_length(rExpr->fields) == 1)
        {
            Value *v = makeNode(Value);
            v->type = T_String;
            if ((stmt->relation->alias != NULL) && 
                (stmt->relation->alias->aliasname != NULL)) 
            {
                v->val.str = pstrdup(stmt->relation->alias->aliasname);
            }
            else 
            {
                v->val.str = pstrdup(stmt->relation->relname);
            }
            rExpr->fields = lcons(v, rExpr->fields);
        }
    }

    commonTableExpr->ctename = pstrdup("halo_update_cte");
    commonTableExpr->aliascolnames = NIL;
    commonTableExpr->ctematerialized = CTEMaterializeDefault;
    {
        UpdateStmt *cteUpdateStmt = makeNode(UpdateStmt);
        cteUpdateStmt->relation = stmt->relation;
        cteUpdateStmt->targetList = retrieveSetTargets(stmt->targetList, cteUpdateStmt->relation);
        cteUpdateStmt->fromClause = copyObject(stmt->fromClause);
        cteUpdateStmt->whereClause = mergeWhereClauseOnClause(stmt->whereClause, stmt->onClause);
        cteUpdateStmt->returningList = NIL;
        {
            ResTarget *resTarget = makeNode(ResTarget);
            ColumnRef *columnRef = makeNode(ColumnRef);
            if ((cteUpdateStmt->relation->alias!= NULL) && 
                (cteUpdateStmt->relation->alias->aliasname != NULL))
            {
                columnRef->fields = list_make2(makeString(cteUpdateStmt->relation->alias->aliasname), makeNode(A_Star));
            }
            else 
            {
                columnRef->fields = list_make2(makeString(cteUpdateStmt->relation->relname), makeNode(A_Star));
            }
            columnRef->location = -1;
            resTarget->name = NULL;
            resTarget->indirection = NIL;
            resTarget->val = (Node *)columnRef;
            resTarget->location = -1;
            cteUpdateStmt->returningList = lappend(cteUpdateStmt->returningList, (Node *)resTarget);
        }
        commonTableExpr->ctequery = (Node *)cteUpdateStmt;
    }
    commonTableExpr->location = -1;

    ctes = list_make1(commonTableExpr);
    withClause->ctes = ctes;
    withClause->recursive = false;
    withClause->location = -1;

    updateStmt->relation = (RangeVar*)linitial(stmt->fromClause);
    updateStmt->targetList = retrieveSetTargets(stmt->targetList, updateStmt->relation);
    {
        ListCell *lc;
        foreach (lc, updateStmt->targetList)
        {
            ResTarget *resTarget = (ResTarget *)lfirst(lc);
            rectifyExpr(updateStmt->relation, commonTableExpr->ctename, resTarget->val);
        }
    }
    updateStmt->fromClause = list_make1(makeRangeVar(NULL, pstrdup(commonTableExpr->ctename), -1));
    updateStmt->whereClause = stmt->onClause;
    {
        A_Expr *aExpr = (A_Expr*)(updateStmt->whereClause);
        ColumnRef *lExpr = (ColumnRef*)(aExpr->lexpr);
        ColumnRef *rExpr = (ColumnRef*)(aExpr->rexpr);
        char *lRelName = strVal(linitial(lExpr->fields));
        int lRelNameLen = strlen(lRelName);
        char *rRelName = strVal(linitial(rExpr->fields));
        int rRelNameLen = strlen(rRelName);
        if (((lRelNameLen == strlen(updateStmt->relation->relname)) && 
             (strncasecmp(lRelName, updateStmt->relation->relname, lRelNameLen) == 0)) || 
            ((updateStmt->relation->alias != NULL) && 
             (updateStmt->relation->alias->aliasname != NULL) && 
             (lRelNameLen == strlen(updateStmt->relation->alias->aliasname)) && 
             (strncasecmp(lRelName, updateStmt->relation->alias->aliasname, lRelNameLen) == 0)))

        {
            Value *val = linitial(rExpr->fields);
            val->val.str = pstrdup(commonTableExpr->ctename);
        }
        else if (((rRelNameLen == strlen(updateStmt->relation->relname)) && 
                  (strncasecmp(rRelName, updateStmt->relation->relname, rRelNameLen) == 0)) || 
                 ((updateStmt->relation->alias != NULL) && 
                  (updateStmt->relation->alias->aliasname != NULL) && 
                  (rRelNameLen == strlen(updateStmt->relation->alias->aliasname)) && 
                  (strncasecmp(rRelName, updateStmt->relation->alias->aliasname, rRelNameLen) == 0)))
        {
            Value *val = linitial(lExpr->fields);
            val->val.str = pstrdup(commonTableExpr->ctename);
        }
        else 
        {
            // TODO: error
        }
    }
    updateStmt->withClause = withClause;

    return transformUpdateStmtInternal(pstate, updateStmt);
}


Query *
mys_transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt)
{
    if (halo_mysql_support_multiple_table_update)  
    {
        if (stmt->onClause == NULL)
        {
            return transformUpdateStmtInternal(pstate, stmt);
        }
        else 
        {
            bool isRealMultipleTabUpdate = false;
            ListCell *lc;
            int index = 1;
            foreach (lc, stmt->targetList)
            {
                if ((index % 2) == 1)
                {
                    Node *node = lfirst(lc);
                    if (node != NULL)
                    {
                        char *targetRelName = NULL;
                        int targetRelNameLen = 0;
                        A_Const *rel;
                        if (!IsA(node, A_Const))
                        {
                            ereport(ERROR,
                                    (errcode(ERRCODE_SYNTAX_ERROR),
                                     errmsg("syntax error in table name or alias after set.")));
                        }
                        rel = castNode(A_Const, node);
                        targetRelName = rel->val.val.str;
                        targetRelNameLen = strlen(targetRelName);
                        if (((targetRelNameLen == strlen(stmt->relation->relname)) && 
                             (strncasecmp(targetRelName, stmt->relation->relname, targetRelNameLen) == 0)) || 
                            ((stmt->relation->alias != NULL) && 
                             (stmt->relation->alias->aliasname != NULL) && 
                             (targetRelNameLen == strlen(stmt->relation->alias->aliasname) && 
                              (strncasecmp(targetRelName, stmt->relation->alias->aliasname, targetRelNameLen) == 0))))
                        {
                            /* do nothing; */
                        }
                        else
                        {
                            isRealMultipleTabUpdate = true;
                            break;
                        }
                    }
                }

                ++index;
            }
            if (isRealMultipleTabUpdate)
            {
                return transformUpdateStmtToCte(pstate, stmt);
            }
            else 
            {
                List *newTargetList;
                int i;

                newTargetList = NIL;
                i = 1;
                foreach (lc, stmt->targetList)
                {
                    Node *node = lfirst(lc);
                    if ((i % 2) == 0)
                    {
                        if (!IsA(node, ResTarget))
                        {
                            ereport(ERROR,
                                    (errcode(ERRCODE_SYNTAX_ERROR),
                                     errmsg("syntax error after set.")));
                        }

                        newTargetList = lappend(newTargetList, node);
                    }
                    ++i;
                }
                stmt->targetList = newTargetList;
                stmt->whereClause = mergeWhereClauseOnClause(stmt->whereClause, stmt->onClause);
                stmt->onClause = NULL;

                return transformUpdateStmtInternal(pstate, stmt);
            }
        }
    }
    else 
    {
        if (stmt->onClause == NULL)
        {
            return transformUpdateStmtInternal(pstate, stmt);
        }
        else 
        {
            List *newTargetList;
            int i;
            ListCell* lc;

            newTargetList = NIL;
            i = 1;
            foreach (lc, stmt->targetList)
            {
                Node *node = lfirst(lc);
                if ((i % 2) == 0)
                {
                    if (!IsA(node, ResTarget))
                    {
                        ereport(ERROR,
                                (errcode(ERRCODE_SYNTAX_ERROR),
                                 errmsg("syntax error after set.")));
                    }

                    newTargetList = lappend(newTargetList, node);
                }
                ++i;
            }
            stmt->targetList = newTargetList;
            stmt->whereClause = copyObject(stmt->onClause);
            stmt->onClause = NULL;

            return transformUpdateStmtInternal(pstate, stmt);
        }
    }
}

/*
 * transform a CallStmt
 */
Query *
mys_transformCallStmt(ParseState *pstate, CallStmt *stmt)
{
	List	   *targs;
	ListCell   *lc;
	Node	   *node;
	FuncExpr   *fexpr;
	HeapTuple	proctup;
	Datum		proargmodes;
	bool		isNull;
    char *fname = NULL;
    char *namespace = NULL;
	List *outargs = NIL;
    char argMode[FUNC_MAX_ARGS];
	Query *result;

    targs = NIL;

    {
        bool findOne = false;
		CatCList *catlist = NULL;
        Oid namespaceId = InvalidOid;

		/* deconstruct the name list */
		DeconstructQualifiedName(stmt->funccall->funcname, &namespace, &fname);

		if (namespace)
		{
			/* use exact schema given */
			namespaceId = LookupExplicitNamespace(namespace, true);
		}
		else
		{
			namespaceId = getCurrentNamespaceOid();
		}

        if (!OidIsValid(namespaceId))
        {
            elog(ERROR, "No database selected");
        }
        else
        {
            namespace = get_namespace_name(namespaceId);
        }

		catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(fname));

        for (int i = 0; i < catlist->n_members; i++)
		{
			HeapTuple	proctup = &catlist->members[i]->tuple;
			Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);

			if (procform->prokind == PROKIND_PROCEDURE &&
                procform->pronamespace == namespaceId)
            {
                findOne = true;

                proargmodes = SysCacheGetAttr(PROCOID, proctup,
                                              Anum_pg_proc_proargmodes,
                                              &isNull);
                
                if (!isNull)
                {
                    /*
                     * Split the list into input arguments in fexpr->args and output
                     * arguments in stmt->outargs.  INOUT arguments appear in both lists.
                    */
                    ArrayType  *arr;
                    int			numargs;
                    char	   *argmodes;
                    int			j;

                    arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
                    numargs = list_length(stmt->funccall->args);
                    if (ARR_NDIM(arr) != 1 ||
                        ARR_DIMS(arr)[0] != numargs ||
                        ARR_HASNULL(arr) ||
                        ARR_ELEMTYPE(arr) != CHAROID)
                        elog(ERROR, "Incorrect number of arguments for PROCEDURE %s.%s; expected %d, got %d",
                             namespace, fname, ARR_DIMS(arr)[0], numargs);
                    argmodes = (char *) ARR_DATA_PTR(arr);

                    j = 0;
                    foreach(lc, stmt->funccall->args)
                    {
                        Node *n = lfirst(lc);
                        Node *temp = NULL;

                        argMode[j] = argmodes[j];

                        switch (argmodes[j])
                        {
                            case PROARGMODE_IN:
                            case PROARGMODE_VARIADIC:
                                {
                                    temp = transformExpr(pstate, n, EXPR_KIND_CALL_ARGUMENT);
                                    targs = lappend(targs, temp);
                                }
                                break;
                            case PROARGMODE_OUT:
                                {
                                    if (IsA(n, UserVarRef))
                                    {
                                        A_Const *n1 = makeNode(A_Const);

                                        n1->val.type = T_Null;
                                        n1->location = -1;
                                        lfirst(lc) = n1;

                                        outargs = lappend(outargs, n);

                                        temp = transformExpr(pstate, (Node *)n1, EXPR_KIND_CALL_ARGUMENT);
                                        targs = lappend(targs, temp);
                                    }
                                    else
                                    {
                                        temp = transformExpr(pstate, n, EXPR_KIND_CALL_ARGUMENT);
                                        if (IsA(temp, Param))
                                        {
                                            targs = lappend(targs, temp);
                                            outargs = lappend(outargs, temp);
                                        }
                                        else
                                        {
                                            elog(ERROR, "OUT or INOUT argument %d for routine %s.%s is not a variable",
                                                 j, namespace, fname);
                                        }
                                    }
                                }
                                break;
                            case PROARGMODE_INOUT:
                                {
                                    if (IsA(n, UserVarRef))
                                    {
                                        outargs = lappend(outargs, n);

                                        temp = transformExpr(pstate, n, EXPR_KIND_CALL_ARGUMENT);
                                        targs = lappend(targs, temp);
                                    }
                                    else
                                    {
                                        temp = transformExpr(pstate, n, EXPR_KIND_CALL_ARGUMENT);
                                        if (IsA(temp, Param))
                                        {
                                            targs = lappend(targs, temp);
                                            outargs = lappend(outargs, temp);
                                        }
                                        else
                                        {
                                            elog(ERROR, "OUT or INOUT argument %d for routine %s.%s is not a variable",
                                                 j, namespace, fname);
                                        }
                                    }
                                }
                                break;
                            default:
                                /* note we don't support PROARGMODE_TABLE */
                                elog(ERROR, "invalid argmode %c for procedure",
                                    argmodes[i]);
                                break;
                        }
                        j++;
                    }
                }
                else
                {
                    /* 全是入参 */
                    int j = 0;
                    foreach(lc, stmt->funccall->args)
                    {
                        targs = lappend(targs, transformExpr(pstate,
                                                            (Node *) lfirst(lc),
                                                            EXPR_KIND_CALL_ARGUMENT));
                        argMode[j] = PROARGMODE_IN;
                        j++;
                    }
                }
                /* 只分析catlist中的一个procedure，create procedure处理流程会保证同名procedure有相同的参数信息 */
                break;
            }
		}
		ReleaseSysCacheList(catlist);

        if (!findOne)
        {
            elog(ERROR, "procedure %s.%s does not exist", namespace, fname);
        }
    }

	/*
	 * First, do standard parse analysis on the procedure call and its
	 * arguments, allowing us to identify the called procedure.
	 */
	// targs = NIL;
	// foreach(lc, stmt->funccall->args)
	// {
	// 	targs = lappend(targs, transformExpr(pstate,
	// 										 (Node *) lfirst(lc),
	// 										 EXPR_KIND_CALL_ARGUMENT));
	// }

    if (pstate)
    {
        if (pstate->p_pre_procedurecall_hook == NULL)
        {
            /* Nothing to do */
        }
        else
        {
            pstate->p_pre_procedurecall_hook(pstate, stmt, targs);
        }
    }

	node = ParseFuncOrColumn(pstate,
							 stmt->funccall->funcname,
							 targs,
							 pstate->p_last_srf,
							 stmt->funccall,
							 true,
							 stmt->funccall->location);

	assign_expr_collations(pstate, node);

	fexpr = castNode(FuncExpr, node);

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", fexpr->funcid);

	/*
	 * Expand the argument list to deal with named-argument notation and
	 * default arguments.  For ordinary FuncExprs this'd be done during
	 * planning, but a CallStmt doesn't go through planning, and there seems
	 * no good reason not to do it here.
	 */
	fexpr->args = expand_function_arguments(fexpr->args,
											true,
											fexpr->funcresulttype,
											proctup);

	/* Fetch proargmodes; if it's null, there are no output args */
	proargmodes = SysCacheGetAttr(PROCOID, proctup,
								  Anum_pg_proc_proargmodes,
								  &isNull);
	if (!isNull)
	{
		/*
		 * Split the list into input arguments in fexpr->args and output
		 * arguments in stmt->outargs.  INOUT arguments appear in both lists.
		 */
		ArrayType  *arr;
		int			numargs;
		char	   *argmodes;
		List	   *inargs;
		int			i;

		arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
		numargs = list_length(fexpr->args);
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
				 numargs);
		argmodes = (char *) ARR_DATA_PTR(arr);

		inargs = NIL;
		i = 0;
		foreach(lc, fexpr->args)
		{
			Node	   *n = lfirst(lc);

            if (argMode[i] != argmodes[i])
            {
                /* 不期待的错误，在MySQL下，对create procedure的处理会保证所有的同名函数有相同的参数数量和模式 */
                elog(ERROR, "procedure %s.%s have multi, and their parameters have different argmode", namespace, fname);
            }

			switch (argmodes[i])
			{
				case PROARGMODE_IN:
				case PROARGMODE_VARIADIC:
					inargs = lappend(inargs, n);
					break;
				case PROARGMODE_OUT:
					//outargs = lappend(outargs, n);
					break;
				case PROARGMODE_INOUT:
					inargs = lappend(inargs, n);
					//outargs = lappend(outargs, copyObject(n));
					break;
				default:
					/* note we don't support PROARGMODE_TABLE */
					elog(ERROR, "invalid argmode %c for procedure",
						 argmodes[i]);
					break;
			}
			i++;
		}
		fexpr->args = inargs;
	}

	stmt->funcexpr = fexpr;
	stmt->outargs = outargs;

	ReleaseSysCache(proctup);

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

static void
FindFirstType(ParseState *pstate, SelectStmt *stmt,
						  bool isTopLevel, List **targetlist)
{
    SelectStmt *StmtStack[100];
    int count = 0;
    bool isLeaf;
    bool isFirst = true;
    int targetNum = 0;
    ListCell   *tl;

    SelectStmt *root = copyObject(stmt);

    ParseState *ps = make_parsestate(pstate);
    ps->p_parent_cte = pstate->p_parent_cte;
    ps->p_locked_from_parent = false;
    ps->p_resolve_unknowns = false;

    while (root != NULL || count != 0)
    {
        while (root != NULL)
        {
            if (root->op == SETOP_NONE)
            {
                Assert(root->larg == NULL && root->rarg == NULL);
                isLeaf = true;
            }
            else
            {
                Assert(root->larg != NULL && root->rarg != NULL);
                if (root->sortClause || root->limitOffset || root->limitCount ||
                    root->lockingClause || root->withClause)
                    isLeaf = true;
                else
                    isLeaf = false;
            }

            if (isLeaf)
            {
                Query *selectQuery;
                char		selectName[32];
                // ParseNamespaceItem *nsitem;
                //RangeTblRef *rtr;
                ListCell   *subTl;
                List *targetList;
                int	next_resno;
                int temp_count=0;
                bool error = false;

                targetList = NULL;
                next_resno = 1;

                selectQuery = parse_sub_analyze((Node *) root, ps,
                                            NULL, false, false);

                    /*
                * Check for bogus references to Vars on the current query level (but
                * upper-level references are okay). Normally this can't happen
                * because the namespace will be empty, but it could happen if we are
                * inside a rule.
                */
                if (pstate->p_namespace)
                {
                    if (contain_vars_of_level((Node *) selectQuery, 1))
                        ereport(ERROR,
                                (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                                errmsg("UNION/INTERSECT/EXCEPT member statement cannot refer to other relations of same query level"),
                                parser_errposition(pstate,
                                                    locate_var_of_level((Node *) selectQuery, 1))));
                }

                if (!isFirst)
                {
                    Assert(targetNum > 0);
                }

                foreach(subTl, selectQuery->targetList)
                {
                    TargetEntry *tle = (TargetEntry *) lfirst(subTl);
                    Node *colnode = (Node *) tle->expr;
                    Oid	coltype = exprType(colnode);

                    if (tle->resjunk)
                        continue;
                    
                    if (!error)
                    {
                        if (isFirst)
                        {
                            TargetEntry *tle_temp;
                            char *colname;

                            pstate->targettypelist = lappend_oid(pstate->targettypelist, coltype);

                            colname = pstrdup(tle->resname);
                            tle_temp = makeTargetEntry(tle->expr, next_resno++, colname, false);
                            targetList = lappend(targetList, tle_temp);
                        }
                        else
                        {
                            if (temp_count >= targetNum)
                            {
                                error = true;
                            }
                            else
                            {
                                if (list_nth_cell(pstate->targettypelist, temp_count)->oid_value == UNKNOWNOID)
                                {
                                    list_nth_cell(pstate->targettypelist, temp_count)->oid_value = coltype;
                                }
                            }
                        }
                    }

                    temp_count++;
                }

                if (error)
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_SYNTAX_ERROR),
                             errmsg("each query must have the same number of columns %d, %d", 
                                    targetNum,
                                    temp_count)));
                }

                if (isFirst &&
                    ps->p_parent_cte &&
                    ps->p_parent_cte->cterecursive)
                {
                    analyzeCTETargetList(ps, ps->p_parent_cte, targetList);
                }

                if (isFirst)
                {
                    targetNum = list_length(pstate->targettypelist);
                    isFirst = false;
                }

                /*
                * Make the leaf query be a subquery in the top-level rangetable.
                */
                snprintf(selectName, sizeof(selectName), "*SELECT* %d",
                        list_length(ps->p_rtable) + 1);
                // nsitem = addRangeTableEntryForSubquery(ps,
                //                                     selectQuery,
                //                                     makeAlias(selectName, NIL),
                //                                     false,
                //                                     false);

                /*
                * Return a RangeTblRef to replace the SelectStmt in the set-op tree.
                */
                // rtr = makeNode(RangeTblRef);
                // rtr->rtindex = nsitem->p_rtindex;
                root = NULL;
            }
            else
            {
                if (root->rarg != NULL)
                {
                    StmtStack[count] = root->rarg;
                    count++;
                }
                root = root->larg;
            }
        }

        if (count > 0)
        {
            root = StmtStack[count-1];
            count--;
        }
    }

    tl = NULL;
    foreach (tl, pstate->targettypelist)
    {
        if (lfirst_oid(tl) == UNKNOWNOID)
        {
            lfirst_oid(tl) = TEXTOID;
        }
    }

    if (ps->p_parent_cte)
    {
        ps->p_parent_cte->ctecolnames = NULL;
        ps->p_parent_cte->ctecoltypes = NULL;
        ps->p_parent_cte->ctecoltypmods = NULL;
        ps->p_parent_cte->ctecolcollations = NULL;
    }

    free_parsestate(ps);

    return;
}

Query *
mys_transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *leftmostSelect;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	SetOperationStmt *sostmt;
	List	   *sortClause;
	Node	   *limitOffset;
	Node	   *limitCount;
	List	   *lockingClause;
	WithClause *withClause;
	Node	   *node;
	ListCell   *left_tlist,
			   *lct,
			   *lcm,
			   *lcc,
			   *l;
	List	   *targetvars,
			   *targetnames,
			   *sv_namespace;
	int			sv_rtable_length;
	ParseNamespaceItem *jnsitem;
	ParseNamespaceColumn *sortnscolumns;
	int			sortcolindex;
	int			tllen;

	qry->commandType = CMD_SELECT;

	/*
	 * Find leftmost leaf SelectStmt.  We currently only need to do this in
	 * order to deliver a suitable error message if there's an INTO clause
	 * there, implying the set-op tree is in a context that doesn't allow
	 * INTO.  (transformSetOperationTree would throw error anyway, but it
	 * seems worth the trouble to throw a different error for non-leftmost
	 * INTO, so we produce that error in transformSetOperationTree.)
	 */
	leftmostSelect = stmt->larg;
	while (leftmostSelect && leftmostSelect->op != SETOP_NONE)
		leftmostSelect = leftmostSelect->larg;
	Assert(leftmostSelect && IsA(leftmostSelect, SelectStmt) &&
		   leftmostSelect->larg == NULL);
	if (leftmostSelect->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
									exprLocation((Node *) leftmostSelect->intoClause))));

	/*
	 * We need to extract ORDER BY and other top-level clauses here and not
	 * let transformSetOperationTree() see them --- else it'll just recurse
	 * right back here!
	 */
	sortClause = stmt->sortClause;
	limitOffset = stmt->limitOffset;
	limitCount = stmt->limitCount;
	lockingClause = stmt->lockingClause;
	withClause = stmt->withClause;

	stmt->sortClause = NIL;
	stmt->limitOffset = NULL;
	stmt->limitCount = NULL;
	stmt->lockingClause = NIL;
	stmt->withClause = NULL;

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(lockingClause))->strength))));

	/* Process the WITH clause independently of all else */
	if (withClause)
	{
		qry->hasRecursive = withClause->recursive;
		qry->cteList = transformWithClause(pstate, withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * Recursively transform the components of the tree.
	 */
    FindFirstType(pstate, stmt, true, NULL);

	sostmt = castNode(SetOperationStmt,
					  transformSetOperationTree(pstate, stmt, true, NULL));
	Assert(sostmt);
	qry->setOperations = (Node *) sostmt;

	/*
	 * Re-find leftmost SELECT (now it's a sub-query in rangetable)
	 */
	node = sostmt->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist for outer query using column names of
	 * leftmost select and common datatypes/collations of topmost set
	 * operation.  Also make lists of the dummy vars and their names for use
	 * in parsing ORDER BY.
	 *
	 * Note: we use leftmostRTI as the varno of the dummy variables. It
	 * shouldn't matter too much which RT index they have, as long as they
	 * have one that corresponds to a real RT entry; else funny things may
	 * happen when the tree is mashed by rule rewriting.
	 */
	qry->targetList = NIL;
	targetvars = NIL;
	targetnames = NIL;
	sortnscolumns = (ParseNamespaceColumn *)
		palloc0(list_length(sostmt->colTypes) * sizeof(ParseNamespaceColumn));
	sortcolindex = 0;

	forfour(lct, sostmt->colTypes,
			lcm, sostmt->colTypmods,
			lcc, sostmt->colCollations,
			left_tlist, leftmostQuery->targetList)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		Oid			colCollation = lfirst_oid(lcc);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;
		Var		   *var;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		var = makeVar(leftmostRTI,
					  lefttle->resno,
					  colType,
					  colTypmod,
					  colCollation,
					  0);
		var->location = exprLocation((Node *) lefttle->expr);
		tle = makeTargetEntry((Expr *) var,
							  (AttrNumber) pstate->p_next_resno++,
							  colName,
							  false);
		qry->targetList = lappend(qry->targetList, tle);
		targetvars = lappend(targetvars, var);
		targetnames = lappend(targetnames, makeString(colName));
		sortnscolumns[sortcolindex].p_varno = leftmostRTI;
		sortnscolumns[sortcolindex].p_varattno = lefttle->resno;
		sortnscolumns[sortcolindex].p_vartype = colType;
		sortnscolumns[sortcolindex].p_vartypmod = colTypmod;
		sortnscolumns[sortcolindex].p_varcollid = colCollation;
		sortnscolumns[sortcolindex].p_varnosyn = leftmostRTI;
		sortnscolumns[sortcolindex].p_varattnosyn = lefttle->resno;
		sortcolindex++;
	}

	/*
	 * As a first step towards supporting sort clauses that are expressions
	 * using the output columns, generate a namespace entry that makes the
	 * output columns visible.  A Join RTE node is handy for this, since we
	 * can easily control the Vars generated upon matches.
	 *
	 * Note: we don't yet do anything useful with such cases, but at least
	 * "ORDER BY upper(foo)" will draw the right error message rather than
	 * "foo not found".
	 */
	sv_rtable_length = list_length(pstate->p_rtable);

	jnsitem = addRangeTableEntryForJoin(pstate,
										targetnames,
										sortnscolumns,
										JOIN_INNER,
										0,
										targetvars,
										NIL,
										NIL,
										NULL,
										NULL,
										false);

	sv_namespace = pstate->p_namespace;
	pstate->p_namespace = NIL;

	/* add jnsitem to column namespace only */
	addNSItemToQuery(pstate, jnsitem, false, false, true);

	/*
	 * For now, we don't support resjunk sort clauses on the output of a
	 * setOperation tree --- you can only use the SQL92-spec options of
	 * selecting an output column by name or number.  Enforce by checking that
	 * transformSortClause doesn't add any items to tlist.
	 */
	tllen = list_length(qry->targetList);

	//preTransformSortClause(stmt->targetList, sortClause);
	qry->sortClause = transformSortClause(pstate,
										  sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	/* restore namespace, remove join RTE from rtable */
	pstate->p_namespace = sv_namespace;
	pstate->p_rtable = list_truncate(pstate->p_rtable, sv_rtable_length);

	if (tllen != list_length(qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid UNION/INTERSECT/EXCEPT ORDER BY clause"),
				 errdetail("Only result column names can be used, not expressions or functions."),
				 errhint("Add the expression/function to every SELECT, or move the UNION into a FROM clause."),
				 parser_errposition(pstate,
									exprLocation(list_nth(qry->targetList, tllen)))));

	qry->limitOffset = transformLimitClause(pstate, limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* this must be done after collations, for reliable comparison of exprs */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
	{
		if (pstate->p_hasAggs && !pstate->p_hasOrphanCols && 
				(!(qry->groupClause || qry->havingQual || qry->groupingSets)))
		{
			/* no need to check */
		}
		else
			mys_parseCheckAggregates(pstate, qry);
	}

	return qry;
}

/*
 * select_common_type()
 *		Determine the common supertype of a list of input expressions.
 *		This is used for determining the output type of CASE, UNION,
 *		and similar constructs.
 *
 * 'exprs' is a *nonempty* list of expressions.  Note that earlier items
 * in the list will be preferred if there is doubt.
 * 'context' is a phrase to use in the error message if we fail to select
 * a usable type.  Pass NULL to have the routine return InvalidOid
 * rather than throwing an error on failure.
 * 'which_expr': if not NULL, receives a pointer to the particular input
 * expression from which the result type was taken.
 *
 * Caution: "failure" just means that there were inputs of different type
 * categories.  It is not guaranteed that all the inputs are coercible to the
 * selected type; caller must check that (see verify_common_type).
 */
static Oid
mys_select_common_type(ParseState *pstate, List *exprs, const char *context,
                       Node **which_expr)
{
    if ((context != NULL) && 
        ((strcmp(context, "UNION") == 0) || 
         (strcmp(context, "INTERSECT") == 0) || 
         (strcmp(context, "EXCEPT") == 0)))
    {
        Node *lexpr;
        Oid	ltype;
        Node *rexpr;
        Oid	rtype;

        Assert(exprs != NIL);
        lexpr = (Node *) linitial(exprs);
        ltype = exprType(lexpr);
        ltype = getBaseType(ltype);
        rexpr = (Node *) lsecond(exprs);
        rtype = exprType(rexpr);
        rtype = getBaseType(rtype);
        if (((ltype == UNKNOWNOID) && IsA(lexpr, Const) && !(((Const *)lexpr)->constisnull)) && 
            ((rtype == UNKNOWNOID) && IsA(rexpr, Const) && !(((Const *)rexpr)->constisnull)))
        {
			if (which_expr)
				*which_expr = lexpr;
            return TEXTOID;
        }
        else if ((ltype == UNKNOWNOID) && IsA(lexpr, Const) && !(((Const *)lexpr)->constisnull))
        {
			if (which_expr)
				*which_expr = rexpr;
            return TEXTOID;
        }
        else if ((rtype == UNKNOWNOID) && IsA(rexpr, Const) && !(((Const *)rexpr)->constisnull))
        {
			if (which_expr)
				*which_expr = lexpr;
            return TEXTOID;
        }
        else 
        {
            if (((ltype == TEXTOID) || (ltype == VARCHAROID) || (ltype == BPCHAROID)) && (rtype != UNKNOWNOID))
            {
                if (which_expr)
                    *which_expr = rexpr;
                return TEXTOID;
            }
            else if (((rtype == TEXTOID) || (rtype == VARCHAROID) || (rtype == BPCHAROID)) && (ltype != UNKNOWNOID))
            {
                if (which_expr)
                    *which_expr = lexpr;
                return TEXTOID;
            }
            else 
            {
                Node	   *pexpr;
                Oid			ptype;
                TYPCATEGORY pcategory;
                bool		pispreferred;
                ListCell   *lc;

                Assert(exprs != NIL);
                pexpr = (Node *) linitial(exprs);
                lc = list_second_cell(exprs);
                ptype = exprType(pexpr);

                /*
                 * If all input types are valid and exactly the same, just pick that type.
                 * This is the only way that we will resolve the result as being a domain
                 * type; otherwise domains are smashed to their base types for comparison.
                 */
                if (ptype != UNKNOWNOID)
                {
                    for_each_cell(lc, exprs, lc)
                    {
                        Node	   *nexpr = (Node *) lfirst(lc);
                        Oid			ntype = exprType(nexpr);

                        if (ntype != ptype)
                            break;
                    }
                    if (lc == NULL)			/* got to the end of the list? */
                    {
                        if (which_expr)
                            *which_expr = pexpr;
                        return ptype;
                    }
                }

                /*
                 * Nope, so set up for the full algorithm.  Note that at this point, lc
                 * points to the first list item with type different from pexpr's; we need
                 * not re-examine any items the previous loop advanced over.
                 */
                ptype = getBaseType(ptype);
                get_type_category_preferred(ptype, &pcategory, &pispreferred);

                for_each_cell(lc, exprs, lc)
                {
                    Node	   *nexpr = (Node *) lfirst(lc);
                    Oid			ntype = getBaseType(exprType(nexpr));

                    /* move on to next one if no new information... */
                    if (ntype != UNKNOWNOID && ntype != ptype)
                    {
                        TYPCATEGORY ncategory;
                        bool		nispreferred;

                        get_type_category_preferred(ntype, &ncategory, &nispreferred);
                        if (ptype == UNKNOWNOID)
                        {
                            /* so far, only unknowns so take anything... */
                            pexpr = nexpr;
                            ptype = ntype;
                            pcategory = ncategory;
                            pispreferred = nispreferred;
                        }
                        else if (ncategory != pcategory)
                        {
                            /*
                             * both types in different categories? then not much hope...
                             */
                            if (context == NULL)
                                return InvalidOid;

                            /* 针对一下union情况做特别处理：*/
                            /* select distinct null as abc union select col from tab_u; */
                            /* select id from tab_u union select distinct null as abc; */
                            /* 以下报错先注释掉 */
                            //ereport(ERROR,
                            //		(errcode(ERRCODE_DATATYPE_MISMATCH),
                            ///*------
                            //  translator: first %s is name of a SQL construct, eg CASE */
                            //		 errmsg("%s types %s and %s cannot be matched",
                            //				context,
                            //				format_type_be(ptype),
                            //				format_type_be(ntype)),
                            //		 parser_errposition(pstate, exprLocation(nexpr))));
                            if (strncasecmp(context, "UNION", 5) == 0) 
                            {
                                if (IsA(pexpr, Const) && (((Const *)pexpr)->constisnull))
                                {
                                    ((Const *)pexpr)->consttype = ntype;
                                    ptype = ntype;
                                }
                                else if (IsA(nexpr, Const) && (((Const *)nexpr)->constisnull))
                                {
                                    ((Const *)nexpr)->consttype = ptype;
                                }
                                else 
                                {
                                    if (((((ptype == INT4OID) || (ptype == INT2OID) || (ptype == INT8OID)) && 
                                          ((ntype == TEXTOID) || (ntype == BPCHAROID) || (ntype == VARCHAROID))) || 
                                         (((ntype == INT4OID) || (ntype == INT2OID) || (ntype == INT8OID)) && 
                                          ((ptype == TEXTOID) || (ptype == BPCHAROID) || (ptype == VARCHAROID)))) || 
                                        (((ptype == TIMEOID) || (ptype == DATEOID) || (ptype == TIMESTAMPOID) || (ptype == TIMESTAMPTZOID)) && 
                                         ((ntype == TEXTOID) || (ntype == BPCHAROID) || (ntype == VARCHAROID))))
                                    {
                                        ptype = TEXTOID;
                                        ntype = TEXTOID;
                                    }
                                }
                            }
                            else 
                            {
                                ereport(ERROR,
                                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                                         /*------
                                           translator: first %s is name of a SQL construct, eg CASE */
                                         errmsg("%s types %s and %s cannot be matched",
                                                context,
                                                format_type_be(ptype),
                                                format_type_be(ntype)),
                                         parser_errposition(pstate, exprLocation(nexpr))));
                            }
                        }
                        else if (!pispreferred &&
                                 can_coerce_type(1, &ptype, &ntype, COERCION_IMPLICIT) &&
                                 !can_coerce_type(1, &ntype, &ptype, COERCION_IMPLICIT))
                        {
                            /*
                             * take new type if can coerce to it implicitly but not the
                             * other way; but if we have a preferred type, stay on it.
                             */
                            pexpr = nexpr;
                            ptype = ntype;
                            pcategory = ncategory;
                            pispreferred = nispreferred;
                        }
                    }
                }

                /*
                 * If all the inputs were UNKNOWN type --- ie, unknown-type literals ---
                 * then resolve as type TEXT.  This situation comes up with constructs
                 * like SELECT (CASE WHEN foo THEN 'bar' ELSE 'baz' END); SELECT 'foo'
                 * UNION SELECT 'bar'; It might seem desirable to leave the construct's
                 * output type as UNKNOWN, but that really doesn't work, because we'd
                 * probably end up needing a runtime coercion from UNKNOWN to something
                 * else, and we usually won't have it.  We need to coerce the unknown
                 * literals while they are still literals, so a decision has to be made
                 * now.
                 */
                // if (ptype == UNKNOWNOID)
                // 	ptype = TEXTOID;
                
                if ((context != NULL) && 
                    ((strcmp(context, "UNION") == 0) || 
                     (strcmp(context, "INTERSECT") == 0) || 
                     (strcmp(context, "EXCEPT") == 0)))
                {
                    if (ptype == UNKNOWNOID)
                        ptype = UNKNOWNOID;
                }
                else
                {
                    if (ptype == UNKNOWNOID)
                        ptype = TEXTOID;
                }
                

                if (which_expr)
                    *which_expr = pexpr;
                return ptype;
            }
        }
    }
    else 
    {
        Node	   *pexpr;
        Oid			ptype;
        TYPCATEGORY pcategory;
        bool		pispreferred;
        ListCell   *lc;

        Assert(exprs != NIL);
        pexpr = (Node *) linitial(exprs);
        lc = list_second_cell(exprs);
        ptype = exprType(pexpr);

        /*
         * If all input types are valid and exactly the same, just pick that type.
         * This is the only way that we will resolve the result as being a domain
         * type; otherwise domains are smashed to their base types for comparison.
         */
        if (ptype != UNKNOWNOID)
        {
            for_each_cell(lc, exprs, lc)
            {
                Node	   *nexpr = (Node *) lfirst(lc);
                Oid			ntype = exprType(nexpr);

                if (ntype != ptype)
                    break;
            }
            if (lc == NULL)			/* got to the end of the list? */
            {
                if (which_expr)
                    *which_expr = pexpr;
                return ptype;
            }
        }

        /*
         * Nope, so set up for the full algorithm.  Note that at this point, lc
         * points to the first list item with type different from pexpr's; we need
         * not re-examine any items the previous loop advanced over.
         */
        ptype = getBaseType(ptype);
        get_type_category_preferred(ptype, &pcategory, &pispreferred);

        for_each_cell(lc, exprs, lc)
        {
            Node	   *nexpr = (Node *) lfirst(lc);
            Oid			ntype = getBaseType(exprType(nexpr));

            /* move on to next one if no new information... */
            if (ntype != UNKNOWNOID && ntype != ptype)
            {
                TYPCATEGORY ncategory;
                bool		nispreferred;

                get_type_category_preferred(ntype, &ncategory, &nispreferred);
                if (ptype == UNKNOWNOID)
                {
                    /* so far, only unknowns so take anything... */
                    pexpr = nexpr;
                    ptype = ntype;
                    pcategory = ncategory;
                    pispreferred = nispreferred;
                }
                else if (ncategory != pcategory)
                {
                    /*
                     * both types in different categories? then not much hope...
                     */
                    if (context == NULL)
                        return InvalidOid;

                    /* 针对一下union情况做特别处理：*/
                    /* select distinct null as abc union select col from tab_u; */
                    /* select id from tab_u union select distinct null as abc; */
                    /* 以下报错先注释掉 */
                    //ereport(ERROR,
                    //		(errcode(ERRCODE_DATATYPE_MISMATCH),
                    ///*------
                    //  translator: first %s is name of a SQL construct, eg CASE */
                    //		 errmsg("%s types %s and %s cannot be matched",
                    //				context,
                    //				format_type_be(ptype),
                    //				format_type_be(ntype)),
                    //		 parser_errposition(pstate, exprLocation(nexpr))));
                    if (strncasecmp(context, "UNION", 5) == 0) 
                    {
                        if (IsA(pexpr, Const) && (((Const *)pexpr)->constisnull))
                        {
                            ((Const *)pexpr)->consttype = ntype;
                            ptype = ntype;
                        }
                        else if (IsA(nexpr, Const) && (((Const *)nexpr)->constisnull))
                        {
                            ((Const *)nexpr)->consttype = ptype;
                        }
                        else 
                        {
                            if (((((ptype == INT4OID) || (ptype == INT2OID) || (ptype == INT8OID)) && 
                                  ((ntype == TEXTOID) || (ntype == BPCHAROID) || (ntype == VARCHAROID))) || 
                                 (((ntype == INT4OID) || (ntype == INT2OID) || (ntype == INT8OID)) && 
                                  ((ptype == TEXTOID) || (ptype == BPCHAROID) || (ptype == VARCHAROID)))) || 
                                (((ptype == TIMEOID) || (ptype == DATEOID) || (ptype == TIMESTAMPOID) || (ptype == TIMESTAMPTZOID)) && 
                                 ((ntype == TEXTOID) || (ntype == BPCHAROID) || (ntype == VARCHAROID))))
                            {
                                ptype = TEXTOID;
                                ntype = TEXTOID;
                            }
                        }
                    }
                    else 
                    {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATATYPE_MISMATCH),
                                 /*------
                                   translator: first %s is name of a SQL construct, eg CASE */
                                 errmsg("%s types %s and %s cannot be matched",
                                        context,
                                        format_type_be(ptype),
                                        format_type_be(ntype)),
                                 parser_errposition(pstate, exprLocation(nexpr))));
                    }
                }
                else if (!pispreferred &&
                         can_coerce_type(1, &ptype, &ntype, COERCION_IMPLICIT) &&
                         !can_coerce_type(1, &ntype, &ptype, COERCION_IMPLICIT))
                {
                    /*
                     * take new type if can coerce to it implicitly but not the
                     * other way; but if we have a preferred type, stay on it.
                     */
                    pexpr = nexpr;
                    ptype = ntype;
                    pcategory = ncategory;
                    pispreferred = nispreferred;
                }
            }
        }

        /*
         * If all the inputs were UNKNOWN type --- ie, unknown-type literals ---
         * then resolve as type TEXT.  This situation comes up with constructs
         * like SELECT (CASE WHEN foo THEN 'bar' ELSE 'baz' END); SELECT 'foo'
         * UNION SELECT 'bar'; It might seem desirable to leave the construct's
         * output type as UNKNOWN, but that really doesn't work, because we'd
         * probably end up needing a runtime coercion from UNKNOWN to something
         * else, and we usually won't have it.  We need to coerce the unknown
         * literals while they are still literals, so a decision has to be made
         * now.
         */
        // if (ptype == UNKNOWNOID)
        // 	ptype = TEXTOID;
        
        if ((context != NULL) && 
            ((strcmp(context, "UNION") == 0) || 
             (strcmp(context, "INTERSECT") == 0) || 
             (strcmp(context, "EXCEPT") == 0)))
        {
            if (ptype == UNKNOWNOID)
                ptype = UNKNOWNOID;
        }
        else
        {
            if (ptype == UNKNOWNOID)
                ptype = TEXTOID;
        }
        

        if (which_expr)
            *which_expr = pexpr;
        return ptype;
    }
}

/*
 * mys_transformSetOperationTree
 *		Recursively transform leaves and internal nodes of a set-op tree
 *
 * In addition to returning the transformed node, if targetlist isn't NULL
 * then we return a list of its non-resjunk TargetEntry nodes.  For a leaf
 * set-op node these are the actual targetlist entries; otherwise they are
 * dummy entries created to carry the type, typmod, collation, and location
 * (for error messages) of each output column of the set-op node.  This info
 * is needed only during the internal recursion of this function, so outside
 * callers pass NULL for targetlist.  Note: the reason for passing the
 * actual targetlist entries of a leaf node is so that upper levels can
 * replace UNKNOWN Consts with properly-coerced constants.
 */
Node *
mys_transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
                              bool isTopLevel, List **targetlist)
{
	bool		isLeaf;

	Assert(stmt && IsA(stmt, SelectStmt));

	/* Guard against stack overflow due to overly complex set-expressions */
	check_stack_depth();

	/*
	 * Validity-check both leaf and internal SELECTs for disallowed ops.
	 */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT"),
				 parser_errposition(pstate,
									exprLocation((Node *) stmt->intoClause))));

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	/*
	 * If an internal node of a set-op tree has ORDER BY, LIMIT, FOR UPDATE,
	 * or WITH clauses attached, we need to treat it like a leaf node to
	 * generate an independent sub-Query tree.  Otherwise, it can be
	 * represented by a SetOperationStmt node underneath the parent Query.
	 */
	if (stmt->op == SETOP_NONE)
	{
		Assert(stmt->larg == NULL && stmt->rarg == NULL);
		isLeaf = true;
	}
	else
	{
		Assert(stmt->larg != NULL && stmt->rarg != NULL);
		if (stmt->sortClause || stmt->limitOffset || stmt->limitCount ||
			stmt->lockingClause || stmt->withClause)
			isLeaf = true;
		else
			isLeaf = false;
	}

	if (isLeaf)
	{
		/* Process leaf SELECT */
		Query	   *selectQuery;
		char		selectName[32];
		ParseNamespaceItem *nsitem;
		RangeTblRef *rtr;
		ListCell   *tl;

		/*
		 * Transform SelectStmt into a Query.
		 *
		 * This works the same as SELECT transformation normally would, except
		 * that we prevent resolving unknown-type outputs as TEXT.  This does
		 * not change the subquery's semantics since if the column type
		 * matters semantically, it would have been resolved to something else
		 * anyway.  Doing this lets us resolve such outputs using
		 * select_common_type(), below.
		 *
		 * Note: previously transformed sub-queries don't affect the parsing
		 * of this sub-query, because they are not in the toplevel pstate's
		 * namespace list.
		 */
		selectQuery = parse_sub_analyze((Node *) stmt, pstate,
										NULL, false, false);

		/*
		 * Check for bogus references to Vars on the current query level (but
		 * upper-level references are okay). Normally this can't happen
		 * because the namespace will be empty, but it could happen if we are
		 * inside a rule.
		 */
		if (pstate->p_namespace)
		{
			if (contain_vars_of_level((Node *) selectQuery, 1))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("UNION/INTERSECT/EXCEPT member statement cannot refer to other relations of same query level"),
						 parser_errposition(pstate,
											locate_var_of_level((Node *) selectQuery, 1))));
		}

		/*
		 * Extract a list of the non-junk TLEs for upper-level processing.
		 */
		if (targetlist)
		{
			*targetlist = NIL;
			foreach(tl, selectQuery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tl);

				if (!tle->resjunk)
					*targetlist = lappend(*targetlist, tle);
			}
		}

		/*
		 * Make the leaf query be a subquery in the top-level rangetable.
		 */
		snprintf(selectName, sizeof(selectName), "*SELECT* %d",
				 list_length(pstate->p_rtable) + 1);
		nsitem = addRangeTableEntryForSubquery(pstate,
											   selectQuery,
											   makeAlias(selectName, NIL),
											   false,
											   false);

		/*
		 * Return a RangeTblRef to replace the SelectStmt in the set-op tree.
		 */
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = nsitem->p_rtindex;
		return (Node *) rtr;
	}
	else
	{
		/* Process an internal node (set operation node) */
		SetOperationStmt *op = makeNode(SetOperationStmt);
		List	   *ltargetlist;
		List	   *rtargetlist;
		ListCell   *ltl;
		ListCell   *rtl;
		const char *context;
        int count = 0;
		bool recursive = (pstate->p_parent_cte &&
						  pstate->p_parent_cte->cterecursive);

		context = (stmt->op == SETOP_UNION ? "UNION" :
				   (stmt->op == SETOP_INTERSECT ? "INTERSECT" :
					"EXCEPT"));

		op->op = stmt->op;
		op->all = stmt->all;

		/*
		 * Recursively transform the left child node.
		 */
		op->larg = transformSetOperationTree(pstate, stmt->larg,
											 false,
											 &ltargetlist);

		/*
		 * If we are processing a recursive union query, now is the time to
		 * examine the non-recursive term's output columns and mark the
		 * containing CTE as having those result columns.  We should do this
		 * only at the topmost setop of the CTE, of course.
		 */
		if (isTopLevel && recursive)
			determineRecursiveColTypes(pstate, op->larg, ltargetlist);

		/*
		 * Recursively transform the right child node.
		 */
		op->rarg = transformSetOperationTree(pstate, stmt->rarg,
											 false,
											 &rtargetlist);

		/*
		 * Verify that the two children have the same number of non-junk
		 * columns, and determine the types of the merged output columns.
		 */
		if (list_length(ltargetlist) != list_length(rtargetlist))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("each %s query must have the same number of columns",
							context),
					 parser_errposition(pstate,
										exprLocation((Node *) rtargetlist))));

		if (targetlist)
			*targetlist = NIL;
		op->colTypes = NIL;
		op->colTypmods = NIL;
		op->colCollations = NIL;
		op->groupClauses = NIL;
		forboth(ltl, ltargetlist, rtl, rtargetlist)
		{
			TargetEntry *ltle = (TargetEntry *) lfirst(ltl);
			TargetEntry *rtle = (TargetEntry *) lfirst(rtl);
			Node	   *lcolnode = (Node *) ltle->expr;
			Node	   *rcolnode = (Node *) rtle->expr;
			Oid			lcoltype = exprType(lcolnode);
			Oid			rcoltype = exprType(rcolnode);
			Node	   *bestexpr;
			int			bestlocation;
			Oid			rescoltype;
			int32		rescoltypmod;
			Oid			rescolcoll;

			/* select common type, same as CASE et al */
            rescoltype = mys_select_common_type(pstate,
                                                list_make2(lcolnode, rcolnode),
                                                context,
                                                &bestexpr);
			bestlocation = exprLocation(bestexpr);

            if (rescoltype == UNKNOWNOID)
            {
                rescoltype = list_nth_cell(pstate->targettypelist, count)->oid_value;
                //rescoltype = TEXTOID;
            }

            count++;

			/*
			 * Verify the coercions are actually possible.  If not, we'd fail
			 * later anyway, but we want to fail now while we have sufficient
			 * context to produce an error cursor position.
			 *
			 * For all non-UNKNOWN-type cases, we verify coercibility but we
			 * don't modify the child's expression, for fear of changing the
			 * child query's semantics.
			 *
			 * If a child expression is an UNKNOWN-type Const or Param, we
			 * want to replace it with the coerced expression.  This can only
			 * happen when the child is a leaf set-op node.  It's safe to
			 * replace the expression because if the child query's semantics
			 * depended on the type of this output column, it'd have already
			 * coerced the UNKNOWN to something else.  We want to do this
			 * because (a) we want to verify that a Const is valid for the
			 * target type, or resolve the actual type of an UNKNOWN Param,
			 * and (b) we want to avoid unnecessary discrepancies between the
			 * output type of the child query and the resolved target type.
			 * Such a discrepancy would disable optimization in the planner.
			 *
			 * If it's some other UNKNOWN-type node, eg a Var, we do nothing
			 * (knowing that coerce_to_common_type would fail).  The planner
			 * is sometimes able to fold an UNKNOWN Var to a constant before
			 * it has to coerce the type, so failing now would just break
			 * cases that might work.
			 */
			if (lcoltype != UNKNOWNOID)
                lcolnode = coerce_to_common_type(pstate, lcolnode,
                                                 rescoltype, context);
			else if (IsA(lcolnode, Const) ||
					 IsA(lcolnode, Param))
			{
                lcolnode = coerce_to_common_type(pstate, lcolnode,
                                                 rescoltype, context);
				ltle->expr = (Expr *) lcolnode;
			}

			if (rcoltype != UNKNOWNOID)
                rcolnode = coerce_to_common_type(pstate, rcolnode,
                                                 rescoltype, context);
			else if (IsA(rcolnode, Const) ||
					 IsA(rcolnode, Param))
			{
                rcolnode = coerce_to_common_type(pstate, rcolnode,
                                                 rescoltype, context);
				rtle->expr = (Expr *) rcolnode;
			}

			rescoltypmod = select_common_typmod(pstate,
												list_make2(lcolnode, rcolnode),
												rescoltype);

			/*
			 * Select common collation.  A common collation is required for
			 * all set operators except UNION ALL; see SQL:2008 7.13 <query
			 * expression> Syntax Rule 15c.  (If we fail to identify a common
			 * collation for a UNION ALL column, the colCollations element
			 * will be set to InvalidOid, which may result in a runtime error
			 * if something at a higher query level wants to use the column's
			 * collation.)
			 */
			rescolcoll = select_common_collation(pstate,
												 list_make2(lcolnode, rcolnode),
												 (op->op == SETOP_UNION && op->all));

			/* emit results */
			op->colTypes = lappend_oid(op->colTypes, rescoltype);
			op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);
			op->colCollations = lappend_oid(op->colCollations, rescolcoll);

			/*
			 * For all cases except UNION ALL, identify the grouping operators
			 * (and, if available, sorting operators) that will be used to
			 * eliminate duplicates.
			 */
			if (op->op != SETOP_UNION || !op->all)
			{
				ParseCallbackState pcbstate;

				setup_parser_errposition_callback(&pcbstate, pstate,
												  bestlocation);

				/* If it's a recursive union, we need to require hashing support. */
				op->groupClauses = lappend(op->groupClauses,
										   makeSortGroupClauseForSetOp(rescoltype, recursive));

				cancel_parser_errposition_callback(&pcbstate);
			}

			/*
			 * Construct a dummy tlist entry to return.  We use a SetToDefault
			 * node for the expression, since it carries exactly the fields
			 * needed, but any other expression node type would do as well.
			 */
			if (targetlist)
			{
				SetToDefault *rescolnode = makeNode(SetToDefault);
				TargetEntry *restle;

				rescolnode->typeId = rescoltype;
				rescolnode->typeMod = rescoltypmod;
				rescolnode->collation = rescolcoll;
				rescolnode->location = bestlocation;
				restle = makeTargetEntry((Expr *) rescolnode,
										 0, /* no need to set resno */
										 NULL,
										 false);
				*targetlist = lappend(*targetlist, restle);
			}
		}

		return (Node *) op;
	}
}

/*
 * Process the outputs of the non-recursive term of a recursive union
 * to set up the parent CTE's columns
 */
static void
determineRecursiveColTypes(ParseState *pstate, Node *larg, List *nrtargetlist)
{
	Node	   *node;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	List	   *targetList;
	ListCell   *left_tlist;
	ListCell   *nrtl;
	int			next_resno;

	/*
	 * Find leftmost leaf SELECT
	 */
	node = larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist using column names of leftmost select and
	 * dummy result expressions of the non-recursive term.
	 */
	targetList = NIL;
	next_resno = 1;

	forboth(nrtl, nrtargetlist, left_tlist, leftmostQuery->targetList)
	{
		TargetEntry *nrtle = (TargetEntry *) lfirst(nrtl);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		tle = makeTargetEntry(nrtle->expr,
							  next_resno++,
							  colName,
							  false);
		targetList = lappend(targetList, tle);
	}

	/* Now build CTE's output column info using dummy targetlist */
	analyzeCTETargetList(pstate, pstate->p_parent_cte, targetList);
}

