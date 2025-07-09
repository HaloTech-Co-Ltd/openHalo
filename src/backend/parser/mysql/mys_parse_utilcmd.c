
/*-------------------------------------------------------------------------
 *
 * mys_parse_utilcmd.c
 *	  MySQL perform parse analysis work for various utility commands
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
 *	  src/backend/parser/mysql/mys_parse_utilcmd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_trigger_d.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "parser/mysql/mys_parse_utilcmd.h"
#include "postgres_ext.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


typedef struct
{
    bool hasNull;
    bool hasNotNull;
} ExplicitColumnAttr;

/* Mys的表选项 */
typedef struct
{
    Node *auto_increment;
    char *comment;
} MysTableOption;

/* State shared by transformCreateStmt and its subroutines */
typedef struct
{
	ParseState *pstate;			/* overall parser state */
	const char *stmtType;		/* "CREATE [FOREIGN] TABLE" or "ALTER TABLE" */
	RangeVar   *relation;		/* relation to create or alter */
	Relation	rel;			/* opened/locked rel, if ALTER */
	List	   *inhRelations;	/* relations to inherit from */
	bool		isforeign;		/* true if CREATE/ALTER FOREIGN TABLE */
	bool		isalter;		/* true if altering existing table */
	List	   *columns;		/* ColumnDef items */
    List       *explicitColumnsAttr;     /* 用户显式指定的字段属性 */
	List	   *ckconstraints;	/* CHECK constraints */
	List	   *fkconstraints;	/* FOREIGN KEY constraints */
	List	   *ixconstraints;	/* index-creating constraints */
	List	   *likeclauses;	/* LIKE clauses that need post-processing */
	List	   *extstats;		/* cloned extended statistics */
	List	   *blist;			/* "before list" of things to do before
								 * creating the table */
	List	   *alist;			/* "after list" of things to do after creating
								 * the table */
	IndexStmt  *pkey;			/* PRIMARY KEY index, if any */
	bool		ispartitioned;	/* true if table is partitioned */
	PartitionBoundSpec *partbound;	/* transformed FOR VALUES */
	bool		ofType;			/* true if statement contains OF typename */
    
    List *AutoIncrement;        /* 有auto_increment属性的字段在columns中的下标，从0开始 */
    bool AutoIncrementIsIndex;  /* 自增字段是否为index */
    MysTableOption *mysTableOption;
    
} CreateStmtContext;

/* State shared by transformCreateSchemaStmt and its subroutines */
typedef struct
{
	const char *stmtType;		/* "CREATE SCHEMA" or "ALTER SCHEMA" */
	char	   *schemaname;		/* name of schema */
	RoleSpec   *authrole;		/* owner of schema */
	List	   *sequences;		/* CREATE SEQUENCE items */
	List	   *tables;			/* CREATE TABLE items */
	List	   *views;			/* CREATE VIEW items */
	List	   *indexes;		/* CREATE INDEX items */
	List	   *triggers;		/* CREATE TRIGGER items */
	List	   *grants;			/* GRANT items */
} CreateSchemaStmtContext;


static void transformColumnDefinition(CreateStmtContext *cxt,
									  ColumnDef *column);
static void transformTableConstraint(CreateStmtContext *cxt,
									 Constraint *constraint);
static void transformTableLikeClause(CreateStmtContext *cxt,
									 TableLikeClause *table_like_clause);
static void transformOfType(CreateStmtContext *cxt,
							TypeName *ofTypename);
static CreateStatsStmt *generateClonedExtStatsStmt(RangeVar *heapRel,
												   Oid heapRelid, Oid source_statsid);
static bool mysCheckPrimary(Relation rel, char *colname);
static void generateSerialExtraStmts(CreateStmtContext *cxt, ColumnDef *column,
                                     Oid seqtypid, List *seqoptions,
                                     bool for_identity, bool col_exists,
                                     char **snamespace_p, char **sname_p);
static void transformIndexConstraints(CreateStmtContext *cxt);
static IndexStmt *transformIndexConstraint(Constraint *constraint,
										   CreateStmtContext *cxt);
static void transformExtendedStatistics(CreateStmtContext *cxt);
static void transformFKConstraints(CreateStmtContext *cxt,
								   bool skipValidation,
								   bool isAddConstraint);
static void transformCheckConstraints(CreateStmtContext *cxt,
									  bool skipValidation);
static void transformConstraintAttrs(CreateStmtContext *cxt,
									 List *constraintList);
static void transformColumnType(CreateStmtContext *cxt, ColumnDef *column);
static void transformPartitionCmd(CreateStmtContext *cxt, PartitionCmd *cmd);
static List *MysProcessTableOption(CreateStmtContext *cxt, List *options);
static void MysProcessOnUpdateNow(CreateStmtContext *cxt, ColumnDef *colDef);
static bool isStrTypeColumn(ColumnDef *columnDef);
static bool isTableCollationCaseInsensitive(List *tabOptions);
static void setColumnCollateToCaseInsensitive(ColumnDef *columnDef);
static void rectifySpecifiedColumnCollate(ColumnDef *columnDef);
static void rectifyColumnCollate(List *tabOptions, ColumnDef *columnDef);
static void rectifyColumnsCollate(CreateStmt *stmt);
static void rectifyColumnCollateForAlter(ColumnDef *columnDef);


List *
mys_expandTableLikeClause(RangeVar *heapRel, TableLikeClause *table_like_clause)
{
	List	   *result = NIL;
	List	   *atsubcmds = NIL;
	AttrNumber	parent_attno;
	Relation	relation;
	Relation	childrel;
	TupleDesc	tupleDesc;
	TupleConstr *constr;
	AttrMap    *attmap;
	char	   *comment;
    Oid namespaceID;
    char *namespaceName;

	/*
	 * Open the relation referenced by the LIKE clause.  We should still have
	 * the table lock obtained by transformTableLikeClause (and this'll throw
	 * an assertion failure if not).  Hence, no need to recheck privileges
	 * etc.  We must open the rel by OID not name, to be sure we get the same
	 * table.
	 */
	if (!OidIsValid(table_like_clause->relationOid))
		elog(ERROR, "expandTableLikeClause called on untransformed LIKE clause");

	relation = relation_open(table_like_clause->relationOid, NoLock);

	tupleDesc = RelationGetDescr(relation);
	constr = tupleDesc->constr;

	/*
	 * Open the newly-created child relation; we have lock on that too.
	 */
	childrel = relation_openrv(heapRel, NoLock);

	/*
	 * Construct a map from the LIKE relation's attnos to the child rel's.
	 * This re-checks type match etc, although it shouldn't be possible to
	 * have a failure since both tables are locked.
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(childrel), tupleDesc);

    /*
     * Process defaults, if required.
     */
    // if ((table_like_clause->options &
    //      (CREATE_TABLE_LIKE_DEFAULTS | CREATE_TABLE_LIKE_GENERATED)) &&
    //     constr != NULL)
    // {
    // if (table_like_clause->options && (constr != NULL))
    // {
    //     for (parent_attno = 1; parent_attno <= tupleDesc->natts; parent_attno++)
    //     {
    //         Form_pg_attribute attribute = TupleDescAttr(tupleDesc, 
    //                                                     parent_attno - 1);

    //         /*
    //          * Ignore dropped columns in the parent.
    //          */
    //         if (attribute->attisdropped)
    //             continue;

    //         /*
    //          * Copy default, if present and it should be copied.  We have
    //          * separate options for plain default expressions and GENERATED
    //          * defaults.
    //          */
    //         // if (attribute->atthasdef &&
    //         //     (attribute->attgenerated ?
    //         //      (table_like_clause->options & CREATE_TABLE_LIKE_GENERATED) :
    //         //      (table_like_clause->options & CREATE_TABLE_LIKE_DEFAULTS)))
    //         if (attribute->atthasdef || attribute->attgenerated)
    //         {
    //             Oid origSeqID;
    //             Oid origNamespaceID;
    //             char *origNamespaceName;
    //             char origSeqName[1024];
    //             RangeVar *origSeq;

    //             origNamespaceID = RelationGetNamespace(relation);
    //             origNamespaceName = get_namespace_name(origNamespaceID);
    //             // 这里有一个隐含的条件：
    //             // 在修改表的名称，或者自增序列对应的字段的名称时，
    //             // 必须同步把自增序列的名称也改过来。
    //             snprintf(origSeqName, 1024, "%s_%s_seq", 
    //                      RelationGetRelationName(relation), attribute->attname.data);
    //             origSeq = makeRangeVar(origNamespaceName, origSeqName, -1);
    //             origSeqID = RangeVarGetRelid(origSeq, NoLock, true);
    //             if (origSeqID == InvalidOid)
    //             {
    //                 Node	   *this_default = NULL;
    //                 AttrDefault *attrdef = constr->defval;
    //                 AlterTableCmd *atsubcmd;
    //                 bool		found_whole_row;

    //                 /* Find default in constraint structure */
    //                 for (int i = 0; i < constr->num_defval; i++)
    //                 {
    //                     if (attrdef[i].adnum == parent_attno)
    //                     {
    //                         this_default = stringToNode(attrdef[i].adbin);
    //                         break;
    //                     }
    //                 }
    //                 if (this_default == NULL)
    //                     elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
    //                          parent_attno, RelationGetRelationName(relation));

    //                 atsubcmd = makeNode(AlterTableCmd);
    //                 atsubcmd->subtype = AT_CookedColumnDefault;
    //                 atsubcmd->num = attmap->attnums[parent_attno - 1];
    //                 atsubcmd->def = map_variable_attnos(this_default,
    //                                                     1, 
    //                                                     0,
    //                                                     attmap,
    //                                                     InvalidOid,
    //                                                     &found_whole_row);

    //                 /*
    //                  * Prevent this for the same reason as for constraints below.
    //                  * Note that defaults cannot contain any vars, so it's OK that
    //                  * the error message refers to generated columns.
    //                  */
    //                 if (found_whole_row)
    //                     ereport(ERROR,
    //                             (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
    //                              errmsg("cannot convert whole-row table reference"),
    //                              errdetail("Generation expression for column \"%s\" contains a whole-row reference to table \"%s\".",
    //                                        NameStr(attribute->attname),
    //                                        RelationGetRelationName(relation))));

    //                 atsubcmds = lappend(atsubcmds, atsubcmd);
    //             }   // if (origSeqID == InvalidOid)
    //             else 
    //             {
    //                 CreateSeqStmt *createSeqStmt;
    //                 Oid namespaceID = RelationGetNamespace(childrel);
    //                 char *namespaceName = get_namespace_name(namespaceID);
    //                 char *seqName = ChooseRelationName(RelationGetRelationName(childrel), 
    //                                                    attribute->attname.data, 
    //                                                    "seq", 
    //                                                    namespaceID, 
    //                                                    false);
    //                 List *seqOptions = sequence_options(origSeqID);

    //                 AlterSeqStmt *altSeqStmt;
    //                 List *attNameList;

    //                 AlterTableStmt *altTableStmt;
    //                 AlterTableCmd *setDefault;
    //                 FuncCall *nextvalFuncCall;
    //                 List *nextvalFuncName;
    //                 List *nextvalFuncArgs;
    //                 A_Const *nextvalFuncArg;
    //                 char nextvalFuncArgStr[1024];

    //                 CreateFunctionStmt *createFuncStmt;
    //                 int newFuncNameSize = 256;
    //                 char newFuncName[256];
    //                 int funcOptsSize = 1024;
    //                 char funcOpts[1024];
    //                 CreateTrigStmt *createTrigStmt;
    //                 int newTrigNameSize = 256;
    //                 char newTrigName[256];
    //                 RangeVar *rangeVar;

    //                 createSeqStmt = makeNode(CreateSeqStmt);
    //                 createSeqStmt->sequence = makeRangeVar(namespaceName, seqName, -1);
    //                 createSeqStmt->options = seqOptions;
    //                 createSeqStmt->options = lcons(makeDefElem("as", 
    //                                                            (Node *) makeTypeNameFromOid(attribute->atttypid, -1), 
    //                                                            -1), 
    //                                                createSeqStmt->options);
    //                 createSeqStmt->ownerId = InvalidOid; //createSeqStmt->ownerId = RelationGetRelid(relation);
    //                 createSeqStmt->for_identity = false; //createSeqStmt->for_identity = true;
    //                 result = lappend(result, createSeqStmt);

    //                 altSeqStmt = makeNode(AlterSeqStmt);
    //                 altSeqStmt->sequence = makeRangeVar(namespaceName, seqName, -1); 
    //                 attNameList = list_make3(makeString(namespaceName), 
    //                                          makeString(RelationGetRelationName(childrel)), 
    //                                          makeString(attribute->attname.data));
    //                 altSeqStmt->options = list_make1(makeDefElem("owned_by", 
    //                                                              (Node *)attNameList, 
    //                                                              -1));
    //                 altSeqStmt->for_identity = false;
    //                 result = lappend(result, altSeqStmt);

    //                 altTableStmt = makeNode(AlterTableStmt);
    //                 altTableStmt->relation = makeRangeVar(namespaceName, 
    //                                                       RelationGetRelationName(childrel), 
    //                                                       -1);
    //                 altTableStmt->objtype = OBJECT_TABLE;
    //                 altTableStmt->missing_ok = false;
    //                 setDefault = makeNode(AlterTableCmd);
    //                 setDefault->subtype = AT_ColumnDefault;
    //                 setDefault->name = pstrdup(attribute->attname.data);
    //                 nextvalFuncName = list_make1(makeString("nextval"));
    //                 snprintf(nextvalFuncArgStr, 1024, "%s.%s", namespaceName, seqName);
    //                 nextvalFuncArg = makeNode(A_Const);
    //                 nextvalFuncArg->val.type = T_String;
    //                 nextvalFuncArg->val.val.str = pstrdup(nextvalFuncArgStr);
    //                 nextvalFuncArg->location = -1;
    //                 nextvalFuncArgs = list_make1(nextvalFuncArg);
    //                 nextvalFuncCall = makeFuncCall(nextvalFuncName, 
    //                                                nextvalFuncArgs, 
    //                                                COERCE_EXPLICIT_CALL, 
    //                                                -1);
    //                 setDefault->def = (Node*)nextvalFuncCall;
    //                 altTableStmt->cmds = list_make1(setDefault);
    //                 result = lappend(result, altTableStmt);

    //                 // 处理自增跳空的触发器
    //                 createFuncStmt = makeNode(CreateFunctionStmt);
    //                 snprintf(newFuncName, 
    //                          newFuncNameSize, 
    //                          "func_reset_serial_for_%s", 
    //                          seqName);
    //                 createFuncStmt->funcname = list_make2(makeString(pstrdup(namespaceName)), 
    //                                                       makeString(pstrdup(newFuncName)));
    //                 createFuncStmt->is_procedure = false;
    //                 createFuncStmt->replace = true;
    //                 createFuncStmt->parameters = NULL;
    //                 createFuncStmt->returnType = makeTypeName(pstrdup("trigger"));
    //                 createFuncStmt->sql_body = NULL;
    //                 createFuncStmt->proaccess = NON_PACKAGE_MEMBER;
    //                 snprintf(funcOpts, 
    //                          funcOptsSize, 
    //                          "BEGIN /
    //                              IF NEW.%s is not null THEN /
    //                                 if (select nextval('%s.%s')) <= New.%s then /
    //                                     PERFORM setval('%s.%s', New.%s, true); /
    //                                 else /
    //                                     PERFORM setval('%s.%s', (select currval('%s.%s') - 1), true); /
    //                                 end if; /
    //                              ELSE /
    //                                 New.%s = (select nextval('%s.%s')); /
    //                              END IF; /
    //                              RETURN NEW; /
    //                          END;", 
    //                          NameStr(tupleDesc->attrs[parent_attno - 1].attname), 
    //                          namespaceName, seqName, NameStr(tupleDesc->attrs[parent_attno - 1].attname),
    //                          namespaceName, seqName, NameStr(tupleDesc->attrs[parent_attno - 1].attname), 
    //                          namespaceName, seqName, namespaceName, seqName,
    //                          NameStr(tupleDesc->attrs[parent_attno - 1].attname), namespaceName, seqName);
    //                 createFuncStmt->options = lappend(createFuncStmt->options, 
    //                                                   makeDefElem("as", 
    //                                                               (Node *)list_make1(makeString(pstrdup(funcOpts))), 
    //                                                               -1));
    //                 createFuncStmt->options = lappend(createFuncStmt->options, 
    //                                                   makeDefElem("language", 
    //                                                               (Node *)makeString(pstrdup("plpgsql")), 
    //                                                               -1));
    //                 result = lappend(result, createFuncStmt);

    //                 createTrigStmt = makeNode(CreateTrigStmt);
    //                 snprintf(newTrigName, 
    //                          newTrigNameSize, 
    //                          "trigger_reset_serial_for_%s", 
    //                          seqName);
    //                 createTrigStmt->trigname = pstrdup(newTrigName);
    //                 createTrigStmt->replace = true;
    //                 createTrigStmt->isconstraint = false;
    //                 rangeVar = makeRangeVar(namespaceName, RelationGetRelationName(childrel), -1);
    //                 createTrigStmt->relation = rangeVar;
    //                 createTrigStmt->funcname = list_make2(makeString(pstrdup(namespaceName)), 
    //                                                       makeString(pstrdup(newFuncName)));
    //                 createTrigStmt->args = NULL;
    //                 createTrigStmt->row = true;
    //                 createTrigStmt->timing = TRIGGER_TYPE_BEFORE;
    //                 createTrigStmt->events = TRIGGER_TYPE_INSERT;
    //                 createTrigStmt->columns = NIL;
    //                 createTrigStmt->whenClause = NULL;
    //                 createTrigStmt->transitionRels = NIL;
    //                 createTrigStmt->deferrable = true;
    //                 createTrigStmt->initdeferred = false;
    //                 createTrigStmt->constrrel = NULL;
    //                 result = lappend(result, createTrigStmt);
    //             }   // else of if (origSeqID == InvalidOid)
    //         }   // if (attribute->atthasdef || attribute->attgenerated)
    //     }   // create table tab_gen(id1 int, id2 int, id3 int generated always as (id1 + id2));
    // }   // if (table_like_clause->options && (constr != NULL))

    
	// MySQL的create table like 没丢掉字段的check，但是MySQL的check机制实际是无效的
	// Halo的create table like没丢掉字段的check，check机制有效
	/*
	 * Copy CHECK constraints if requested, being careful to adjust attribute
	 * numbers so they match the child.
	 */
	//if ((table_like_clause->options & CREATE_TABLE_LIKE_CONSTRAINTS) &&
	//	constr != NULL)
	//{
	//	int			ccnum;

	//	for (ccnum = 0; ccnum < constr->num_check; ccnum++)
	//	{
	//		char	   *ccname = constr->check[ccnum].ccname;
	//		char	   *ccbin = constr->check[ccnum].ccbin;
	//		bool		ccnoinherit = constr->check[ccnum].ccnoinherit;
	//		Node	   *ccbin_node;
	//		bool		found_whole_row;
	//		Constraint *n;
	//		AlterTableCmd *atsubcmd;

	//		ccbin_node = map_variable_attnos(stringToNode(ccbin),
	//										 1, 0,
	//										 attmap,
	//										 InvalidOid, &found_whole_row);

	//		/*
	//		 * We reject whole-row variables because the whole point of LIKE
	//		 * is that the new table's rowtype might later diverge from the
	//		 * parent's.  So, while translation might be possible right now,
	//		 * it wouldn't be possible to guarantee it would work in future.
	//		 */
	//		if (found_whole_row)
	//			ereport(ERROR,
	//					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
	//					 errmsg("cannot convert whole-row table reference"),
	//					 errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
	//							   ccname,
	//							   RelationGetRelationName(relation))));

	//		n = makeNode(Constraint);
	//		n->contype = CONSTR_CHECK;
	//		n->conname = pstrdup(ccname);
	//		n->location = -1;
	//		n->is_no_inherit = ccnoinherit;
	//		n->raw_expr = NULL;
	//		n->cooked_expr = nodeToString(ccbin_node);

	//		/* We can skip validation, since the new table should be empty. */
	//		n->skip_validation = true;
	//		n->initially_valid = true;

	//		atsubcmd = makeNode(AlterTableCmd);
	//		atsubcmd->subtype = AT_AddConstraint;
	//		atsubcmd->def = (Node *) n;
	//		atsubcmds = lappend(atsubcmds, atsubcmd);

	//		/* Copy comment on constraint */
	//		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
	//			(comment = GetComment(get_relation_constraint_oid(RelationGetRelid(relation),
	//															  n->conname, false),
	//								  ConstraintRelationId,
	//								  0)) != NULL)
	//		{
	//			CommentStmt *stmt = makeNode(CommentStmt);

	//			stmt->objtype = OBJECT_TABCONSTRAINT;
	//			stmt->object = (Node *) list_make3(makeString(heapRel->schemaname),
	//											   makeString(heapRel->relname),
	//											   makeString(n->conname));
	//			stmt->comment = comment;

	//			result = lappend(result, stmt);
	//		}
	//	}
	//}
    

	// /*
	//  * If we generated any ALTER TABLE actions above, wrap them into a single
	//  * ALTER TABLE command.  Stick it at the front of the result, so it runs
	//  * before any CommentStmts we made above.
	//  */
	// if (atsubcmds)
	// {
	//     AlterTableStmt *atcmd = makeNode(AlterTableStmt);

	//     atcmd->relation = copyObject(heapRel);
	//     atcmd->cmds = atsubcmds;
	//     atcmd->objtype = OBJECT_TABLE;
	//     atcmd->missing_ok = false;
	//     result = lcons(atcmd, result);
	// }

	/*
	 * Process indexes if required.
	 */
	if ((table_like_clause->options & CREATE_TABLE_LIKE_INDEXES) &&
		relation->rd_rel->relhasindex)
	{
		List	   *parent_indexes;
		ListCell   *l;

		parent_indexes = RelationGetIndexList(relation);

		foreach(l, parent_indexes)
		{
			Oid			parent_index_oid = lfirst_oid(l);
			Relation	parent_index;
			IndexStmt  *index_stmt;

			parent_index = index_open(parent_index_oid, AccessShareLock);

			/* Build CREATE INDEX statement to recreate the parent_index */
			index_stmt = generateClonedIndexStmt(heapRel,
												 parent_index,
												 attmap,
												 NULL);

			/* Copy comment on index, if requested */
			if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS)
			{
				comment = GetComment(parent_index_oid, RelationRelationId, 0);

				/*
				 * We make use of IndexStmt's idxcomment option, so as not to
				 * need to know now what name the index will have.
				 */
				index_stmt->idxcomment = comment;
			}

			result = lappend(result, index_stmt);

			index_close(parent_index, AccessShareLock);
		}
	}

    
    if (table_like_clause->options && (constr != NULL))
    {
        for (parent_attno = 1; parent_attno <= tupleDesc->natts; parent_attno++)
        {
            Form_pg_attribute attribute = TupleDescAttr(tupleDesc, 
                                                        parent_attno - 1);

            /*
             * Ignore dropped columns in the parent.
             */
            if (attribute->attisdropped)
                continue;

            /*
             * Copy default, if present and it should be copied.  We have
             * separate options for plain default expressions and GENERATED
             * defaults.
             */
            // if (attribute->atthasdef &&
            //     (attribute->attgenerated ?
            //      (table_like_clause->options & CREATE_TABLE_LIKE_GENERATED) :
            //      (table_like_clause->options & CREATE_TABLE_LIKE_DEFAULTS)))
            if (attribute->atthasdef || attribute->attgenerated)
            {
                Oid origNamespaceID;
                char *origNamespaceName;

                origNamespaceID = RelationGetNamespace(relation);
                origNamespaceName = get_namespace_name(origNamespaceID);
                if (getColumnDefaultSeq(relation, NameStr(attribute->attname)) == InvalidOid)
                {
                    Node	   *this_default = NULL;
                    AttrDefault *attrdef = constr->defval;
                    AlterTableCmd *atsubcmd;
                    bool		found_whole_row;

                    /* Find default in constraint structure */
                    for (int i = 0; i < constr->num_defval; i++)
                    {
                        if (attrdef[i].adnum == parent_attno)
                        {
                            this_default = stringToNode(attrdef[i].adbin);
                            break;
                        }
                    }
                    if (this_default == NULL)
                        elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
                             parent_attno, RelationGetRelationName(relation));

                    atsubcmd = makeNode(AlterTableCmd);
                    atsubcmd->subtype = AT_CookedColumnDefault;
                    atsubcmd->num = attmap->attnums[parent_attno - 1];
                    atsubcmd->def = map_variable_attnos(this_default,
                                                        1, 
                                                        0,
                                                        attmap,
                                                        InvalidOid,
                                                        &found_whole_row);

                    /*
                     * Prevent this for the same reason as for constraints below.
                     * Note that defaults cannot contain any vars, so it's OK that
                     * the error message refers to generated columns.
                     */
                    if (found_whole_row)
                        ereport(ERROR,
                                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                 errmsg("cannot convert whole-row table reference"),
                                 errdetail("Generation expression for column \"%s\" contains a whole-row reference to table \"%s\".",
                                           NameStr(attribute->attname),
                                           RelationGetRelationName(relation))));

                    atsubcmds = lappend(atsubcmds, atsubcmd);
                }
                else 
                {
                    CreateSeqStmt *createSeqStmt;
                    AlterSeqStmt *altSeqStmt;
                    AlterTableCmd *setDefault;
                    CreateFunctionStmt *createFuncStmt;
                    CreateTrigStmt *createTrigStmt;
                    AlterObjectDependsStmt *alterDependStmt1;
                    AlterObjectDependsStmt *alterDependStmt2;

                    Oid origSeqID;
                    char *origSeqName;
                    RangeVar *origSeq;
                    List *seqOptions;
                    Oid namespaceID;
                    char *namespaceName;
                    char *relName;
                    char *colName;
                    char *seqName;

                    namespaceID = RelationGetNamespace(childrel);
                    namespaceName = get_namespace_name(namespaceID);
                    relName = RelationGetRelationName(childrel);
                    colName = NameStr(attribute->attname);
                    seqName = mysBuildSeqName(relName);

                    origSeqName = mysBuildSeqName(RelationGetRelationName(relation));
                    origSeq = makeRangeVar(origNamespaceName, origSeqName, -1);
                    origSeqID = RangeVarGetRelid(origSeq, NoLock, true);
                    seqOptions = sequence_options(origSeqID);

                    createSeqStmt = createSeq(namespaceName, relName, seqOptions, 
                                              attribute->atttypid, 
                                              childrel->rd_rel->relowner);
                    result = lappend(result, createSeqStmt);

                    altSeqStmt = createAlterSeq(namespaceName, relName, 
                                                NameStr(attribute->attname));
                    result = lappend(result, altSeqStmt);

                    setDefault = setDefaultValWithSeq(namespaceName, 
                                                      seqName, 
                                                      colName);
                    atsubcmds = lappend(atsubcmds, setDefault);

                    createFuncStmt = createAutoIncrementTriggerFunc(namespaceName, 
                                                                    relName, 
                                                                    colName);
                    result = lappend(result, createFuncStmt);

                    createTrigStmt = createAutoIncrementTrigger(namespaceName, relName);
                    result = lappend(result, createTrigStmt);

                    alterDependStmt1 = bindTriggerFunctionToTrigger(createTrigStmt->funcname, 
                                                                    createTrigStmt->relation, 
                                                                    createTrigStmt->trigname);
                    result = lappend(result, alterDependStmt1);

                    alterDependStmt2 = bindTriggerToSeq(createTrigStmt->relation, 
                                                        createTrigStmt->trigname, 
                                                        namespaceName, 
                                                        seqName);
                    result = lappend(result, alterDependStmt2);
                }   // else of if (origSeqID == InvalidOid)
            }   // if (attribute->atthasdef || attribute->attgenerated)
        }   // create table tab_gen(id1 int, id2 int, id3 int generated always as (id1 + id2));
    }   // if (table_like_clause->options && (constr != NULL))

    /*
	 * If we generated any ALTER TABLE actions above, wrap them into a single
	 * ALTER TABLE command. Stick it at the end of the result, so it runs
	 * after CreateSeqStmt we made above.
	 */
	if (atsubcmds)
	{
		AlterTableStmt *atcmd = makeNode(AlterTableStmt);

		atcmd->relation = copyObject(heapRel);
		atcmd->cmds = atsubcmds;
		atcmd->objtype = OBJECT_TABLE;
		atcmd->missing_ok = false;
		result = lappend(result, atcmd);
	}

    /* 处理on update timestamp 的触发器 */
    namespaceID = RelationGetNamespace(childrel);
    namespaceName = get_namespace_name(namespaceID);
    for (int i = 0; i < tupleDesc->natts; i++)
    {
        if (!tupleDesc->attrs[i].attisdropped && 
            existAutoUpdateTrigOnThisAtt(relation, &(tupleDesc->attrs[i])))
        {
            CreateFunctionStmt *createFuncStmt;
            CreateTrigStmt *createTrigStmt;
            AlterObjectDependsStmt *alterDependStmt1;
            AlterObjectDependsStmt *alterDependStmt2;
            char *relName;
            char *colName;

            relName = RelationGetRelationName(childrel);
            colName = NameStr(tupleDesc->attrs[i].attname);

            createFuncStmt = createAutoUpdateTimeStampTriggerFunc(namespaceName, 
                                                                  relName, 
                                                                  colName);
            result = lappend(result, createFuncStmt);

            createTrigStmt = createAutoUpdateTimeStampTrigger(namespaceName, 
                                                              relName, 
                                                              colName);
            result = lappend(result, createTrigStmt);

            alterDependStmt1 = bindTriggerFunctionToTrigger(createTrigStmt->funcname, 
                                                            createTrigStmt->relation, 
                                                            createTrigStmt->trigname);
            result = lappend(result, alterDependStmt1);

            alterDependStmt2 = bindTriggerToColumn(createTrigStmt->relation, 
                                                   createTrigStmt->trigname, 
                                                   colName);
            result = lappend(result, alterDependStmt2);
        }
        else 
        {
            continue;
        }
    }
    

	/* Done with child rel */
	table_close(childrel, NoLock);

	/*
	 * Close the parent rel, but keep our AccessShareLock on it until xact
	 * commit.  That will prevent someone else from deleting or ALTERing the
	 * parent before the child is committed.
	 */
	table_close(relation, NoLock);

	return result;
}


CreateFunctionStmt *
createTriggerFunc(char *namespace, char *funcName, char *funcBody)
{
    CreateFunctionStmt *createFuncStmt;

    createFuncStmt = makeNode(CreateFunctionStmt);
    createFuncStmt->funcname = list_make2(makeString(pstrdup(namespace)), 
                                          makeString(pstrdup(funcName)));
    createFuncStmt->is_procedure = false;
    createFuncStmt->replace = true;
    createFuncStmt->parameters = NULL;
    createFuncStmt->returnType = makeTypeName(pstrdup("trigger"));
    createFuncStmt->sql_body = NULL;
    createFuncStmt->proaccess = NON_PACKAGE_MEMBER;
    createFuncStmt->options = lappend(createFuncStmt->options, 
                                      makeDefElem("as", 
                                                  (Node *)list_make1(makeString(pstrdup(funcBody))), 
                                                  -1));
    createFuncStmt->options = lappend(createFuncStmt->options, 
                                      makeDefElem("language", 
                                                  (Node *)makeString(pstrdup("plpgsql")), 
                                                  -1));

    return createFuncStmt;
}


CreateTrigStmt *
createTrigger(char *namespace, char *relName, char *trigFuncName, char *trigName, 
              int16 trigTiming, int16 trigEvents)
{
    CreateTrigStmt *createTrigStmt;
    RangeVar *rangeVar;

    createTrigStmt = makeNode(CreateTrigStmt);
    createTrigStmt->trigname = pstrdup(trigName);
    createTrigStmt->replace = true;
    createTrigStmt->isconstraint = false;
    rangeVar = makeRangeVar(namespace, relName, -1);
    createTrigStmt->relation = rangeVar;
    createTrigStmt->funcname = list_make2(makeString(pstrdup(namespace)), 
                                          makeString(pstrdup(trigFuncName)));
    createTrigStmt->args = NIL;
    createTrigStmt->row = true;
    createTrigStmt->timing = trigTiming;
    createTrigStmt->events = trigEvents;
    createTrigStmt->columns = NIL;
    createTrigStmt->whenClause = NULL;
    createTrigStmt->transitionRels = NIL;
    createTrigStmt->deferrable = true;
    createTrigStmt->initdeferred = false;
    createTrigStmt->constrrel = NULL;

    return createTrigStmt;
}


CreateFunctionStmt *
createAutoIncrementTriggerFunc(char *namespaceName, char *relName, char *colName)
{
    char *funcName;
    char funcBody[1024];
    int funcBodySize = 1024;
    char *seqName;

    funcName = mysBuildTrigFuncNameForAutoInc(relName);
    seqName = mysBuildSeqName(relName);
    snprintf(funcBody, 
             funcBodySize, 
            "BEGIN "\
                "if New.%s is not null then "\
                    "if 0 < New.%s then "\
                        "PERFORM mysql.setval('%s.%s'::regclass, New.%s, true);"\
                    "elsif 0 = New.%s then "\
                        "New.%s = (select nextval('%s.%s'::regclass)); "\
                    "end if; "\
                "else "\
                    "New.%s = (select nextval('%s.%s'::regclass)); "\
                "end if; "\
                "RETURN NEW; "\
            "END;",
             colName,
             colName,
             namespaceName, seqName, colName,
             colName,
             colName, namespaceName, seqName,
             colName, namespaceName, seqName); 

    return createTriggerFunc(namespaceName, funcName, funcBody);
}


CreateSeqStmt *
createSeq(char *namespace, char *relName, List *seqOptions, 
          Oid seqTypeId, Oid ownerId)
{
    CreateSeqStmt *createSeqStmt;
    char *seqName;

    createSeqStmt = makeNode(CreateSeqStmt);
    seqName = mysBuildSeqName(relName);
    createSeqStmt->sequence = makeRangeVar(namespace, seqName, -1);
    createSeqStmt->options = seqOptions;
    if (seqTypeId != InvalidOid)
    {
        createSeqStmt->options = lcons(makeDefElem("as", 
                                                   (Node *) makeTypeNameFromOid(seqTypeId, -1), 
                                                   -1), 
                                       createSeqStmt->options);
    }
    createSeqStmt->ownerId = ownerId;
    createSeqStmt->for_identity = false;

    return createSeqStmt;
}


AlterSeqStmt *
createAlterSeq(char *namespaceName, char *relName, char *colName)
{
    AlterSeqStmt * altSeqStmt;
    char *seqName;
    List *attNameList;

    /* 把序列和字段绑定起来 */
    altSeqStmt = makeNode(AlterSeqStmt);
    seqName = mysBuildSeqName(relName);
    altSeqStmt->sequence = makeRangeVar(namespaceName, seqName, -1); 
    attNameList = list_make3(makeString(namespaceName), 
                             makeString(relName), 
                             makeString(colName));
    altSeqStmt->options = list_make1(makeDefElem("owned_by", 
                                                 (Node *)attNameList, 
                                                 -1));
    altSeqStmt->for_identity = false;

    return altSeqStmt;
}


AlterTableCmd *
setDefaultValWithSeq(char *namespaceName, char *seqName, char *colName)
{
    AlterTableCmd *setDefault;
    FuncCall *nextvalFuncCall;
    List *nextvalFuncName;
    List *nextvalFuncArgs;
    A_Const *nextvalFuncArg;
    char nextvalFuncArgStr[1024];

    setDefault = makeNode(AlterTableCmd);
    setDefault->subtype = AT_ColumnDefault;
    setDefault->name = pstrdup(colName);

    nextvalFuncName = list_make1(makeString("nextval"));
    snprintf(nextvalFuncArgStr, 1024, "%s.%s", namespaceName, seqName);
    nextvalFuncArg = makeNode(A_Const);
    nextvalFuncArg->val.type = T_String;
    nextvalFuncArg->val.val.str = pstrdup(nextvalFuncArgStr);
    nextvalFuncArg->location = -1;
    nextvalFuncArgs = list_make1(nextvalFuncArg);
    nextvalFuncCall = makeFuncCall(nextvalFuncName, 
                                   nextvalFuncArgs, 
                                   COERCE_EXPLICIT_CALL, 
                                   -1);
    setDefault->def = (Node*)nextvalFuncCall;

    return setDefault;
}


CreateTrigStmt *
createAutoIncrementTrigger(char *namespace, char *relName)
{
    char *trigFuncName;
    char *trigName;

    trigFuncName = mysBuildTrigFuncNameForAutoInc(relName);
    trigName = mysBuildTrigNameForAutoInc(relName);

    return createTrigger(namespace, relName, trigFuncName, trigName, 
                         TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
}


CreateFunctionStmt *
createAutoUpdateTimeStampTriggerFunc(char *namespace, char *relName, char *colName)
{
    char *funcName;
    char funcBody[1024];
    int funcBodySize = 1024;

    funcName = mysBuildTrigFuncNameForOnUpdateNow(relName, colName);
    snprintf(funcBody, 
             funcBodySize, 
             "begin if (OLD.%s is null and NEW.%s is null) or (NEW.%s = OLD.%s) THEN NEW.%s=pg_catalog.now(); end if; return NEW; end;",
             colName, 
             colName, 
             colName, 
             colName, 
             colName);

    return createTriggerFunc(namespace, funcName, funcBody);
}

CreateTrigStmt *
createAutoUpdateTimeStampTrigger(char *namespace, char *relName, char *colName)
{
    char *trigFuncName;
    char *trigName;

    trigFuncName = mysBuildTrigFuncNameForOnUpdateNow(relName, colName);
    trigName = mysBuildTrigNameForOnUpdateNow(relName, colName);

    return createTrigger(namespace, relName, trigFuncName, trigName, 
                         TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
}


bool 
existAutoUpdateTrigOnThisAtt(Relation relation, FormData_pg_attribute *att)
{
    bool ret;
    HeapTuple typTuple;
    char *typeName;

    ret = false;

    typTuple = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(att->atttypid));
    typeName = NameStr(((Form_pg_type)GETSTRUCT(typTuple))->typname);
    if (strncmp(typeName, "timestamp", 9) == 0)
    {
        Relation	trigRel;
        SysScanDesc trigScan;
        ScanKeyData trigSkey[1];
        HeapTuple	trigTup;
        char expectedFuncBody[512];
        int expectedFuncBodyLen;
        char *attName;

        attName = NameStr(att->attname);
        for (int i = 0; i < strlen(attName); i++)
        {
            if (('A' <= attName[i]) && (attName[i] <= 'Z'))
            {
                attName += 32;
            }
        }
        pg_snprintf(expectedFuncBody, 
                    512, 
                    "beginif(old.%sisnullandnew.%sisnull)or(new.%s=old.%s)thennew.%s=pg_catalog.now();endif;returnnew;end",
                    attName,
                    attName,
                    attName,
                    attName,
                    attName);
        expectedFuncBodyLen = strlen(expectedFuncBody);

        trigRel = table_open(TriggerRelationId, AccessShareLock);
        ScanKeyInit(&trigSkey[0],
                    Anum_pg_trigger_tgrelid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(relation->rd_id));
        trigScan = systable_beginscan(trigRel, TriggerRelidNameIndexId, true,
                                      NULL, 1, trigSkey);
        while (HeapTupleIsValid(trigTup = systable_getnext(trigScan)))
        {
            Form_pg_trigger trig;
            Relation	funcRel;
            SysScanDesc funcScan;
            ScanKeyData funcSkey[1];
            HeapTuple	funcTup;

            trig = (Form_pg_trigger) GETSTRUCT(trigTup);
            funcRel = table_open(ProcedureRelationId, AccessShareLock);
            ScanKeyInit(&funcSkey[0],
                        Anum_pg_proc_oid,
                        BTEqualStrategyNumber, F_OIDEQ,
                        ObjectIdGetDatum(trig->tgfoid));
            funcScan = systable_beginscan(funcRel, ProcedureOidIndexId, true,
                                          NULL, 1, funcSkey);
            funcTup = systable_getnext(funcScan);
            if (HeapTupleIsValid(funcTup))
            {
                bool isnull;
                Datum funbBodyDatum = SysCacheGetAttr(PROCOID, 
                                                      funcTup, 
                                                      Anum_pg_proc_prosrc, 
                                                      &isnull);
                if (!isnull)
                {
                    char *funcBody;
                    int funcBodyLen;
                    funcBody = TextDatumGetCString(funbBodyDatum);
                    funcBodyLen = strlen(funcBody);
                    if (funcBodyLen < 10240)
                    {
                        char buf[10240];
                        int j = 0;
                        for (int i = 0; i < funcBodyLen; i++)
                        {
                            if ((funcBody[i] != ' ') && 
                                (funcBody[i] != '\n') &&
                                (funcBody[i] != '\r') &&
                                (funcBody[i] != '\t'))
                            {
                                if (('A' <= funcBody[i]) && (funcBody[i] <= 'Z'))
                                {
                                    buf[j++] = funcBody[i] + 32;
                                }
                                else 
                                {
                                    buf[j++] = funcBody[i];
                                }
                            }
                        }
                        buf[j] = '\0';

                        if (j < 256)
                        {
                            for (int i = 0; i < j; i++)
                            {
                                if (strncmp((buf + i), expectedFuncBody, expectedFuncBodyLen) == 0)
                                {
                                    ret = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            systable_endscan(funcScan);
            table_close(funcRel, AccessShareLock);

            if (ret)
            {
                break;
            }
        }

        systable_endscan(trigScan);
        table_close(trigRel, AccessShareLock);
    }

    heap_freetuple(typTuple);

    return ret;
}

/*
 * standard_transformCreateStmt -
 *	  parse analysis for CREATE TABLE
 *
 * Returns a List of utility commands to be done in sequence.  One of these
 * will be the transformed CreateStmt, but there may be additional actions
 * to be done before and after the actual DefineRelation() call.
 * In addition to normal utility commands such as AlterTableStmt and
 * IndexStmt, the result list may contain TableLikeClause(s), representing
 * the need to perform additional parse analysis after DefineRelation().
 *
 * SQL allows constraints to be scattered all over, so thumb through
 * the columns and collect all constraints into one place.
 * If there are any implied indices (e.g. UNIQUE or PRIMARY KEY)
 * then expand those into multiple IndexStmt blocks.
 *	  - thomas 1997-12-02
 */
List *
mys_transformCreateStmt(CreateStmt *stmt, const char *queryString)
{
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *result;
	List	   *save_alist;
	ListCell   *elements;
	Oid			namespaceid;
	Oid			existing_relid;
	ParseCallbackState pcbstate;

    rectifyColumnsCollate(stmt);

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	/*
	 * Look up the creation namespace.  This also checks permissions on the
	 * target namespace, locks it against concurrent drops, checks for a
	 * preexisting relation in that namespace with the same name, and updates
	 * stmt->relation->relpersistence if the selected namespace is temporary.
	 */
	setup_parser_errposition_callback(&pcbstate, pstate,
									  stmt->relation->location);
	namespaceid =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock,
											 &existing_relid);
	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * If the relation already exists and the user specified "IF NOT EXISTS",
	 * bail out with a NOTICE.
	 */
	if (stmt->if_not_exists && OidIsValid(existing_relid))
	{
		/*
		 * If we are in an extension script, insist that the pre-existing
		 * object be a member of the extension, to avoid security risks.
		 */
		ObjectAddress address;

		ObjectAddressSet(address, RelationRelationId, existing_relid);
		checkMembershipInCurrentExtension(&address);

		/* OK to skip */
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists, skipping",
						stmt->relation->relname)));
		return NIL;
	}

	/*
	 * If the target relation name isn't schema-qualified, make it so.  This
	 * prevents some corner cases in which added-on rewritten commands might
	 * think they should apply to other relations that have the same name and
	 * are earlier in the search path.  But a local temp table is effectively
	 * specified to be in pg_temp, so no need for anything extra in that case.
	 */
	if (stmt->relation->schemaname == NULL
		&& stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
		stmt->relation->schemaname = get_namespace_name(namespaceid);

	/* Set up CreateStmtContext */
	cxt.pstate = pstate;
	if (IsA(stmt, CreateForeignTableStmt))
	{
		cxt.stmtType = "CREATE FOREIGN TABLE";
		cxt.isforeign = true;
	}
	else
	{
		cxt.stmtType = "CREATE TABLE";
		cxt.isforeign = false;
	}
	cxt.relation = stmt->relation;
	cxt.rel = NULL;
	cxt.inhRelations = stmt->inhRelations;
	cxt.isalter = false;
	cxt.columns = NIL;
    cxt.explicitColumnsAttr = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.likeclauses = NIL;
	cxt.extstats = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.ispartitioned = stmt->partspec != NULL;
	cxt.partbound = stmt->partbound;
	cxt.ofType = (stmt->ofTypename != NULL);
    cxt.AutoIncrement = NIL;
    cxt.AutoIncrementIsIndex = false;
    cxt.mysTableOption = NULL;

	Assert(!stmt->ofTypename || !stmt->inhRelations);	/* grammar enforces */

	if (stmt->ofTypename)
		transformOfType(&cxt, stmt->ofTypename);

	if (stmt->partspec)
	{
		if (stmt->inhRelations && !stmt->partbound)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot create partitioned table as inheritance child")));
	}

    stmt->options = MysProcessTableOption(&cxt, stmt->options);

	/*
	 * Run through each primary element in the table creation clause. Separate
	 * column defs from constraints, and do preliminary analysis.
	 */
	foreach(elements, stmt->tableElts)
	{
		Node	   *element = lfirst(elements);

		switch (nodeTag(element))
		{
			case T_ColumnDef:
				transformColumnDefinition(&cxt, (ColumnDef *) element);
				break;

			case T_Constraint:
				transformTableConstraint(&cxt, (Constraint *) element);
				break;

			case T_TableLikeClause:
				transformTableLikeClause(&cxt, (TableLikeClause *) element);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(element));
				break;
		}
	}

	/*
	 * Transfer anything we already have in cxt.alist into save_alist, to keep
	 * it separate from the output of transformIndexConstraints.  (This may
	 * not be necessary anymore, but we'll keep doing it to preserve the
	 * historical order of execution of the alist commands.)
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	Assert(stmt->constraints == NIL);

	/*
	 * Postprocess constraints that give rise to index definitions.
	 */
	transformIndexConstraints(&cxt);

	/*
	 * Re-consideration of LIKE clauses should happen after creation of
	 * indexes, but before creation of foreign keys.  This order is critical
	 * because a LIKE clause may attempt to create a primary key.  If there's
	 * also a pkey in the main CREATE TABLE list, creation of that will not
	 * check for a duplicate at runtime (since index_check_primary_key()
	 * expects that we rejected dups here).  Creation of the LIKE-generated
	 * pkey behaves like ALTER TABLE ADD, so it will check, but obviously that
	 * only works if it happens second.  On the other hand, we want to make
	 * pkeys before foreign key constraints, in case the user tries to make a
	 * self-referential FK.
	 */
	cxt.alist = list_concat(cxt.alist, cxt.likeclauses);

	/*
	 * Postprocess foreign-key constraints.
	 */
	transformFKConstraints(&cxt, true, false);

	/*
	 * Postprocess check constraints.
	 *
	 * For regular tables all constraints can be marked valid immediately,
	 * because the table is new therefore empty. Not so for foreign tables.
	 */
	transformCheckConstraints(&cxt, !cxt.isforeign);

	/*
	 * Postprocess extended statistics.
	 */
	transformExtendedStatistics(&cxt);

	/*
	 * Output results.
	 */
	stmt->tableElts = cxt.columns;
	stmt->constraints = cxt.ckconstraints;

	result = lappend(cxt.blist, stmt);
	result = list_concat(result, cxt.alist);
	result = list_concat(result, save_alist);

	return result;
}

static bool
isStrTypeColumn(ColumnDef *columnDef)
{
    char *typeName;
    if (list_length(columnDef->typeName->names) == 1)
    {
        typeName = strVal(linitial(columnDef->typeName->names));
    }
    else if (list_length(columnDef->typeName->names) == 2)
    {
        typeName = strVal(lsecond(columnDef->typeName->names));
    }
    else
    {
        return false;
    }

    if ((strncasecmp(typeName, "char", 4) == 0) ||
        (strncasecmp(typeName, "bpchar", 6) == 0) ||
        (strncasecmp(typeName, "varchar", 7) == 0) ||
        (strncasecmp(typeName, "tinytext", 8) == 0) ||
        (strncasecmp(typeName, "mediumtext", 10) == 0) ||
        (strncasecmp(typeName, "text", 4) == 0) ||
        (strncasecmp(typeName, "longtext", 8) == 0))
    {
        return true;
    }
    else
    {
        return false;
    }
}

static bool
isTableCollationCaseInsensitive(List *tabOptions)
{
    if (tabOptions != NIL)
    {
        ListCell *lc;
        foreach (lc, tabOptions)
        {
            DefElem *de = (DefElem *) lfirst(lc);
            if (strncasecmp(de->defname, "collate", 7) == 0)
            {
                List *collateNames = (List *)de->arg;
                char *collateName = strVal(linitial(collateNames));
                int collateNameLen = strlen(collateName);
                if ((3 < collateNameLen) &&
                    (strncasecmp((collateName + (collateNameLen - 3)), "_ci", 3) == 0))
                {
                    return true;
                }
                else
                {
                    return false;
                }
            }
        }
        return true;
    }
    else
    {
        return true;
    }
}

static void
setColumnCollateToCaseInsensitive(ColumnDef *columnDef)
{
    CollateClause *collateClause;
    collateClause = makeNode(CollateClause);
    collateClause->arg = NULL;
    collateClause->collname = list_make1(makeString(pstrdup("case_insensitive")));
    collateClause->location = -1;
    columnDef->collClause = collateClause;
}

static void
rectifySpecifiedColumnCollate(ColumnDef *columnDef)
{
    char *collateName = strVal(linitial(columnDef->collClause->collname));
    int collateNameLen = strlen(collateName);
    if ((3 < collateNameLen) &&
        (strncasecmp((collateName + (collateNameLen - 3)), "_ci", 3) == 0))
    {
        setColumnCollateToCaseInsensitive(columnDef);
    }
    else
    {
        columnDef->collClause = NULL;
    }
}

static void
rectifyColumnCollate(List *tabOptions, ColumnDef *columnDef)
{
    if (isStrTypeColumn(columnDef))
    {
        if (columnDef->collClause == NULL)
        {
            if (isTableCollationCaseInsensitive(tabOptions))
            {
                setColumnCollateToCaseInsensitive(columnDef);
            }
            else
            {
                /* do nothing; */
            }
        }
        else
        {
            rectifySpecifiedColumnCollate(columnDef);
        }
    }
}

static void
rectifyColumnsCollate(CreateStmt *stmt)
{
    ListCell *lc;
    foreach (lc, stmt->tableElts)
    {
        Node *node = lfirst(lc);
        if (IsA(node, ColumnDef))
        {
            ColumnDef *cd = castNode(ColumnDef, node);
            rectifyColumnCollate(stmt->options, cd);
        }
    }
}

static void
rectifyColumnCollateForAlter(ColumnDef *columnDef)
{
    if (isStrTypeColumn(columnDef))
    {
        if (columnDef->collClause == NULL)
        {
            setColumnCollateToCaseInsensitive(columnDef);
        }
        else
        {
            rectifySpecifiedColumnCollate(columnDef);
        }
    }
}

//static void 
//rectifyCollate(CreateStmt *stmt)
//{
//    int tableCollationVal;
//    if (stmt->options == NIL)
//    {
//        tableCollationVal = 0;
//    }
//    else 
//    {
//        ListCell *lc;
//
//        tableCollationVal = 0;
//        foreach (lc, stmt->options)
//        {
//            DefElem *de = (DefElem *) lfirst(lc);
//            if (strncasecmp(de->defname, "collate", 7) == 0)
//            {
//                List *collateNames = (List *)de->arg;
//                char *collateName = strVal(linitial(collateNames));
//                int collateNameLen = strlen(collateName);
//                if (strncasecmp((collateName + (collateNameLen - 3)), "_ci", 3) == 0)
//                {
//                    tableCollationVal = 1;
//                }
//                else 
//                {
//                    tableCollationVal = 2;
//                }
//                break;
//            }
//        }
//    }
//    if (tableCollationVal == 0)
//    {
//        ListCell *lc;
//        foreach (lc, stmt->tableElts)
//        {
//            Node *node = lfirst(lc);
//            if (IsA(node, ColumnDef))
//            {
//                ColumnDef *cd = castNode(ColumnDef, node);
//                char *typeName;
//                if (list_length(cd->typeName->names) == 1)
//                {
//                    typeName = strVal(linitial(cd->typeName->names));
//                }
//                else if (list_length(cd->typeName->names) == 2)
//                {
//                    typeName = strVal(lsecond(cd->typeName->names));
//                }
//                else 
//                {
//                    break;
//                }
//                if ((strncasecmp(typeName, "char", 4) == 0) || 
//                    (strncasecmp(typeName, "bpchar", 6) == 0) || 
//                    (strncasecmp(typeName, "varchar", 7) == 0) || 
//                    (strncasecmp(typeName, "tinytext", 8) == 0) || 
//                    (strncasecmp(typeName, "mediumtext", 10) == 0) || 
//                    (strncasecmp(typeName, "text", 4) == 0) || 
//                    (strncasecmp(typeName, "longtext", 8) == 0))
//                {
//                    if (cd->collClause == NULL)
//                    {
//                        CollateClause *collateClause;
//                        collateClause = makeNode(CollateClause);
//                        collateClause->arg = NULL;
//                        collateClause->collname = list_make1(makeString(pstrdup("case_insensitive")));
//                        collateClause->location = -1;
//                        cd->collClause = collateClause;
//                    }
//                    else
//                    {
//                        char *collateName = strVal(linitial(cd->collClause->collname));
//                        if (strncasecmp(collateName, "case_insensitive", 16) == 0)
//                        {
//                            /* do nothing; */
//                        }
//                        else
//                        {
//                            cd->collClause = NULL;
//                        }
//                    }
//                }
//            }
//        }
//    }
//    else if (tableCollationVal == 1)
//    {
//        ListCell *lc;
//        foreach (lc, stmt->tableElts)
//        {
//            Node *node = lfirst(lc);
//            if (IsA(node, ColumnDef))
//            {
//                ColumnDef *cd = castNode(ColumnDef, node);
//                char *typeName;
//                if (list_length(cd->typeName->names) == 1)
//                {
//                    typeName = strVal(linitial(cd->typeName->names));
//                }
//                else if (list_length(cd->typeName->names) == 2)
//                {
//                    typeName = strVal(lsecond(cd->typeName->names));
//                }
//                else 
//                {
//                    break;
//                }
//                if ((strncasecmp(typeName, "char", 4) == 0) || 
//                    (strncasecmp(typeName, "bpchar", 6) == 0) || 
//                    (strncasecmp(typeName, "varchar", 7) == 0) || 
//                    (strncasecmp(typeName, "tinytext", 8) == 0) || 
//                    (strncasecmp(typeName, "mediumtext", 10) == 0) || 
//                    (strncasecmp(typeName, "text", 4) == 0) || 
//                    (strncasecmp(typeName, "longtext", 8) == 0))
//                {
//                    if (cd->collClause == NULL)
//                    {
//                        CollateClause *collateClause;
//                        collateClause = makeNode(CollateClause);
//                        collateClause->arg = NULL;
//                        collateClause->collname = list_make1(makeString(pstrdup("case_insensitive")));
//                        collateClause->location = -1;
//                        cd->collClause = collateClause;
//                    }
//                    else
//                    {
//                        char *collateName = strVal(linitial(cd->collClause->collname));
//                        if (strncasecmp(collateName, "case_insensitive", 16) == 0)
//                        {
//                            /* do nothing; */
//                        }
//                        else
//                        {
//                            cd->collClause = NULL;
//                        }
//                    }
//                }
//            }
//        }
//    }
//    else 
//    {
//        ListCell *lc;
//        foreach (lc, stmt->tableElts)
//        {
//            Node *node = lfirst(lc);
//            if (IsA(node, ColumnDef))
//            {
//                ColumnDef *cd = castNode(ColumnDef, node);
//                char *typeName;
//                if (list_length(cd->typeName->names) == 1)
//                {
//                    typeName = strVal(linitial(cd->typeName->names));
//                }
//                else if (list_length(cd->typeName->names) == 2)
//                {
//                    typeName = strVal(lsecond(cd->typeName->names));
//                }
//                else 
//                {
//                    break;
//                }
//                if ((strncasecmp(typeName, "char", 4) == 0) || 
//                    (strncasecmp(typeName, "bpchar", 6) == 0) || 
//                    (strncasecmp(typeName, "varchar", 7) == 0) || 
//                    (strncasecmp(typeName, "tinytext", 8) == 0) || 
//                    (strncasecmp(typeName, "mediumtext", 10) == 0) || 
//                    (strncasecmp(typeName, "text", 4) == 0) || 
//                    (strncasecmp(typeName, "longtext", 8) == 0))
//                {
//                    if (cd->collClause == NULL)
//                    {
//                        /* do nothing; */
//                    }
//                    else
//                    {
//                        char *collateName = strVal(linitial(cd->collClause->collname));
//                        if (strncasecmp(collateName, "case_insensitive", 16) == 0)
//                        {
//                            /* do nothing; */
//                        }
//                        else
//                        {
//                            cd->collClause = NULL;
//                        }
//                    }
//                }
//            }
//        }
//    }
//}

static void
MysProcessAutoIncrement(CreateStmtContext *cxt)
{
    if (cxt->AutoIncrement == NIL)
    {
        return;
    }
    else if (list_length(cxt->AutoIncrement) > 1)
    {
        elog(ERROR,
             "Incorrect table definition; there can be only one auto increment column");
    }
    else if (linitial_int(cxt->AutoIncrement) != list_length(cxt->columns) - 1)
    {
        // 自增属性不属于当前处理的字段，跳过
    }
    else
    {
        char *snamespace;
		char *sname;
        char *qstring;
		A_Const *snamenode;
        List *seqOptions;
		TypeCast *castnode;
		FuncCall *funccallnode;
        ColumnDef *autoIncrementColumn = 
            (ColumnDef *)(list_nth_cell(cxt->columns, 
                                        linitial_int(cxt->AutoIncrement))->ptr_value);
        char *typName = strVal(llast(autoIncrementColumn->typeName->names));

        if (autoIncrementColumn->raw_default == NULL)
        {
            /* OK */
        }
        else if (IsA(autoIncrementColumn->raw_default, A_Const) && 
				 ((A_Const *)autoIncrementColumn->raw_default)->val.type == T_Null)
        {
            /* OK */
        }
        else
        {
            elog(ERROR, "Invalid default value for \'%s\'", autoIncrementColumn->colname);
        }

        if (strncmp(typName, "int2", 4) == 0)
        {
            autoIncrementColumn->typeName->names = NIL;
			autoIncrementColumn->typeName->typeOid = INT2OID;
        }
        else if (strncmp(typName, "int4", 4) == 0)
        {
            autoIncrementColumn->typeName->names = NIL;
			autoIncrementColumn->typeName->typeOid = INT4OID;
        }
        else if (strncmp(typName, "int8", 4) == 0)
        {
            autoIncrementColumn->typeName->names = NIL;
			autoIncrementColumn->typeName->typeOid = INT8OID;
        }
        else if ((strncmp(typName, "int unsigned", 12) == 0) ||
                 (strncmp(typName, "int signed", 10) == 0) ||
                 (strncmp(typName, "tinyint", 7) == 0) ||
                 (strncmp(typName, "tinyint signed", 14) == 0) ||
                 (strncmp(typName, "tinyint unsigned", 16) == 0) ||
                 (strncmp(typName, "smallint", 8) == 0) ||
                 (strncmp(typName, "smallint signed", 15) == 0) ||
                 (strncmp(typName, "smallint unsigned", 17) == 0) ||
                 (strncmp(typName, "mediumint", 9) == 0) ||
                 (strncmp(typName, "mediumint signed", 16) == 0) ||
                 (strncmp(typName, "mediumint unsigned", 18) == 0) || 
                 (strncmp(typName, "bigint signed", 13) == 0) ||
                 (strncmp(typName, "bigint unsigned", 15) == 0))
        {
            // autoIncrementColumn->typeName->names = NIL;
            // autoIncrementColumn->typeName->typeOid = INT8OID;
        }
        else
        {
            elog(ERROR,
                 "Incorrect column specifier for column '%s'", 
                 autoIncrementColumn->colname);
        }

        seqOptions = NIL;
        if (cxt->mysTableOption && cxt->mysTableOption->auto_increment != 0)
        {
            seqOptions = 
                lappend(seqOptions, 
                        makeDefElem("start", 
                                    cxt->mysTableOption->auto_increment, 
                                    -1));
        }

        generateSerialExtraStmts(cxt, 
                                 autoIncrementColumn,
								 autoIncrementColumn->typeName->typeOid, 
                                 seqOptions,
								 false, 
                                 false,
								 &snamespace, 
                                 &sname);
        
        qstring = quote_qualified_identifier(snamespace, sname);
		snamenode = makeNode(A_Const);
		snamenode->val.type = T_String;
		snamenode->val.val.str = qstring;
		snamenode->location = -1;
		castnode = makeNode(TypeCast);
		castnode->typeName = SystemTypeName("regclass");
		castnode->arg = (Node *) snamenode;
		castnode->location = -1;
		funccallnode = makeFuncCall(SystemFuncName("nextval"),
									list_make1(castnode),
									COERCE_EXPLICIT_CALL,
									-1);
		autoIncrementColumn->raw_default = (Node *) funccallnode;
        //autoIncrementColumn->is_not_null = true;
    }
}

static List *
MysProcessTableOption(CreateStmtContext *cxt, List *options)
{
    MysTableOption *tableOption;
    ListCell *lc;

    if (options == NIL)
    {
        return NIL;
    }

    if (cxt->mysTableOption == NULL)
    {
        tableOption = palloc0(sizeof(MysTableOption));
        cxt->mysTableOption = tableOption;
    }
    else
    {
        tableOption = cxt->mysTableOption;
    }

    foreach(lc, options)
    {
        DefElem *option = (DefElem *)lfirst(lc);

        if (strcmp(option->defname, "auto_increment") == 0)
        {
            tableOption->auto_increment = option->arg;
        }
        else if (strcmp(option->defname, "comment") == 0)
        {
            CommentStmt *cs = makeNode(CommentStmt);
            RangeVar *relation = cxt->relation;

            if (relation->catalogname != NULL)
            {
                cs->object = (Node *) list_make3(makeString(relation->catalogname), 
                                                 makeString(relation->schemaname), 
                                                 makeString(relation->relname));
            }
            else if (relation->schemaname != NULL)
            {
                cs->object = (Node *) list_make2(makeString(relation->schemaname), 
                                                 makeString(relation->relname));
            }
            else
            {
                cs->object = (Node *)list_make1(makeString(relation->relname));
            }

            cs->objtype = OBJECT_TABLE;
            cs->comment = strVal(option->arg);

            cxt->alist = lappend(cxt->alist, cs);
        }

        options = foreach_delete_current(options, lc);
    }

    return options;
}

static void
MysProcessOnUpdateNow(CreateStmtContext *cxt, ColumnDef *colDef)
{
    CreateFunctionStmt *createFuncStmt;
    CreateTrigStmt *createTrigStmt;
    AlterObjectDependsStmt *alterObjectDependsStmt1;
    AlterObjectDependsStmt *alterObjectDependsStmt2;

    RangeVar *relation;
    Oid namespaceid;
    char *namespace;

    relation = cxt->relation;
    namespaceid = RangeVarGetCreationNamespace(relation);
    namespace = get_namespace_name(namespaceid);
    RangeVarAdjustRelationPersistence(relation, namespaceid);

    createFuncStmt = createAutoUpdateTimeStampTriggerFunc(namespace, 
                                                          relation->relname, 
                                                          colDef->colname);
    cxt->alist = lappend(cxt->alist, createFuncStmt);

    createTrigStmt = createAutoUpdateTimeStampTrigger(namespace, 
                                                      relation->relname, 
                                                      colDef->colname);
    cxt->alist = lappend(cxt->alist, createTrigStmt);

    alterObjectDependsStmt1 = bindTriggerFunctionToTrigger(createFuncStmt->funcname, 
                                                           createTrigStmt->relation, 
                                                           createTrigStmt->trigname);
    cxt->alist = lappend(cxt->alist, alterObjectDependsStmt1);

    alterObjectDependsStmt2 = bindTriggerToColumn(createTrigStmt->relation, 
                                                  createTrigStmt->trigname, 
                                                  colDef->colname);

    cxt->alist = lappend(cxt->alist, alterObjectDependsStmt2);
}

/*
 * generateSerialExtraStmts
 *		Generate CREATE SEQUENCE and ALTER SEQUENCE ... OWNED BY statements
 *		to create the sequence for a serial or identity column.
 *
 * This includes determining the name the sequence will have.  The caller
 * can ask to get back the name components by passing non-null pointers
 * for snamespace_p and sname_p.
 */
static void
generateSerialExtraStmts(CreateStmtContext *cxt, ColumnDef *column,
						 Oid seqtypid, List *seqoptions,
						 bool for_identity, bool col_exists,
						 char **snamespace_p, char **sname_p)
{
	ListCell   *option;
	DefElem    *nameEl = NULL;
	Oid			snamespaceid;
	char	   *snamespace;
    char       *relName;
	char	   *sname;
	CreateSeqStmt *seqstmt;
	AlterSeqStmt *altseqstmt;
    Oid seqstmtOwnerId;
	int			nameEl_idx = -1;

    CreateFunctionStmt *funcStmt;
    CreateTrigStmt *trigStmt;
    AlterObjectDependsStmt *alterDependStmt1;
    AlterObjectDependsStmt *alterDependStmt2;

	/*
	 * Determine namespace and name to use for the sequence.
	 *
	 * First, check if a sequence name was passed in as an option.  This is
	 * used by pg_dump.  Else, generate a name.
	 *
	 * Although we use ChooseRelationName, it's not guaranteed that the
	 * selected sequence name won't conflict; given sufficiently long field
	 * names, two different serial columns in the same table could be assigned
	 * the same sequence name, and we'd not notice since we aren't creating
	 * the sequence quite yet.  In practice this seems quite unlikely to be a
	 * problem, especially since few people would need two serial columns in
	 * one table.
	 */
	foreach(option, seqoptions)
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "sequence_name") == 0)
		{
			if (nameEl)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			nameEl = defel;
			nameEl_idx = foreach_current_index(option);
		}
	}

	if (nameEl)
	{
		RangeVar   *rv = makeRangeVarFromNameList(castNode(List, nameEl->arg));

		snamespace = rv->schemaname;
        relName = rv->relname;
		if (!snamespace)
		{
			/* Given unqualified SEQUENCE NAME, select namespace */
			if (cxt->rel)
				snamespaceid = RelationGetNamespace(cxt->rel);
			else
				snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
			snamespace = get_namespace_name(snamespaceid);
		}
		sname = rv->relname;
		/* Remove the SEQUENCE NAME item from seqoptions */
		seqoptions = list_delete_nth_cell(seqoptions, nameEl_idx);
	}
	else
	{
		if (cxt->rel)
        {
			snamespaceid = RelationGetNamespace(cxt->rel);
            relName = RelationGetRelationName(cxt->rel);
        }
		else
		{
			snamespaceid = RangeVarGetCreationNamespace(cxt->relation);
            relName = cxt->relation->relname;
			RangeVarAdjustRelationPersistence(cxt->relation, snamespaceid);
		}
		snamespace = get_namespace_name(snamespaceid);
        sname = mysBuildSeqName(cxt->relation->relname);
		/* sname = ChooseRelationName(cxt->relation->relname, */
		/*                            column->colname, */
		/*                            "seq", */
		/*                            snamespaceid, */
		/*                            false); */
	}

	ereport(DEBUG1,
			(errmsg_internal("%s will create implicit sequence \"%s\" for serial column \"%s.%s\"",
							 cxt->stmtType, sname,
							 cxt->relation->relname, column->colname)));

	/*
	 * Build a CREATE SEQUENCE command to create the sequence object, and add
	 * it to the list of things to be done before this CREATE/ALTER TABLE.
	 */
	//seqstmt = makeNode(CreateSeqStmt);
	//seqstmt->for_identity = for_identity;
	//seqstmt->sequence = makeRangeVar(snamespace, sname, -1);
	//seqstmt->options = seqoptions;

    /*
	 * If a sequence data type was specified, add it to the options.  Prepend
	 * to the list rather than append; in case a user supplied their own AS
	 * clause, the "redundant options" error will point to their occurrence,
	 * not our synthetic one.
	 */
	//if (seqtypid)
	//	seqstmt->options = lcons(makeDefElem("as",
	//										 (Node *) makeTypeNameFromOid(seqtypid, -1),
	//										 -1),
	//							 seqstmt->options);

	/*
	 * If this is ALTER ADD COLUMN, make sure the sequence will be owned by
	 * the table's owner.  The current user might be someone else (perhaps a
	 * superuser, or someone who's only a member of the owning role), but the
	 * SEQUENCE OWNED BY mechanisms will bleat unless table and sequence have
	 * exactly the same owning role.
	 */
	if (cxt->rel)
		seqstmtOwnerId = cxt->rel->rd_rel->relowner;
	else
		seqstmtOwnerId = InvalidOid;

    seqstmt = createSeq(snamespace, relName, seqoptions, 
                        seqtypid, seqstmtOwnerId);

	cxt->blist = lappend(cxt->blist, seqstmt);

	/*
	 * Store the identity sequence name that we decided on.  ALTER TABLE ...
	 * ADD COLUMN ... IDENTITY needs this so that it can fill the new column
	 * with values from the sequence, while the association of the sequence
	 * with the table is not set until after the ALTER TABLE.
	 */
	column->identitySequence = seqstmt->sequence;

	/*
	 * Build an ALTER SEQUENCE ... OWNED BY command to mark the sequence as
	 * owned by this column, and add it to the appropriate list of things to
	 * be done along with this CREATE/ALTER TABLE.  In a CREATE or ALTER ADD
	 * COLUMN, it must be done after the statement because we don't know the
	 * column's attnum yet.  But if we do have the attnum (in AT_AddIdentity),
	 * we can do the marking immediately, which improves some ALTER TABLE
	 * behaviors.
	 */
	//altseqstmt = makeNode(AlterSeqStmt);
	//altseqstmt->sequence = makeRangeVar(snamespace, sname, -1);
	//attnamelist = list_make3(makeString(snamespace),
	//						 makeString(cxt->relation->relname),
	//						 makeString(column->colname));
	//altseqstmt->options = list_make1(makeDefElem("owned_by",
	//											 (Node *) attnamelist, -1));
	//altseqstmt->for_identity = for_identity;
    altseqstmt = createAlterSeq(snamespace, relName, column->colname);
	if (col_exists)
		cxt->blist = lappend(cxt->blist, altseqstmt);
	else
		cxt->alist = lappend(cxt->alist, altseqstmt);

    funcStmt = createAutoIncrementTriggerFunc(snamespace, relName, column->colname);
    cxt->alist = lappend(cxt->alist, funcStmt);

    trigStmt = createAutoIncrementTrigger(snamespace, relName);
    cxt->alist = lappend(cxt->alist, trigStmt);

    alterDependStmt1 = bindTriggerFunctionToTrigger(funcStmt->funcname, 
                                                    trigStmt->relation, 
                                                    trigStmt->trigname);
    cxt->alist = lappend(cxt->alist, alterDependStmt1);

    alterDependStmt2 = bindTriggerToSeq(trigStmt->relation, 
                                        trigStmt->trigname, 
                                        snamespace, 
                                        seqstmt->sequence->relname);
    cxt->alist = lappend(cxt->alist, alterDependStmt2);

	if (snamespace_p)
		*snamespace_p = snamespace;
	if (sname_p)
		*sname_p = sname;
}


AlterObjectDependsStmt *
bindTriggerToSeq(RangeVar *relation, char *trigName, 
                 char *nameSpace, char *seqName)
{
    AlterObjectDependsStmt *alterDependStmt;

    alterDependStmt = makeNode(AlterObjectDependsStmt);
    alterDependStmt->objectType = OBJECT_TRIGGER;
    alterDependStmt->relation = copyObject(relation);
    alterDependStmt->object = (Node *)list_make1(makeString(trigName));
    alterDependStmt->refObjectType = OBJECT_SEQUENCE;
    alterDependStmt->refRelation = NULL;
    alterDependStmt->refObject = (Node *)list_make2(makeString(nameSpace), 
                                                    makeString(seqName));
    alterDependStmt->deptype = DEPENDENCY_INTERNAL;
    alterDependStmt->extname = NULL;
    alterDependStmt->remove = false;
    
    return alterDependStmt;
}


AlterObjectDependsStmt *
bindTriggerFunctionToTrigger(List *funcName, RangeVar *relation, char *trigName)
{
    AlterObjectDependsStmt *alterDependStmt;
    ObjectWithArgs *objectOfAlterDependStmt;

    objectOfAlterDependStmt = makeNode(ObjectWithArgs);
    objectOfAlterDependStmt->objname = copyObject(funcName);
    objectOfAlterDependStmt->objargs = NIL;

    alterDependStmt = makeNode(AlterObjectDependsStmt);
    alterDependStmt->objectType = OBJECT_FUNCTION;
    alterDependStmt->relation = NULL;
    alterDependStmt->object = (Node *)objectOfAlterDependStmt;
    alterDependStmt->refObjectType = OBJECT_TRIGGER;
    alterDependStmt->refRelation = copyObject(relation);
    alterDependStmt->refObject = (Node *)list_make1(makeString(pstrdup(trigName)));
    alterDependStmt->deptype = DEPENDENCY_AUTO;
    alterDependStmt->extname = NULL;
    alterDependStmt->remove = false;
    
    return alterDependStmt;
}


AlterObjectDependsStmt *
bindTriggerToColumn(RangeVar *relation, char *trigName, char *colName)
{
    AlterObjectDependsStmt *altObjDepStmt;

    altObjDepStmt = makeNode(AlterObjectDependsStmt);
    altObjDepStmt->objectType = OBJECT_TRIGGER;
    altObjDepStmt->relation = copyObject(relation);
    altObjDepStmt->object = (Node *)list_make1(makeString(trigName));
    altObjDepStmt->refObjectType = OBJECT_COLUMN;
    altObjDepStmt->refRelation = copyObject(relation);
    altObjDepStmt->refObject = (Node *)list_make1(makeString(pstrdup(colName)));
    altObjDepStmt->deptype = DEPENDENCY_INTERNAL;
    altObjDepStmt->extname = NULL;
    altObjDepStmt->remove = false;

    return altObjDepStmt;
}


static char *
ChooseEnumTypeName(const char *prefix, const char *name1, const char *name2, Oid namespaceid)
{
    int	pass = 0;
	char *enumTypName = NULL;
	char modlabel[NAMEDATALEN];

	/* try the unmodified label first */
	strlcpy(modlabel, name2, sizeof(modlabel));

	for (;;)
	{
		enumTypName = makeObjectName(prefix, name1, modlabel);

		if (!OidIsValid(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
                                        PointerGetDatum(enumTypName),
                                        ObjectIdGetDatum(namespaceid))))
		{
			break;
		}

		/* found a conflict, so try a new name component */
		pfree(enumTypName);
		snprintf(modlabel, sizeof(modlabel), "%s%d", name2, ++pass);
	}

	return enumTypName;
}

static void
generateEnumExtraStmts(CreateStmtContext *cxt, ColumnDef *column)
{
    ListCell *lc;
    Oid newTypeNamespaceOid;
    char *newTypeNamespace;
    char *newTypeName;
    CreateEnumStmt *createEnum;
    List *createEnumVals;
    AlterObjectDependsStmt *alterTypeDepend;
    TypeName *alterTypeDependObj;
    RangeVar *relation = cxt->relation;

    newTypeNamespaceOid = RangeVarGetCreationNamespace(relation);
    RangeVarAdjustRelationPersistence(relation, newTypeNamespaceOid);
    newTypeNamespace = get_namespace_name(newTypeNamespaceOid);
    newTypeName = ChooseEnumTypeName("enum", relation->relname, 
                                     column->colname, 
                                     newTypeNamespaceOid);

    createEnumVals = NIL;
    foreach(lc, column->typeName->typmods)
    {
        Node *n = (Node *)lfirst(lc);
        
        if (IsA(n, A_Const) && ((A_Const *)n)->val.type == T_String)
        {
            char *val = pstrdup(((A_Const *)n)->val.val.str);
            createEnumVals = lappend(createEnumVals, makeString(val));
        }
        else
        {
            ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("vals of enum can only be string"),
					 parser_errposition(cxt->pstate,
										column->typeName->location)));
        }
    }

    createEnum = makeNode(CreateEnumStmt);
    createEnum->typeName = list_make2(makeString(pstrdup(newTypeNamespace)), 
                                      makeString(pstrdup(newTypeName)));
    createEnum->vals = createEnumVals;

    alterTypeDepend = makeNode(AlterObjectDependsStmt);
    alterTypeDepend->objectType = OBJECT_TYPE;
    alterTypeDepend->relation = NULL;
    alterTypeDependObj = makeNode(TypeName);
    alterTypeDependObj->names = list_make2(makeString(pstrdup(newTypeNamespace)), 
                                           makeString(pstrdup(newTypeName)));
    alterTypeDepend->object = (Node *)alterTypeDependObj;
    alterTypeDepend->refObjectType = OBJECT_COLUMN;
    alterTypeDepend->refRelation = NULL;
    alterTypeDepend->refObject = (Node *)list_make3(makeString(pstrdup(newTypeNamespace)), 
                                                    makeString(pstrdup(relation->relname)), 
                                                    makeString(pstrdup(column->colname)));
    alterTypeDepend->extname = NULL;
    alterTypeDepend->remove = false;

    column->typeName->names = list_make2(makeString(pstrdup(newTypeNamespace)), 
                                         makeString(pstrdup(newTypeName)));
    column->typeName->typmods = NIL;

    cxt->blist = lappend(cxt->blist, createEnum);
    cxt->alist = lappend(cxt->alist, alterTypeDepend);
}

static void
generateSetExtraStmts(CreateStmtContext *cxt, ColumnDef *column)
{
    Oid newTypeNamespaceOid;
    char *newTypeNamespace;
    char *newDomainName;
    CreateDomainStmt *createSet;
    A_Const *varcharLen;
    Constraint *checkCons;
    FuncCall *funcCall;
    A_Const *setItems;
    ColumnRef *value;
    char origSetDef[1024];
    int origSetDefOffset;
    ListCell *lc;
    AlterObjectDependsStmt *alterTypeDepend;
    TypeName *alterTypeDependObj;
    RangeVar *relation = cxt->relation;

    newTypeNamespaceOid = RangeVarGetCreationNamespace(relation);
    RangeVarAdjustRelationPersistence(relation, newTypeNamespaceOid);
    newTypeNamespace = get_namespace_name(newTypeNamespaceOid);
    newDomainName = mysBuildSetDomainName(relation->relname, column->colname);

    createSet = makeNode(CreateDomainStmt);
    createSet->domainname = list_make2(makeString(pstrdup(newTypeNamespace)), 
                                       makeString(pstrdup(newDomainName)));
    createSet->typeName = makeTypeNameFromNameList(list_make2(makeString(pstrdup("pg_catalog")), 
                                                              makeString(pstrdup("varchar"))));
    varcharLen = makeNode(A_Const);
    varcharLen->val.type = T_Integer;
    varcharLen->val.val.ival = 128;    // varchar(128)
    createSet->typeName->typmods = list_make1(varcharLen);
    createSet->typeName->arrayBounds = NIL;
    checkCons = makeNode(Constraint);
    checkCons->contype = CONSTR_CHECK;
    checkCons->conname = pstrdup("halo_set_check");
    origSetDefOffset = 0;
    foreach(lc, column->typeName->typmods)
    {
        Node *n = (Node *)lfirst(lc);
        if (IsA(n, A_Const) && ((A_Const *)n)->val.type == T_String)
        {
            char *val = pstrdup(((A_Const *)n)->val.val.str);
            int valLen = strlen(val);
            memcpy((origSetDef + origSetDefOffset), val, valLen);
            origSetDefOffset += valLen;
            memcpy((origSetDef + origSetDefOffset), ",", 1);
            origSetDefOffset += 1;
            if (924 < origSetDefOffset)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("len of set content can not longger than 924"),
                         parser_errposition(cxt->pstate,
                                            column->typeName->location)));
            }      
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("vals of set can only be string"),
                     parser_errposition(cxt->pstate,
                                        column->typeName->location)));
        }
    }
    origSetDef[origSetDefOffset - 1] = '\0';
    setItems = makeNode(A_Const);
    setItems->val.type = T_String;
    setItems->val.val.str = pstrdup(origSetDef);
    setItems->location = -1;
    value = makeNode(ColumnRef);
    value->fields = list_make1(makeString(pstrdup("value")));
    value->location = -1;
    funcCall = makeFuncCall(list_make1(makeString("check_set")), 
                            list_make2(setItems, value), 
                            COERCE_EXPLICIT_CALL, 
                            -1);
    checkCons->raw_expr = (Node*)funcCall;
    createSet->constraints = lappend(createSet->constraints, checkCons);
    cxt->blist = lappend(cxt->blist, createSet);

    column->typeName->names = list_make2(makeString(pstrdup(newTypeNamespace)), 
                                         makeString(pstrdup(newDomainName)));
    column->typeName->typmods = NIL;

    alterTypeDepend = makeNode(AlterObjectDependsStmt);
    alterTypeDepend->objectType = OBJECT_TYPE;
    alterTypeDepend->relation = NULL;
    alterTypeDependObj = makeNode(TypeName);
    alterTypeDependObj->names = list_make2(makeString(pstrdup(newTypeNamespace)), 
                                           makeString(pstrdup(newDomainName)));
    alterTypeDepend->object = (Node *)alterTypeDependObj;
    alterTypeDepend->refObjectType = OBJECT_COLUMN;
    alterTypeDepend->refRelation = NULL;
    alterTypeDepend->refObject = (Node *)list_make3(makeString(pstrdup(newTypeNamespace)), 
                                                    makeString(pstrdup(relation->relname)), 
                                                    makeString(pstrdup(column->colname)));
    alterTypeDepend->extname = NULL;
    alterTypeDepend->remove = false;
    cxt->alist = lappend(cxt->alist, alterTypeDepend);
}

/*
 * MysqlPreProcessType -
 *      处理Mysql的enum, set, year类型(处理的字段是llast(cxt->columns))
 * 
 * return true: 字段类型需要新建
 * 
 */
static bool
MysqlPreProcessType(CreateStmtContext *cxt, ColumnDef *column)
{
    bool haveMysSpecialType = false;

    if (column->typeName
        && list_length(column->typeName->names) == 1
        && !column->typeName->pct_type)
    {
        char *typname = strVal(linitial(column->typeName->names));

        if (strcmp(typname, "enum") == 0)
        {
            haveMysSpecialType = true;
            generateEnumExtraStmts(cxt, column);
        }
        else if (strcmp(typname, "set") == 0)
        {
            haveMysSpecialType = true;
            generateSetExtraStmts(cxt, column);
        }
    }

    return haveMysSpecialType;
}


static List *
mysPreProcessConstraintAttrs(CreateStmtContext *cxt, List *constraintList, bool *sawOnUpdateNow)
{
    ListCell *clist;
    ColumnDef *cd = (ColumnDef *)llast(cxt->columns);

    foreach(clist, constraintList)
	{
        Node *n = (Node *) lfirst(clist);

        if (IsA(n, CommentStmt))
        {
            Oid namespaceOid;
            char *namespace;
            CommentStmt *userDefColComent = (CommentStmt *) n;

            namespaceOid = RangeVarGetCreationNamespace(cxt->relation);
            RangeVarAdjustRelationPersistence(cxt->relation, namespaceOid);
            namespace = get_namespace_name(namespaceOid);

            userDefColComent->object = (Node *)list_make3(makeString(namespace), 
                                                          makeString(cxt->relation->relname),
                                                          makeString(cd->colname));

            cxt->alist = lappend(cxt->alist, userDefColComent);

            constraintList = foreach_delete_current(constraintList, clist);
            continue;
        }
        else if (IsA(n, String))
        {
            if (strncmp(strVal(n), "on update current_timestamp", 27) == 0)
            {
                int len1 = (int)strtoul(strVal(n) + 28, NULL, 10);

                // create table test6(id int, update_time timestamp on update now());
                // datetime类型映射导致的错误
                if ((strncasecmp(strVal(llast(cd->typeName->names)), "timestamp", 9) == 0) ||
                    (strncasecmp(strVal(llast(cd->typeName->names)), "datetime", 8) == 0))
                {
                    if (cd->typeName->typmods != NULL)
                    {
                        A_Const *len2;

                        Assert(IsA(linitial(cd->typeName->typmods), A_Const));

                        len2 = (A_Const *) linitial(cd->typeName->typmods);

                        Assert(nodeTag(&(len2->val)) == T_Integer);

                        if (len1 != len2->val.val.ival)
                            ereport(ERROR,
                                    (errcode(ERRCODE_SYNTAX_ERROR),
                                     errmsg("Invalid ON UPDATE clause for '%s' column", cd->colname),
                                     parser_errposition(cxt->pstate, cd->location)));
                    }
                }
                else
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_SYNTAX_ERROR),
                             errmsg("Invalid ON UPDATE clause for '%s' column", cd->colname),
                             parser_errposition(cxt->pstate, cd->location)));
                }

                *sawOnUpdateNow = true;
            }
            else
            {
                elog(ERROR, "unrecognized node type: %d",
				     (int) nodeTag(n));
            }

            constraintList = foreach_delete_current(constraintList, clist);
            continue;
        }
    }

    return constraintList;
}


/*
 * transformColumnDefinition -
 *		transform a single ColumnDef within CREATE TABLE
 *		Also used in ALTER TABLE ADD COLUMN
 */
static void
transformColumnDefinition(CreateStmtContext *cxt, ColumnDef *column)
{
	bool		is_serial;
    bool        is_mys_special_type;
	bool		saw_nullable;
	bool		saw_default;
	bool		saw_identity;
	bool		saw_generated;
    bool sawOnUpdateNow;
	ListCell   *clist;
    ExplicitColumnAttr *explicitColumnAttr;

    explicitColumnAttr = palloc0(sizeof(ExplicitColumnAttr));
	cxt->columns = lappend(cxt->columns, column);
    cxt->explicitColumnsAttr = lappend(cxt->explicitColumnsAttr, explicitColumnAttr);

    /* Check for enum, set, year pseudo-types */
    is_mys_special_type = false;
    is_mys_special_type = MysqlPreProcessType(cxt, column);

	/* Check for SERIAL pseudo-types */
	is_serial = false;
	if (column->typeName
		&& list_length(column->typeName->names) == 1
		&& !column->typeName->pct_type)
	{
		char	   *typname = strVal(linitial(column->typeName->names));

		if (strcmp(typname, "smallserial") == 0 ||
			strcmp(typname, "serial2") == 0)
		{
			is_serial = true;
			column->typeName->names = list_make2(makeString("pg_catalog"), makeString("int2"));
			column->typeName->typeOid = INT2OID;
		}
		else if (strcmp(typname, "serial") == 0 ||
				 strcmp(typname, "serial4") == 0)
		{
			is_serial = true;
			column->typeName->names = list_make2(makeString("pg_catalog"), makeString("int4"));;
			column->typeName->typeOid = INT4OID;
		}
		else if (strcmp(typname, "bigserial") == 0 ||
				 strcmp(typname, "serial8") == 0)
		{
			is_serial = true;
			column->typeName->names = list_make2(makeString("pg_catalog"), makeString("int8"));;
			column->typeName->typeOid = INT8OID;
		}

		/*
		 * We have to reject "serial[]" explicitly, because once we've set
		 * typeid, LookupTypeName won't notice arrayBounds.  We don't need any
		 * special coding for serial(typmod) though.
		 */
		if (is_serial && column->typeName->arrayBounds != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array of serial is not implemented"),
					 parser_errposition(cxt->pstate,
										column->typeName->location)));
	}

	/* Do necessary work on the column type declaration */
    if (is_mys_special_type)
    {
		/* OK */
    }
    else
    {
        if (column->typeName)
            transformColumnType(cxt, column);
    }

	/* Special actions for SERIAL pseudo-types */
	if (is_serial)
	{
        if (cxt->AutoIncrement)
        {
            elog(ERROR,
                 "Incorrect table definition; there can be only one auto increment column");
        }
        else
        {
            cxt->AutoIncrement = lappend_int(cxt->AutoIncrement, list_length(cxt->columns) - 1);
        }
	}

	/* Process column constraints, if any... */
    sawOnUpdateNow = false;
    column->constraints = mysPreProcessConstraintAttrs(cxt, column->constraints, &sawOnUpdateNow);
    if (sawOnUpdateNow)
    {
        MysProcessOnUpdateNow(cxt, column);
    }

	transformConstraintAttrs(cxt, column->constraints);

	saw_nullable = false;
	saw_default = false;
	saw_identity = false;
	saw_generated = false;

	foreach(clist, column->constraints)
	{
		Constraint *constraint = lfirst_node(Constraint, clist);

		switch (constraint->contype)
		{
			case CONSTR_NULL:
				column->is_not_null = false;
                explicitColumnAttr->hasNull = true;
				saw_nullable = true;
				break;

			case CONSTR_NOTNULL:
				column->is_not_null = true;
                explicitColumnAttr->hasNotNull = true;
				saw_nullable = true;
				break;
            
            case CONSTR_AUTOINC:
                column->is_not_null = true;

                if (cxt->AutoIncrement)
                {
                    elog(ERROR,
                         "Incorrect table definition; there can be only one auto increment column");
                }
                else
                {
                    cxt->AutoIncrement = lappend_int(cxt->AutoIncrement, list_length(cxt->columns) - 1);
                }
                break;

			case CONSTR_DEFAULT:
				if (saw_default)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default values specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_default = true;
				break;

			case CONSTR_IDENTITY:
				{
					Type		ctype;
					Oid			typeOid;

					if (cxt->ofType)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on typed tables")));
					if (cxt->partbound)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on partitions")));

					ctype = typenameType(cxt->pstate, column->typeName, NULL);
					typeOid = ((Form_pg_type) GETSTRUCT(ctype))->oid;
					ReleaseSysCache(ctype);

					if (saw_identity)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple identity specifications for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));

					generateSerialExtraStmts(cxt, column,
											 typeOid, constraint->options,
											 true, false,
											 NULL, NULL);

					column->identity = constraint->generated_when;
					saw_identity = true;

					/* An identity column is implicitly NOT NULL */
					if (saw_nullable && !column->is_not_null)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));
					column->is_not_null = true;
					saw_nullable = true;
					break;
				}

			case CONSTR_GENERATED:
				if (cxt->ofType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("generated columns are not supported on typed tables")));
				if (cxt->partbound)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("generated columns are not supported on partitions")));

				if (saw_generated)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple generation clauses specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->generated = ATTRIBUTE_GENERATED_STORED;
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_generated = true;
				break;

			case CONSTR_CHECK:
				cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
				break;

			case CONSTR_PRIMARY:
                column->is_not_null = true;
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("primary key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				/* FALL THRU */

			case CONSTR_UNIQUE:
                if (cxt->isforeign)
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("unique constraints are not supported on foreign tables"),
                            parser_errposition(cxt->pstate,
                                                constraint->location)));
                if (constraint->keys == NIL)
                {
                    IndexElem *indexElem = makeNode(IndexElem);
                    indexElem->name = column->colname;
                    indexElem->expr = NULL;
                    indexElem->indexcolname = NULL;
                    indexElem->collation = NIL;
                    indexElem->opclass = NIL;
                    indexElem->opclassopts = NIL;
                    indexElem->ordering = SORTBY_DEFAULT;
                    indexElem->nulls_ordering = SORTBY_NULLS_DEFAULT;

                    constraint->keys = list_make1(indexElem);
                }
                cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
                break;

			case CONSTR_EXCLUSION:
				/* grammar does not allow EXCLUDE as a column constraint */
				elog(ERROR, "column exclusion constraints are not supported");
				break;

			case CONSTR_FOREIGN:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));

				/*
				 * Fill in the current attribute's name and throw it into the
				 * list of FK constraints to be processed later.
				 */
				constraint->fk_attrs = list_make1(makeString(column->colname));
				cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				/* transformConstraintAttrs took care of these */
				break;

			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 constraint->contype);
				break;
		}

		if (saw_default && saw_identity)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and identity specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_default && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_identity && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both identity and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));
	}

    MysProcessAutoIncrement(cxt);

	/*
	 * If needed, generate ALTER FOREIGN TABLE ALTER COLUMN statement to add
	 * per-column foreign data wrapper options to this column after creation.
	 */
	if (column->fdwoptions != NIL)
	{
		AlterTableStmt *stmt;
		AlterTableCmd *cmd;

		cmd = makeNode(AlterTableCmd);
		cmd->subtype = AT_AlterColumnGenericOptions;
		cmd->name = column->colname;
		cmd->def = (Node *) column->fdwoptions;
		cmd->behavior = DROP_RESTRICT;
		cmd->missing_ok = false;

		stmt = makeNode(AlterTableStmt);
		stmt->relation = cxt->relation;
		stmt->cmds = NIL;
		stmt->objtype = OBJECT_FOREIGN_TABLE;
		stmt->cmds = lappend(stmt->cmds, cmd);

		cxt->alist = lappend(cxt->alist, stmt);
	}
}

/*
 * transformTableConstraint
 *		transform a Constraint node within CREATE TABLE or ALTER TABLE
 */
static void
transformTableConstraint(CreateStmtContext *cxt, Constraint *constraint)
{
	switch (constraint->contype)
	{
        case CONSTR_KEY:
            if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;
		case CONSTR_PRIMARY:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("primary key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_UNIQUE:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unique constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_EXCLUSION:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("exclusion constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			if (cxt->ispartitioned)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("exclusion constraints are not supported on partitioned tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
			break;

		case CONSTR_CHECK:
			cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
			break;

		case CONSTR_FOREIGN:
			if (cxt->isforeign)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("foreign key constraints are not supported on foreign tables"),
						 parser_errposition(cxt->pstate,
											constraint->location)));
			cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
			break;

		case CONSTR_NULL:
		case CONSTR_NOTNULL:
		case CONSTR_DEFAULT:
		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			elog(ERROR, "invalid context for constraint type %d",
				 constraint->contype);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 constraint->contype);
			break;
	}
}

/*
 * transformTableLikeClause
 *
 * Change the LIKE <srctable> portion of a CREATE TABLE statement into
 * column definitions that recreate the user defined column portions of
 * <srctable>.  Also, if there are any LIKE options that we can't fully
 * process at this point, add the TableLikeClause to cxt->likeclauses, which
 * will cause utility.c to call expandTableLikeClause() after the new
 * table has been created.
 */
static void
transformTableLikeClause(CreateStmtContext *cxt, TableLikeClause *table_like_clause)
{
	AttrNumber	parent_attno;
	Relation	relation;
	TupleDesc	tupleDesc;
	AclResult	aclresult;
	char	   *comment;
	ParseCallbackState pcbstate;

	setup_parser_errposition_callback(&pcbstate, cxt->pstate,
									  table_like_clause->relation->location);

	/* we could support LIKE in many cases, but worry about it another day */
	if (cxt->isforeign)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LIKE is not supported for creating foreign tables")));

	/* Open the relation referenced by the LIKE clause */
	relation = relation_openrv(table_like_clause->relation, AccessShareLock);

	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_VIEW &&
		relation->rd_rel->relkind != RELKIND_MATVIEW &&
		relation->rd_rel->relkind != RELKIND_COMPOSITE_TYPE &&
		relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, view, materialized view, composite type, or foreign table",
						RelationGetRelationName(relation))));

	cancel_parser_errposition_callback(&pcbstate);

	/*
	 * Check for privileges
	 */
	if (relation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
	{
		aclresult = pg_type_aclcheck(relation->rd_rel->reltype, GetUserId(),
									 ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TYPE,
						   RelationGetRelationName(relation));
	}
	else
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(relation), GetUserId(),
									  ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(relation->rd_rel->relkind),
						   RelationGetRelationName(relation));
	}

	tupleDesc = RelationGetDescr(relation);

	/*
	 * Insert the copied attributes into the cxt for the new table definition.
	 * We must do this now so that they appear in the table in the relative
	 * position where the LIKE clause is, as required by SQL99.
	 */
	for (parent_attno = 1; parent_attno <= tupleDesc->natts;
		 parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
													parent_attno - 1);
		char	   *attributeName = NameStr(attribute->attname);
		ColumnDef  *def;

		/*
		 * Ignore dropped columns in the parent.
		 */
		if (attribute->attisdropped)
			continue;

		/*
		 * Create a new column, which is marked as NOT inherited.
		 *
		 * For constraints, ONLY the NOT NULL constraint is inherited by the
		 * new column definition per SQL99.
		 */
		def = makeNode(ColumnDef);
		def->colname = pstrdup(attributeName);
		def->typeName = makeTypeNameFromOid(attribute->atttypid,
											attribute->atttypmod);
		def->inhcount = 0;
		def->is_local = true;
		def->is_not_null = attribute->attnotnull;
		def->is_from_type = false;
		def->storage = 0;
		def->raw_default = NULL;
		def->cooked_default = NULL;
		def->collClause = NULL;
		def->collOid = attribute->attcollation;
		def->constraints = NIL;
		def->location = -1;

		/*
		 * Add to column list
		 */
		cxt->columns = lappend(cxt->columns, def);

		/*
		 * Although we don't transfer the column's default/generation
		 * expression now, we need to mark it GENERATED if appropriate.
		 */
		if (attribute->atthasdef && attribute->attgenerated &&
			(table_like_clause->options & CREATE_TABLE_LIKE_GENERATED))
			def->generated = attribute->attgenerated;

		/*
		 * Copy identity if requested
		 */
		if (attribute->attidentity &&
			(table_like_clause->options & CREATE_TABLE_LIKE_IDENTITY))
		{
			Oid			seq_relid;
			List	   *seq_options;

			/*
			 * find sequence owned by old column; extract sequence parameters;
			 * build new create sequence command
			 */
			seq_relid = getIdentitySequence(RelationGetRelid(relation), attribute->attnum, false);
			seq_options = sequence_options(seq_relid);
			generateSerialExtraStmts(cxt, def,
									 InvalidOid, seq_options,
									 true, false,
									 NULL, NULL);
			def->identity = attribute->attidentity;
		}

		/* Likewise, copy storage if requested */
		if (table_like_clause->options & CREATE_TABLE_LIKE_STORAGE)
			def->storage = attribute->attstorage;
		else
			def->storage = 0;

		/* Likewise, copy compression if requested */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMPRESSION) != 0
			&& CompressionMethodIsValid(attribute->attcompression))
			def->compression =
				pstrdup(GetCompressionMethodName(attribute->attcompression));
		else
			def->compression = NULL;

		/* Likewise, copy comment if requested */
		if ((table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS) &&
			(comment = GetComment(attribute->attrelid,
								  RelationRelationId,
								  attribute->attnum)) != NULL)
		{
			CommentStmt *stmt = makeNode(CommentStmt);

			stmt->objtype = OBJECT_COLUMN;
			stmt->object = (Node *) list_make3(makeString(cxt->relation->schemaname),
											   makeString(cxt->relation->relname),
											   makeString(def->colname));
			stmt->comment = comment;

			cxt->alist = lappend(cxt->alist, stmt);
		}
	}

	/*
	 * We cannot yet deal with defaults, CHECK constraints, or indexes, since
	 * we don't yet know what column numbers the copied columns will have in
	 * the finished table.  If any of those options are specified, add the
	 * LIKE clause to cxt->likeclauses so that expandTableLikeClause will be
	 * called after we do know that.  Also, remember the relation OID so that
	 * expandTableLikeClause is certain to open the same table.
	 */
	if (table_like_clause->options &
		(CREATE_TABLE_LIKE_DEFAULTS |
		 CREATE_TABLE_LIKE_GENERATED |
		 CREATE_TABLE_LIKE_CONSTRAINTS |
		 CREATE_TABLE_LIKE_INDEXES))
	{
		table_like_clause->relationOid = RelationGetRelid(relation);
		cxt->likeclauses = lappend(cxt->likeclauses, table_like_clause);
	}

	/*
	 * We may copy extended statistics if requested, since the representation
	 * of CreateStatsStmt doesn't depend on column numbers.
	 */
	if (table_like_clause->options & CREATE_TABLE_LIKE_STATISTICS)
	{
		List	   *parent_extstats;
		ListCell   *l;

		parent_extstats = RelationGetStatExtList(relation);

		foreach(l, parent_extstats)
		{
			Oid			parent_stat_oid = lfirst_oid(l);
			CreateStatsStmt *stats_stmt;

			stats_stmt = generateClonedExtStatsStmt(cxt->relation,
													RelationGetRelid(relation),
													parent_stat_oid);

			/* Copy comment on statistics object, if requested */
			if (table_like_clause->options & CREATE_TABLE_LIKE_COMMENTS)
			{
				comment = GetComment(parent_stat_oid, StatisticExtRelationId, 0);

				/*
				 * We make use of CreateStatsStmt's stxcomment option, so as
				 * not to need to know now what name the statistics will have.
				 */
				stats_stmt->stxcomment = comment;
			}

			cxt->extstats = lappend(cxt->extstats, stats_stmt);
		}

		list_free(parent_extstats);
	}

	/*
	 * Close the parent rel, but keep our AccessShareLock on it until xact
	 * commit.  That will prevent someone else from deleting or ALTERing the
	 * parent before we can run expandTableLikeClause.
	 */
	table_close(relation, NoLock);
}


static void
transformOfType(CreateStmtContext *cxt, TypeName *ofTypename)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			i;
	Oid			ofTypeId;

	AssertArg(ofTypename);

	tuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(tuple);
	ofTypeId = ((Form_pg_type) GETSTRUCT(tuple))->oid;
	ofTypename->typeOid = ofTypeId; /* cached for later */

	tupdesc = lookup_rowtype_tupdesc(ofTypeId, -1);
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		ColumnDef  *n;

		if (attr->attisdropped)
			continue;

		n = makeNode(ColumnDef);
		n->colname = pstrdup(NameStr(attr->attname));
		n->typeName = makeTypeNameFromOid(attr->atttypid, attr->atttypmod);
		n->inhcount = 0;
		n->is_local = true;
		n->is_not_null = false;
		n->is_from_type = true;
		n->storage = 0;
		n->raw_default = NULL;
		n->cooked_default = NULL;
		n->collClause = NULL;
		n->collOid = attr->attcollation;
		n->constraints = NIL;
		n->location = -1;
		cxt->columns = lappend(cxt->columns, n);
	}
	DecrTupleDescRefCount(tupdesc);

	ReleaseSysCache(tuple);
}


/*
 * Generate a CreateStatsStmt node using information from an already existing
 * extended statistic "source_statsid", for the rel identified by heapRel and
 * heapRelid.
 */
static CreateStatsStmt *
generateClonedExtStatsStmt(RangeVar *heapRel, Oid heapRelid,
						   Oid source_statsid)
{
	HeapTuple	ht_stats;
	Form_pg_statistic_ext statsrec;
	CreateStatsStmt *stats;
	List	   *stat_types = NIL;
	List	   *def_names = NIL;
	bool		isnull;
	Datum		datum;
	ArrayType  *arr;
	char	   *enabled;
	int			i;

	Assert(OidIsValid(heapRelid));
	Assert(heapRel != NULL);

	/*
	 * Fetch pg_statistic_ext tuple of source statistics object.
	 */
	ht_stats = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(source_statsid));
	if (!HeapTupleIsValid(ht_stats))
		elog(ERROR, "cache lookup failed for statistics object %u", source_statsid);
	statsrec = (Form_pg_statistic_ext) GETSTRUCT(ht_stats);

	/* Determine which statistics types exist */
	datum = SysCacheGetAttr(STATEXTOID, ht_stats,
							Anum_pg_statistic_ext_stxkind, &isnull);
	Assert(!isnull);
	arr = DatumGetArrayTypeP(datum);
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != CHAROID)
		elog(ERROR, "stxkind is not a 1-D char array");
	enabled = (char *) ARR_DATA_PTR(arr);
	for (i = 0; i < ARR_DIMS(arr)[0]; i++)
	{
		if (enabled[i] == STATS_EXT_NDISTINCT)
			stat_types = lappend(stat_types, makeString("ndistinct"));
		else if (enabled[i] == STATS_EXT_DEPENDENCIES)
			stat_types = lappend(stat_types, makeString("dependencies"));
		else if (enabled[i] == STATS_EXT_MCV)
			stat_types = lappend(stat_types, makeString("mcv"));
		else if (enabled[i] == STATS_EXT_EXPRESSIONS)
			/* expression stats are not exposed to users */
			continue;
		else
			elog(ERROR, "unrecognized statistics kind %c", enabled[i]);
	}

	/* Determine which columns the statistics are on */
	for (i = 0; i < statsrec->stxkeys.dim1; i++)
	{
		StatsElem  *selem = makeNode(StatsElem);
		AttrNumber	attnum = statsrec->stxkeys.values[i];

		selem->name = get_attname(heapRelid, attnum, false);
		selem->expr = NULL;

		def_names = lappend(def_names, selem);
	}

	/*
	 * Now handle expressions, if there are any. The order (with respect to
	 * regular attributes) does not really matter for extended stats, so we
	 * simply append them after simple column references.
	 *
	 * XXX Some places during build/estimation treat expressions as if they
	 * are before attributes, but for the CREATE command that's entirely
	 * irrelevant.
	 */
	datum = SysCacheGetAttr(STATEXTOID, ht_stats,
							Anum_pg_statistic_ext_stxexprs, &isnull);

	if (!isnull)
	{
		ListCell   *lc;
		List	   *exprs = NIL;
		char	   *exprsString;

		exprsString = TextDatumGetCString(datum);
		exprs = (List *) stringToNode(exprsString);

		foreach(lc, exprs)
		{
			StatsElem  *selem = makeNode(StatsElem);

			selem->name = NULL;
			selem->expr = (Node *) lfirst(lc);

			def_names = lappend(def_names, selem);
		}

		pfree(exprsString);
	}

	/* finally, build the output node */
	stats = makeNode(CreateStatsStmt);
	stats->defnames = NULL;
	stats->stat_types = stat_types;
	stats->exprs = def_names;
	stats->relations = list_make1(heapRel);
	stats->stxcomment = NULL;
	stats->transformed = true;	/* don't need transformStatsStmt again */
	stats->if_not_exists = false;

	/* Clean up */
	ReleaseSysCache(ht_stats);

	return stats;
}


/*
 * transformIndexConstraints
 *		Handle UNIQUE, PRIMARY KEY, EXCLUDE constraints, which create indexes.
 *		We also merge in any index definitions arising from
 *		LIKE ... INCLUDING INDEXES.
 */
static void
transformIndexConstraints(CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *indexlist = NIL;
	List	   *finalindexlist = NIL;
	ListCell   *lc;

	/*
	 * Run through the constraints that need to generate an index. For PRIMARY
	 * KEY, mark each column as NOT NULL and create an index. For UNIQUE or
	 * EXCLUDE, create an index as for PRIMARY KEY, but do not insist on NOT
	 * NULL.
	 */
	foreach(lc, cxt->ixconstraints)
	{
		Constraint *constraint = lfirst_node(Constraint, lc);

		Assert(constraint->contype == CONSTR_KEY ||
               constraint->contype == CONSTR_PRIMARY ||
			   constraint->contype == CONSTR_UNIQUE ||
			   constraint->contype == CONSTR_EXCLUSION);

		index = transformIndexConstraint(constraint, cxt);

		indexlist = lappend(indexlist, index);
	}

    /*
     * 下面这几行代码，用于检查自增序列必须首先是索引或主键的前提，
     * 这个前提是MySQL内核的要求，
     * 在Halo内核中，并不需要这个前提，
     * 同时下面这几行代码，会导致psql下建表语句中无法直接使用“id bigserial”,
     * 为了方便DBA操作，将下面几行代码注释掉
     */
    // if ((cxt->AutoIncrement == NIL) || (list_length(cxt->AutoIncrement) == 1 && cxt->AutoIncrementIsIndex))
    // {
    //     // do nothing;
    // }
    // else
    // {
    //     elog(ERROR,
    //          "Incorrect table definition; the auto increment column must be defined as a key");
    // }

	/*
	 * Scan the index list and remove any redundant index specifications. This
	 * can happen if, for instance, the user writes UNIQUE PRIMARY KEY. A
	 * strict reading of SQL would suggest raising an error instead, but that
	 * strikes me as too anal-retentive. - tgl 2001-02-14
	 *
	 * XXX in ALTER TABLE case, it'd be nice to look for duplicate
	 * pre-existing indexes, too.
	 */
	if (cxt->pkey != NULL)
	{
		/* Make sure we keep the PKEY index in preference to others... */
		finalindexlist = list_make1(cxt->pkey);
	}

	foreach(lc, indexlist)
	{
		bool		keep = true;
		ListCell   *k;

		index = lfirst(lc);

		/* if it's pkey, it's already in finalindexlist */
		if (index == cxt->pkey)
			continue;

		foreach(k, finalindexlist)
		{
			IndexStmt  *priorindex = lfirst(k);

			if (equal(index->indexParams, priorindex->indexParams) &&
				equal(index->indexIncludingParams, priorindex->indexIncludingParams) &&
				equal(index->whereClause, priorindex->whereClause) &&
				equal(index->excludeOpNames, priorindex->excludeOpNames) &&
				strcmp(index->accessMethod, priorindex->accessMethod) == 0 &&
				index->deferrable == priorindex->deferrable &&
				index->initdeferred == priorindex->initdeferred)
			{
				priorindex->unique |= index->unique;

				/*
				 * If the prior index is as yet unnamed, and this one is
				 * named, then transfer the name to the prior index. This
				 * ensures that if we have named and unnamed constraints,
				 * we'll use (at least one of) the names for the index.
				 */
				if (priorindex->idxname == NULL)
					priorindex->idxname = index->idxname;
				keep = false;
				break;
			}
		}

		if (keep)
			finalindexlist = lappend(finalindexlist, index);
	}

	/*
	 * Now append all the IndexStmts to cxt->alist.  If we generated an ALTER
	 * TABLE SET NOT NULL statement to support a primary key, it's already in
	 * cxt->alist.
	 */
	cxt->alist = list_concat(cxt->alist, finalindexlist);
}

/*
 * transformIndexConstraint
 *		Transform one UNIQUE, PRIMARY KEY, or EXCLUDE constraint for
 *		transformIndexConstraints.
 *
 * We return an IndexStmt.  For a PRIMARY KEY constraint, we additionally
 * produce NOT NULL constraints, either by marking ColumnDefs in cxt->columns
 * as is_not_null or by adding an ALTER TABLE SET NOT NULL command to
 * cxt->alist.
 */
static IndexStmt *
transformIndexConstraint(Constraint *constraint, CreateStmtContext *cxt)
{
	IndexStmt  *index;
	List	   *notnullcmds = NIL;
	ListCell   *lc;

	index = makeNode(IndexStmt);

    if (constraint->contype == CONSTR_KEY)
    {
        index->unique = false;
        index->primary = false;
    }
    else
    {
	    index->unique = (constraint->contype != CONSTR_EXCLUSION);
	    index->primary = (constraint->contype == CONSTR_PRIMARY);
    }

	if (index->primary)
	{
		if (cxt->pkey != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("multiple primary keys for table \"%s\" are not allowed",
							cxt->relation->relname),
					 parser_errposition(cxt->pstate, constraint->location)));
		cxt->pkey = index;

		/*
		 * In ALTER TABLE case, a primary index might already exist, but
		 * DefineIndex will check for it.
		 */
	}
	if (constraint->contype == CONSTR_KEY)
    {
        index->isconstraint = false;
    }
    else
    {
	    index->isconstraint = true;
    }
	index->deferrable = constraint->deferrable;
	index->initdeferred = constraint->initdeferred;

	if (constraint->conname != NULL)
        index->idxname = pstrdup(constraint->conname);
	else
		index->idxname = NULL;	/* DefineIndex will choose name */

	index->relation = cxt->relation;
	index->accessMethod = constraint->access_method ? constraint->access_method : DEFAULT_INDEX_TYPE;
	index->options = constraint->options;
	index->tableSpace = constraint->indexspace;
	index->whereClause = constraint->where_clause;
	index->indexParams = NIL;
	index->indexIncludingParams = NIL;
	index->excludeOpNames = NIL;
	index->idxcomment = NULL;
	index->indexOid = InvalidOid;
	index->oldNode = InvalidOid;
	index->oldCreateSubid = InvalidSubTransactionId;
	index->oldFirstRelfilenodeSubid = InvalidSubTransactionId;
	index->transformed = false;
	index->concurrent = false;
	index->if_not_exists = false;
	index->reset_default_tblspc = constraint->reset_default_tblspc;

	/*
	 * If it's an EXCLUDE constraint, the grammar returns a list of pairs of
	 * IndexElems and operator names.  We have to break that apart into
	 * separate lists.
	 */
	if (constraint->contype == CONSTR_EXCLUSION)
	{
		foreach(lc, constraint->exclusions)
		{
			List	   *pair = (List *) lfirst(lc);
			IndexElem  *elem;
			List	   *opname;

			Assert(list_length(pair) == 2);
			elem = linitial_node(IndexElem, pair);
			opname = lsecond_node(List, pair);

			index->indexParams = lappend(index->indexParams, elem);
			index->excludeOpNames = lappend(index->excludeOpNames, opname);
		}
	}

	/*
	 * For UNIQUE and PRIMARY KEY, we have a list of IndexElem Nodes.
	 *
	 * Make sure referenced keys exist.  If we are making a PRIMARY KEY index,
	 * also make sure they are NOT NULL.
	 */
	else
	{
		foreach(lc, constraint->keys)
		{
            IndexElem *indexParam = NULL;
            char	   *key = NULL;
			bool		found = false;
			bool		forced_not_null = false;
			ColumnDef  *column = NULL;
			ListCell   *columns = NULL;
            ExplicitColumnAttr *explicitColumnAttr = NULL;
            ListCell *lcExplicitColumnAttr = NULL;
			IndexElem  *iparam;

            indexParam = (IndexElem *)lfirst(lc);

            if (indexParam->expr)
            {
                index->indexParams = lappend(index->indexParams, indexParam);
                index->isconstraint = false;
            }
            else
            {
                key = indexParam->name;

                /* Make sure referenced column exists. */
                forboth(columns, cxt->columns, lcExplicitColumnAttr, cxt->explicitColumnsAttr)
                {
                    column = castNode(ColumnDef, lfirst(columns));
                    explicitColumnAttr = (ExplicitColumnAttr *) lfirst(lcExplicitColumnAttr);
                    if (strcmp(column->colname, key) == 0)
                    {
                        if (cxt->AutoIncrement && (columns__state.i == linitial_int(cxt->AutoIncrement)))
                        {
                            cxt->AutoIncrementIsIndex = true;
                        }
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    /*
                    * column is defined in the new table.  For PRIMARY KEY, we
                    * can apply the NOT NULL constraint cheaply here ... unless
                    * the column is marked is_from_type, in which case marking it
                    * here would be ineffective (see MergeAttributes).
                    */
                    if (constraint->contype == CONSTR_PRIMARY && explicitColumnAttr->hasNull)
                    {
                        elog(ERROR, "All parts of a PRIMARY KEY must be NOT NULL; if you need NULL in a key, use UNIQUE instead");
                    }
                    if (constraint->contype == CONSTR_PRIMARY &&
                        !column->is_from_type)
                    {
                        column->is_not_null = true;
                        forced_not_null = true;
                    }
                }
                else if (SystemAttributeByName(key) != NULL)
                {
                    /*
                    * column will be a system column in the new table, so accept
                    * it. System columns can't ever be null, so no need to worry
                    * about PRIMARY/NOT NULL constraint.
                    */
                    found = true;
                }
                else if (cxt->inhRelations)
                {
                    /* try inherited tables */
                    ListCell   *inher;

                    foreach(inher, cxt->inhRelations)
                    {
                        RangeVar   *inh = castNode(RangeVar, lfirst(inher));
                        Relation	rel;
                        int			count;

                        rel = table_openrv(inh, AccessShareLock);
                        /* check user requested inheritance from valid relkind */
                        if (rel->rd_rel->relkind != RELKIND_RELATION &&
                            rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
                            rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
                            ereport(ERROR,
                                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                                    errmsg("inherited relation \"%s\" is not a table or foreign table",
                                            inh->relname)));
                        for (count = 0; count < rel->rd_att->natts; count++)
                        {
                            Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att,
                                                                    count);
                            char	   *inhname = NameStr(inhattr->attname);

                            if (inhattr->attisdropped)
                                continue;
                            if (strcmp(key, inhname) == 0)
                            {
                                found = true;

                                /*
                                * It's tempting to set forced_not_null if the
                                * parent column is already NOT NULL, but that
                                * seems unsafe because the column's NOT NULL
                                * marking might disappear between now and
                                * execution.  Do the runtime check to be safe.
                                */
                                break;
                            }
                        }
                        table_close(rel, NoLock);
                        if (found)
                            break;
                    }
                }

                /*
                * In the ALTER TABLE case, don't complain about index keys not
                * created in the command; they may well exist already.
                * DefineIndex will complain about them if not.
                */
                if (!found && !cxt->isalter)
                    ereport(ERROR,
                            (errcode(ERRCODE_UNDEFINED_COLUMN),
                            errmsg("column \"%s\" named in key does not exist", key),
                            parser_errposition(cxt->pstate, constraint->location)));

                /* Check for PRIMARY KEY(foo, foo) */
                foreach(columns, index->indexParams)
                {
                    iparam = (IndexElem *) lfirst(columns);
                    if (iparam->name && strcmp(key, iparam->name) == 0)
                    {
                        if (index->primary)
                            ereport(ERROR,
                                    (errcode(ERRCODE_DUPLICATE_COLUMN),
                                    errmsg("column \"%s\" appears twice in primary key constraint",
                                            key),
                                    parser_errposition(cxt->pstate, constraint->location)));
                        else
                            ereport(ERROR,
                                    (errcode(ERRCODE_DUPLICATE_COLUMN),
                                    errmsg("column \"%s\" appears twice in unique constraint",
                                            key),
                                    parser_errposition(cxt->pstate, constraint->location)));
                    }
                }

                /* OK, add it to the index definition */
                index->indexParams = lappend(index->indexParams, indexParam);

                /*
                * For a primary-key column, also create an item for ALTER TABLE
                * SET NOT NULL if we couldn't ensure it via is_not_null above.
                */
                if (constraint->contype == CONSTR_PRIMARY && !forced_not_null)
                {
                    AlterTableCmd *notnullcmd = makeNode(AlterTableCmd);

                    notnullcmd->subtype = AT_SetNotNull;
                    notnullcmd->name = pstrdup(key);
                    notnullcmds = lappend(notnullcmds, notnullcmd);
                }
            }
		}
	}

	/*
	 * Add included columns to index definition.  This is much like the
	 * simple-column-name-list code above, except that we don't worry about
	 * NOT NULL marking; included columns in a primary key should not be
	 * forced NOT NULL.  We don't complain about duplicate columns, either,
	 * though maybe we should?
	 */
	foreach(lc, constraint->including)
	{
		char	   *key = strVal(lfirst(lc));
		bool		found = false;
		ColumnDef  *column = NULL;
		ListCell   *columns;
		IndexElem  *iparam;

		foreach(columns, cxt->columns)
		{
			column = lfirst_node(ColumnDef, columns);
			if (strcmp(column->colname, key) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			if (SystemAttributeByName(key) != NULL)
			{
				/*
				 * column will be a system column in the new table, so accept
				 * it.
				 */
				found = true;
			}
			else if (cxt->inhRelations)
			{
				/* try inherited tables */
				ListCell   *inher;

				foreach(inher, cxt->inhRelations)
				{
					RangeVar   *inh = lfirst_node(RangeVar, inher);
					Relation	rel;
					int			count;

					rel = table_openrv(inh, AccessShareLock);
					/* check user requested inheritance from valid relkind */
					if (rel->rd_rel->relkind != RELKIND_RELATION &&
						rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
						rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("inherited relation \"%s\" is not a table or foreign table",
										inh->relname)));
					for (count = 0; count < rel->rd_att->natts; count++)
					{
						Form_pg_attribute inhattr = TupleDescAttr(rel->rd_att,
																  count);
						char	   *inhname = NameStr(inhattr->attname);

						if (inhattr->attisdropped)
							continue;
						if (strcmp(key, inhname) == 0)
						{
							found = true;
							break;
						}
					}
					table_close(rel, NoLock);
					if (found)
						break;
				}
			}
		}

		/*
		 * In the ALTER TABLE case, don't complain about index keys not
		 * created in the command; they may well exist already. DefineIndex
		 * will complain about them if not.
		 */
		if (!found && !cxt->isalter)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" named in key does not exist", key),
					 parser_errposition(cxt->pstate, constraint->location)));

		/* OK, add it to the index definition */
		iparam = makeNode(IndexElem);
		iparam->name = pstrdup(key);
		iparam->expr = NULL;
		iparam->indexcolname = NULL;
		iparam->collation = NIL;
		iparam->opclass = NIL;
		iparam->opclassopts = NIL;
		index->indexIncludingParams = lappend(index->indexIncludingParams, iparam);
	}

	/*
	 * If we found anything that requires run-time SET NOT NULL, build a full
	 * ALTER TABLE command for that and add it to cxt->alist.
	 */
	if (notnullcmds)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = copyObject(cxt->relation);
		alterstmt->cmds = notnullcmds;
		alterstmt->objtype = OBJECT_TABLE;
		alterstmt->missing_ok = false;

		cxt->alist = lappend(cxt->alist, alterstmt);
	}

	return index;
}

/*
 * transformExtendedStatistics
 *     Handle extended statistic objects
 *
 * Right now, there's nothing to do here, so we just append the list to
 * the existing "after" list.
 */
static void
transformExtendedStatistics(CreateStmtContext *cxt)
{
	cxt->alist = list_concat(cxt->alist, cxt->extstats);
}

/*
 * transformCheckConstraints
 *		handle CHECK constraints
 *
 * Right now, there's nothing to do here when called from ALTER TABLE,
 * but the other constraint-transformation functions are called in both
 * the CREATE TABLE and ALTER TABLE paths, so do the same here, and just
 * don't do anything if we're not authorized to skip validation.
 */
static void
transformCheckConstraints(CreateStmtContext *cxt, bool skipValidation)
{
	ListCell   *ckclist;

	if (cxt->ckconstraints == NIL)
		return;

	/*
	 * If creating a new table (but not a foreign table), we can safely skip
	 * validation of check constraints, and nonetheless mark them valid. (This
	 * will override any user-supplied NOT VALID flag.)
	 */
	if (skipValidation)
	{
		foreach(ckclist, cxt->ckconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(ckclist);

			constraint->skip_validation = true;
			constraint->initially_valid = true;
		}
	}
}

/*
 * transformFKConstraints
 *		handle FOREIGN KEY constraints
 */
static void
transformFKConstraints(CreateStmtContext *cxt,
					   bool skipValidation, bool isAddConstraint)
{
	ListCell   *fkclist;

	if (cxt->fkconstraints == NIL)
		return;

	/*
	 * If CREATE TABLE or adding a column with NULL default, we can safely
	 * skip validation of FK constraints, and nonetheless mark them valid.
	 * (This will override any user-supplied NOT VALID flag.)
	 */
	if (skipValidation)
	{
		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);

			constraint->skip_validation = true;
			constraint->initially_valid = true;
		}
	}

	/*
	 * For CREATE TABLE or ALTER TABLE ADD COLUMN, gin up an ALTER TABLE ADD
	 * CONSTRAINT command to execute after the basic command is complete. (If
	 * called from ADD CONSTRAINT, that routine will add the FK constraints to
	 * its own subcommand list.)
	 *
	 * Note: the ADD CONSTRAINT command must also execute after any index
	 * creation commands.  Thus, this should run after
	 * transformIndexConstraints, so that the CREATE INDEX commands are
	 * already in cxt->alist.  See also the handling of cxt->likeclauses.
	 */
	if (!isAddConstraint)
	{
		AlterTableStmt *alterstmt = makeNode(AlterTableStmt);

		alterstmt->relation = cxt->relation;
		alterstmt->cmds = NIL;
		alterstmt->objtype = OBJECT_TABLE;

		foreach(fkclist, cxt->fkconstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(fkclist);
			AlterTableCmd *altercmd = makeNode(AlterTableCmd);

			altercmd->subtype = AT_AddConstraint;
			altercmd->name = NULL;
			altercmd->def = (Node *) constraint;
			alterstmt->cmds = lappend(alterstmt->cmds, altercmd);
		}

		cxt->alist = lappend(cxt->alist, alterstmt);
	}
}


static void
transformModifyColumnConstraints(CreateStmtContext *cxt, char *oldColName, 
                                 ColumnDef *column, ExplicitColumnAttr *explicitColumnAttr)
{
    ListCell *clist;
    bool saw_nullable = false;
	bool saw_default = false;
	bool saw_identity = false;
	bool saw_generated = false;

    foreach(clist, column->constraints)
	{
		Constraint *constraint = lfirst_node(Constraint, clist);

		switch (constraint->contype)
		{
			case CONSTR_NULL:
				column->is_not_null = false;
                explicitColumnAttr->hasNull = true;
				saw_nullable = true;
				break;

			case CONSTR_NOTNULL:
				column->is_not_null = true;
                explicitColumnAttr->hasNotNull = true;
				saw_nullable = true;
				break;
            
            case CONSTR_AUTOINC:
                column->is_not_null = true;

                if (cxt->AutoIncrement)
                {
                    elog(ERROR,
                         "Incorrect table definition; there can be only one auto increment column");
                }
                else
                {
                    cxt->AutoIncrement = lappend_int(cxt->AutoIncrement, list_length(cxt->columns) - 1);
                }
                break;

			case CONSTR_DEFAULT:
				if (saw_default)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default values specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_default = true;
				break;

			case CONSTR_IDENTITY:
				{
					Type		ctype;
					Oid			typeOid;

					if (cxt->ofType)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on typed tables")));
					if (cxt->partbound)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("identity columns are not supported on partitions")));

					ctype = typenameType(cxt->pstate, column->typeName, NULL);
					typeOid = ((Form_pg_type) GETSTRUCT(ctype))->oid;
					ReleaseSysCache(ctype);

					if (saw_identity)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("multiple identity specifications for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));

					generateSerialExtraStmts(cxt, column,
											 typeOid, constraint->options,
											 true, false,
											 NULL, NULL);

					column->identity = constraint->generated_when;
					saw_identity = true;

					/* An identity column is implicitly NOT NULL */
					if (saw_nullable && !column->is_not_null)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("conflicting NULL/NOT NULL declarations for column \"%s\" of table \"%s\"",
										column->colname, cxt->relation->relname),
								 parser_errposition(cxt->pstate,
													constraint->location)));
					column->is_not_null = true;
					saw_nullable = true;
					break;
				}

			case CONSTR_GENERATED:
				if (cxt->ofType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("generated columns are not supported on typed tables")));
				if (cxt->partbound)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("generated columns are not supported on partitions")));

				if (saw_generated)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple generation clauses specified for column \"%s\" of table \"%s\"",
									column->colname, cxt->relation->relname),
							 parser_errposition(cxt->pstate,
												constraint->location)));
				column->generated = ATTRIBUTE_GENERATED_STORED;
				column->raw_default = constraint->raw_expr;
				Assert(constraint->cooked_expr == NULL);
				saw_generated = true;
				break;

			case CONSTR_CHECK:
				cxt->ckconstraints = lappend(cxt->ckconstraints, constraint);
				break;

			case CONSTR_PRIMARY:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("primary key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));
                
                column->is_not_null = true;
				/* FALL THRU */

			case CONSTR_UNIQUE:
                if (cxt->isforeign)
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("unique constraints are not supported on foreign tables"),
                            parser_errposition(cxt->pstate,
                                                constraint->location)));
                if (constraint->keys == NIL)
                {
                    IndexElem *indexElem = makeNode(IndexElem);
                    indexElem->name = column->colname;
                    indexElem->expr = NULL;
                    indexElem->indexcolname = NULL;
                    indexElem->collation = NIL;
                    indexElem->opclass = NIL;
                    indexElem->opclassopts = NIL;
                    indexElem->ordering = SORTBY_DEFAULT;
                    indexElem->nulls_ordering = SORTBY_NULLS_DEFAULT;

                    constraint->keys = list_make1(indexElem);
                }
                cxt->ixconstraints = lappend(cxt->ixconstraints, constraint);
                break;

			case CONSTR_EXCLUSION:
				/* grammar does not allow EXCLUDE as a column constraint */
				elog(ERROR, "column exclusion constraints are not supported");
				break;

			case CONSTR_FOREIGN:
				if (cxt->isforeign)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key constraints are not supported on foreign tables"),
							 parser_errposition(cxt->pstate,
												constraint->location)));

				/*
				 * Fill in the current attribute's name and throw it into the
				 * list of FK constraints to be processed later.
				 */
				constraint->fk_attrs = list_make1(makeString(column->colname));
				cxt->fkconstraints = lappend(cxt->fkconstraints, constraint);
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				/* transformConstraintAttrs took care of these */
				break;

			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 constraint->contype);
				break;
		}

		if (saw_default && saw_identity)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and identity specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_default && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both default and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));

		if (saw_identity && saw_generated)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("both identity and generation expression specified for column \"%s\" of table \"%s\"",
							column->colname, cxt->relation->relname),
					 parser_errposition(cxt->pstate,
										constraint->location)));
	}

    if (mysCheckPrimary(cxt->rel, oldColName))
    {
        column->is_not_null = true;
    }

    // if (cxt->AutoIncrement != NIL)
    // {
    //     if (saw_nullable)
    //     {
    //         if (!column->is_not_null)
    //         {
    //             // 自增列可为null, 表现与Mysql不一样(当没有显示的插入该字段的值时，Mysql的默认值为null或用户指定的其他值，Halo实现的默认值为nextval)
    //             elog (ERROR, "auto_increment column must be not null");
    //         }

    //         if (saw_default && column->is_not_null)
    //         {
    //             if (IsA(column->raw_default, A_Const) && ((A_Const *)column->raw_default)->val.type == T_Null)
    //             {

    //             }
    //             else
    //             {
    //                 elog(ERROR, "Invalid default value for \'%s\'", column->colname);
    //             }
    //         }
    //     }
    // }
}


// // 检查表中的某个字段是否是Index的一部分
// static bool
// mysCheckIndex(Relation rel, char *colname)
// {
//     HeapTuple tuple;
// 	Form_pg_attribute attTup;
// 	AttrNumber attnum;
//     List *indexoidlist;
// 	ListCell *indexoidscan;
//     bool isIndex = false;

//     tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colname);
// 	if (!HeapTupleIsValid(tuple))
// 		ereport(ERROR,
// 				(errcode(ERRCODE_UNDEFINED_COLUMN),
// 				 errmsg("column \"%s\" of relation \"%s\" does not exist",
// 						colname, RelationGetRelationName(rel))));
// 	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
// 	attnum = attTup->attnum;

// 	/* Prevent them from altering a system attribute */
// 	if (attnum <= 0)
// 		ereport(ERROR,
// 				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
// 				 errmsg("cannot alter system column \"%s\"",
// 						colname)));

// 	if (attTup->attidentity)
// 		ereport(ERROR,
// 				(errcode(ERRCODE_SYNTAX_ERROR),
// 				 errmsg("column \"%s\" of relation \"%s\" is an identity column",
// 						colname, RelationGetRelationName(rel))));

// 	/*
// 	 * Check that the attribute is not in a primary key or in an index used as
// 	 * a replica identity.
// 	 *
// 	 * Note: we'll throw error even if the pkey index is not valid.
// 	 */

// 	/* Loop over all indexes on the relation */
// 	indexoidlist = RelationGetIndexList(rel);

// 	foreach(indexoidscan, indexoidlist)
// 	{
// 		Oid			indexoid = lfirst_oid(indexoidscan);
// 		HeapTuple	indexTuple;
// 		Form_pg_index indexStruct;
// 		int			i;

// 		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
// 		if (!HeapTupleIsValid(indexTuple))
// 			elog(ERROR, "cache lookup failed for index %u", indexoid);
// 		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

//         /*
//             * Loop over each attribute in the index 
//             * and see if it matches the to-be-altered
//             * attribute.
//             */
//         for (i = 0; i < indexStruct->indnkeyatts; i++)
//         {
//             if (indexStruct->indkey.values[i] == attnum)
//             {
//                 isIndex = true;
//                 break;
//             }
//         }

// 		ReleaseSysCache(indexTuple);
// 	}

// 	list_free(indexoidlist);

//     return isIndex;
// }

// 检查表中的某个字段是否是primary key的一部分
static bool
mysCheckPrimary(Relation rel, char *colname)
{
    HeapTuple tuple;
	Form_pg_attribute attTup;
	AttrNumber attnum;
    List *indexoidlist;
	ListCell *indexoidscan;
    bool isPartOfPrimary = false;

    tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colname);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colname, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colname)));

	if (attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is an identity column",
						colname, RelationGetRelationName(rel))));

	/*
	 * Check that the attribute is not in a primary key or in an index used as
	 * a replica identity.
	 *
	 * Note: we'll throw error even if the pkey index is not valid.
	 */

	/* Loop over all indexes on the relation */
	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;
		int			i;

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * If the index is not a primary key or an index used as replica
		 * identity, skip the check.
		 */
		if (indexStruct->indisprimary || indexStruct->indisreplident)
		{
			/*
			 * Loop over each attribute in the primary key or the index used
			 * as replica identity and see if it matches the to-be-altered
			 * attribute.
			 */
			for (i = 0; i < indexStruct->indnkeyatts; i++)
			{
				if (indexStruct->indkey.values[i] == attnum)
				{
					if (indexStruct->indisprimary)
                    {
						isPartOfPrimary = true;
                        break;
                    }
				}
			}
		}

		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

    return isPartOfPrimary;
}

static char *
get_trigger_name(Oid trigOid)
{
    Relation trigRel;
    ScanKeyData trigKey;
    SysScanDesc trigScan;
    HeapTuple trigTuple;
    char *trigName = NULL;
    
    trigRel = table_open(TriggerRelationId, AccessShareLock);

    ScanKeyInit(&trigKey, Anum_pg_trigger_oid, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(trigOid));
    
    trigScan = systable_beginscan(trigRel, TriggerOidIndexId, true,
                                    NULL, 1, &trigKey);
    
    trigTuple = systable_getnext(trigScan);
    
    if (HeapTupleIsValid(trigTuple))
	{
		trigName = pstrdup(((Form_pg_trigger) GETSTRUCT(trigTuple))->tgname.data);
	}

    systable_endscan(trigScan);
	table_close(trigRel, AccessShareLock);

    return trigName;
}


static void
dropOnUpdateNowTrigDependWithColumn(Oid onUpdateNowTrigOid, Relation rel, char *colName)
{
    Relation depRel;
    ScanKeyData depKeys[2];
    SysScanDesc depScan;
    HeapTuple depTuple;

    depRel = table_open(DependRelationId, AccessExclusiveLock);

    ScanKeyInit(&depKeys[0],
                Anum_pg_depend_classid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(TriggerRelationId));
    ScanKeyInit(&depKeys[1],
                Anum_pg_depend_objid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(onUpdateNowTrigOid));
    
    depScan = systable_beginscan(depRel, DependDependerIndexId, true, NULL, 2, depKeys);

    while (HeapTupleIsValid(depTuple = systable_getnext(depScan)))
    {
        Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

        if (foundDep->refclassid == RelationRelationId &&
            foundDep->refobjid == RelationGetRelid(rel) &&
            foundDep->refobjsubid == get_attnum(RelationGetRelid(rel), colName) &&
            foundDep->deptype == DEPENDENCY_INTERNAL)
        {
            CatalogTupleDelete(depRel, &depTuple->t_self);
            break;
        }
    }

    systable_endscan(depScan);
    table_close(depRel, AccessExclusiveLock);
    CommandCounterIncrement();
}


static AttrNumber
mysGetTableAutoIncColNum(Relation rel)
{
    int i;
    // 字段号为0的字段不存在
    AttrNumber attnum = 0;
    TupleDesc columnsDesc = rel->rd_att;

    if (columnsDesc->constr == NULL || (columnsDesc->constr != NULL && columnsDesc->constr->num_defval <= 0))
        return attnum;
    
    for (i = 0; i < columnsDesc->constr->num_defval; i++)
    {
        AttrDefault attrDef = columnsDesc->constr->defval[i];
        Form_pg_attribute attrForm = &(columnsDesc->attrs[attrDef.adnum - 1]);
        
        if (getColumnDefaultSeq(rel, NameStr(attrForm->attname)))
        {
            if (attnum != 0)
            {
                // 不应该发生
                elog(ERROR, "Incorrect table definition; there can be only one auto increment column");
            }
            else
            {
                attnum = attrForm->attnum;
            }
        }
    }

    return attnum;
}


static void
mysDropOnUpdateNowForModifyColumn(CreateStmtContext *cxt, char *oldColName)
{
    Oid oldOnUpdateNowTrigOid;

    oldOnUpdateNowTrigOid = mysGetColumnOnUpdateNowTrig(cxt->rel, oldColName);
    
    if (OidIsValid(oldOnUpdateNowTrigOid))
    {
        DropStmt *dropTrig;

        dropOnUpdateNowTrigDependWithColumn(oldOnUpdateNowTrigOid, cxt->rel, oldColName);
        
        dropTrig = makeNode(DropStmt);
        dropTrig->removeType = OBJECT_TRIGGER;
        dropTrig->missing_ok = false;
        dropTrig->objects = list_make1(list_make3(makeString(get_namespace_name(get_rel_namespace(RelationGetRelid(cxt->rel)))), 
                                                  makeString(get_rel_name(RelationGetRelid(cxt->rel))),
                                                  makeString(get_trigger_name(oldOnUpdateNowTrigOid))));
        dropTrig->behavior = DROP_RESTRICT;
        dropTrig->concurrent = false;

        cxt->blist = lappend(cxt->blist, dropTrig);
    }
}


static void
mysDropAutoIncForModifyColumn(CreateStmtContext *cxt, char *oldColName)
{
    Oid oldSeqOid = getColumnDefaultSeq(cxt->rel, oldColName);
    if (OidIsValid(oldSeqOid))
    {
        DropStmt *dropSeq;

        dropSeq = makeNode(DropStmt);
        dropSeq->removeType = OBJECT_SEQUENCE;
        dropSeq->missing_ok = false;
        dropSeq->objects = list_make1(list_make2(makeString(get_namespace_name(get_rel_namespace(oldSeqOid))), makeString(get_rel_name(oldSeqOid))));
        dropSeq->behavior = DROP_RESTRICT;
        dropSeq->concurrent = false;

        cxt->alist = lappend(cxt->alist, dropSeq);
    }
}


static void
mysDropCommentForModifyColumn(CreateStmtContext *cxt, ColumnDef *column)
{
    CommentStmt *colComment;
    char *relNamespace = get_namespace_name(get_rel_namespace(cxt->rel->rd_id));
    char *relName = get_rel_name(cxt->rel->rd_id);
    
    colComment = makeNode(CommentStmt);
    colComment->object = (Node *)list_make3(makeString(relNamespace), makeString(relName), makeString(pstrdup(column->colname)));
    colComment->objtype = OBJECT_COLUMN;
    colComment->comment = NULL;

    cxt->alist = lappend(cxt->alist, colComment);
}


static void
mysProcessAutoIncForModifyColumn(CreateStmtContext *cxt, char *oldColName, ColumnDef *column)
{
    /* bool rename = (strcmp(oldColName, column->colname) != 0); */

    if (cxt->AutoIncrement)
    {
        AttrNumber attnum;
        Assert(list_length(cxt->AutoIncrement) == 1);

        // 检查目标表的其它字段是否已有自增列
        attnum = mysGetTableAutoIncColNum(cxt->rel);
        if (attnum > 0)
        {
            if (get_attnum(cxt->rel->rd_id, oldColName) != attnum)
            {
                elog(ERROR, "Incorrect table definition; there can be only one auto increment column");
            }
            else
            {
                // 该字段已是自增列，重新构造字段默认值(phase 2会将原有默认值删除)
                char *seqString;
                A_Const *seqNameNode;
                TypeCast *castNode;
                FuncCall *funcCallNode;
                Oid seqOid = getColumnDefaultSeq(cxt->rel, oldColName);
                char *seqNamespace = get_namespace_name(get_rel_namespace(seqOid));
                char *seqName = get_rel_name(seqOid);

                seqString = quote_qualified_identifier(seqNamespace, seqName);
                seqNameNode = makeNode(A_Const);
                seqNameNode->val.type = T_String;
                seqNameNode->val.val.str = seqString;
                seqNameNode->location = -1;
                castNode = makeNode(TypeCast);
                castNode->typeName = SystemTypeName("regclass");
                castNode->arg = (Node *) seqNameNode;
                castNode->location = -1;
                funcCallNode = makeFuncCall(SystemFuncName("nextval"),
                                            list_make1(castNode),
                                            COERCE_EXPLICIT_CALL,
                                            -1);
                
                column->raw_default = (Node *)funcCallNode;
                cxt->AutoIncrementIsIndex = true;

                /* if (rename) */
                {
                    List *stmts = NIL;

                    mysProcessAutoIncForRenameAtt(cxt->rel, oldColName, column->colname, &stmts);

                    cxt->alist = list_concat(cxt->alist, stmts);
                }
            }
        }
        else
        {
            //MysProcessModifyColumnAutoIncrement(cxt, oldColName);
            MysProcessAutoIncrement(cxt);

            /* 在phase 4检查表结构是否正确 */
            cxt->AutoIncrement = NIL;
        }
    }
    else
    {
        mysDropAutoIncForModifyColumn(cxt, oldColName);
    }
}


/* 在原来的set类型上重建check约束，调用者负责将新字段定义的数据类型(set)改为原来的set类型名 */
static void
mysProcessSet2SetForModifyColumn(CreateStmtContext *cxt, char *setTypeNamespace, char *setTypeName, ColumnDef *column)
{
    AlterDomainStmt *dropSetOldCheck;
    AlterDomainStmt *addSetNewCheck;
    char *setCheckName;
    Constraint *checkCons;
    FuncCall *funcCall;
    A_Const *setItems;
	ColumnRef *columnRef;
    char origSetDef[1024];
    int origSetDefOffset;
    ListCell *lc;
    
    setCheckName = mysBuildCheckNameForSet();

    {
        dropSetOldCheck = makeNode(AlterDomainStmt);
        dropSetOldCheck->subtype = 'X';
        dropSetOldCheck->typeName = list_make2(makeString(setTypeNamespace), makeString(setTypeName));
        dropSetOldCheck->name = setCheckName;
        dropSetOldCheck->behavior = DROP_RESTRICT;
        dropSetOldCheck->missing_ok = false;

        cxt->blist = lappend(cxt->blist, dropSetOldCheck);
    }
    

    {
        checkCons = makeNode(Constraint);
        checkCons->contype = CONSTR_CHECK;
        checkCons->conname = setCheckName;
        checkCons->skip_validation = false;
        checkCons->initially_valid = true;
        origSetDefOffset = 0;
        foreach(lc, column->typeName->typmods)
        {
            Node *n = (Node *)lfirst(lc);
            if (IsA(n, A_Const) && ((A_Const *)n)->val.type == T_String)
            {
                char *val = pstrdup(((A_Const *)n)->val.val.str);
                int valLen = strlen(val);
                memcpy((origSetDef + origSetDefOffset), val, valLen);
                origSetDefOffset += valLen;
                memcpy((origSetDef + origSetDefOffset), ",", 1);
                origSetDefOffset += 1;
                if (924 < origSetDefOffset)
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("len of set content can not longger than 924"),
                            parser_errposition(cxt->pstate,
                                                column->typeName->location)));
                }      
            }
            else
            {
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("vals of set can only be string"),
                        parser_errposition(cxt->pstate,
                                            column->typeName->location)));
            }
        }
        origSetDef[origSetDefOffset - 1] = '\0';

        columnRef = makeNode(ColumnRef);
        columnRef->fields = list_make1(makeString(pstrdup("value")));
        columnRef->location = -1;
        setItems = makeNode(A_Const);
        setItems->val.type = T_String;
        setItems->val.val.str = pstrdup(origSetDef);
        setItems->location = -1;
        funcCall = makeFuncCall(list_make1(makeString("check_set")), 
                                list_make2(setItems, columnRef), 
                                COERCE_EXPLICIT_CALL, 
                                -1);
        checkCons->raw_expr = (Node*)funcCall;

        addSetNewCheck = makeNode(AlterDomainStmt);
        addSetNewCheck->subtype = 'C';
        addSetNewCheck->typeName = list_make2(makeString(setTypeNamespace), makeString(setTypeName));
        addSetNewCheck->def = (Node *)checkCons;

        cxt->blist = lappend(cxt->blist, addSetNewCheck);
    }

    column->typeName->names = list_make2(makeString(setTypeNamespace), 
                                         makeString(setTypeName));
    column->typeName->typmods = NIL;
}


static void
mysProcessYearSetEnumForModifyColumn(CreateStmtContext *cxt, char *oldColName, ColumnDef *column)
{
    bool rename = (strcmp(oldColName, column->colname) != 0);
    TypeName *typeName = column->typeName;
    char *newTypeName;
    HeapTuple attTuple;
    Form_pg_attribute attInfo;
    Oid oldColumnTypeOid;
    HeapTuple oldColumnTypeTuple;
    Form_pg_type oldColumnTypeInfo;

    attTuple = SearchSysCacheCopyAttName(RelationGetRelid(cxt->rel), oldColName);
	if (!HeapTupleIsValid(attTuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						oldColName, RelationGetRelationName(cxt->rel))));
	attInfo = (Form_pg_attribute) GETSTRUCT(attTuple);
    
    oldColumnTypeOid = attInfo->atttypid;
    oldColumnTypeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(oldColumnTypeOid));
    oldColumnTypeInfo = (Form_pg_type) GETSTRUCT(oldColumnTypeTuple);
    newTypeName = strVal(llast(typeName->names));

    if (oldColumnTypeInfo->typtype == TYPTYPE_ENUM)
    {
        // TODO: 原类型(可能是别人显示创建的类型，不是为了支持Mysql的enum, set, year)是否应该删除，根据什么判断
        if (strncmp(newTypeName, "enum", 4) == 0)
        {
            DropStmt *dropType = makeNode(DropStmt);
            TypeName *typeName = makeNode(TypeName);
            RenameStmt *renameStmt = makeNode(RenameStmt);
            
            // 创建新的enum类型
            generateEnumExtraStmts(cxt, column);

            // 删除原有enum类型(使用oid进行删除)
            dropType->removeType = OBJECT_TYPE;
            dropType->missing_ok = false;
            dropType->objects = list_make1(typeName);
            dropType->behavior = DROP_RESTRICT;
            dropType->concurrent = false;
            typeName->typeOid = oldColumnTypeOid;
            cxt->alist = lappend(cxt->alist, dropType);

            // 将新enum类型重命名为旧enum类型名(新字段名可能与旧字段名相同)
            renameStmt->renameType = OBJECT_TYPE;
            renameStmt->object = (Node*)column->typeName->names;
            renameStmt->subname = NULL;
            renameStmt->newname = pstrdup(NameStr(oldColumnTypeInfo->typname));
            renameStmt->missing_ok = false;

            cxt->alist = lappend(cxt->alist, renameStmt);
        }
        else if (strncmp(newTypeName, "set", 3) == 0)
        {
            // TODO
        }
        else
        {
            DropStmt *dropType = makeNode(DropStmt);
            TypeName *typeName = makeNode(TypeName);
            dropType->removeType = OBJECT_TYPE;
            dropType->missing_ok = false;
            dropType->objects = list_make1(typeName);
            dropType->behavior = DROP_RESTRICT;
            dropType->concurrent = false;
            typeName->typeOid = oldColumnTypeOid;

            cxt->alist = lappend(cxt->alist, dropType);
        }
    }
    else if (oldColumnTypeInfo->typtype == TYPTYPE_DOMAIN)
    {
        char *setTypeNamespace = get_namespace_name(oldColumnTypeInfo->typnamespace);
        char *setTypeName = pstrdup(NameStr(oldColumnTypeInfo->typname));
        if (strncmp(setTypeName, "set_", 4) == 0)
        {
            if (strncmp(newTypeName, "set", 3) == 0)
            {
                mysProcessSet2SetForModifyColumn(cxt, setTypeNamespace, setTypeName, column);
            }
        }
    }
    else
    {
        if (strncmp(newTypeName, "enum", 4) == 0)
        {
            // FIXME:
        }
        else if (strncmp(newTypeName, "set", 3) == 0)
        {
            // FIXME:
        }
        else
        {
            // FIXME:
        }
    }

    ReleaseSysCache(oldColumnTypeTuple);

    if (rename)
    {
        List *stmts = NIL;

        mysProcessSetEnumForRenameAtt(cxt->rel, 
                                      oldColName, 
                                      column->colname, 
                                      &stmts);
        cxt->alist = list_concat(cxt->alist, stmts);
    }
}


/* 
 * 注意：字段重命名发生在phase 2，在当前阶段(phase 1)新的字段名并未生效。
 *      1.在该阶段检查字段是否存在，以及需要在phase 2之前执行的语句(beforeStmt)，都需要使用旧的字段名。
 *      2.需要在phase 2之后执行的语句(afterStmt)，都需要使用新的字段名。
 * ~/src/backend/commands/mysql/mys_tablecms.c : ATController(phase 2)
 */
static void
transformModifyColumnDefinition(CreateStmtContext *cxt, char *oldColName, ColumnDef *column)
{
    bool sawOnUpdateNow;
    ExplicitColumnAttr *explicitColumnAttr = palloc0(sizeof(ExplicitColumnAttr));

    rectifyColumnCollateForAlter(column);

    cxt->columns = lappend(cxt->columns, column);
    cxt->explicitColumnsAttr = lappend(cxt->explicitColumnsAttr, explicitColumnAttr);

    mysProcessYearSetEnumForModifyColumn(cxt, oldColName, column);

    mysDropOnUpdateNowForModifyColumn(cxt, oldColName);

    mysDropCommentForModifyColumn(cxt, column);

    sawOnUpdateNow = false;
    column->constraints = mysPreProcessConstraintAttrs(cxt, column->constraints, &sawOnUpdateNow);
    if (sawOnUpdateNow)
    {
        MysProcessOnUpdateNow(cxt, column);
    }

    transformConstraintAttrs(cxt, column->constraints);

    transformModifyColumnConstraints(cxt, oldColName, column, explicitColumnAttr);

    mysProcessAutoIncForModifyColumn(cxt, oldColName, column);
}

/*
 * mys_transformAlterTableStmt -
 *		parse analysis for ALTER TABLE
 *
 * Returns the transformed AlterTableStmt.  There may be additional actions
 * to be done before and after the transformed statement, which are returned
 * in *beforeStmts and *afterStmts as lists of utility command parsetrees.
 *
 * To avoid race conditions, it's important that this function rely only on
 * the passed-in relid (and not on stmt->relation) to determine the target
 * relation.
 */
AlterTableStmt *
mys_transformAlterTableStmt(Oid relid, AlterTableStmt *stmt,
						const char *queryString,
						List **beforeStmts, List **afterStmts)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ParseState *pstate;
	CreateStmtContext cxt;
	List	   *save_alist;
	ListCell   *lcmd,
			   *l;
	List	   *newcmds = NIL;
	bool		skipValidation = true;
	AlterTableCmd *newcmd;
	ParseNamespaceItem *nsitem;

	/* Caller is responsible for locking the relation */
	rel = relation_open(relid, NoLock);
	tupdesc = RelationGetDescr(rel);

	/* Set up pstate */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	nsitem = addRangeTableEntryForRelation(pstate,
										   rel,
										   AccessShareLock,
										   NULL,
										   false,
										   true);
	addNSItemToQuery(pstate, nsitem, false, true, true);

	/* Set up CreateStmtContext */
	cxt.pstate = pstate;
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		cxt.stmtType = "ALTER FOREIGN TABLE";
		cxt.isforeign = true;
	}
	else
	{
		cxt.stmtType = "ALTER TABLE";
		cxt.isforeign = false;
	}
	cxt.relation = stmt->relation;
	cxt.rel = rel;
	cxt.inhRelations = NIL;
	cxt.isalter = true;
	cxt.columns = NIL;
    cxt.explicitColumnsAttr = NIL;
	cxt.ckconstraints = NIL;
	cxt.fkconstraints = NIL;
	cxt.ixconstraints = NIL;
	cxt.likeclauses = NIL;
	cxt.extstats = NIL;
	cxt.blist = NIL;
	cxt.alist = NIL;
	cxt.pkey = NULL;
	cxt.ispartitioned = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	cxt.partbound = NULL;
	cxt.ofType = false;
    cxt.AutoIncrement = NIL;
    cxt.AutoIncrementIsIndex = false;
    cxt.mysTableOption = NULL;

	/*
	 * Transform ALTER subcommands that need it (most don't).  These largely
	 * re-use code from CREATE TABLE.
	 */
	foreach(lcmd, stmt->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		switch (cmd->subtype)
		{
			case AT_AddColumn:
			case AT_AddColumnRecurse:
				{
					ColumnDef  *def = castNode(ColumnDef, cmd->def);

					transformColumnDefinition(&cxt, def);

                    if (cxt.AutoIncrement != NIL)
                    {
                        if (mysGetTableAutoIncColNum(cxt.rel) > 0)
                        {
                            elog(ERROR, "Incorrect table definition; there can be only one auto increment column");
                        }
                    }

                    /* 在phase 4检查表结构是否正确。应对alter table tabName add column newColName int auto_increment, add primay key(newColName) */
                    cxt.AutoIncrement = NIL;

					/*
					 * If the column has a non-null default, we can't skip
					 * validation of foreign keys.
					 */
					if (def->raw_default != NULL)
						skipValidation = false;

					/*
					 * All constraints are processed in other ways. Remove the
					 * original list
					 */
					def->constraints = NIL;

					newcmds = lappend(newcmds, cmd);
					break;
				}

            case AT_ModifyColumn:
            case AT_ChangeColumn:
                {
                    ColumnDef  *def = castNode(ColumnDef, cmd->def);

                    transformModifyColumnDefinition(&cxt, cmd->name, def);

                    /*
					 * All constraints are processed in other ways. Remove the
					 * original list
					 */
                    def->constraints = NIL;

                    newcmds = lappend(newcmds, cmd);
                    
                    break;
                }

			case AT_AddConstraint:
			case AT_AddConstraintRecurse:

				/*
				 * The original AddConstraint cmd node doesn't go to newcmds
				 */
				if (IsA(cmd->def, Constraint))
				{
					transformTableConstraint(&cxt, (Constraint *) cmd->def);
					if (((Constraint *) cmd->def)->contype == CONSTR_FOREIGN)
						skipValidation = false;
				}
				else
					elog(ERROR, "unrecognized node type: %d",
						 (int) nodeTag(cmd->def));
				break;

			case AT_AlterColumnType:
				{
					ColumnDef  *def = castNode(ColumnDef, cmd->def);
					AttrNumber	attnum;

					/*
					 * For ALTER COLUMN TYPE, transform the USING clause if
					 * one was specified.
					 */
					if (def->raw_default)
					{
						def->cooked_default =
							transformExpr(pstate, def->raw_default,
										  EXPR_KIND_ALTER_COL_TRANSFORM);
					}

					/*
					 * For identity column, create ALTER SEQUENCE command to
					 * change the data type of the sequence.
					 */
					attnum = get_attnum(relid, cmd->name);
					if (attnum == InvalidAttrNumber)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, RelationGetRelationName(rel))));

					if (attnum > 0 &&
						TupleDescAttr(tupdesc, attnum - 1)->attidentity)
					{
						Oid			seq_relid = getIdentitySequence(relid, attnum, false);
						Oid			typeOid = typenameTypeId(pstate, def->typeName);
						AlterSeqStmt *altseqstmt = makeNode(AlterSeqStmt);

						altseqstmt->sequence = makeRangeVar(get_namespace_name(get_rel_namespace(seq_relid)),
															get_rel_name(seq_relid),
															-1);
						altseqstmt->options = list_make1(makeDefElem("as", (Node *) makeTypeNameFromOid(typeOid, -1), -1));
						altseqstmt->for_identity = true;
						cxt.blist = lappend(cxt.blist, altseqstmt);
					}

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AddIdentity:
				{
					Constraint *def = castNode(Constraint, cmd->def);
					ColumnDef  *newdef = makeNode(ColumnDef);
					AttrNumber	attnum;

					newdef->colname = cmd->name;
					newdef->identity = def->generated_when;
					cmd->def = (Node *) newdef;

					attnum = get_attnum(relid, cmd->name);
					if (attnum == InvalidAttrNumber)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, RelationGetRelationName(rel))));

					generateSerialExtraStmts(&cxt, newdef,
											 get_atttype(relid, attnum),
											 def->options, true, true,
											 NULL, NULL);

					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_SetIdentity:
				{
					/*
					 * Create an ALTER SEQUENCE statement for the internal
					 * sequence of the identity column.
					 */
					ListCell   *lc;
					List	   *newseqopts = NIL;
					List	   *newdef = NIL;
					AttrNumber	attnum;
					Oid			seq_relid;

					/*
					 * Split options into those handled by ALTER SEQUENCE and
					 * those for ALTER TABLE proper.
					 */
					foreach(lc, castNode(List, cmd->def))
					{
						DefElem    *def = lfirst_node(DefElem, lc);

						if (strcmp(def->defname, "generated") == 0)
							newdef = lappend(newdef, def);
						else
							newseqopts = lappend(newseqopts, def);
					}

					attnum = get_attnum(relid, cmd->name);
					if (attnum == InvalidAttrNumber)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" of relation \"%s\" does not exist",
										cmd->name, RelationGetRelationName(rel))));

					seq_relid = getIdentitySequence(relid, attnum, true);

					if (seq_relid)
					{
						AlterSeqStmt *seqstmt;

						seqstmt = makeNode(AlterSeqStmt);
						seqstmt->sequence = makeRangeVar(get_namespace_name(get_rel_namespace(seq_relid)),
														 get_rel_name(seq_relid), -1);
						seqstmt->options = newseqopts;
						seqstmt->for_identity = true;
						seqstmt->missing_ok = false;

						cxt.blist = lappend(cxt.blist, seqstmt);
					}

					/*
					 * If column was not an identity column, we just let the
					 * ALTER TABLE command error out later.  (There are cases
					 * this fails to cover, but we'll need to restructure
					 * where creation of the sequence dependency linkage
					 * happens before we can fix it.)
					 */

					cmd->def = (Node *) newdef;
					newcmds = lappend(newcmds, cmd);
					break;
				}

			case AT_AttachPartition:
			case AT_DetachPartition:
				{
					PartitionCmd *partcmd = (PartitionCmd *) cmd->def;

					transformPartitionCmd(&cxt, partcmd);
					/* assign transformed value of the partition bound */
					partcmd->bound = cxt.partbound;
				}

				newcmds = lappend(newcmds, cmd);
				break;

			default:

				/*
				 * Currently, we shouldn't actually get here for subcommand
				 * types that don't require transformation; but if we do, just
				 * emit them unchanged.
				 */
				newcmds = lappend(newcmds, cmd);
				break;
		}
	}

	/*
	 * Transfer anything we already have in cxt.alist into save_alist, to keep
	 * it separate from the output of transformIndexConstraints.
	 */
	save_alist = cxt.alist;
	cxt.alist = NIL;

	/* Postprocess constraints */
	transformIndexConstraints(&cxt);
	transformFKConstraints(&cxt, skipValidation, true);
	transformCheckConstraints(&cxt, false);

	/*
	 * Push any index-creation commands into the ALTER, so that they can be
	 * scheduled nicely by tablecmds.c.  Note that tablecmds.c assumes that
	 * the IndexStmt attached to an AT_AddIndex or AT_AddIndexConstraint
	 * subcommand has already been through transformIndexStmt.
	 */
	foreach(l, cxt.alist)
	{
		Node	   *istmt = (Node *) lfirst(l);

		/*
		 * We assume here that cxt.alist contains only IndexStmts and possibly
		 * ALTER TABLE SET NOT NULL statements generated from primary key
		 * constraints.  We absorb the subcommands of the latter directly.
		 */
		if (IsA(istmt, IndexStmt))
		{
			IndexStmt  *idxstmt = (IndexStmt *) istmt;

			idxstmt = transformIndexStmt(relid, idxstmt, queryString);
			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = OidIsValid(idxstmt->indexOid) ? AT_AddIndexConstraint : AT_AddIndex;
			newcmd->def = (Node *) idxstmt;
			newcmds = lappend(newcmds, newcmd);
		}
		else if (IsA(istmt, AlterTableStmt))
		{
			AlterTableStmt *alterstmt = (AlterTableStmt *) istmt;

			newcmds = list_concat(newcmds, alterstmt->cmds);
		}
		else
			elog(ERROR, "unexpected stmt type %d", (int) nodeTag(istmt));
	}
	cxt.alist = NIL;

	/* Append any CHECK or FK constraints to the commands list */
	foreach(l, cxt.ckconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}
	foreach(l, cxt.fkconstraints)
	{
		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		newcmd->def = (Node *) lfirst(l);
		newcmds = lappend(newcmds, newcmd);
	}

	/* Append extended statistics objects */
	transformExtendedStatistics(&cxt);

	/* Close rel */
	relation_close(rel, NoLock);

	/*
	 * Output results.
	 */
	stmt->cmds = newcmds;

	*beforeStmts = cxt.blist;
	*afterStmts = list_concat(cxt.alist, save_alist);

	return stmt;
}


/*
 * Preprocess a list of column constraint clauses
 * to attach constraint attributes to their primary constraint nodes
 * and detect inconsistent/misplaced constraint attributes.
 *
 * NOTE: currently, attributes are only supported for FOREIGN KEY, UNIQUE,
 * EXCLUSION, and PRIMARY KEY constraints, but someday they ought to be
 * supported for other constraint types.
 */
static void
transformConstraintAttrs(CreateStmtContext *cxt, List *constraintList)
{
	Constraint *lastprimarycon = NULL;
	bool		saw_deferrability = false;
	bool		saw_initially = false;
	ListCell   *clist;

#define SUPPORTS_ATTRS(node)				\
	((node) != NULL &&						\
	 ((node)->contype == CONSTR_PRIMARY ||	\
	  (node)->contype == CONSTR_UNIQUE ||	\
	  (node)->contype == CONSTR_EXCLUSION || \
	  (node)->contype == CONSTR_FOREIGN))

	foreach(clist, constraintList)
	{
		Constraint *con = (Constraint *) lfirst(clist);

		if (!IsA(con, Constraint))
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(con));
		switch (con->contype)
		{
			case CONSTR_ATTR_DEFERRABLE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced DEFERRABLE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_deferrability)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_deferrability = true;
				lastprimarycon->deferrable = true;
				break;

			case CONSTR_ATTR_NOT_DEFERRABLE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced NOT DEFERRABLE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_deferrability)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DEFERRABLE/NOT DEFERRABLE clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_deferrability = true;
				lastprimarycon->deferrable = false;
				if (saw_initially &&
					lastprimarycon->initdeferred)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
							 parser_errposition(cxt->pstate, con->location)));
				break;

			case CONSTR_ATTR_DEFERRED:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY DEFERRED clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_initially)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_initially = true;
				lastprimarycon->initdeferred = true;

				/*
				 * If only INITIALLY DEFERRED appears, assume DEFERRABLE
				 */
				if (!saw_deferrability)
					lastprimarycon->deferrable = true;
				else if (!lastprimarycon->deferrable)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("constraint declared INITIALLY DEFERRED must be DEFERRABLE"),
							 parser_errposition(cxt->pstate, con->location)));
				break;

			case CONSTR_ATTR_IMMEDIATE:
				if (!SUPPORTS_ATTRS(lastprimarycon))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("misplaced INITIALLY IMMEDIATE clause"),
							 parser_errposition(cxt->pstate, con->location)));
				if (saw_initially)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INITIALLY IMMEDIATE/DEFERRED clauses not allowed"),
							 parser_errposition(cxt->pstate, con->location)));
				saw_initially = true;
				lastprimarycon->initdeferred = false;
				break;

			default:
				/* Otherwise it's not an attribute */
				lastprimarycon = con;
				/* reset flags for new primary node */
				saw_deferrability = false;
				saw_initially = false;
				break;
		}
	}
}

/*
 * Special handling of type definition for a column
 */
static void
transformColumnType(CreateStmtContext *cxt, ColumnDef *column)
{
	/*
	 * All we really need to do here is verify that the type is valid,
	 * including any collation spec that might be present.
	 */
	Type		ctype = typenameType(cxt->pstate, column->typeName, NULL);

	if (column->collClause)
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(ctype);

		LookupCollation(cxt->pstate,
						column->collClause->collname,
						column->collClause->location);
		/* Complain if COLLATE is applied to an uncollatable type */
		if (!OidIsValid(typtup->typcollation))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("collations are not supported by type %s",
							format_type_be(typtup->oid)),
					 parser_errposition(cxt->pstate,
										column->collClause->location)));
	}

	ReleaseSysCache(ctype);
}


/*
 * transformPartitionCmd
 *		Analyze the ATTACH/DETACH PARTITION command
 *
 * In case of the ATTACH PARTITION command, cxt->partbound is set to the
 * transformed value of cmd->bound.
 */
static void
transformPartitionCmd(CreateStmtContext *cxt, PartitionCmd *cmd)
{
	Relation	parentRel = cxt->rel;

	switch (parentRel->rd_rel->relkind)
	{
		case RELKIND_PARTITIONED_TABLE:
			/* transform the partition bound, if any */
			Assert(RelationGetPartitionKey(parentRel) != NULL);
			if (cmd->bound != NULL)
				cxt->partbound = transformPartitionBound(cxt->pstate, parentRel,
														 cmd->bound);
			break;
		case RELKIND_PARTITIONED_INDEX:

			/*
			 * A partitioned index cannot have a partition bound set.  ALTER
			 * INDEX prevents that with its grammar, but not ALTER TABLE.
			 */
			if (cmd->bound != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("\"%s\" is not a partitioned table",
								RelationGetRelationName(parentRel))));
			break;
		case RELKIND_RELATION:
			/* the table must be partitioned */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("table \"%s\" is not partitioned",
							RelationGetRelationName(parentRel))));
			break;
		case RELKIND_INDEX:
			/* the index must be partitioned */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("index \"%s\" is not partitioned",
							RelationGetRelationName(parentRel))));
			break;
		default:
			/* parser shouldn't let this case through */
			elog(ERROR, "\"%s\" is not a partitioned table or index",
				 RelationGetRelationName(parentRel));
			break;
	}
}

