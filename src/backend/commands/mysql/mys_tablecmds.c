
/*-------------------------------------------------------------------------
 *
 * mys_tablecmds.c
 *	  MySQL sequence process
 *
 * 
 * 版权所有 (c) 2019-2023, 易景科技保留所有权利。
 * Copyright (c) 2019-2023, Halo Tech Co.,Ltd. All rights reserved.
 * 
 * 易景科技是Halo Database、Halo Database Management System、羲和数据
 * 库、羲和数据库管理系统（后面简称 Halo ）软件的发明人同时也为知识产权权
 * 利人。Halo 软件的知识产权，以及与本软件相关的所有信息内容（包括但不限
 * 于文字、图片、音频、视频、图表、界面设计、版面框架、有关数据或电子文档等）
 * 均受中华人民共和国法律法规和相应的国际条约保护，易景科技享有上述知识产
 * 权，但相关权利人依照法律规定应享有的权利除外。未免疑义，本条所指的“知识
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
 *	  src/backend/commands/mysql/mys_tablecmds.c
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/toast_compression.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/mysql/mys_tablecmds.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/mysql/mys_parse_utilcmd.h"
#include "parser/parser.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/predicate.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/int8.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/reltrigger.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * State information for ALTER TABLE
 *
 * The pending-work queue for an ALTER TABLE is a List of AlteredTableInfo
 * structs, one for each table modified by the operation (the named table
 * plus any child tables that are affected).  We save lists of subcommands
 * to apply to this table (possibly modified by parse transformation steps);
 * these lists will be executed in Phase 2.  If a Phase 3 step is needed,
 * necessary information is stored in the constraints and newvals lists.
 *
 * Phase 2 is divided into multiple passes; subcommands are executed in
 * a pass determined by subcommand type.
 */

#define AT_PASS_UNSET			-1	/* UNSET will cause ERROR */
#define AT_PASS_DROP			0	/* DROP (all flavors) */
#define AT_PASS_ALTER_TYPE		1	/* ALTER COLUMN TYPE */
#define AT_PASS_MODIFY_COL      2  /* MODIFY COLUMN */
#define AT_PASS_OLD_INDEX		3	/* re-add existing indexes */
#define AT_PASS_OLD_CONSTR		4	/* re-add existing constraints */
/* We could support a RENAME COLUMN pass here, but not currently used */
#define AT_PASS_ADD_COL			5	/* ADD COLUMN */
#define AT_PASS_ADD_CONSTR		6	/* ADD constraints (initial examination) */
#define AT_PASS_COL_ATTRS		7	/* set column attributes, eg NOT NULL */
#define AT_PASS_ADD_INDEXCONSTR	8	/* ADD index-based constraints */
#define AT_PASS_ADD_INDEX		9	/* ADD indexes */
#define AT_PASS_ADD_OTHERCONSTR	10	/* ADD other constraints, defaults */
#define AT_PASS_MISC			11	/* other stuff */
#define AT_NUM_PASSES			12


/* Alter table target-type flags for ATSimplePermissions */
#define     ATT_ORDINARY_TABLE      0x0001
#define     ATT_PARTITIONED_TABLE   0x0002
#define		ATT_VIEW				0x0004
#define		ATT_MATVIEW				0x0008
#define		ATT_INDEX				0x0010
#define		ATT_COMPOSITE_TYPE		0x0020
#define		ATT_FOREIGN_TABLE		0x0040
#define		ATT_PARTITIONED_INDEX	0x0080


/*
 * Partition tables are expected to be dropped when the parent partitioned
 * table gets dropped. Hence for partitioning we use AUTO dependency.
 * Otherwise, for regular inheritance use NORMAL dependency.
 */
#define child_dependency_type(child_is_partition)	\
	((child_is_partition) ? DEPENDENCY_AUTO : DEPENDENCY_NORMAL)


typedef struct AlteredTableInfo
{
	/* Information saved before any work commences: */
	Oid			relid;			/* Relation to work on */
	char		relkind;		/* Its relkind */
	TupleDesc	oldDesc;		/* Pre-modification tuple descriptor */

	/*
	 * Transiently set during Phase 2, normally set to NULL.
	 *
	 * ATRewriteCatalogs sets this when it starts, and closes when ATExecCmd
	 * returns control.  This can be exploited by ATExecCmd subroutines to
	 * close/reopen across transaction boundaries.
	 */
	Relation	rel;

	/* Information saved by Phase 1 for Phase 2: */
	List	   *subcmds[AT_NUM_PASSES]; /* Lists of AlterTableCmd */
	/* Information saved by Phases 1/2 for Phase 3: */
	List	   *constraints;	/* List of NewConstraint */
	List	   *newvals;		/* List of NewColumnValue */
	List	   *afterStmts;		/* List of utility command parsetrees */
	bool		verify_new_notnull; /* T if we should recheck NOT NULL */
	int			rewrite;		/* Reason for forced rewrite, if any */
	Oid			newTableSpace;	/* new tablespace; 0 means no change */
	bool		chgPersistence; /* T if SET LOGGED/UNLOGGED is used */
	char		newrelpersistence;	/* if above is true */
	Expr	   *partition_constraint;	/* for attach partition validation */
	/* true, if validating default due to some other attach/detach */
	bool		validate_default;
	/* Objects to rebuild after completing ALTER TYPE operations */
	List	   *changedConstraintOids;	/* OIDs of constraints to rebuild */
	List	   *changedConstraintDefs;	/* string definitions of same */
	List	   *changedIndexOids;	/* OIDs of indexes to rebuild */
	List	   *changedIndexDefs;	/* string definitions of same */
	char	   *replicaIdentityIndex;	/* index to reset as REPLICA IDENTITY */
	char	   *clusterOnIndex; /* index to use for CLUSTER */
	List	   *changedStatisticsOids;	/* OIDs of statistics to rebuild */
	List	   *changedStatisticsDefs;	/* string definitions of same */
    bool       isTableEmpty;    
} AlteredTableInfo;


/* Struct describing one new constraint to check in Phase 3 scan */
/* Note: new NOT NULL constraints are handled elsewhere */
typedef struct NewConstraint
{
	char	   *name;			/* Constraint name, or NULL if none */
	ConstrType	contype;		/* CHECK or FOREIGN */
	Oid			refrelid;		/* PK rel, if FOREIGN */
	Oid			refindid;		/* OID of PK's index, if FOREIGN */
	Oid			conid;			/* OID of pg_constraint entry, if FOREIGN */
	Node	   *qual;			/* Check expr or CONSTR_FOREIGN Constraint */
	ExprState  *qualstate;		/* Execution state for CHECK expr */
} NewConstraint;


/*
 * Struct describing one new column value that needs to be computed during
 * Phase 3 copy (this could be either a new column with a non-null default, or
 * a column that we're changing the type of).  Columns without such an entry
 * are just copied from the old table during ATRewriteTable.  Note that the
 * expr is an expression over *old* table values, except when is_generated
 * is true; then it is an expression over columns of the *new* tuple.
 */
typedef struct NewColumnValue
{
	AttrNumber	attnum;			/* which column */
	Expr	   *expr;			/* expression to compute */
	ExprState  *exprstate;		/* execution state */
	bool		is_generated;	/* is it a GENERATED expression? */
} NewColumnValue;


// static void ATController(AlterTableStmt *parsetree, Relation rel, List *cmds, bool recurse, 
//                          LOCKMODE lockmode, AlterTableUtilityContext *context);
// static ObjectAddress ATExecAddColumn(List **wqueue, AlteredTableInfo *tab, Relation rel,
// 				                     AlterTableCmd **cmd, bool recurse, bool recursing,
// 				                     LOCKMODE lockmode, int cur_pass, AlterTableUtilityContext *context);
// static void ATExecCmd(List **wqueue, AlteredTableInfo *tab, AlterTableCmd *cmd, LOCKMODE lockmode, 
//                       int cur_pass, AlterTableUtilityContext *context);
// static AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
// static void ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing, bool is_view, 
//                             AlterTableCmd *cmd, LOCKMODE lockmode, AlterTableUtilityContext *context);
// static void ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd, bool recurse, bool recursing, 
//                       LOCKMODE lockmode, AlterTableUtilityContext *context);
// static void ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode, AlterTableUtilityContext *context);
// static void ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap, LOCKMODE lockmode);
// static void ATRewriteTables(AlterTableStmt *parsetree, List **wqueue, LOCKMODE lockmode,
// 				            AlterTableUtilityContext *context);
// static void ATSimplePermissions(Relation rel, int allowed_targets);
// static void ATWrongRelkindError(Relation rel, int allowed_targets);
// static void RangeVarCallbackForAlterRelation(const RangeVar *rv, Oid relid, Oid oldrelid, void *arg);


static void MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel);
static void MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel);
static void StoreCatalogInheritance1(Oid relationId, Oid parentOid,
									 int32 seqNumber, Relation inhRelation,
									 bool child_is_partition);
static ObjectAddress ATExecAlterConstraint(Relation rel, AlterTableCmd *cmd,
										   bool recurse, bool recursing, LOCKMODE lockmode);
static bool ATExecAlterConstrRecurse(Constraint *cmdcon, Relation conrel, Relation tgrel,
									 Relation rel, HeapTuple contuple, List **otherrelids,
									 LOCKMODE lockmode);
static ObjectAddress ATExecValidateConstraint(List **wqueue,
											  Relation rel, char *constrName,
											  bool recurse, bool recursing, LOCKMODE lockmode);
static int	transformColumnNameList(Oid relId, List *colList,
									int16 *attnums, Oid *atttypids);
static int	transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
									   List **attnamelist,
									   int16 *attnums, Oid *atttypids,
									   Oid *opclasses);
static Oid	transformFkeyCheckAttrs(Relation pkrel,
									int numattrs, int16 *attnums,
									Oid *opclasses);
static void checkFkeyPermissions(Relation rel, int16 *attnums, int natts);
static CoercionPathType findFkeyCast(Oid targetTypeId, Oid sourceTypeId,
									 Oid *funcid);
static void validateForeignKeyConstraint(char *conname,
										 Relation rel, Relation pkrel,
										 Oid pkindOid, Oid constraintOid);
static void ATController(AlterTableStmt *parsetree,
						 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode,
						 AlterTableUtilityContext *context);
static void ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
					  bool recurse, bool recursing, LOCKMODE lockmode,
					  AlterTableUtilityContext *context);
static void ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode,
							  AlterTableUtilityContext *context);
static void ATExecCmd(List **wqueue, AlteredTableInfo *tab,
					  AlterTableCmd *cmd, LOCKMODE lockmode, int cur_pass,
					  AlterTableUtilityContext *context);
static AlterTableCmd *ATParseTransformCmd(List **wqueue, AlteredTableInfo *tab,
										  Relation rel, AlterTableCmd *cmd,
										  bool recurse, LOCKMODE lockmode,
										  int cur_pass,
										  AlterTableUtilityContext *context);
static void ATRewriteTables(AlterTableStmt *parsetree,
							List **wqueue, LOCKMODE lockmode,
							AlterTableUtilityContext *context);
static void ATCheckTables(List **wqueue, LOCKMODE lockmode, AlterTableUtilityContext *context);
static void ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap, LOCKMODE lockmode);
static AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
static void ATSimplePermissions(Relation rel, int allowed_targets);
static void ATWrongRelkindError(Relation rel, int allowed_targets);
static void ATSimpleRecursion(List **wqueue, Relation rel,
							  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
							  AlterTableUtilityContext *context);
static void ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode);
static void ATTypedTableRecursion(List **wqueue, Relation rel, AlterTableCmd *cmd,
								  LOCKMODE lockmode,
								  AlterTableUtilityContext *context);
static List *find_typed_table_dependencies(Oid typeOid, const char *typeName,
										   DropBehavior behavior);
static void ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
							bool is_view, AlterTableCmd *cmd, LOCKMODE lockmode,
							AlterTableUtilityContext *context);
static ObjectAddress ATExecAddColumn(List **wqueue, AlteredTableInfo *tab,
									 Relation rel, AlterTableCmd **cmd,
									 bool recurse, bool recursing,
									 LOCKMODE lockmode, int cur_pass,
									 AlterTableUtilityContext *context);
static bool check_for_column_name_collision(Relation rel, const char *colname,
											bool if_not_exists);
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid);
static void add_column_collation_dependency(Oid relid, int32 attnum, Oid collid);
static void ATPrepDropNotNull(Relation rel, bool recurse, bool recursing);
static ObjectAddress ATExecDropNotNull(Relation rel, const char *colName, LOCKMODE lockmode);
static void ATPrepSetNotNull(List **wqueue, Relation rel,
							 AlterTableCmd *cmd, bool recurse, bool recursing,
							 LOCKMODE lockmode,
							 AlterTableUtilityContext *context);
static ObjectAddress ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
									  const char *colName, LOCKMODE lockmode);
static void ATExecCheckNotNull(AlteredTableInfo *tab, Relation rel,
							   const char *colName, LOCKMODE lockmode);
static bool NotNullImpliedByRelConstraints(Relation rel, Form_pg_attribute attr);
static bool ConstraintImpliedByRelConstraint(Relation scanrel,
											 List *testConstraint, List *provenConstraint);
static ObjectAddress ATExecColumnDefault(Relation rel, const char *colName,
										 Node *newDefault, LOCKMODE lockmode);
static ObjectAddress ATExecCookedColumnDefault(Relation rel, AttrNumber attnum,
											   Node *newDefault);
static ObjectAddress ATExecAddIdentity(Relation rel, const char *colName,
									   Node *def, LOCKMODE lockmode);
static ObjectAddress ATExecSetIdentity(Relation rel, const char *colName,
									   Node *def, LOCKMODE lockmode);
static ObjectAddress ATExecDropIdentity(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode);
static void ATPrepDropExpression(Relation rel, AlterTableCmd *cmd, bool recurse, bool recursing, LOCKMODE lockmode);
static ObjectAddress ATExecDropExpression(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode);
static ObjectAddress ATExecSetStatistics(Relation rel, const char *colName, int16 colNum,
										 Node *newValue, LOCKMODE lockmode);
static ObjectAddress ATExecSetOptions(Relation rel, const char *colName,
									  Node *options, bool isReset, LOCKMODE lockmode);
static ObjectAddress ATExecSetStorage(Relation rel, const char *colName,
									  Node *newValue, LOCKMODE lockmode);
static void ATPrepDropColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
							 AlterTableCmd *cmd, LOCKMODE lockmode,
							 AlterTableUtilityContext *context);
static ObjectAddress ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
									  DropBehavior behavior,
									  bool recurse, bool recursing,
									  bool missing_ok, LOCKMODE lockmode,
									  ObjectAddresses *addrs);
static ObjectAddress ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
									IndexStmt *stmt, bool is_rebuild, LOCKMODE lockmode);
static ObjectAddress ATExecAddStatistics(AlteredTableInfo *tab, Relation rel,
										 CreateStatsStmt *stmt, bool is_rebuild, LOCKMODE lockmode);
static ObjectAddress ATExecAddConstraint(List **wqueue,
										 AlteredTableInfo *tab, Relation rel,
										 Constraint *newConstraint, bool recurse, bool is_readd,
										 LOCKMODE lockmode);
static char *ChooseForeignKeyConstraintNameAddition(List *colnames);
static ObjectAddress ATExecAddIndexConstraint(AlteredTableInfo *tab, Relation rel,
											  IndexStmt *stmt, LOCKMODE lockmode);
static ObjectAddress ATAddCheckConstraint(List **wqueue,
										  AlteredTableInfo *tab, Relation rel,
										  Constraint *constr,
										  bool recurse, bool recursing, bool is_readd,
										  LOCKMODE lockmode);
static ObjectAddress ATAddForeignKeyConstraint(List **wqueue, AlteredTableInfo *tab,
											   Relation rel, Constraint *fkconstraint,
											   bool recurse, bool recursing,
											   LOCKMODE lockmode);
static ObjectAddress addFkRecurseReferenced(List **wqueue, Constraint *fkconstraint,
											Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
											int numfks, int16 *pkattnum, int16 *fkattnum,
											Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
											bool old_check_ok);
static void addFkRecurseReferencing(List **wqueue, Constraint *fkconstraint,
									Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
									int numfks, int16 *pkattnum, int16 *fkattnum,
									Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
									bool old_check_ok, LOCKMODE lockmode);
static void CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
									   Relation partitionRel);
static void CloneFkReferenced(Relation parentRel, Relation partitionRel);
static void CloneFkReferencing(List **wqueue, Relation parentRel,
							   Relation partRel);
static void createForeignKeyCheckTriggers(Oid myRelOid, 
Oid refRelOid,
										  Constraint *fkconstraint, 
										  Oid constraintOid,
										  Oid indexOid);
static void createForeignKeyActionTriggers(Relation rel, Oid refRelOid,
										   Constraint *fkconstraint, 
										   Oid constraintOid,
										   Oid indexOid);
static bool tryAttachPartitionForeignKey(ForeignKeyCacheInfo *fk,
										 Oid partRelid,
										 Oid parentConstrOid, 
										 int numfks,
										 AttrNumber *mapped_conkey, 
										 AttrNumber *confkey,
										 Oid *conpfeqop);
static void ATExecDropConstraint(Relation rel, const char *constrName,
								 DropBehavior behavior,
								 bool recurse, bool recursing,
								 bool missing_ok, LOCKMODE lockmode);
static void ATExecDropPrimaryKey(Relation rel, DropBehavior behavior, bool missing_ok, LOCKMODE lockmode);
static void ATExecDropIndex(Relation rel, char *indexName, DropBehavior behavior, bool missing_ok, LOCKMODE lockmode);
static void ATExecDropForeignKey(Relation rel, char *fkeyName, DropBehavior behavior, bool missing_ok, LOCKMODE lockmode);
static void ATExecDropCheck(Relation rel, char *checkName, DropBehavior behavior, bool recurse, bool recursing,
                            bool missing_ok, LOCKMODE lockmode);
static void ATPrepAlterColumnType(List **wqueue,
								  AlteredTableInfo *tab, Relation rel,
								  bool recurse, bool recursing,
								  AlterTableCmd *cmd, LOCKMODE lockmode,
								  AlterTableUtilityContext *context);
static bool ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno);
static ObjectAddress ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
										   AlterTableCmd *cmd, LOCKMODE lockmode);
static void RememberConstraintForRebuilding(Oid conoid, AlteredTableInfo *tab);
static void RememberIndexForRebuilding(Oid indoid, AlteredTableInfo *tab);
static void RememberStatisticsForRebuilding(Oid indoid, AlteredTableInfo *tab);
static void ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab,
								   LOCKMODE lockmode);
static void ATPostAlterTypeParse(Oid oldId, Oid oldRelId, Oid refRelId,
								 char *cmd, List **wqueue, LOCKMODE lockmode,
								 bool rewrite);
static void RebuildConstraintComment(AlteredTableInfo *tab, int pass,
									 Oid objid, Relation rel, List *domname,
									 const char *conname);
static void TryReuseIndex(Oid oldId, IndexStmt *stmt);
static void TryReuseForeignKey(Oid oldId, Constraint *con);
static ObjectAddress ATExecAlterColumnGenericOptions(Relation rel, const char *colName,
													 List *options, LOCKMODE lockmode);
static ObjectAddress ATExecClusterOn(Relation rel, const char *indexName,
									 LOCKMODE lockmode);
static void ATExecDropCluster(Relation rel, LOCKMODE lockmode);
static bool ATPrepChangePersistence(Relation rel, bool toLogged);
static void ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel,
								const char *tablespacename, LOCKMODE lockmode);
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode);
static void ATExecSetTableSpaceNoStorage(Relation rel, Oid newTableSpace);
static void ATExecSetRelOptions(Relation rel, List *defList,
								AlterTableType operation,
								LOCKMODE lockmode);
static void ATExecEnableDisableTrigger(Relation rel, const char *trigname,
									   char fires_when, bool skip_system, bool recurse,
									   LOCKMODE lockmode);
static void ATExecEnableDisableRule(Relation rel, const char *rulename,
									char fires_when, LOCKMODE lockmode);
static void ATPrepAddInherit(Relation child_rel);
static ObjectAddress ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode);
static ObjectAddress ATExecDropInherit(Relation rel, RangeVar *parent, LOCKMODE lockmode);
static void drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid,
								   DependencyType deptype);
static ObjectAddress ATExecAddOf(Relation rel, const TypeName *ofTypename, LOCKMODE lockmode);
static void ATExecDropOf(Relation rel, LOCKMODE lockmode);
static void ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt *stmt, LOCKMODE lockmode);
static void ATExecGenericOptions(Relation rel, List *options);
static void ATExecSetRowSecurity(Relation rel, bool rls);
static void ATExecForceNoForceRowSecurity(Relation rel, bool force_rls);
static ObjectAddress ATExecSetCompression(AlteredTableInfo *tab, 
                                          Relation rel,
										  const char *column, 
										  Node *newValue, 
										  LOCKMODE lockmode);

static void index_copy_data(Relation rel, RelFileNode newrnode);

static void CreateInheritance(Relation child_rel, Relation parent_rel);
static void RemoveInheritance(Relation child_rel, Relation parent_rel,
							  bool allow_detached);
static ObjectAddress ATExecAttachPartition(List **wqueue, Relation rel,
										   PartitionCmd *cmd,
										   AlterTableUtilityContext *context);
static void AttachPartitionEnsureIndexes(Relation rel, Relation attachrel);
static void QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
											   List *partConstraint,
											   bool validate_default);
static void CloneRowTriggersToPartition(Relation parent, Relation partition);
static void DetachAddConstraintIfNeeded(List **wqueue, Relation partRel);
static void DropClonedTriggersFromPartition(Oid partitionId);
static ObjectAddress ATExecDetachPartition(List **wqueue, AlteredTableInfo *tab,
										   Relation rel, RangeVar *name,
										   bool concurrent);
static void DetachPartitionFinalize(Relation rel, Relation partRel,
									bool concurrent, Oid defaultPartOid);
static ObjectAddress ATExecDetachPartitionFinalize(Relation rel, RangeVar *name);
static ObjectAddress ATExecAttachPartitionIdx(List **wqueue, Relation rel,
											  RangeVar *name);
static void validatePartitionedIndex(Relation partedIdx, Relation partedTbl);
static void refuseDupeIndexAttach(Relation parentIdx, Relation partIdx,
								  Relation partitionTbl);
static List *GetParentedForeignKeyRefs(Relation partition);
static void ATDetachCheckNoForeignKeyRefs(Relation partition);
static char GetAttributeCompression(Oid atttypid, char *compression);

static bool isTableEmpty(Relation rel);
static void ATPrepChangeColumn(AlteredTableInfo *tab, 
                               Relation rel, 
                               AlterTableCmd *cmd, 
							   LOCKMODE lockmode);
static ObjectAddress ATExecChangeColumn(AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd, LOCKMODE lockmode);
static ObjectAddress ATExecTableOption(AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd, LOCKMODE lockmode);
static void SetIndexStorageProperties(Relation rel, Relation attrelation,
			            			  AttrNumber attnum, bool setstorage, char newstorage,
						              bool setcompression, char newcompression, LOCKMODE lockmode);
static AttrNumber mysGetTableAutoIncColNum(Relation rel);
static Oid mysGetTableAutoIncSeqOid(Relation rel);
static List *getColumnIndexs(Relation rel, char *colName);
static void MarkInheritDetached(Relation child_rel, Relation parent_rel);
static void RememberReplicaIdentityForRebuilding(Oid indoid, AlteredTableInfo *tab);
static void RememberClusterOnForRebuilding(Oid indoid, AlteredTableInfo *tab);
static bool constraints_equivalent(HeapTuple a, HeapTuple b, TupleDesc tupleDesc);
static void CreateFKCheckTrigger(Oid myRelOid, Oid refRelOid, 
                                 Constraint *fkconstraint,
			            		 Oid constraintOid, Oid indexOid, bool on_insert);
static char *mysCheckConstraintName(Oid relOid, char *userInputConstraintName);

static void ATExecTableOptionAutoIncrement(Relation rel, Node *newStartValue, bool isTableEmpty);
static void ATExecTableOptionComment(Relation rel, Node *newStartValue);
static char *tailor(char *prefix, char *firstPart, char *secondPart);

/*
 * mysAlterTableGetLockLevel
 *
 * Sets the overall lock level required for the supplied list of subcommands.
 * Policy for doing this set according to needs of AlterTable(), see
 * comments there for overall explanation.
 *
 * Function is called before and after parsing, so it must give same
 * answer each time it is called. Some subcommands are transformed
 * into other subcommand types, so the transform must never be made to a
 * lower lock level than previously assigned. All transforms are noted below.
 *
 * Since this is called before we lock the table we cannot use table metadata
 * to influence the type of lock we acquire.
 *
 * There should be no lockmodes hardcoded into the subcommand functions. All
 * lockmode decisions for ALTER TABLE are made here only. The one exception is
 * ALTER TABLE RENAME which is treated as a different statement type T_RenameStmt
 * and does not travel through this section of code and cannot be combined with
 * any of the subcommands given here.
 *
 * Note that Hot Standby only knows about AccessExclusiveLocks on the primary
 * so any changes that might affect SELECTs running on standbys need to use
 * AccessExclusiveLocks even if you think a lesser lock would do, unless you
 * have a solution for that also.
 *
 * Also note that pg_dump uses only an AccessShareLock, meaning that anything
 * that takes a lock less than AccessExclusiveLock can change object definitions
 * while pg_dump is running. Be careful to check that the appropriate data is
 * derived by pg_dump using an MVCC snapshot, rather than syscache lookups,
 * otherwise we might end up with an inconsistent dump that can't restore.
 */
LOCKMODE
mysAlterTableGetLockLevel(List *cmds)
{
	/*
	 * This only works if we read catalog tables using MVCC snapshots.
	 */
	ListCell   *lcmd;
	LOCKMODE	lockmode = ShareUpdateExclusiveLock;

	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
		LOCKMODE	cmd_lockmode = AccessExclusiveLock; /* default for compiler */

		switch (cmd->subtype)
		{
				/*
				 * These subcommands rewrite the heap, so require full locks.
				 */
			case AT_AddColumn:	/* may rewrite heap, in some cases and visible
								 * to SELECT */
            case AT_ModifyColumn:
            case AT_ChangeColumn:
            case AT_TableOption:
			case AT_SetTableSpace:	/* must rewrite heap */
			case AT_AlterColumnType:	/* must rewrite heap */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands may require addition of toast tables. If
				 * we add a toast table to a table currently being scanned, we
				 * might miss data added to the new toast table by concurrent
				 * insert transactions.
				 */
			case AT_SetStorage: /* may add toast tables, see
								 * ATRewriteCatalogs() */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Removing constraints can affect SELECTs that have been
				 * optimized assuming the constraint holds true. See also
				 * CloneFkReferenced.
				 */
			case AT_DropConstraint: /* as DROP INDEX */
            case AT_DropPrimaryKey:
            case AT_DropIndex:
            case AT_DropForeignKey:
            case AT_DropCheck:
			case AT_DropNotNull:	/* may change some SQL plans */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Subcommands that may be visible to concurrent SELECTs
				 */
			case AT_DropColumn: /* change visible to SELECT */
			case AT_AddColumnToView:	/* CREATE VIEW */
			case AT_DropOids:	/* used to equiv to DropColumn */
			case AT_EnableAlwaysRule:	/* may change SELECT rules */
			case AT_EnableReplicaRule:	/* may change SELECT rules */
			case AT_EnableRule: /* may change SELECT rules */
			case AT_DisableRule:	/* may change SELECT rules */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Changing owner may remove implicit SELECT privileges
				 */
			case AT_ChangeOwner:	/* change visible to SELECT */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Changing foreign table options may affect optimization.
				 */
			case AT_GenericOptions:
			case AT_AlterColumnGenericOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect write operations only.
				 */
			case AT_EnableTrig:
			case AT_EnableAlwaysTrig:
			case AT_EnableReplicaTrig:
			case AT_EnableTrigAll:
			case AT_EnableTrigUser:
			case AT_DisableTrig:
			case AT_DisableTrigAll:
			case AT_DisableTrigUser:
				cmd_lockmode = ShareRowExclusiveLock;
				break;

				/*
				 * These subcommands affect write operations only. XXX
				 * Theoretically, these could be ShareRowExclusiveLock.
				 */
			case AT_ColumnDefault:
			case AT_CookedColumnDefault:
			case AT_AlterConstraint:
			case AT_AddIndex:	/* from ADD CONSTRAINT */
			case AT_AddIndexConstraint:
			case AT_ReplicaIdentity:
			case AT_SetNotNull:
			case AT_EnableRowSecurity:
			case AT_DisableRowSecurity:
			case AT_ForceRowSecurity:
			case AT_NoForceRowSecurity:
			case AT_AddIdentity:
			case AT_DropIdentity:
			case AT_SetIdentity:
			case AT_DropExpression:
			case AT_SetCompression:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_AddConstraint:
			case AT_AddConstraintRecurse:	/* becomes AT_AddConstraint */
			case AT_ReAddConstraint:	/* becomes AT_AddConstraint */
			case AT_ReAddDomainConstraint:	/* becomes AT_AddConstraint */
				if (IsA(cmd->def, Constraint))
				{
					Constraint *con = (Constraint *) cmd->def;

					switch (con->contype)
					{
						case CONSTR_EXCLUSION:
						case CONSTR_PRIMARY:
						case CONSTR_UNIQUE:

							/*
							 * Cases essentially the same as CREATE INDEX. We
							 * could reduce the lock strength to ShareLock if
							 * we can work out how to allow concurrent catalog
							 * updates. XXX Might be set down to
							 * ShareRowExclusiveLock but requires further
							 * analysis.
							 */
							cmd_lockmode = AccessExclusiveLock;
							break;
						case CONSTR_FOREIGN:

							/*
							 * We add triggers to both tables when we add a
							 * Foreign Key, so the lock level must be at least
							 * as strong as CREATE TRIGGER.
							 */
							cmd_lockmode = ShareRowExclusiveLock;
							break;

						default:
							cmd_lockmode = AccessExclusiveLock;
					}
				}
				break;

				/*
				 * These subcommands affect inheritance behaviour. Queries
				 * started before us will continue to see the old inheritance
				 * behaviour, while queries started after we commit will see
				 * new behaviour. No need to prevent reads or writes to the
				 * subtable while we hook it up though. Changing the TupDesc
				 * may be a problem, so keep highest lock.
				 */
			case AT_AddInherit:
			case AT_DropInherit:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect implicit row type conversion. They
				 * have affects similar to CREATE/DROP CAST on queries. don't
				 * provide for invalidating parse trees as a result of such
				 * changes, so we keep these at AccessExclusiveLock.
				 */
			case AT_AddOf:
			case AT_DropOf:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Only used by CREATE OR REPLACE VIEW which must conflict
				 * with an SELECTs currently using the view.
				 */
			case AT_ReplaceRelOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect general strategies for performance
				 * and maintenance, though don't change the semantic results
				 * from normal data reads and writes. Delaying an ALTER TABLE
				 * behind currently active writes only delays the point where
				 * the new strategy begins to take effect, so there is no
				 * benefit in waiting. In this case the minimum restriction
				 * applies: we don't currently allow concurrent catalog
				 * updates.
				 */
			case AT_SetStatistics:	/* Uses MVCC in getTableAttrs() */
			case AT_ClusterOn:	/* Uses MVCC in getIndexes() */
			case AT_DropCluster:	/* Uses MVCC in getIndexes() */
			case AT_SetOptions: /* Uses MVCC in getTableAttrs() */
			case AT_ResetOptions:	/* Uses MVCC in getTableAttrs() */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_SetLogged:
			case AT_SetUnLogged:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_ValidateConstraint: /* Uses MVCC in getConstraints() */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

				/*
				 * Rel options are more complex than first appears. Options
				 * are set here for tables, views and indexes; for historical
				 * reasons these can all be used with ALTER TABLE, so we can't
				 * decide between them using the basic grammar.
				 */
			case AT_SetRelOptions:	/* Uses MVCC in getIndexes() and
									 * getTables() */
			case AT_ResetRelOptions:	/* Uses MVCC in getIndexes() and
										 * getTables() */
				cmd_lockmode = AlterTableGetRelOptionsLockLevel((List *) cmd->def);
				break;

			case AT_AttachPartition:
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_DetachPartition:
				if (((PartitionCmd *) cmd->def)->concurrent)
					cmd_lockmode = ShareUpdateExclusiveLock;
				else
					cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_DetachPartitionFinalize:
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_CheckNotNull:

				/*
				 * This only examines the table's schema; but lock must be
				 * strong enough to prevent concurrent DROP NOT NULL.
				 */
				cmd_lockmode = AccessShareLock;
				break;

			default:			/* oops */
				elog(ERROR, "unrecognized alter table type: %d",
					 (int) cmd->subtype);
				break;
		}

		/*
		 * Take the greatest lockmode from any subcommand
		 */
		if (cmd_lockmode > lockmode)
			lockmode = cmd_lockmode;
	}

	return lockmode;
}


/*
 * AlterTable
 *		Execute ALTER TABLE, which can be a list of subcommands
 *
 * ALTER TABLE is performed in three phases:
 *		1. Examine subcommands and perform pre-transformation checking.
 *		2. Validate and transform subcommands, and update system catalogs.
 *		3. Scan table(s) to check new constraints, and optionally recopy
 *		   the data into new table(s).
 * Phase 3 is not performed unless one or more of the subcommands requires
 * it.  The intention of this design is to allow multiple independent
 * updates of the table schema to be performed with only one pass over the
 * data.
 *
 * ATPrepCmd performs phase 1.  A "work queue" entry is created for
 * each table to be affected (there may be multiple affected tables if the
 * commands traverse a table inheritance hierarchy).  Also we do preliminary
 * validation of the subcommands.  Because earlier subcommands may change
 * the catalog state seen by later commands, there are limits to what can
 * be done in this phase.  Generally, this phase acquires table locks,
 * checks permissions and relkind, and recurses to find child tables.
 *
 * ATRewriteCatalogs performs phase 2 for each affected table.
 * Certain subcommands need to be performed before others to avoid
 * unnecessary conflicts; for example, DROP COLUMN should come before
 * ADD COLUMN.  Therefore phase 1 divides the subcommands into multiple
 * lists, one for each logical "pass" of phase 2.
 *
 * ATRewriteTables performs phase 3 for those tables that need it.
 *
 * For most subcommand types, phases 2 and 3 do no explicit recursion,
 * since phase 1 already does it.  However, for certain subcommand types
 * it is only possible to determine how to recurse at phase 2 time; for
 * those cases, phase 1 sets the cmd->recurse flag (or, in some older coding,
 * changes the command subtype of a "Recurse" variant XXX to be cleaned up.)
 *
 * Thanks to the magic of MVCC, an error anywhere along the way rolls back
 * the whole operation; we don't have to do anything special to clean up.
 *
 * The caller must lock the relation, with an appropriate lock level
 * for the subcommands requested, using AlterTableGetLockLevel(stmt->cmds)
 * or higher. We pass the lock level down
 * so that we can apply it recursively to inherited tables. Note that the
 * lock level we want as we recurse might well be higher than required for
 * that specific subcommand. So we pass down the overall lock requirement,
 * rather than reassess it at lower levels.
 *
 * The caller also provides a "context" which is to be passed back to
 * utility.c when we need to execute a subcommand such as CREATE INDEX.
 * Some of the fields therein, such as the relid, are used here as well.
 */
void
mysAlterTable(AlterTableStmt *stmt, LOCKMODE lockmode,
		      AlterTableUtilityContext *context)
{
	Relation	rel;

	/* Caller is required to provide an adequate lock. */
	rel = relation_open(context->relid, NoLock);

	CheckTableNotInUse(rel, "ALTER TABLE");

	ATController(stmt, rel, stmt->cmds, stmt->relation->inh, lockmode, context);
}


/*
 * ATController provides top level control over the phases.
 *
 * parsetree is passed in to allow it to be passed to event triggers
 * when requested.
 */
static void
ATController(AlterTableStmt *parsetree,
			 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode,
			 AlterTableUtilityContext *context)
{
	List	   *wqueue = NIL;
	ListCell   *lcmd;

	/* Phase 1: preliminary examination of commands, create work queue */
	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		ATPrepCmd(&wqueue, rel, cmd, recurse, false, lockmode, context);
	}

	/* Close the relation, but keep lock until commit */
	relation_close(rel, NoLock);

	/* Phase 2: update system catalogs */
	ATRewriteCatalogs(&wqueue, lockmode, context);

	/* Phase 3: scan/rewrite tables as needed, and run afterStmts */
	ATRewriteTables(parsetree, &wqueue, lockmode, context);

    /* Phase 4: check table definition */
    ATCheckTables(&wqueue, lockmode, context);
}


/*
 * ATPrepCmd
 *
 * Traffic cop for ALTER TABLE Phase 1 operations, including simple
 * recursion and permission checks.
 *
 * Caller must have acquired appropriate lock type on relation already.
 * This lock should be held until commit.
 */
static void
ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing, LOCKMODE lockmode,
		  AlterTableUtilityContext *context)
{
	AlteredTableInfo *tab;
	int	pass = AT_PASS_UNSET;

	/* Find or create work queue entry for this table */
	tab = ATGetQueueEntry(wqueue, rel);

	/*
	 * Disallow any ALTER TABLE other than ALTER TABLE DETACH FINALIZE on
	 * partitions that are pending detach.
	 */
	if (rel->rd_rel->relispartition &&
		cmd->subtype != AT_DetachPartitionFinalize &&
		PartitionHasPendingDetach(RelationGetRelid(rel)))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot alter partition \"%s\" with an incomplete detach",
					   RelationGetRelationName(rel)),
				errhint("Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation."));

	/*
	 * Copy the original subcommand for each table, so we can scribble on it.
	 * This avoids conflicts when different child tables need to make
	 * different parse transformations (for example, the same column may have
	 * different column numbers in different children).
	 */
	cmd = copyObject(cmd);

	/*
	 * Do permissions and relkind checking, recursion to child tables if
	 * needed, and any additional phase-1 processing needed.  (But beware of
	 * adding any processing that looks at table details that another
	 * subcommand could change.  In some cases we reject multiple subcommands
	 * that could try to change the same state in contrary ways.)
	 */
	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
			ATSimplePermissions(rel,
								ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, false, cmd,
							lockmode, context);
			/* Recursion occurs during execution phase */
			pass = AT_PASS_ADD_COL;
			break;
        case AT_ModifyColumn:
            ATSimplePermissions(rel,
								ATT_ORDINARY_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
            cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, recurse, lockmode,
									  AT_PASS_UNSET, context);
            Assert(cmd != NULL);
            ATPrepChangeColumn(tab, rel, cmd, lockmode);
            pass = AT_PASS_MODIFY_COL;
            break;
        case AT_ChangeColumn:
            ATSimplePermissions(rel,
								ATT_ORDINARY_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
            cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, recurse, lockmode,
									  AT_PASS_UNSET, context);
            Assert(cmd != NULL);
            ATPrepChangeColumn(tab, rel, cmd, lockmode);
            pass = AT_PASS_MODIFY_COL;
            break;
        case AT_TableOption:
            ATSimplePermissions(rel,
								ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
            tab->isTableEmpty = isTableEmpty(rel);
            pass = AT_PASS_MISC;
            break;
		case AT_AddColumnToView:	/* add column via CREATE OR REPLACE VIEW */
			ATSimplePermissions(rel, ATT_VIEW);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, true, cmd,
							lockmode, context);
			/* Recursion occurs during execution phase */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */

			/*
			 * We allow defaults on views so that INSERT into a view can have
			 * default-ish behavior.  This works because the rewriter
			 * substitutes default values into INSERTs before it expands
			 * rules.
			 */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* No command-specific prep needed */
			pass = cmd->def ? AT_PASS_ADD_OTHERCONSTR : AT_PASS_DROP;
			break;
		case AT_CookedColumnDefault:	/* add a pre-cooked default */
			/* This is currently used only in CREATE TABLE */
			/* (so the permission check really isn't necessary) */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			pass = AT_PASS_ADD_OTHERCONSTR;
			break;
		case AT_AddIdentity:
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			pass = AT_PASS_ADD_OTHERCONSTR;
			break;
		case AT_SetIdentity:
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/* This should run after AddIdentity, so do it in MISC pass */
			pass = AT_PASS_MISC;
			break;
		case AT_DropIdentity:
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			pass = AT_PASS_DROP;
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATPrepDropNotNull(rel, recurse, recursing);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			pass = AT_PASS_DROP;
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* Need command-specific recursion decision */
			ATPrepSetNotNull(wqueue, rel, cmd, recurse, recursing,
							 lockmode, context);
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_CheckNotNull:	/* check column is already marked NOT NULL */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* No command-specific prep needed */
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_DropExpression: /* ALTER COLUMN DROP EXPRESSION */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			ATPrepDropExpression(rel, cmd, recurse, recursing, lockmode);
			pass = AT_PASS_DROP;
			break;
		case AT_SetStatistics:	/* ALTER COLUMN SET STATISTICS */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_INDEX | ATT_PARTITIONED_INDEX | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_SetOptions:		/* ALTER COLUMN SET ( options ) */
		case AT_ResetOptions:	/* ALTER COLUMN RESET ( options ) */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			pass = AT_PASS_MISC;
			break;
		case AT_SetStorage:		/* ALTER COLUMN SET STORAGE */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_SetCompression: /* ALTER COLUMN SET COMPRESSION */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			ATSimplePermissions(rel,
								ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepDropColumn(wqueue, rel, recurse, recursing, cmd,
							 lockmode, context);
			/* Recursion occurs during execution phase */
			pass = AT_PASS_DROP;
			break;
		case AT_AddIndex:		/* ADD INDEX */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_INDEX;
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* Recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_AddConstraintRecurse;
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_AddIndexConstraint: /* ADD CONSTRAINT USING INDEX */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_ADD_INDEXCONSTR;
			break;
		case AT_DropConstraint: /* DROP CONSTRAINT */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATCheckPartitionsNotInUse(rel, lockmode);
			/* Other recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_DropConstraintRecurse;
			pass = AT_PASS_DROP;
			break;
        case AT_DropPrimaryKey:
            ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATCheckPartitionsNotInUse(rel, lockmode);
            /* This command never recurses */
            /* No command-specific prep needed */
            pass = AT_PASS_DROP;
            break;
        case AT_DropIndex:
            ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATCheckPartitionsNotInUse(rel, lockmode);
            /* This command never recurses */
            /* No command-specific prep needed */
            pass = AT_PASS_DROP;
            break;
        case AT_DropForeignKey:
            ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATCheckPartitionsNotInUse(rel, lockmode);
            /* This command never recurses */
            /* No command-specific prep needed */
            pass = AT_PASS_DROP;
            break;
        case AT_DropCheck:
            ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
            ATCheckPartitionsNotInUse(rel, lockmode);
            /* Other recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_DropCheckRecurse;
			pass = AT_PASS_DROP;
			break;
		case AT_AlterColumnType:	/* ALTER COLUMN TYPE */
			ATSimplePermissions(rel,
								ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			/* See comments for ATPrepAlterColumnType */
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, recurse, lockmode,
									  AT_PASS_UNSET, context);
			Assert(cmd != NULL);
			/* Performs own recursion */
			ATPrepAlterColumnType(wqueue, tab, rel, recurse, recursing, cmd,
								  lockmode, context);
			pass = AT_PASS_ALTER_TYPE;
			break;
		case AT_AlterColumnGenericOptions:
			ATSimplePermissions(rel, ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_SetLogged:		/* SET LOGGED */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE);
			if (tab->chgPersistence)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot change persistence setting twice")));
			tab->chgPersistence = ATPrepChangePersistence(rel, true);
			/* force rewrite if necessary; see comment in ATRewriteTables */
			if (tab->chgPersistence)
			{
				tab->rewrite |= AT_REWRITE_ALTER_PERSISTENCE;
				tab->newrelpersistence = RELPERSISTENCE_PERMANENT;
			}
			pass = AT_PASS_MISC;
			break;
		case AT_SetUnLogged:	/* SET UNLOGGED */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE);
			if (tab->chgPersistence)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot change persistence setting twice")));
			tab->chgPersistence = ATPrepChangePersistence(rel, false);
			/* force rewrite if necessary; see comment in ATRewriteTables */
			if (tab->chgPersistence)
			{
				tab->rewrite |= AT_REWRITE_ALTER_PERSISTENCE;
				tab->newrelpersistence = RELPERSISTENCE_UNLOGGED;
			}
			pass = AT_PASS_MISC;
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			pass = AT_PASS_DROP;
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_INDEX |
								ATT_PARTITIONED_INDEX);
			/* This command never recurses */
			ATPrepSetTableSpace(tab, rel, cmd->name, lockmode);
			pass = AT_PASS_MISC;	/* doesn't actually matter */
			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:	/* RESET (...) */
		case AT_ReplaceRelOptions:	/* reset them all, then set just these */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW | ATT_MATVIEW | ATT_INDEX);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_AddInherit:		/* INHERIT */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			ATPrepAddInherit(rel);
			pass = AT_PASS_MISC;
			break;
		case AT_DropInherit:	/* NO INHERIT */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_AlterConstraint:	/* ALTER CONSTRAINT */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE);
			/* Recursion occurs during execution phase */
			pass = AT_PASS_MISC;
			break;
		case AT_ValidateConstraint: /* VALIDATE CONSTRAINT */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* Recursion occurs during execution phase */
			/* No command-specific prep needed except saving recurse flag */
			if (recurse)
				cmd->subtype = AT_ValidateConstraintRecurse;
			pass = AT_PASS_MISC;
			break;
		case AT_ReplicaIdentity:	/* REPLICA IDENTITY ... */
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW);
			pass = AT_PASS_MISC;
			/* This command never recurses */
			/* No command-specific prep needed */
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER variants */
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:	/* DISABLE TRIGGER variants */
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* Set up recursion for phase 2; no other prep needed */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_MISC;
			break;
		case AT_EnableRule:		/* ENABLE/DISABLE RULE variants */
		case AT_EnableAlwaysRule:
		case AT_EnableReplicaRule:
		case AT_DisableRule:
		case AT_AddOf:			/* OF */
		case AT_DropOf:			/* NOT OF */
		case AT_EnableRowSecurity:
		case AT_DisableRowSecurity:
		case AT_ForceRowSecurity:
		case AT_NoForceRowSecurity:
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE);
			/* These commands never recurse */
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_GenericOptions:
			ATSimplePermissions(rel, ATT_FOREIGN_TABLE);
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_AttachPartition:
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_PARTITIONED_INDEX);
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_DetachPartition:
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE);
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		case AT_DetachPartitionFinalize:
			ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE);
			/* No command-specific prep needed */
			pass = AT_PASS_MISC;
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			pass = AT_PASS_UNSET;	/* keep compiler quiet */
			break;
	}
	Assert(pass > AT_PASS_UNSET);

	/* Add the subcommand to the appropriate list for phase 2 */
	tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd);
}


/*
 * ATRewriteCatalogs
 *
 * Traffic cop for ALTER TABLE Phase 2 operations.  Subcommands are
 * dispatched in a "safe" execution order (designed to avoid unnecessary
 * conflicts).
 */
static void
ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode,
				  AlterTableUtilityContext *context)
{
	int			pass;
	ListCell   *ltab;

	/*
	 * We process all the tables "in parallel", one pass at a time.  This is
	 * needed because we may have to propagate work from one table to another
	 * (specifically, ALTER TYPE on a foreign key's PK has to dispatch the
	 * re-adding of the foreign key constraint to the other table).  Work can
	 * only be propagated into later passes, however.
	 */
	for (pass = 0; pass < AT_NUM_PASSES; pass++)
	{
		/* Go through each table that needs to be processed */
		foreach(ltab, *wqueue)
		{
			AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
			List	   *subcmds = tab->subcmds[pass];
			ListCell   *lcmd;

			if (subcmds == NIL)
				continue;

			/*
			 * Open the relation and store it in tab.  This allows subroutines
			 * close and reopen, if necessary.  Appropriate lock was obtained
			 * by phase 1, needn't get it again.
			 */
			tab->rel = relation_open(tab->relid, NoLock);

			foreach(lcmd, subcmds)
				ATExecCmd(wqueue, tab,
						  castNode(AlterTableCmd, lfirst(lcmd)),
						  lockmode, pass, context);

			/*
			 * After the ALTER TYPE pass, do cleanup work (this is not done in
			 * ATExecAlterColumnType since it should be done only once if
			 * multiple columns of a table are altered).
			 */
			if (pass == AT_PASS_ALTER_TYPE || pass == AT_PASS_MODIFY_COL)
				ATPostAlterTypeCleanup(wqueue, tab, lockmode);

			if (tab->rel)
			{
				relation_close(tab->rel, NoLock);
				tab->rel = NULL;
			}
		}
	}

	/* Check to see if a toast table must be added. */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/*
		 * If the table is source table of ATTACH PARTITION command, we did
		 * not modify anything about it that will change its toasting
		 * requirement, so no need to check.
		 */
		if (((tab->relkind == RELKIND_RELATION ||
			  tab->relkind == RELKIND_PARTITIONED_TABLE) &&
			 tab->partition_constraint == NULL) ||
			tab->relkind == RELKIND_MATVIEW)
			AlterTableCreateToastTable(tab->relid, (Datum) 0, lockmode);
	}
}


/*
 * ATRewriteTables: ALTER TABLE phase 3
 */
static void
ATRewriteTables(AlterTableStmt *parsetree, List **wqueue, LOCKMODE lockmode,
				AlterTableUtilityContext *context)
{
	ListCell   *ltab;

	/* Go through each table that needs to be checked or rewritten */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/* Relations without storage may be ignored here */
		if (!RELKIND_HAS_STORAGE(tab->relkind))
			continue;

		/*
		 * If we change column data types, the operation has to be propagated
		 * to tables that use this table's rowtype as a column type.
		 * tab->newvals will also be non-NULL in the case where we're adding a
		 * column with a default.  We choose to forbid that case as well,
		 * since composite types might eventually support defaults.
		 *
		 * (Eventually we'll probably need to check for composite type
		 * dependencies even when we're just scanning the table without a
		 * rewrite, but at the moment a composite type does not enforce any
		 * constraints, so it's not necessary/appropriate to enforce them just
		 * during ALTER.)
		 */
		if (tab->newvals != NIL || tab->rewrite > 0)
		{
			Relation	rel;

			rel = table_open(tab->relid, NoLock);
			find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
			table_close(rel, NoLock);
		}

		/*
		 * We only need to rewrite the table if at least one column needs to
		 * be recomputed, or we are changing its persistence.
		 *
		 * There are two reasons for requiring a rewrite when changing
		 * persistence: on one hand, we need to ensure that the buffers
		 * belonging to each of the two relations are marked with or without
		 * BM_PERMANENT properly.  On the other hand, since rewriting creates
		 * and assigns a new relfilenode, we automatically create or drop an
		 * init fork for the relation as appropriate.
		 */
		if (tab->rewrite > 0)
		{
			/* Build a temporary relation and copy data */
			Relation	OldHeap;
			Oid			OIDNewHeap;
			Oid			NewTableSpace;
			char		persistence;

			OldHeap = table_open(tab->relid, NoLock);

			/*
			 * We don't support rewriting of system catalogs; there are too
			 * many corner cases and too little benefit.  In particular this
			 * is certainly not going to work for mapped catalogs.
			 */
			if (IsSystemRelation(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite system relation \"%s\"",
								RelationGetRelationName(OldHeap))));

			if (RelationIsUsedAsCatalogTable(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite table \"%s\" used as a catalog table",
								RelationGetRelationName(OldHeap))));

			/*
			 * Don't allow rewrite on temp tables of other backends ... their
			 * local buffer manager is not going to cope.
			 */
			if (RELATION_IS_OTHER_TEMP(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite temporary tables of other sessions")));

			/*
			 * Select destination tablespace (same as original unless user
			 * requested a change)
			 */
			if (tab->newTableSpace)
				NewTableSpace = tab->newTableSpace;
			else
				NewTableSpace = OldHeap->rd_rel->reltablespace;

			/*
			 * Select persistence of transient table (same as original unless
			 * user requested a change)
			 */
			persistence = tab->chgPersistence ?
				tab->newrelpersistence : OldHeap->rd_rel->relpersistence;

			table_close(OldHeap, NoLock);

			/*
			 * Fire off an Event Trigger now, before actually rewriting the
			 * table.
			 *
			 * We don't support Event Trigger for nested commands anywhere,
			 * here included, and parsetree is given NULL when coming from
			 * AlterTableInternal.
			 *
			 * And fire it only once.
			 */
			if (parsetree)
				EventTriggerTableRewrite((Node *) parsetree,
										 tab->relid,
										 tab->rewrite);

			/*
			 * Create transient table that will receive the modified data.
			 *
			 * Ensure it is marked correctly as logged or unlogged.  We have
			 * to do this here so that buffers for the new relfilenode will
			 * have the right persistence set, and at the same time ensure
			 * that the original filenode's buffers will get read in with the
			 * correct setting (i.e. the original one).  Otherwise a rollback
			 * after the rewrite would possibly result with buffers for the
			 * original filenode having the wrong persistence setting.
			 *
			 * NB: This relies on swap_relation_files() also swapping the
			 * persistence. That wouldn't work for pg_class, but that can't be
			 * unlogged anyway.
			 */
			OIDNewHeap = make_new_heap(tab->relid, NewTableSpace, persistence,
									   lockmode);

			/*
			 * Copy the heap data into the new table with the desired
			 * modifications, and test the current data within the table
			 * against new constraints generated by ALTER TABLE commands.
			 */
			ATRewriteTable(tab, OIDNewHeap, lockmode);

			/*
			 * Swap the physical files of the old and new heaps, then rebuild
			 * indexes and discard the old heap.  We can use RecentXmin for
			 * the table's new relfrozenxid because we rewrote all the tuples
			 * in ATRewriteTable, so no older Xid remains in the table.  Also,
			 * we never try to swap toast tables by content, since we have no
			 * interest in letting this code work on system catalogs.
			 */
			finish_heap_swap(tab->relid, OIDNewHeap,
							 false, false, true,
							 !OidIsValid(tab->newTableSpace),
							 RecentXmin,
							 ReadNextMultiXactId(),
							 persistence);
		}
		else
		{
			/*
			 * If required, test the current data within the table against new
			 * constraints generated by ALTER TABLE commands, but don't
			 * rebuild data.
			 */
			if (tab->constraints != NIL || tab->verify_new_notnull ||
				tab->partition_constraint != NULL)
				ATRewriteTable(tab, InvalidOid, lockmode);

			/*
			 * If we had SET TABLESPACE but no reason to reconstruct tuples,
			 * just do a block-by-block copy.
			 */
			if (tab->newTableSpace)
				ATExecSetTableSpace(tab->relid, tab->newTableSpace, lockmode);
		}
	}

	/*
	 * Foreign key constraints are checked in a final pass, since (a) it's
	 * generally best to examine each one separately, and (b) it's at least
	 * theoretically possible that we have changed both relations of the
	 * foreign key, and we'd better have finished both rewrites before we try
	 * to read the tables.
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		Relation	rel = NULL;
		ListCell   *lcon;

		/* Relations without storage may be ignored here too */
		if (!RELKIND_HAS_STORAGE(tab->relkind))
			continue;

		foreach(lcon, tab->constraints)
		{
			NewConstraint *con = lfirst(lcon);

			if (con->contype == CONSTR_FOREIGN)
			{
				Constraint *fkconstraint = (Constraint *) con->qual;
				Relation	refrel;

				if (rel == NULL)
				{
					/* Long since locked, no need for another */
					rel = table_open(tab->relid, NoLock);
				}

				refrel = table_open(con->refrelid, RowShareLock);

				validateForeignKeyConstraint(fkconstraint->conname, rel, refrel,
											 con->refindid,
											 con->conid);

				/*
				 * No need to mark the constraint row as validated, we did
				 * that when we inserted the row earlier.
				 */

				table_close(refrel, NoLock);
			}
		}

		if (rel)
			table_close(rel, NoLock);
	}

	/* Finally, run any afterStmts that were queued up */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		ListCell   *lc;

		foreach(lc, tab->afterStmts)
		{
			Node	   *stmt = (Node *) lfirst(lc);

			ProcessUtilityForAlterTable(stmt, context);
			CommandCounterIncrement();
		}
	}
}


static void
mysAlterTableCheckAutoInc(Relation rel)
{
    AttrNumber autoIncAttNum;

    if ((autoIncAttNum = mysGetTableAutoIncColNum(rel)) > 0)
    {
        char *autoIncAttName = get_attname(rel->rd_id, autoIncAttNum, false);

        if (getColumnIndexs(rel, autoIncAttName) == NIL)
        {
            elog(ERROR, "Incorrect table definition; the auto_increment column `%s` must be defined as a key", autoIncAttName);
        }
    }
}


/*
 * ATCheckTables: ALTER TABLE phase 4
 */
static void
ATCheckTables(List **wqueue, LOCKMODE lockmode, AlterTableUtilityContext *context)
{
    ListCell *ltab;

    foreach(ltab, *wqueue)
	{
        Relation rel;
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

        /* Appropriate lock was obtained by phase 1, needn't get it again. */
        rel = relation_open(tab->relid, NoLock);

        mysAlterTableCheckAutoInc(rel);

        relation_close(rel, NoLock);
	}
}


/*
 * ATRewriteTable: scan or rewrite one table
 *
 * OIDNewHeap is InvalidOid if we don't need to rewrite
 */
static void
ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap, LOCKMODE lockmode)
{
	Relation	oldrel;
	Relation	newrel;
	TupleDesc	oldTupDesc;
	TupleDesc	newTupDesc;
	bool		needscan = false;
	List	   *notnull_attrs;
	int			i;
	ListCell   *l;
	EState	   *estate;
	CommandId	mycid;
	BulkInsertState bistate;
	int			ti_options;
	ExprState  *partqualstate = NULL;

	/*
	 * Open the relation(s).  We have surely already locked the existing
	 * table.
	 */
	oldrel = table_open(tab->relid, NoLock);
	oldTupDesc = tab->oldDesc;
	newTupDesc = RelationGetDescr(oldrel);	/* includes all mods */

	if (OidIsValid(OIDNewHeap))
		newrel = table_open(OIDNewHeap, lockmode);
	else
		newrel = NULL;

	/*
	 * Prepare a BulkInsertState and options for table_tuple_insert.  The FSM
	 * is empty, so don't bother using it.
	 */
	if (newrel)
	{
		mycid = GetCurrentCommandId(true);
		bistate = GetBulkInsertState();
		ti_options = TABLE_INSERT_SKIP_FSM;
	}
	else
	{
		/* keep compiler quiet about using these uninitialized */
		mycid = 0;
		bistate = NULL;
		ti_options = 0;
	}

	/*
	 * Generate the constraint and default execution states
	 */

	estate = CreateExecutorState();

	/* Build the needed expression execution states */
	foreach(l, tab->constraints)
	{
		NewConstraint *con = lfirst(l);

		switch (con->contype)
		{
			case CONSTR_CHECK:
				needscan = true;
				con->qualstate = ExecPrepareExpr((Expr *) con->qual, estate);
				break;
			case CONSTR_FOREIGN:
				/* Nothing to do here */
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	/* Build expression execution states for partition check quals */
	if (tab->partition_constraint)
	{
		needscan = true;
		partqualstate = ExecPrepareExpr(tab->partition_constraint, estate);
	}

	foreach(l, tab->newvals)
	{
		NewColumnValue *ex = lfirst(l);

		/* expr already planned */
		ex->exprstate = ExecInitExpr((Expr *) ex->expr, NULL);
	}

	notnull_attrs = NIL;
	if (newrel || tab->verify_new_notnull)
	{
		/*
		 * If we are rebuilding the tuples OR if we added any new but not
		 * verified NOT NULL constraints, check all not-null constraints. This
		 * is a bit of overkill but it minimizes risk of bugs, and
		 * heap_attisnull is a pretty cheap test anyway.
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(newTupDesc, i);

			if (attr->attnotnull && !attr->attisdropped)
				notnull_attrs = lappend_int(notnull_attrs, i);
		}
		if (notnull_attrs)
			needscan = true;
	}

	if (newrel || needscan)
	{
		ExprContext *econtext;
		TupleTableSlot *oldslot;
		TupleTableSlot *newslot;
		TableScanDesc scan;
		MemoryContext oldCxt;
		List	   *dropped_attrs = NIL;
		ListCell   *lc;
		Snapshot	snapshot;

		if (newrel)
			ereport(DEBUG1,
					(errmsg_internal("rewriting table \"%s\"",
									 RelationGetRelationName(oldrel))));
		else
			ereport(DEBUG1,
					(errmsg_internal("verifying table \"%s\"",
									 RelationGetRelationName(oldrel))));

		if (newrel)
		{
			/*
			 * All predicate locks on the tuples or pages are about to be made
			 * invalid, because we move tuples around.  Promote them to
			 * relation locks.
			 */
			TransferPredicateLocksToHeapRelation(oldrel);
		}

		econtext = GetPerTupleExprContext(estate);

		/*
		 * Create necessary tuple slots. When rewriting, two slots are needed,
		 * otherwise one suffices. In the case where one slot suffices, we
		 * need to use the new tuple descriptor, otherwise some constraints
		 * can't be evaluated.  Note that even when the tuple layout is the
		 * same and no rewrite is required, the tupDescs might not be
		 * (consider ADD COLUMN without a default).
		 */
		if (tab->rewrite)
		{
			Assert(newrel != NULL);
			oldslot = MakeSingleTupleTableSlot(oldTupDesc,
											   table_slot_callbacks(oldrel));
			newslot = MakeSingleTupleTableSlot(newTupDesc,
											   table_slot_callbacks(newrel));

			/*
			 * Set all columns in the new slot to NULL initially, to ensure
			 * columns added as part of the rewrite are initialized to NULL.
			 * That is necessary as tab->newvals will not contain an
			 * expression for columns with a NULL default, e.g. when adding a
			 * column without a default together with a column with a default
			 * requiring an actual rewrite.
			 */
			ExecStoreAllNullTuple(newslot);
		}
		else
		{
			oldslot = MakeSingleTupleTableSlot(newTupDesc,
											   table_slot_callbacks(oldrel));
			newslot = NULL;
		}

		/*
		 * Any attributes that are dropped according to the new tuple
		 * descriptor can be set to NULL. We precompute the list of dropped
		 * attributes to avoid needing to do so in the per-tuple loop.
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			if (TupleDescAttr(newTupDesc, i)->attisdropped)
				dropped_attrs = lappend_int(dropped_attrs, i);
		}

		/*
		 * Scan through the rows, generating a new row if needed and then
		 * checking all the constraints.
		 */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(oldrel, snapshot, 0, NULL);

		/*
		 * Switch to per-tuple memory context and reset it for each tuple
		 * produced, so we don't leak memory.
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while (table_scan_getnextslot(scan, ForwardScanDirection, oldslot))
		{
			TupleTableSlot *insertslot;

			if (tab->rewrite > 0)
			{
				/* Extract data from old tuple */
				slot_getallattrs(oldslot);
				ExecClearTuple(newslot);

				/* copy attributes */
				memcpy(newslot->tts_values, oldslot->tts_values,
					   sizeof(Datum) * oldslot->tts_nvalid);
				memcpy(newslot->tts_isnull, oldslot->tts_isnull,
					   sizeof(bool) * oldslot->tts_nvalid);

				/* Set dropped attributes to null in new tuple */
				foreach(lc, dropped_attrs)
					newslot->tts_isnull[lfirst_int(lc)] = true;

				/*
				 * Constraints and GENERATED expressions might reference the
				 * tableoid column, so fill tts_tableOid with the desired
				 * value.  (We must do this each time, because it gets
				 * overwritten with newrel's OID during storing.)
				 */
				newslot->tts_tableOid = RelationGetRelid(oldrel);

				/*
				 * Process supplied expressions to replace selected columns.
				 *
				 * First, evaluate expressions whose inputs come from the old
				 * tuple.
				 */
				econtext->ecxt_scantuple = oldslot;

				foreach(l, tab->newvals)
				{
					NewColumnValue *ex = lfirst(l);

					if (ex->is_generated)
						continue;

					newslot->tts_values[ex->attnum - 1]
						= ExecEvalExpr(ex->exprstate,
									   econtext,
									   &newslot->tts_isnull[ex->attnum - 1]);
				}

				ExecStoreVirtualTuple(newslot);

				/*
				 * Now, evaluate any expressions whose inputs come from the
				 * new tuple.  We assume these columns won't reference each
				 * other, so that there's no ordering dependency.
				 */
				econtext->ecxt_scantuple = newslot;

				foreach(l, tab->newvals)
				{
					NewColumnValue *ex = lfirst(l);

					if (!ex->is_generated)
						continue;

					newslot->tts_values[ex->attnum - 1]
						= ExecEvalExpr(ex->exprstate,
									   econtext,
									   &newslot->tts_isnull[ex->attnum - 1]);
				}

				insertslot = newslot;
			}
			else
			{
				/*
				 * If there's no rewrite, old and new table are guaranteed to
				 * have the same AM, so we can just use the old slot to verify
				 * new constraints etc.
				 */
				insertslot = oldslot;
			}

			/* Now check any constraints on the possibly-changed tuple */
			econtext->ecxt_scantuple = insertslot;

			foreach(l, notnull_attrs)
			{
				int			attn = lfirst_int(l);

				if (slot_attisnull(insertslot, attn + 1))
				{
					Form_pg_attribute attr = TupleDescAttr(newTupDesc, attn);

					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("column \"%s\" of relation \"%s\" contains null values",
									NameStr(attr->attname),
									RelationGetRelationName(oldrel)),
							 errtablecol(oldrel, attn + 1)));
				}
			}

			foreach(l, tab->constraints)
			{
				NewConstraint *con = lfirst(l);

				switch (con->contype)
				{
					case CONSTR_CHECK:
						if (!ExecCheck(con->qualstate, econtext))
							ereport(ERROR,
									(errcode(ERRCODE_CHECK_VIOLATION),
									 errmsg("check constraint \"%s\" of relation \"%s\" is violated by some row",
											con->name,
											RelationGetRelationName(oldrel)),
									 errtableconstraint(oldrel, con->name)));
						break;
					case CONSTR_FOREIGN:
						/* Nothing to do here */
						break;
					default:
						elog(ERROR, "unrecognized constraint type: %d",
							 (int) con->contype);
				}
			}

			if (partqualstate && !ExecCheck(partqualstate, econtext))
			{
				if (tab->validate_default)
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("updated partition constraint for default partition \"%s\" would be violated by some row",
									RelationGetRelationName(oldrel)),
							 errtable(oldrel)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("partition constraint of relation \"%s\" is violated by some row",
									RelationGetRelationName(oldrel)),
							 errtable(oldrel)));
			}

			/* Write the tuple out to the new relation */
			if (newrel)
				table_tuple_insert(newrel, insertslot, mycid,
								   ti_options, bistate);

			ResetExprContext(econtext);

			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(oldCxt);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		ExecDropSingleTupleTableSlot(oldslot);
		if (newslot)
			ExecDropSingleTupleTableSlot(newslot);
	}

	FreeExecutorState(estate);

	table_close(oldrel, NoLock);
	if (newrel)
	{
		FreeBulkInsertState(bistate);

		table_finish_bulk_insert(newrel, ti_options);

		table_close(newrel, NoLock);
	}
}


/*
 * ATExecCmd: dispatch a subcommand to appropriate execution routine
 */
static void
ATExecCmd(List **wqueue, AlteredTableInfo *tab,
		  AlterTableCmd *cmd, LOCKMODE lockmode, int cur_pass,
		  AlterTableUtilityContext *context)
{
	ObjectAddress address = InvalidObjectAddress;
	Relation	rel = tab->rel;

	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
		case AT_AddColumnToView:	/* add column via CREATE OR REPLACE VIEW */
			address = ATExecAddColumn(wqueue, tab, rel, &cmd,
									  false, false,
									  lockmode, cur_pass, context);
			break;
		case AT_AddColumnRecurse:
			address = ATExecAddColumn(wqueue, tab, rel, &cmd,
									  true, false,
									  lockmode, cur_pass, context);
			break;
        case AT_ModifyColumn:
            address = ATExecChangeColumn(tab, rel, cmd, lockmode);
            break;
        case AT_ChangeColumn:
            address = ATExecChangeColumn(tab, rel, cmd, lockmode);
            break;
        case AT_TableOption:
            address = ATExecTableOption(tab, rel, cmd, lockmode);
            break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */
			address = ATExecColumnDefault(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_CookedColumnDefault:	/* add a pre-cooked default */
			address = ATExecCookedColumnDefault(rel, cmd->num, cmd->def);
			break;
		case AT_AddIdentity:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			address = ATExecAddIdentity(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_SetIdentity:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			address = ATExecSetIdentity(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_DropIdentity:
			address = ATExecDropIdentity(rel, cmd->name, cmd->missing_ok, lockmode);
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
			address = ATExecDropNotNull(rel, cmd->name, lockmode);
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
			address = ATExecSetNotNull(tab, rel, cmd->name, lockmode);
			break;
		case AT_CheckNotNull:	/* check column is already marked NOT NULL */
			ATExecCheckNotNull(tab, rel, cmd->name, lockmode);
			break;
		case AT_DropExpression:
			address = ATExecDropExpression(rel, cmd->name, cmd->missing_ok, lockmode);
			break;
		case AT_SetStatistics:	/* ALTER COLUMN SET STATISTICS */
			address = ATExecSetStatistics(rel, cmd->name, cmd->num, cmd->def, lockmode);
			break;
		case AT_SetOptions:		/* ALTER COLUMN SET ( options ) */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, false, lockmode);
			break;
		case AT_ResetOptions:	/* ALTER COLUMN RESET ( options ) */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, true, lockmode);
			break;
		case AT_SetStorage:		/* ALTER COLUMN SET STORAGE */
			address = ATExecSetStorage(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_SetCompression:
			address = ATExecSetCompression(tab, rel, cmd->name, cmd->def,
										   lockmode);
			break;
		case AT_DropColumn:		/* DROP COLUMN */
			address = ATExecDropColumn(wqueue, rel, cmd->name,
									   cmd->behavior, false, false,
									   cmd->missing_ok, lockmode,
									   NULL);
			break;
		case AT_DropColumnRecurse:	/* DROP COLUMN with recursion */
			address = ATExecDropColumn(wqueue, rel, cmd->name,
									   cmd->behavior, true, false,
									   cmd->missing_ok, lockmode,
									   NULL);
			break;
		case AT_AddIndex:		/* ADD INDEX */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, false,
									 lockmode);
			break;
		case AT_ReAddIndex:		/* ADD INDEX */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, true,
									 lockmode);
			break;
		case AT_ReAddStatistics:	/* ADD STATISTICS */
			address = ATExecAddStatistics(tab, rel, (CreateStatsStmt *) cmd->def,
										  true, lockmode);
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
			/* Transform the command only during initial examination */
			if (cur_pass == AT_PASS_ADD_CONSTR)
				cmd = ATParseTransformCmd(wqueue, tab, rel, cmd,
										  false, lockmode,
										  cur_pass, context);
			/* Depending on constraint type, might be no more work to do now */
			if (cmd != NULL)
				address =
					ATExecAddConstraint(wqueue, tab, rel,
										(Constraint *) cmd->def,
										false, false, lockmode);
			break;
		case AT_AddConstraintRecurse:	/* ADD CONSTRAINT with recursion */
			/* Transform the command only during initial examination */
			if (cur_pass == AT_PASS_ADD_CONSTR)
				cmd = ATParseTransformCmd(wqueue, tab, rel, cmd,
										  true, lockmode,
										  cur_pass, context);
			/* Depending on constraint type, might be no more work to do now */
			if (cmd != NULL)
				address =
					ATExecAddConstraint(wqueue, tab, rel,
										(Constraint *) cmd->def,
										true, false, lockmode);
			break;
		case AT_ReAddConstraint:	/* Re-add pre-existing check constraint */
			address =
				ATExecAddConstraint(wqueue, tab, rel, (Constraint *) cmd->def,
									true, true, lockmode);
			break;
		case AT_ReAddDomainConstraint:	/* Re-add pre-existing domain check
										 * constraint */
			address =
				AlterDomainAddConstraint(((AlterDomainStmt *) cmd->def)->typeName,
										 ((AlterDomainStmt *) cmd->def)->def,
										 NULL);
			break;
		case AT_ReAddComment:	/* Re-add existing comment */
			address = CommentObject((CommentStmt *) cmd->def);
			break;
		case AT_AddIndexConstraint: /* ADD CONSTRAINT USING INDEX */
			address = ATExecAddIndexConstraint(tab, rel, (IndexStmt *) cmd->def,
											   lockmode);
			break;
		case AT_AlterConstraint:	/* ALTER CONSTRAINT */
			address = ATExecAlterConstraint(rel, cmd, false, false, lockmode);
			break;
		case AT_ValidateConstraint: /* VALIDATE CONSTRAINT */
			address = ATExecValidateConstraint(wqueue, rel, cmd->name, false,
											   false, lockmode);
			break;
		case AT_ValidateConstraintRecurse:	/* VALIDATE CONSTRAINT with
											 * recursion */
			address = ATExecValidateConstraint(wqueue, rel, cmd->name, true,
											   false, lockmode);
			break;
		case AT_DropConstraint: /* DROP CONSTRAINT */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior,
								 false, false,
								 cmd->missing_ok, lockmode);
			break;
		case AT_DropConstraintRecurse:	/* DROP CONSTRAINT with recursion */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior,
								 true, false,
								 cmd->missing_ok, lockmode);
			break;
        case AT_DropPrimaryKey:
            ATExecDropPrimaryKey(rel, cmd->behavior, cmd->missing_ok, lockmode);
            break;
        case AT_DropIndex:
            ATExecDropIndex(rel, cmd->name, cmd->behavior, cmd->missing_ok, lockmode);
            break;
        case AT_DropForeignKey:
            ATExecDropForeignKey(rel, cmd->name, cmd->behavior, cmd->missing_ok, lockmode);
            break;
        case AT_DropCheck:
            ATExecDropCheck(rel, cmd->name, cmd->behavior, false, false, cmd->missing_ok, lockmode);
            break;
        case AT_DropCheckRecurse:
            ATExecDropCheck(rel, cmd->name, cmd->behavior, true, false, cmd->missing_ok, lockmode);
            break;
		case AT_AlterColumnType:	/* ALTER COLUMN TYPE */
			/* parse transformation was done earlier */
			address = ATExecAlterColumnType(tab, rel, cmd, lockmode);
			break;
		case AT_AlterColumnGenericOptions:	/* ALTER COLUMN OPTIONS */
			address =
				ATExecAlterColumnGenericOptions(rel, cmd->name,
												(List *) cmd->def, lockmode);
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			ATExecChangeOwner(RelationGetRelid(rel),
							  get_rolespec_oid(cmd->newowner, false),
							  false, lockmode);
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
			address = ATExecClusterOn(rel, cmd->name, lockmode);
			break;
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
			ATExecDropCluster(rel, lockmode);
			break;
		case AT_SetLogged:		/* SET LOGGED */
		case AT_SetUnLogged:	/* SET UNLOGGED */
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
			/* nothing to do here, oid columns don't exist anymore */
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */

			/*
			 * Only do this for partitioned tables and indexes, for which this
			 * is just a catalog change.  Other relation types which have
			 * storage are handled by Phase 3.
			 */
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
				rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
				ATExecSetTableSpaceNoStorage(rel, tab->newTableSpace);

			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:	/* RESET (...) */
		case AT_ReplaceRelOptions:	/* replace entire option list */
			ATExecSetRelOptions(rel, (List *) cmd->def, cmd->subtype, lockmode);
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_ORIGIN, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableAlwaysTrig:	/* ENABLE ALWAYS TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ALWAYS, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableReplicaTrig:	/* ENABLE REPLICA TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_REPLICA, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrig:	/* DISABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_DISABLED, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableTrigAll:	/* ENABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_FIRES_ON_ORIGIN, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrigAll: /* DISABLE TRIGGER ALL */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableTrigUser: /* ENABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_FIRES_ON_ORIGIN, true,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrigUser:	/* DISABLE TRIGGER USER */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, true,
									   cmd->recurse,
									   lockmode);
			break;

		case AT_EnableRule:		/* ENABLE RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_ORIGIN, lockmode);
			break;
		case AT_EnableAlwaysRule:	/* ENABLE ALWAYS RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ALWAYS, lockmode);
			break;
		case AT_EnableReplicaRule:	/* ENABLE REPLICA RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_REPLICA, lockmode);
			break;
		case AT_DisableRule:	/* DISABLE RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_DISABLED, lockmode);
			break;

		case AT_AddInherit:
			address = ATExecAddInherit(rel, (RangeVar *) cmd->def, lockmode);
			break;
		case AT_DropInherit:
			address = ATExecDropInherit(rel, (RangeVar *) cmd->def, lockmode);
			break;
		case AT_AddOf:
			address = ATExecAddOf(rel, (TypeName *) cmd->def, lockmode);
			break;
		case AT_DropOf:
			ATExecDropOf(rel, lockmode);
			break;
		case AT_ReplicaIdentity:
			ATExecReplicaIdentity(rel, (ReplicaIdentityStmt *) cmd->def, lockmode);
			break;
		case AT_EnableRowSecurity:
			ATExecSetRowSecurity(rel, true);
			break;
		case AT_DisableRowSecurity:
			ATExecSetRowSecurity(rel, false);
			break;
		case AT_ForceRowSecurity:
			ATExecForceNoForceRowSecurity(rel, true);
			break;
		case AT_NoForceRowSecurity:
			ATExecForceNoForceRowSecurity(rel, false);
			break;
		case AT_GenericOptions:
			ATExecGenericOptions(rel, (List *) cmd->def);
			break;
		case AT_AttachPartition:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
				ATExecAttachPartition(wqueue, rel, (PartitionCmd *) cmd->def,
									  context);
			else
				ATExecAttachPartitionIdx(wqueue, rel,
										 ((PartitionCmd *) cmd->def)->name);
			break;
		case AT_DetachPartition:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			/* ATPrepCmd ensures it must be a table */
			Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			ATExecDetachPartition(wqueue, tab, rel,
								  ((PartitionCmd *) cmd->def)->name,
								  ((PartitionCmd *) cmd->def)->concurrent);
			break;
		case AT_DetachPartitionFinalize:
			ATExecDetachPartitionFinalize(rel, ((PartitionCmd *) cmd->def)->name);
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			break;
	}

	/*
	 * Report the subcommand to interested event triggers.
	 */
	if (cmd)
		EventTriggerCollectAlterTableSubcmd((Node *) cmd, address);

	/*
	 * Bump the command counter to ensure the next subcommand in the sequence
	 * can see the changes so far
	 */
	CommandCounterIncrement();
}


/*
 * ATGetQueueEntry: find or create an entry in the ALTER TABLE work queue
 */
static AlteredTableInfo *
ATGetQueueEntry(List **wqueue, Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	AlteredTableInfo *tab;
	ListCell   *ltab;

	foreach(ltab, *wqueue)
	{
		tab = (AlteredTableInfo *) lfirst(ltab);
		if (tab->relid == relid)
			return tab;
	}

	/*
	 * Not there, so add it.  Note that we make a copy of the relation's
	 * existing descriptor before anything interesting can happen to it.
	 */
	tab = (AlteredTableInfo *) palloc0(sizeof(AlteredTableInfo));
	tab->relid = relid;
	tab->rel = NULL;			/* set later */
	tab->relkind = rel->rd_rel->relkind;
	tab->oldDesc = CreateTupleDescCopyConstr(RelationGetDescr(rel));
	tab->newrelpersistence = RELPERSISTENCE_PERMANENT;
	tab->chgPersistence = false;

	*wqueue = lappend(*wqueue, tab);

	return tab;
}


/*
 * ATSimplePermissions
 *
 * - Ensure that it is a relation (or possibly a view)
 * - Ensure this user is the owner
 * - Ensure that it is not a system table
 */
static void
ATSimplePermissions(Relation rel, int allowed_targets)
{
	int			actual_target;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
            actual_target = ATT_ORDINARY_TABLE;
			break;
		case RELKIND_PARTITIONED_TABLE:
			actual_target = ATT_PARTITIONED_TABLE;
			break;
		case RELKIND_VIEW:
			actual_target = ATT_VIEW;
			break;
		case RELKIND_MATVIEW:
			actual_target = ATT_MATVIEW;
			break;
		case RELKIND_INDEX:
			actual_target = ATT_INDEX;
			break;
		case RELKIND_PARTITIONED_INDEX:
			actual_target = ATT_PARTITIONED_INDEX;
			break;
		case RELKIND_COMPOSITE_TYPE:
			actual_target = ATT_COMPOSITE_TYPE;
			break;
		case RELKIND_FOREIGN_TABLE:
			actual_target = ATT_FOREIGN_TABLE;
			break;
		default:
			actual_target = 0;
			break;
	}

	/* Wrong target type? */
	if ((actual_target & allowed_targets) == 0)
		ATWrongRelkindError(rel, allowed_targets);

	/* Permissions checks */
	if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));
}


/*
 * ATWrongRelkindError
 *
 * Throw an error when a relation has been determined to be of the wrong
 * type.
 */
static void
ATWrongRelkindError(Relation rel, int allowed_targets)
{
	char	   *msg;

	switch (allowed_targets)
	{
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE:
			msg = _("\"%s\" is not a table");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW:
			msg = _("\"%s\" is not a table or view");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, view, or foreign table");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW | ATT_MATVIEW | ATT_INDEX:
			msg = _("\"%s\" is not a table, view, materialized view, or index");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW:
			msg = _("\"%s\" is not a table or materialized view");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_INDEX:
			msg = _("\"%s\" is not a table, materialized view, or index");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_INDEX | ATT_PARTITIONED_INDEX:
			msg = _("\"%s\" is not a table, materialized view, index, or partitioned index");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_INDEX | ATT_PARTITIONED_INDEX | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, materialized view, index, partitioned index, or foreign table");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, materialized view, or foreign table");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table or foreign table");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, composite type, or foreign table");
			break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW | ATT_INDEX | ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a table, materialized view, index, or foreign table");
			break;
        case ATT_ORDINARY_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE:
            msg = _("\"%s\" is not a ordinary table, composite type, or foreign table");
            break;
		case ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_PARTITIONED_INDEX:
			msg = _("\"%s\" is not a table or partitioned index");
			break;
		case ATT_VIEW:
			msg = _("\"%s\" is not a view");
			break;
		case ATT_FOREIGN_TABLE:
			msg = _("\"%s\" is not a foreign table");
			break;
		default:
			/* shouldn't get here, add all necessary cases above */
			msg = _("\"%s\" is of the wrong type");
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg(msg, RelationGetRelationName(rel))));
}


/*
 * ALTER TABLE ADD COLUMN
 *
 * Adds an additional attribute to a relation making the assumption that
 * CHECK, NOT NULL, and FOREIGN KEY constraints will be removed from the
 * AT_AddColumn AlterTableCmd by parse_utilcmd.c and added as independent
 * AlterTableCmd's.
 *
 * ADD COLUMN cannot use the normal ALTER TABLE recursion mechanism, because we
 * have to decide at runtime whether to recurse or not depending on whether we
 * actually add a column or merely merge with an existing column.  (We can't
 * check this in a static pre-pass because it won't handle multiple inheritance
 * situations correctly.)
 */
static void
ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				bool is_view, AlterTableCmd *cmd, LOCKMODE lockmode,
				AlterTableUtilityContext *context)
{
	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot add column to typed table")));

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);

	if (recurse && !is_view)
		cmd->subtype = AT_AddColumnRecurse;
}


/*
 * Add a column to a table.  The return value is the address of the
 * new column in the parent relation.
 *
 * cmd is pass-by-ref so that we can replace it with the parse-transformed
 * copy (but that happens only after we check for IF NOT EXISTS).
 */
static ObjectAddress
ATExecAddColumn(List **wqueue, AlteredTableInfo *tab, Relation rel,
				AlterTableCmd **cmd,
				bool recurse, bool recursing,
				LOCKMODE lockmode, int cur_pass,
				AlterTableUtilityContext *context)
{
	Oid			myrelid = RelationGetRelid(rel);
	ColumnDef  *colDef = castNode(ColumnDef, (*cmd)->def);
	bool		if_not_exists = (*cmd)->missing_ok;
	Relation	pgclass,
				attrdesc;
	HeapTuple	reltup;
	FormData_pg_attribute attribute;
	int			newattnum;
	char		relkind;
	HeapTuple	typeTuple;
	Oid			typeOid;
	int32		typmod;
	Oid			collOid;
	Form_pg_type tform;
	Expr	   *defval;
	List	   *children;
	ListCell   *child;
	AlterTableCmd *childcmd;
	AclResult	aclresult;
	ObjectAddress address;
	TupleDesc	tupdesc;
	FormData_pg_attribute *aattr[] = {&attribute};

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	if (rel->rd_rel->relispartition && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot add column to a partition")));

	attrdesc = table_open(AttributeRelationId, RowExclusiveLock);

	/*
	 * Are we adding the column to a recursion child?  If so, check whether to
	 * merge with an existing definition for the column.  If we do merge, we
	 * must not recurse.  Children will already have the column, and recursing
	 * into them would mess up attinhcount.
	 */
	if (colDef->inhcount > 0)
	{
		HeapTuple	tuple;

		/* Does child already have a column by this name? */
		tuple = SearchSysCacheCopyAttName(myrelid, colDef->colname);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);
			Oid			ctypeId;
			int32		ctypmod;
			Oid			ccollid;

			/* Child column must match on type, typmod, and collation */
			typenameTypeIdAndMod(NULL, colDef->typeName, &ctypeId, &ctypmod);
			if (ctypeId != childatt->atttypid ||
				ctypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(rel), colDef->colname)));
			ccollid = GetColumnDefCollation(NULL, colDef, ctypeId);
			if (ccollid != childatt->attcollation)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("child table \"%s\" has different collation for column \"%s\"",
								RelationGetRelationName(rel), colDef->colname),
						 errdetail("\"%s\" versus \"%s\"",
								   get_collation_name(ccollid),
								   get_collation_name(childatt->attcollation))));

			/* Bump the existing child att's inhcount */
			childatt->attinhcount++;
			CatalogTupleUpdate(attrdesc, &tuple->t_self, tuple);

			heap_freetuple(tuple);

			/* Inform the user about the merge */
			ereport(NOTICE,
					(errmsg("merging definition of column \"%s\" for child \"%s\"",
							colDef->colname, RelationGetRelationName(rel))));

			table_close(attrdesc, RowExclusiveLock);
			return InvalidObjectAddress;
		}
	}

	/* skip if the name already exists and if_not_exists is true */
	if (!check_for_column_name_collision(rel, colDef->colname, if_not_exists))
	{
		table_close(attrdesc, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/*
	 * Okay, we need to add the column, so go ahead and do parse
	 * transformation.  This can result in queueing up, or even immediately
	 * executing, subsidiary operations (such as creation of unique indexes);
	 * so we mustn't do it until we have made the if_not_exists check.
	 *
	 * When recursing, the command was already transformed and we needn't do
	 * so again.  Also, if context isn't given we can't transform.  (That
	 * currently happens only for AT_AddColumnToView; we expect that view.c
	 * passed us a ColumnDef that doesn't need work.)
	 */
	if (context != NULL && !recursing)
	{
		*cmd = ATParseTransformCmd(wqueue, tab, rel, *cmd, recurse, lockmode,
								   cur_pass, context);
		Assert(*cmd != NULL);
		colDef = castNode(ColumnDef, (*cmd)->def);
	}

	/*
	 * Cannot add identity column if table has children, because identity does
	 * not inherit.  (Adding column and identity separately will work.)
	 */
	if (colDef->identity &&
		recurse &&
		find_inheritance_children(myrelid, NoLock) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot recursively add identity column to table that has child tables")));

	pgclass = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relkind = ((Form_pg_class) GETSTRUCT(reltup))->relkind;

	/* Determine the new attribute's number */
	newattnum = ((Form_pg_class) GETSTRUCT(reltup))->relnatts + 1;
	if (newattnum > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	typeTuple = typenameType(NULL, colDef->typeName, &typmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	typeOid = tform->oid;

	aclresult = pg_type_aclcheck(typeOid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, typeOid);

	collOid = GetColumnDefCollation(NULL, colDef, typeOid);

	/* make sure datatype is legal for a column */
	CheckAttributeType(colDef->colname, typeOid, collOid,
					   list_make1_oid(rel->rd_rel->reltype),
					   0);

	/* construct new attribute's pg_attribute entry */
	attribute.attrelid = myrelid;
	namestrcpy(&(attribute.attname), colDef->colname);
	attribute.atttypid = typeOid;
	attribute.attstattarget = (newattnum > 0) ? -1 : 0;
	attribute.attlen = tform->typlen;
	attribute.attnum = newattnum;
	attribute.attndims = list_length(colDef->typeName->arrayBounds);
	attribute.atttypmod = typmod;
	attribute.attbyval = tform->typbyval;
	attribute.attalign = tform->typalign;
	attribute.attstorage = tform->typstorage;
	attribute.attcompression = GetAttributeCompression(typeOid,
													   colDef->compression);
	attribute.attnotnull = colDef->is_not_null;
	attribute.atthasdef = false;
	attribute.atthasmissing = false;
	attribute.attidentity = colDef->identity;
	attribute.attgenerated = colDef->generated;
	attribute.attisdropped = false;
	attribute.attislocal = colDef->is_local;
	attribute.attinhcount = colDef->inhcount;
	attribute.attcollation = collOid;

	/* attribute.attacl is handled by InsertPgAttributeTuples() */

	ReleaseSysCache(typeTuple);

	tupdesc = CreateTupleDesc(lengthof(aattr), (FormData_pg_attribute **) &aattr);

	InsertPgAttributeTuples(attrdesc, tupdesc, myrelid, NULL, NULL);

	table_close(attrdesc, RowExclusiveLock);

	/*
	 * Update pg_class tuple as appropriate
	 */
	((Form_pg_class) GETSTRUCT(reltup))->relnatts = newattnum;

	CatalogTupleUpdate(pgclass, &reltup->t_self, reltup);

	heap_freetuple(reltup);

	/* Post creation hook for new attribute */
	InvokeObjectPostCreateHook(RelationRelationId, myrelid, newattnum);

	table_close(pgclass, RowExclusiveLock);

	/* Make the attribute's catalog entry visible */
	CommandCounterIncrement();

	/*
	 * Store the DEFAULT, if any, in the catalogs
	 */
	if (colDef->raw_default)
	{
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attribute.attnum;
		rawEnt->raw_default = copyObject(colDef->raw_default);

		/*
		 * Attempt to skip a complete table rewrite by storing the specified
		 * DEFAULT value outside of the heap.  This may be disabled inside
		 * AddRelationNewConstraints if the optimization cannot be applied.
		 */
		rawEnt->missingMode = (!colDef->generated);

		rawEnt->generated = colDef->generated;

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);

		/* Make the additional catalog changes visible */
		CommandCounterIncrement();

		/*
		 * Did the request for a missing value work? If not we'll have to do a
		 * rewrite
		 */
		if (!rawEnt->missingMode)
			tab->rewrite |= AT_REWRITE_DEFAULT_VAL;
	}

	/*
	 * Tell Phase 3 to fill in the default expression, if there is one.
	 *
	 * If there is no default, Phase 3 doesn't have to do anything, because
	 * that effectively means that the default is NULL.  The heap tuple access
	 * routines always check for attnum > # of attributes in tuple, and return
	 * NULL if so, so without any modification of the tuple data we will get
	 * the effect of NULL values in the new column.
	 *
	 * An exception occurs when the new column is of a domain type: the domain
	 * might have a NOT NULL constraint, or a check constraint that indirectly
	 * rejects nulls.  If there are any domain constraints then we construct
	 * an explicit NULL default value that will be passed through
	 * CoerceToDomain processing.  (This is a tad inefficient, since it causes
	 * rewriting the table which we really don't have to do, but the present
	 * design of domain processing doesn't offer any simple way of checking
	 * the constraints more directly.)
	 *
	 * Note: we use build_column_default, and not just the cooked default
	 * returned by AddRelationNewConstraints, so that the right thing happens
	 * when a datatype's default applies.
	 *
	 * Note: it might seem that this should happen at the end of Phase 2, so
	 * that the effects of subsequent subcommands can be taken into account.
	 * It's intentional that we do it now, though.  The new column should be
	 * filled according to what is said in the ADD COLUMN subcommand, so that
	 * the effects are the same as if this subcommand had been run by itself
	 * and the later subcommands had been issued in new ALTER TABLE commands.
	 *
	 * We can skip this entirely for relations without storage, since Phase 3
	 * is certainly not going to touch them.  System attributes don't have
	 * interesting defaults, either.
	 */
	if (RELKIND_HAS_STORAGE(relkind) && attribute.attnum > 0)
	{
		/*
		 * For an identity column, we can't use build_column_default(),
		 * because the sequence ownership isn't set yet.  So do it manually.
		 */
		if (colDef->identity)
		{
			NextValueExpr *nve = makeNode(NextValueExpr);

			nve->seqid = RangeVarGetRelid(colDef->identitySequence, NoLock, false);
			nve->typeId = typeOid;

			defval = (Expr *) nve;

			/* must do a rewrite for identity columns */
			tab->rewrite |= AT_REWRITE_DEFAULT_VAL;
		}
		else
			defval = (Expr *) build_column_default(rel, attribute.attnum);

		if (!defval && DomainHasConstraints(typeOid))
		{
			Oid			baseTypeId;
			int32		baseTypeMod;
			Oid			baseTypeColl;

			baseTypeMod = typmod;
			baseTypeId = getBaseTypeAndTypmod(typeOid, &baseTypeMod);
			baseTypeColl = get_typcollation(baseTypeId);
			defval = (Expr *) makeNullConst(baseTypeId, baseTypeMod, baseTypeColl);
			defval = (Expr *) coerce_to_target_type(NULL,
													(Node *) defval,
													baseTypeId,
													typeOid,
													typmod,
													COERCION_ASSIGNMENT,
													COERCE_IMPLICIT_CAST,
													-1);
			if (defval == NULL) /* should not happen */
				elog(ERROR, "failed to coerce base type to domain");
		}

		if (defval)
		{
			NewColumnValue *newval;

			newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
			newval->attnum = attribute.attnum;
			newval->expr = expression_planner(defval);
			newval->is_generated = (colDef->generated != '\0');

			tab->newvals = lappend(tab->newvals, newval);
		}

		if (DomainHasConstraints(typeOid))
			tab->rewrite |= AT_REWRITE_DEFAULT_VAL;

		if (!TupleDescAttr(rel->rd_att, attribute.attnum - 1)->atthasmissing)
		{
			/*
			 * If the new column is NOT NULL, and there is no missing value,
			 * tell Phase 3 it needs to check for NULLs.
			 */
			tab->verify_new_notnull |= colDef->is_not_null;
		}
	}

	/*
	 * Add needed dependency entries for the new column.
	 */
	add_column_datatype_dependency(myrelid, newattnum, attribute.atttypid);
	add_column_collation_dependency(myrelid, newattnum, attribute.attcollation);

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * If we are told not to recurse, there had better not be any child
	 * tables; else the addition would put them out of step.
	 */
	if (children && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("column must be added to child tables too")));

	/* Children should see column as singly inherited */
	if (!recursing)
	{
		childcmd = copyObject(*cmd);
		colDef = castNode(ColumnDef, childcmd->def);
		colDef->inhcount = 1;
		colDef->is_local = false;
	}
	else
		childcmd = *cmd;		/* no need to copy again */

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/* find_inheritance_children already got lock */
		childrel = table_open(childrelid, NoLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");

		/* Find or create work queue entry for this table */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* Recurse to child; return value is ignored */
		ATExecAddColumn(wqueue, childtab, childrel,
						&childcmd, recurse, true,
						lockmode, cur_pass, context);

		table_close(childrel, NoLock);
	}

	ObjectAddressSubSet(address, RelationRelationId, myrelid, newattnum);
	return address;
}


/*
 * ATTypedTableRecursion
 *
 * Propagate ALTER TYPE operations to the typed tables of that type.
 * Also check the RESTRICT/CASCADE behavior.  Given CASCADE, also permit
 * recursion to inheritance children of the typed tables.
 */
static void
ATTypedTableRecursion(List **wqueue, Relation rel, AlterTableCmd *cmd,
					  LOCKMODE lockmode, AlterTableUtilityContext *context)
{
	ListCell   *child;
	List	   *children;

	Assert(rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

	children = find_typed_table_dependencies(rel->rd_rel->reltype,
											 RelationGetRelationName(rel),
											 cmd->behavior);

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;

		childrel = relation_open(childrelid, lockmode);
		CheckTableNotInUse(childrel, "ALTER TABLE");
		ATPrepCmd(wqueue, childrel, cmd, true, true, lockmode, context);
		relation_close(childrel, NoLock);
	}
}


/*
 * find_typed_table_dependencies
 *
 * Check to see if a composite type is being used as the type of a
 * typed table.  Abort if any are found and behavior is RESTRICT.
 * Else return the list of tables.
 */
static List *
find_typed_table_dependencies(Oid typeOid, const char *typeName, DropBehavior behavior)
{
	Relation	classRel;
	ScanKeyData key[1];
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	classRel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_reloftype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	scan = table_beginscan_catalog(classRel, 1, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(tuple);

		if (behavior == DROP_RESTRICT)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot alter type \"%s\" because it is the type of a typed table",
							typeName),
					 errhint("Use ALTER ... CASCADE to alter the typed tables too.")));
		else
			result = lappend_oid(result, classform->oid);
	}

	table_endscan(scan);
	table_close(classRel, AccessShareLock);

	return result;
}


/*
 * ATParseTransformCmd: perform parse transformation for one subcommand
 *
 * Returns the transformed subcommand tree, if there is one, else NULL.
 *
 * The parser may hand back additional AlterTableCmd(s) and/or other
 * utility statements, either before or after the original subcommand.
 * Other AlterTableCmds are scheduled into the appropriate slot of the
 * AlteredTableInfo (they had better be for later passes than the current one).
 * Utility statements that are supposed to happen before the AlterTableCmd
 * are executed immediately.  Those that are supposed to happen afterwards
 * are added to the tab->afterStmts list to be done at the very end.
 */
static AlterTableCmd *
ATParseTransformCmd(List **wqueue, AlteredTableInfo *tab, Relation rel,
					AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
					int cur_pass, AlterTableUtilityContext *context)
{
	AlterTableCmd *newcmd = NULL;
	AlterTableStmt *atstmt = makeNode(AlterTableStmt);
	List	   *beforeStmts;
	List	   *afterStmts;
	ListCell   *lc;

	/* Gin up an AlterTableStmt with just this subcommand and this table */
	atstmt->relation =
		makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
					 pstrdup(RelationGetRelationName(rel)),
					 -1);
	atstmt->relation->inh = recurse;
	atstmt->cmds = list_make1(cmd);
	atstmt->objtype = OBJECT_TABLE; /* needn't be picky here */
	atstmt->missing_ok = false;

	/* Transform the AlterTableStmt */
	atstmt = transformAlterTableStmt(RelationGetRelid(rel),
									 atstmt,
									 context->queryString,
									 &beforeStmts,
									 &afterStmts);

	/* Execute any statements that should happen before these subcommand(s) */
	foreach(lc, beforeStmts)
	{
		Node	   *stmt = (Node *) lfirst(lc);

		ProcessUtilityForAlterTable(stmt, context);
		CommandCounterIncrement();
	}

	/* Examine the transformed subcommands and schedule them appropriately */
	foreach(lc, atstmt->cmds)
	{
		AlterTableCmd *cmd2 = lfirst_node(AlterTableCmd, lc);
		int			pass;

		/*
		 * This switch need only cover the subcommand types that can be added
		 * by parse_utilcmd.c; otherwise, we'll use the default strategy of
		 * executing the subcommand immediately, as a substitute for the
		 * original subcommand.  (Note, however, that this does cause
		 * AT_AddConstraint subcommands to be rescheduled into later passes,
		 * which is important for index and foreign key constraints.)
		 *
		 * We assume we needn't do any phase-1 checks for added subcommands.
		 */
		switch (cmd2->subtype)
		{
			case AT_SetNotNull:
				/* Need command-specific recursion decision */
				ATPrepSetNotNull(wqueue, rel, cmd2,
								 recurse, false,
								 lockmode, context);
				pass = AT_PASS_COL_ATTRS;
				break;
			case AT_AddIndex:
				/* This command never recurses */
				/* No command-specific prep needed */
				pass = AT_PASS_ADD_INDEX;
				break;
			case AT_AddIndexConstraint:
				/* This command never recurses */
				/* No command-specific prep needed */
				pass = AT_PASS_ADD_INDEXCONSTR;
				break;
			case AT_AddConstraint:
				/* Recursion occurs during execution phase */
				if (recurse)
					cmd2->subtype = AT_AddConstraintRecurse;
				switch (castNode(Constraint, cmd2->def)->contype)
				{
					case CONSTR_PRIMARY:
					case CONSTR_UNIQUE:
					case CONSTR_EXCLUSION:
						pass = AT_PASS_ADD_INDEXCONSTR;
						break;
					default:
						pass = AT_PASS_ADD_OTHERCONSTR;
						break;
				}
				break;
			case AT_AlterColumnGenericOptions:
				/* This command never recurses */
				/* No command-specific prep needed */
				pass = AT_PASS_MISC;
				break;
			default:
				pass = cur_pass;
				break;
		}

		if (pass < cur_pass)
		{
			/* Cannot schedule into a pass we already finished */
			elog(ERROR, "ALTER TABLE scheduling failure: too late for pass %d",
				 pass);
		}
		else if (pass > cur_pass)
		{
			/* OK, queue it up for later */
			tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd2);
		}
		else
		{
			/*
			 * We should see at most one subcommand for the current pass,
			 * which is the transformed version of the original subcommand.
			 */
			if (newcmd == NULL && cmd->subtype == cmd2->subtype)
			{
				/* Found the transformed version of our subcommand */
				newcmd = cmd2;
			}
			else
				elog(ERROR, "ALTER TABLE scheduling failure: bogus item for pass %d",
					 pass);
		}
	}

	/* Queue up any after-statements to happen at the end */
	tab->afterStmts = list_concat(tab->afterStmts, afterStmts);

	return newcmd;
}

static bool
isTableEmpty(Relation rel)
{
    char *scheName;
    char *relName;
    char query[256];
    int spiResult;

    scheName = get_namespace_name(RelationGetNamespace(rel));
    relName = RelationGetRelationName(rel);
    pg_snprintf(query, 256, "select * from %s.%s limit 1", scheName, relName);
    spiResult = SPI_connect();
    if (spiResult < 0)
    {
        elog(ERROR, "mys_setval3_oid: SPI_connect returned %d", spiResult);
    }
    spiResult = SPI_execute(query, true, 0);
    if (spiResult != SPI_OK_SELECT)
    {
        elog(ERROR, "mys_setval3_oid: SPI_execute returned %d", spiResult);
    }
    if (0 < SPI_processed)
    {
        return false;
    }
    else
    {
        return true;
    }
}

static void
ATPrepChangeColumn(AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd, LOCKMODE lockmode)
{
    HeapTuple attributeTuple;
    Form_pg_attribute attributeForm;
    char *targetColOldName = cmd->name;
    ColumnDef *newColDef = (ColumnDef *)cmd->def;
    TypeName *newTypeName = newColDef->typeName;
    Oid newTypeOid = InvalidOid;
    Oid newTypeCollationOid = InvalidOid;
    int32 newTypeMode;
    AclResult aclResult;

    /* lookup the attribute so we can check inheritance status */
	attributeTuple = SearchSysCacheAttName(RelationGetRelid(rel), targetColOldName);
	if (!HeapTupleIsValid(attributeTuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						targetColOldName, RelationGetRelationName(rel))));
	attributeForm = (Form_pg_attribute) GETSTRUCT(attributeTuple);

	/* Can't alter a system attribute */
	if (attributeForm->attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						targetColOldName)));
    
    /* Look up the target type */
	typenameTypeIdAndMod(NULL, newTypeName, &newTypeOid, &newTypeMode);

	aclResult = pg_type_aclcheck(newTypeOid, GetUserId(), ACL_USAGE);
	if (aclResult != ACLCHECK_OK)
		aclcheck_error_type(aclResult, newTypeOid);

	/* And the collation */
	newTypeCollationOid = GetColumnDefCollation(NULL, newColDef, newTypeOid);
    if (newTypeCollationOid != attributeForm->attcollation)
    {
        tab->rewrite |= AT_REWRITE_COLUMN_REWRITE;
    }

	/* make sure datatype is legal for a column */
	CheckAttributeType(targetColOldName, newTypeOid, newTypeCollationOid,
					   list_make1_oid(rel->rd_rel->reltype),
					   0);
    
    if (tab->relkind == RELKIND_RELATION ||
		tab->relkind == RELKIND_PARTITIONED_TABLE)
    {
        Node *transform;
        NewColumnValue *newval;
        ParseState *pstate = make_parsestate(NULL);

        transform = (Node *) makeVar(1, attributeForm->attnum,
                                     attributeForm->atttypid, attributeForm->atttypmod,
                                     attributeForm->attcollation,
                                     0);
        
        {
            HeapTuple colOldTypeTuple;
            Form_pg_type colOldTypeInfo;
            HeapTuple colNewTypeTuple;
            Form_pg_type colNewTypeInfo;

            colOldTypeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(attributeForm->atttypid));
            colOldTypeInfo = (Form_pg_type) GETSTRUCT(colOldTypeTuple);
            colNewTypeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(newTypeOid));
            colNewTypeInfo = (Form_pg_type) GETSTRUCT(colNewTypeTuple);

            if (colOldTypeInfo->typtype == TYPTYPE_DOMAIN && colNewTypeInfo->typtype == TYPTYPE_DOMAIN)
            {
                if (strncmp(NameStr(colOldTypeInfo->typname), "set_", 4) == 0 && 
                    strncmp(NameStr(colNewTypeInfo->typname), "set_", 4) == 0)
                {
                    /* 
                     * Alter table tabName change oldColName newColName set('a', 'b');
                     *      该情况下，没有创建新的set类型(create domain setTypeName as varchar(128))，
                     *      而是修改了旧set类型的check约束。
                     * 
                     *      需要重新检查数据的合法性
                     */
                    tab->rewrite |= AT_REWRITE_COLUMN_REWRITE;
                }
            }

            if (colOldTypeInfo->typtype == TYPTYPE_ENUM && colNewTypeInfo->typtype == TYPTYPE_ENUM)
            {
                // 现有的enum类型与新建的enum类型没有直接的转换关系
                transform = coerce_to_target_type(pstate,
                                                  transform, exprType(transform),
                                                  TEXTOID, -1,
                                                  COERCION_ASSIGNMENT,
                                                  COERCE_IMPLICIT_CAST,
                                                  -1);

                transform = coerce_to_target_type(pstate,
                                                  transform, exprType(transform),
                                                  newTypeOid, newTypeMode,
                                                  COERCION_EXPLICIT,
                                                  COERCE_EXPLICIT_CAST,
                                                  -1);
            }
            else
            {
                transform = coerce_to_target_type(pstate,
										  transform, exprType(transform),
										  newTypeOid, newTypeMode,
										  COERCION_ASSIGNMENT,
										  COERCE_IMPLICIT_CAST,
										  -1);
            }

            ReleaseSysCache(colOldTypeTuple);
            ReleaseSysCache(colNewTypeTuple);
        }
        
        if (transform == NULL)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("column \"%s\" cannot be cast automatically to type %s",
                            targetColOldName, format_type_be(newTypeOid))));
        }

        /* Fix collations after all else */
		assign_expr_collations(pstate, transform);

		/* Plan the expr now so we can accurately assess the need to rewrite. */
		transform = (Node *) expression_planner((Expr *) transform);

		/*
		 * Add a work queue item to make ATRewriteTable update the column
		 * contents.
		 */
		newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
		newval->attnum = attributeForm->attnum;
		newval->expr = (Expr *) transform;
		newval->is_generated = false;

		tab->newvals = lappend(tab->newvals, newval);
		if (ATColumnChangeRequiresRewrite(transform, attributeForm->attnum))
			tab->rewrite |= AT_REWRITE_COLUMN_REWRITE;
    }

    ReleaseSysCache(attributeTuple);
}


static ObjectAddress
ATExecChangeColumn(AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd, LOCKMODE lockmode)
{
    ColumnDef *newColDef = (ColumnDef *)(cmd->def);
    char *targetColNewName = newColDef->colname;
    char *targetColOldName = cmd->name;
    Relation attributeRel;
    HeapTuple attributeTuple;
    Form_pg_attribute attributeForm;
    HeapTuple targetTypeTuple;
    Form_pg_type targetTypeForm;
    int32 targettypmod;
    Oid targetTypeCollOid;
    AttrNumber attnum;
    Relation depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple depTup;
    ObjectAddress address;

    attributeRel = table_open(AttributeRelationId, RowExclusiveLock);

    attributeTuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), targetColOldName);
    if (!HeapTupleIsValid(attributeTuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        targetColOldName, RelationGetRelationName(rel))));
    }

    attributeForm = (Form_pg_attribute) GETSTRUCT(attributeTuple);
    attnum = attributeForm->attnum;

    /* Look up the target type (should not fail, since prep found it) */
	targetTypeTuple = typenameType(NULL, newColDef->typeName, &targettypmod);
	targetTypeForm = (Form_pg_type) GETSTRUCT(targetTypeTuple);
	/* And the collation */
	targetTypeCollOid = GetColumnDefCollation(NULL, newColDef, targetTypeForm->oid);

    if (strcmp(targetColOldName, targetColNewName) != 0)
    {
        /* new name should not already exist */
	    (void) check_for_column_name_collision(rel, targetColNewName, false);
        namestrcpy(&(attributeForm->attname), targetColNewName);

        CatalogTupleUpdate(attributeRel, &attributeTuple->t_self, attributeTuple);
        CommandCounterIncrement();
    }

	if (tab->rewrite)
    {
        /*
        * Find everything that depends on the column (constraints, indexes, etc),
        * and record enough information to let us recreate the objects.
        * (重建everything that depends on the column发生在该函数之后，要使用新字段名)
        *
        * The actual recreation does not happen here, but only after we have
        * performed all the individual ALTER TYPE operations.  We have to save
        * the info before executing ALTER TYPE, though, else the deparser will
        * get confused.
        */
        depRel = table_open(DependRelationId, RowExclusiveLock);

        ScanKeyInit(&key[0],
                    Anum_pg_depend_refclassid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationRelationId));
        ScanKeyInit(&key[1],
                    Anum_pg_depend_refobjid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationGetRelid(rel)));
        ScanKeyInit(&key[2],
                    Anum_pg_depend_refobjsubid,
                    BTEqualStrategyNumber, F_INT4EQ,
                    Int32GetDatum((int32) attnum));

        scan = systable_beginscan(depRel, DependReferenceIndexId, true,
                                NULL, 3, key);

        while (HeapTupleIsValid(depTup = systable_getnext(scan)))
        {
            Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
            ObjectAddress foundObject;

            /* We don't expect any PIN dependencies on columns */
            if (foundDep->deptype == DEPENDENCY_PIN)
                elog(ERROR, "cannot alter type of a pinned column");

            foundObject.classId = foundDep->classid;
            foundObject.objectId = foundDep->objid;
            foundObject.objectSubId = foundDep->objsubid;

            switch (getObjectClass(&foundObject))
            {
                case OCLASS_CLASS:
                    {
                        char		relKind = get_rel_relkind(foundObject.objectId);

                        if (relKind == RELKIND_INDEX ||
                            relKind == RELKIND_PARTITIONED_INDEX)
                        {
                            Assert(foundObject.objectSubId == 0);
                            RememberIndexForRebuilding(foundObject.objectId, tab);
                        }
                        else if (relKind == RELKIND_SEQUENCE)
                        {
                            /*
                            * This must be a SERIAL column's sequence.  We need
                            * not do anything to it.
                            */
                            Assert(foundObject.objectSubId == 0);
                        }
                        else if (relKind == RELKIND_RELATION &&
                                foundObject.objectSubId != 0 &&
                                get_attgenerated(foundObject.objectId, foundObject.objectSubId))
                        {
                            /*
                            * Changing the type of a column that is used by a
                            * generated column is not allowed by SQL standard. It
                            * might be doable with some thinking and effort.
                            */
                            ereport(ERROR,
                                    (errcode(ERRCODE_SYNTAX_ERROR),
                                    errmsg("cannot alter type of a column used by a generated column"),
                                    errdetail("Column \"%s\" is used by generated column \"%s\".",
                                            targetColOldName, get_attname(foundObject.objectId, foundObject.objectSubId, false))));
                        }
                        else
                        {
                            /* Not expecting any other direct dependencies... */
                            elog(ERROR, "unexpected object depending on column: %s",
                                getObjectDescription(&foundObject, false));
                        }
                        break;
                    }

                case OCLASS_CONSTRAINT:
                    Assert(foundObject.objectSubId == 0);
                    RememberConstraintForRebuilding(foundObject.objectId, tab);
                    break;

                case OCLASS_REWRITE:
                    /* XXX someday see if we can cope with revising views */
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("cannot alter type of a column used by a view or rule"),
                            errdetail("%s depends on column \"%s\"",
                                    getObjectDescription(&foundObject, false),
                                    targetColOldName)));
                    break;

                case OCLASS_TRIGGER:

                    /*
                    * A trigger can depend on a column because the column is
                    * specified as an update target, or because the column is
                    * used in the trigger's WHEN condition.  The first case would
                    * not require any extra work, but the second case would
                    * require updating the WHEN expression, which will take a
                    * significant amount of new code.  Since we can't easily tell
                    * which case applies, we punt for both.  FIXME someday.
                    */
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("cannot alter type of a column used in a trigger definition"),
                            errdetail("%s depends on column \"%s\"",
                                    getObjectDescription(&foundObject, false),
                                    targetColOldName)));
                    break;

                case OCLASS_POLICY:

                    /*
                    * A policy can depend on a column because the column is
                    * specified in the policy's USING or WITH CHECK qual
                    * expressions.  It might be possible to rewrite and recheck
                    * the policy expression, but punt for now.  It's certainly
                    * easy enough to remove and recreate the policy; still, FIXME
                    * someday.
                    */
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("cannot alter type of a column used in a policy definition"),
                            errdetail("%s depends on column \"%s\"",
                                    getObjectDescription(&foundObject, false),
                                    targetColOldName)));
                    break;

                case OCLASS_DEFAULT:

                    /*
                    * Ignore the column's default expression, since we will fix
                    * it below.
                    */
                    break;

                case OCLASS_STATISTIC_EXT:

                    /*
                    * Give the extended-stats machinery a chance to fix anything
                    * that this column type change would break.
                    */
                    RememberStatisticsForRebuilding(foundObject.objectId, tab);
                    break;
                
                case OCLASS_TYPE:
                    {
                        // 如果是Mysql的enum,set,year类型，确保只能enum2enum, set2set, year2year, 并在phase 1中做必要的处理
                        HeapTuple oldColTypeTuple;
                        Form_pg_type oldColTypeInfo;
                        bool ok = false;

                        oldColTypeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(attributeForm->atttypid));
                        oldColTypeInfo = (Form_pg_type) GETSTRUCT(oldColTypeTuple);
                        if (oldColTypeInfo->typtype == TYPTYPE_ENUM)
                        {
                            if (strncmp(NameStr(targetTypeForm->typname), "enum_", 5) == 0 && targetTypeForm->typtype == TYPTYPE_ENUM)
                            {
                                ok = true;
                            }
                        }
                        else if (oldColTypeInfo->typtype == TYPTYPE_DOMAIN)
                        {
                            if (strncmp(NameStr(oldColTypeInfo->typname), "set_", 4) == 0 &&
                                strncmp(NameStr(targetTypeForm->typname), "set_", 4) == 0 &&
                                targetTypeForm->typtype == TYPTYPE_DOMAIN)
                            {
                                ok = true;
                            }
                            else if (strncmp(NameStr(oldColTypeInfo->typname), "year_", 5) == 0 &&
                                    strncmp(NameStr(targetTypeForm->typname), "year_", 5) == 0 &&
                                    targetTypeForm->typtype == TYPTYPE_DOMAIN)
                            {
                                ok = true;
                            }
                        }

                        ReleaseSysCache(oldColTypeTuple);

                        if (!ok)
                        {
                            elog(ERROR, "unexpected object depending on column: %s",
                                getObjectDescription(&foundObject, false));
                        }
                    }
                    break;

                case OCLASS_PROC:
                // case OCLASS_TYPE:
                case OCLASS_CAST:
                case OCLASS_COLLATION:
                case OCLASS_CONVERSION:
                case OCLASS_LANGUAGE:
                case OCLASS_LARGEOBJECT:
                case OCLASS_OPERATOR:
                case OCLASS_OPCLASS:
                case OCLASS_OPFAMILY:
                case OCLASS_AM:
                case OCLASS_AMOP:
                case OCLASS_AMPROC:
                case OCLASS_SCHEMA:
                case OCLASS_TSPARSER:
                case OCLASS_TSDICT:
                case OCLASS_TSTEMPLATE:
                case OCLASS_TSCONFIG:
                case OCLASS_ROLE:
                case OCLASS_DATABASE:
                case OCLASS_TBLSPACE:
                case OCLASS_FDW:
                case OCLASS_FOREIGN_SERVER:
                case OCLASS_USER_MAPPING:
                case OCLASS_DEFACL:
                case OCLASS_EXTENSION:
                case OCLASS_EVENT_TRIGGER:
                case OCLASS_PUBLICATION:
                case OCLASS_PUBLICATION_REL:
                case OCLASS_SUBSCRIPTION:
                case OCLASS_TRANSFORM:
                    /*
                    * We don't expect any of these sorts of objects to depend on
                    * a column.
                    */
                    elog(ERROR, "unexpected object depending on column: %s",
                        getObjectDescription(&foundObject, false));
                    break;

                    /*
                    * There's intentionally no default: case here; we want the
                    * compiler to warn if a new OCLASS hasn't been handled above.
                    */
            }
        }

        systable_endscan(scan);

        /*
        * Now scan for dependencies of this column on other things.  The only
        * thing we should find is the dependency on the column datatype, which we
        * want to remove, possibly a collation dependency, and dependencies on
        * other columns if it is a generated column.
        */
        ScanKeyInit(&key[0],
                    Anum_pg_depend_classid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationRelationId));
        ScanKeyInit(&key[1],
                    Anum_pg_depend_objid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationGetRelid(rel)));
        ScanKeyInit(&key[2],
                    Anum_pg_depend_objsubid,
                    BTEqualStrategyNumber, F_INT4EQ,
                    Int32GetDatum((int32) attnum));

        scan = systable_beginscan(depRel, DependDependerIndexId, true,
                                NULL, 3, key);

        while (HeapTupleIsValid(depTup = systable_getnext(scan)))
        {
            Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
            ObjectAddress foundObject;

            foundObject.classId = foundDep->refclassid;
            foundObject.objectId = foundDep->refobjid;
            foundObject.objectSubId = foundDep->refobjsubid;

            if (foundDep->deptype != DEPENDENCY_NORMAL &&
                foundDep->deptype != DEPENDENCY_AUTO)
                elog(ERROR, "found unexpected dependency type '%c'",
                    foundDep->deptype);
            if (!(foundDep->refclassid == TypeRelationId &&
                foundDep->refobjid == attributeForm->atttypid) &&
                !(foundDep->refclassid == CollationRelationId &&
                foundDep->refobjid == attributeForm->attcollation) &&
                !(foundDep->refclassid == RelationRelationId &&
                foundDep->refobjid == RelationGetRelid(rel) &&
                foundDep->refobjsubid != 0)
                )
                elog(ERROR, "found unexpected dependency for column: %s",
                    getObjectDescription(&foundObject, false));

            CatalogTupleDelete(depRel, &depTup->t_self);
        }

        systable_endscan(scan);

        table_close(depRel, RowExclusiveLock);
    }
	else
	{
		/* Nothing to do */
	}

    attributeForm->atttypid = targetTypeForm->oid;
	attributeForm->atttypmod = targettypmod;
	attributeForm->attcollation = targetTypeCollOid;
	attributeForm->attndims = list_length(newColDef->typeName->arrayBounds);
	attributeForm->attlen = targetTypeForm->typlen;
	attributeForm->attbyval = targetTypeForm->typbyval;
	attributeForm->attalign = targetTypeForm->typalign;
	attributeForm->attstorage = targetTypeForm->typstorage;
	attributeForm->attcompression = InvalidCompressionMethod;

    if (!attributeForm->attnotnull && newColDef->is_not_null)
    {
        attributeForm->attnotnull = true;
        if (!tab->verify_new_notnull)
            tab->verify_new_notnull = true;
    }
    else
    {
        attributeForm->attnotnull = newColDef->is_not_null;
    }

    ReleaseSysCache(targetTypeTuple);

    CatalogTupleUpdate(attributeRel, &attributeTuple->t_self, attributeTuple);

    table_close(attributeRel, RowExclusiveLock);

    /* Install dependencies on new datatype and collation */
	add_column_datatype_dependency(RelationGetRelid(rel), attnum, targetTypeForm->oid);
	add_column_collation_dependency(RelationGetRelid(rel), attnum, targetTypeCollOid);

    /*
	 * Drop any pg_statistic entry for the column, since it's now wrong type
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

    /* 
     * Bump the command counter to ensure that subsequent operations
	 * can see the changes so far
     */
    CommandCounterIncrement();

    // 删除原有默认值
    RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false, true);
    if (newColDef->raw_default)
    {
        /* SET DEFAULT */
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newColDef->raw_default;
		rawEnt->missingMode = false;
		rawEnt->generated = '\0';

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);
    }

    InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attributeForm->attnum);
    
    ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attributeForm->attnum);
    
    /* Cleanup */
	heap_freetuple(attributeTuple);

    return address;
}


static void
ATExecTableOptionAutoIncrement(Relation rel, Node *newStartValue, bool isTableEmpty)
{
    Oid autoIncSeqOid = mysGetTableAutoIncSeqOid(rel);

    if (OidIsValid(autoIncSeqOid))
    {
        Value *val = (Value *)newStartValue;

        if (val->type == T_Integer)
        {
            // TODO: 检查newVal < curVal
            // DirectFunctionCall2(setval_oid, ObjectIdGetDatum(autoIncSeqOid), Int64GetDatum(val->val.ival));
            /* 
             * val->val.ival - 1:
             *      MySQL下，val->val.ival是下一次的起始值。
             *      mys_setval3_oid会将val->val.ival - 1作为序列最后一次的返回值，val->val.ival为下一次的起始值。
             */
            if (isTableEmpty)
            {
                DirectFunctionCall3(setval3_oid,
                                    Int32GetDatum(autoIncSeqOid),
                                    Int64GetDatum(val->val.ival),
                                    BoolGetDatum(false));
            }
            else
            {
                mys_setval3_oid(autoIncSeqOid, val->val.ival, false);
            }
        }
        else if (val->type == T_Float)
        {
            int64 val64;
            /* could be an oversize integer as well as a float ... */
			if (scanint8(val->val.str, false, &val64))
			{
				/*
				 * It might actually fit in int32. Probably only INT_MIN can
				 * occur, but we'll code the test generally just to be sure.
				 */
                if (isTableEmpty)
                {
                    DirectFunctionCall3(setval3_oid,
                                        Int32GetDatum(autoIncSeqOid),
                                        Int64GetDatum(val64),
                                        BoolGetDatum(false));
                }
                else
                {
                    mys_setval3_oid(autoIncSeqOid, val64, false);
                }
			}
            else
            {
                /* scanint8(val->val.str, false, &val64)内部会报错 */
            }
        }
        else
        {
            /* 语法文件不应该有这种情况发生 */
            elog(ERROR, "Please use number to set the start value of auto_increment.");
        }
    }
    else
    {
        /* Nothing to do */
    }
}


static void
ATExecTableOptionComment(Relation rel, Node *newStartValue)
{
	ObjectAddress address;
    char *comment = NULL;

	address.classId = RelationRelationId;
	address.objectId = rel->rd_id;
	address.objectSubId = 0;
    comment = ((Value *)newStartValue)->val.str;

	check_object_ownership(GetUserId(), OBJECT_TABLE, address, NULL, rel);

    CreateComments(address.objectId, address.classId, address.objectSubId, comment);
}


static ObjectAddress
ATExecTableOption(AlteredTableInfo *tab, Relation rel, AlterTableCmd *cmd, LOCKMODE lockmode)
{
    List *tableOption = (List *)(cmd->def);
    ListCell *lc = NULL;
    ObjectAddress address;

    foreach(lc, tableOption)
    {
        Node *node = (Node *)lfirst(lc);

        Assert(nodeTag(node) == T_DefElem);

        if (strcmp(((DefElem *)node)->defname, "auto_increment") == 0)
        {
            ATExecTableOptionAutoIncrement(rel, ((DefElem *)node)->arg, tab->isTableEmpty);
        }
        else if (strcmp(((DefElem *)node)->defname, "comment") == 0)
        {
            ATExecTableOptionComment(rel, ((DefElem *)node)->arg);
        }
    }

    ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), 0);
    
    return address;
}


/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault, LOCKMODE lockmode)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	AttrNumber	attnum;
	ObjectAddress address;

	/*
	 * get the number of the attribute
	 */
	attnum = get_attnum(RelationGetRelid(rel), colName);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (TupleDescAttr(tupdesc, attnum - 1)->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is an identity column",
						colName, RelationGetRelationName(rel)),
				 newDefault ? 0 : errhint("Use ALTER TABLE ... ALTER COLUMN ... DROP IDENTITY instead.")));

	if (TupleDescAttr(tupdesc, attnum - 1)->attgenerated)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is a generated column",
						colName, RelationGetRelationName(rel)),
				 newDefault || TupleDescAttr(tupdesc, attnum - 1)->attgenerated != ATTRIBUTE_GENERATED_STORED ? 0 :
				 errhint("Use ALTER TABLE ... ALTER COLUMN ... DROP EXPRESSION instead.")));

    if (OidIsValid(getColumnDefaultSeq(rel, colName)))
    {
        ObjectAddressSubSet(address, RelationRelationId,
                        RelationGetRelid(rel), attnum);
        return address;
    }

	/*
	 * Remove any old default for the column.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * default.
	 *
	 * We treat removing the existing default as an internal operation when it
	 * is preparatory to adding a new default, but as a user-initiated
	 * operation when the user asked for a drop.
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  newDefault == NULL ? false : true);

	if (newDefault)
	{
		/* SET DEFAULT */
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newDefault;
		rawEnt->missingMode = false;
		rawEnt->generated = '\0';

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * Add a pre-cooked default expression.
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecCookedColumnDefault(Relation rel, AttrNumber attnum,
						  Node *newDefault)
{
	ObjectAddress address;

	/* We assume no checking is required */

	/*
	 * Remove any old default for the column.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * default.  (In ordinary cases, there could not be a default in place
	 * anyway, but it's possible when combining LIKE with inheritance.)
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  true);

	(void) StoreAttrDefault(rel, attnum, newDefault, true, false);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * ALTER TABLE ALTER COLUMN ADD IDENTITY
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecAddIdentity(Relation rel, const char *colName,
				  Node *def, LOCKMODE lockmode)
{
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	ObjectAddress address;
	ColumnDef  *cdef = castNode(ColumnDef, def);

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Can't alter a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Creating a column as identity implies NOT NULL, so adding the identity
	 * to an existing column that is not NOT NULL would create a state that
	 * cannot be reproduced without contortions.
	 */
	if (!attTup->attnotnull)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" must be declared NOT NULL before identity can be added",
						colName, RelationGetRelationName(rel))));

	if (attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" is already an identity column",
						colName, RelationGetRelationName(rel))));

	if (attTup->atthasdef)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" already has a default value",
						colName, RelationGetRelationName(rel))));

	attTup->attidentity = cdef->identity;
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attTup->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	return address;
}


/*
 * ALTER TABLE ALTER COLUMN SET { GENERATED or sequence options }
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecSetIdentity(Relation rel, const char *colName, Node *def, LOCKMODE lockmode)
{
	ListCell   *option;
	DefElem    *generatedEl = NULL;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	ObjectAddress address;

	foreach(option, castNode(List, def))
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "generated") == 0)
		{
			if (generatedEl)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			generatedEl = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/*
	 * Even if there is nothing to change here, we run all the checks.  There
	 * will be a subsequent ALTER SEQUENCE that relies on everything being
	 * there.
	 */

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (!attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" is not an identity column",
						colName, RelationGetRelationName(rel))));

	if (generatedEl)
	{
		attTup->attidentity = defGetInt32(generatedEl);
		CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel),
								  attTup->attnum);
		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	heap_freetuple(tuple);
	table_close(attrelation, RowExclusiveLock);

	return address;
}


/*
 * ALTER TABLE ALTER COLUMN DROP IDENTITY
 *
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecDropIdentity(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	ObjectAddress address;
	Oid			seqid;
	ObjectAddress seqaddress;

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (!attTup->attidentity)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is not an identity column",
							colName, RelationGetRelationName(rel))));
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" is not an identity column, skipping",
							colName, RelationGetRelationName(rel))));
			heap_freetuple(tuple);
			table_close(attrelation, RowExclusiveLock);
			return InvalidObjectAddress;
		}
	}

	attTup->attidentity = '\0';
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attTup->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	/* drop the internal sequence */
	seqid = getIdentitySequence(RelationGetRelid(rel), attnum, false);
	deleteDependencyRecordsForClass(RelationRelationId, seqid,
									RelationRelationId, DEPENDENCY_INTERNAL);
	CommandCounterIncrement();
	seqaddress.classId = RelationRelationId;
	seqaddress.objectId = seqid;
	seqaddress.objectSubId = 0;
	performDeletion(&seqaddress, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	return address;
}


/*
 * ATSimpleRecursion
 *
 * Simple table recursion sufficient for most ALTER TABLE operations.
 * All direct and indirect children are processed in an unspecified order.
 * Note that if a child inherits from the original table via multiple
 * inheritance paths, it will be visited just once.
 */
static void
ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
				  AlterTableUtilityContext *context)
{
	/*
	 * Propagate to children, if desired and if there are (or might be) any
	 * children.
	 */
	if (recurse && rel->rd_rel->relhassubclass)
	{
		Oid			relid = RelationGetRelid(rel);
		ListCell   *child;
		List	   *children;

		children = find_all_inheritors(relid, lockmode, NULL);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;

			if (childrelid == relid)
				continue;
			/* find_all_inheritors already got lock */
			childrel = relation_open(childrelid, NoLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");
			ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode, context);
			relation_close(childrel, NoLock);
		}
	}
}


/*
 * ALTER TABLE ALTER COLUMN DROP NOT NULL
 */
static void
ATPrepDropNotNull(Relation rel, bool recurse, bool recursing)
{
	/*
	 * If the parent is a partitioned table, like check constraints, we do not
	 * support removing the NOT NULL while partitions exist.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(rel, true);

		Assert(partdesc != NULL);
		if (partdesc->nparts > 0 && !recurse && !recursing)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot remove constraint from only the partitioned table when partitions exist"),
					 errhint("Do not specify the ONLY keyword.")));
	}
}


/*
 * Return the address of the modified column.  If the column was already
 * nullable, InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecDropNotNull(Relation rel, const char *colName, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attr_rel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	ObjectAddress address;

	/*
	 * lookup the attribute
	 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is an identity column",
						colName, RelationGetRelationName(rel))));

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
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("column \"%s\" is in a primary key",
										colName)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("column \"%s\" is in index used as replica identity",
										colName)));
				}
			}
		}

		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/* If rel is partition, shouldn't drop NOT NULL if parent has the same */
	if (rel->rd_rel->relispartition)
	{
		Oid			parentId = get_partition_parent(RelationGetRelid(rel), false);
		Relation	parent = table_open(parentId, AccessShareLock);
		TupleDesc	tupDesc = RelationGetDescr(parent);
		AttrNumber	parent_attnum;

		parent_attnum = get_attnum(parentId, colName);
		if (TupleDescAttr(tupDesc, parent_attnum - 1)->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" is marked NOT NULL in parent table",
							colName)));
		table_close(parent, AccessShareLock);
	}

	/*
	 * Okay, actually perform the catalog change ... if needed
	 */
	if (attTup->attnotnull)
	{
		attTup->attnotnull = false;

		CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	table_close(attr_rel, RowExclusiveLock);

	return address;
}


/*
 * ALTER TABLE ALTER COLUMN SET NOT NULL
 */

static void
ATPrepSetNotNull(List **wqueue, Relation rel,
				 AlterTableCmd *cmd, bool recurse, bool recursing,
				 LOCKMODE lockmode, AlterTableUtilityContext *context)
{
	/*
	 * If we're already recursing, there's nothing to do; the topmost
	 * invocation of ATSimpleRecursion already visited all children.
	 */
	if (recursing)
		return;

	/*
	 * If the target column is already marked NOT NULL, we can skip recursing
	 * to children, because their columns should already be marked NOT NULL as
	 * well.  But there's no point in checking here unless the relation has
	 * some children; else we can just wait till execution to check.  (If it
	 * does have children, however, this can save taking per-child locks
	 * unnecessarily.  This greatly improves concurrency in some parallel
	 * restore scenarios.)
	 *
	 * Unfortunately, we can only apply this optimization to partitioned
	 * tables, because traditional inheritance doesn't enforce that child
	 * columns be NOT NULL when their parent is.  (That's a bug that should
	 * get fixed someday.)
	 */
	if (rel->rd_rel->relhassubclass &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		HeapTuple	tuple;
		bool		attnotnull;

		tuple = SearchSysCacheAttName(RelationGetRelid(rel), cmd->name);

		/* Might as well throw the error now, if name is bad */
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							cmd->name, RelationGetRelationName(rel))));

		attnotnull = ((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull;
		ReleaseSysCache(tuple);
		if (attnotnull)
			return;
	}

	/*
	 * If we have ALTER TABLE ONLY ... SET NOT NULL on a partitioned table,
	 * apply ALTER TABLE ... CHECK NOT NULL to every child.  Otherwise, use
	 * normal recursion logic.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
		!recurse)
	{
		AlterTableCmd *newcmd = makeNode(AlterTableCmd);

		newcmd->subtype = AT_CheckNotNull;
		newcmd->name = pstrdup(cmd->name);
		ATSimpleRecursion(wqueue, rel, newcmd, true, lockmode, context);
	}
	else
		ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
}


/*
 * Return the address of the modified column.  If the column was already NOT
 * NULL, InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecSetNotNull(AlteredTableInfo *tab, Relation rel,
				 const char *colName, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	Relation	attr_rel;
	ObjectAddress address;

	/*
	 * lookup the attribute
	 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

	/* Prevent them from altering a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Okay, actually perform the catalog change ... if needed
	 */
	if (!((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
	{
		((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = true;

		CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

		/*
		 * Ordinarily phase 3 must ensure that no NULLs exist in columns that
		 * are set NOT NULL; however, if we can find a constraint which proves
		 * this then we can skip that.  We needn't bother looking if we've
		 * already found that we must verify some other NOT NULL constraint.
		 */
		if (!tab->verify_new_notnull &&
			!NotNullImpliedByRelConstraints(rel, (Form_pg_attribute) GETSTRUCT(tuple)))
		{
			/* Tell Phase 3 it needs to test the constraint */
			tab->verify_new_notnull = true;
		}

		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	table_close(attr_rel, RowExclusiveLock);

	return address;
}


/*
 * ALTER TABLE ALTER COLUMN CHECK NOT NULL
 *
 * This doesn't exist in the grammar, but we generate AT_CheckNotNull
 * commands against the partitions of a partitioned table if the user
 * writes ALTER TABLE ONLY ... SET NOT NULL on the partitioned table,
 * or tries to create a primary key on it (which internally creates
 * AT_SetNotNull on the partitioned table).   Such a command doesn't
 * allow us to actually modify any partition, but we want to let it
 * go through if the partitions are already properly marked.
 *
 * In future, this might need to adjust the child table's state, likely
 * by incrementing an inheritance count for the attnotnull constraint.
 * For now we need only check for the presence of the flag.
 */
static void
ATExecCheckNotNull(AlteredTableInfo *tab, Relation rel,
				   const char *colName, LOCKMODE lockmode)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	if (!((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("constraint must be added to child tables too"),
				 errdetail("Column \"%s\" of relation \"%s\" is not already NOT NULL.",
						   colName, RelationGetRelationName(rel)),
				 errhint("Do not specify the ONLY keyword.")));

	ReleaseSysCache(tuple);
}


/*
 * ALTER TABLE ALTER COLUMN DROP EXPRESSION
 */
static void
ATPrepDropExpression(Relation rel, AlterTableCmd *cmd, bool recurse, bool recursing, LOCKMODE lockmode)
{
	/*
	 * Reject ONLY if there are child tables.  We could implement this, but it
	 * is a bit complicated.  GENERATED clauses must be attached to the column
	 * definition and cannot be added later like DEFAULT, so if a child table
	 * has a generation expression that the parent does not have, the child
	 * column will necessarily be an attlocal column.  So to implement ONLY
	 * here, we'd need extra code to update attislocal of the direct child
	 * tables, somewhat similar to how DROP COLUMN does it, so that the
	 * resulting state can be properly dumped and restored.
	 */
	if (!recurse &&
		find_inheritance_children(RelationGetRelid(rel), lockmode))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / DROP EXPRESSION must be applied to child tables too")));

	/*
	 * Cannot drop generation expression from inherited columns.
	 */
	if (!recursing)
	{
		HeapTuple	tuple;
		Form_pg_attribute attTup;

		tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), cmd->name);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							cmd->name, RelationGetRelationName(rel))));

		attTup = (Form_pg_attribute) GETSTRUCT(tuple);

		if (attTup->attinhcount > 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop generation expression from inherited column")));
	}
}


/*
 * Return the address of the affected column.
 */
static ObjectAddress
ATExecDropExpression(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	ObjectAddress address;

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (attTup->attgenerated != ATTRIBUTE_GENERATED_STORED)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is not a stored generated column",
							colName, RelationGetRelationName(rel))));
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" is not a stored generated column, skipping",
							colName, RelationGetRelationName(rel))));
			heap_freetuple(tuple);
			table_close(attrelation, RowExclusiveLock);
			return InvalidObjectAddress;
		}
	}

	attTup->attgenerated = '\0';
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attTup->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	CommandCounterIncrement();

	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false, false);

	/*
	 * Remove all dependencies of this (formerly generated) column on other
	 * columns in the same table.  (See StoreAttrDefault() for which
	 * dependencies are created.)  We don't expect there to be dependencies
	 * between columns of the same table for other reasons, so it's okay to
	 * remove all of them.
	 */
	{
		Relation	depRel;
		ScanKeyData key[3];
		SysScanDesc scan;
		HeapTuple	tup;

		depRel = table_open(DependRelationId, RowExclusiveLock);

		ScanKeyInit(&key[0],
					Anum_pg_depend_classid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationRelationId));
		ScanKeyInit(&key[1],
					Anum_pg_depend_objid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(rel)));
		ScanKeyInit(&key[2],
					Anum_pg_depend_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(attnum));

		scan = systable_beginscan(depRel, DependDependerIndexId, true,
								  NULL, 3, key);

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

			if (depform->refclassid == RelationRelationId &&
				depform->refobjid == RelationGetRelid(rel) &&
				depform->refobjsubid != 0 &&
				depform->deptype == DEPENDENCY_AUTO)
			{
				CatalogTupleDelete(depRel, &tup->t_self);
			}
		}

		systable_endscan(scan);

		table_close(depRel, RowExclusiveLock);
	}

	return address;
}


/*
 * ALTER TABLE ALTER COLUMN SET STATISTICS
 *
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetStatistics(Relation rel, const char *colName, int16 colNum, Node *newValue, LOCKMODE lockmode)
{
	int			newtarget;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	ObjectAddress address;

	/*
	 * We allow referencing columns by numbers only for indexes, since table
	 * column numbers could contain gaps if columns are later dropped.
	 */
	if (rel->rd_rel->relkind != RELKIND_INDEX &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX &&
		!colName)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot refer to non-index column by number")));

	Assert(IsA(newValue, Integer));
	newtarget = intVal(newValue);

	/*
	 * Limit target to a sane range
	 */
	if (newtarget < -1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("statistics target %d is too low",
						newtarget)));
	}
	else if (newtarget > 10000)
	{
		newtarget = 10000;
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lowering statistics target to %d",
						newtarget)));
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	if (colName)
	{
		tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colName, RelationGetRelationName(rel))));
	}
	else
	{
		tuple = SearchSysCacheCopyAttNum(RelationGetRelid(rel), colNum);

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column number %d of relation \"%s\" does not exist",
							colNum, RelationGetRelationName(rel))));
	}

	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (rel->rd_rel->relkind == RELKIND_INDEX ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
	{
		if (attnum > rel->rd_index->indnkeyatts)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot alter statistics on included column \"%s\" of index \"%s\"",
							NameStr(attrtuple->attname), RelationGetRelationName(rel))));
		else if (rel->rd_index->indkey.values[attnum - 1] != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot alter statistics on non-expression column \"%s\" of index \"%s\"",
							NameStr(attrtuple->attname), RelationGetRelationName(rel)),
					 errhint("Alter statistics on table column instead.")));
	}

	attrtuple->attstattarget = newtarget;

	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	return address;
}


/*
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetOptions(Relation rel, const char *colName, Node *options,
				 bool isReset, LOCKMODE lockmode)
{
	Relation	attrelation;
	HeapTuple	tuple,
				newtuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	Datum		datum,
				newOptions;
	bool		isnull;
	ObjectAddress address;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/* Generate new proposed attoptions (text array) */
	datum = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions,
							&isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 castNode(List, options), NULL, NULL,
									 false, isReset);
	/* Validate new options */
	(void) attribute_reloptions(newOptions, true);

	/* Build new tuple. */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_attribute_attoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_attribute_attoptions - 1] = true;
	repl_repl[Anum_pg_attribute_attoptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrelation),
								 repl_val, repl_null, repl_repl);

	/* Update system catalog. */
	CatalogTupleUpdate(attrelation, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	table_close(attrelation, RowExclusiveLock);

	return address;
}


/*
 * ALTER TABLE ALTER COLUMN SET STORAGE
 *
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetStorage(Relation rel, const char *colName, Node *newValue, LOCKMODE lockmode)
{
	char	   *storagemode;
	char		newstorage;
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	ObjectAddress address;

	Assert(IsA(newValue, String));
	storagemode = strVal(newValue);

	if (pg_strcasecmp(storagemode, "plain") == 0)
		newstorage = TYPSTORAGE_PLAIN;
	else if (pg_strcasecmp(storagemode, "external") == 0)
		newstorage = TYPSTORAGE_EXTERNAL;
	else if (pg_strcasecmp(storagemode, "extended") == 0)
		newstorage = TYPSTORAGE_EXTENDED;
	else if (pg_strcasecmp(storagemode, "main") == 0)
		newstorage = TYPSTORAGE_MAIN;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid storage type \"%s\"",
						storagemode)));
		newstorage = 0;			/* keep compiler quiet */
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * safety check: do not allow toasted storage modes unless column datatype
	 * is TOAST-aware.
	 */
	if (newstorage == TYPSTORAGE_PLAIN || TypeIsToastable(attrtuple->atttypid))
		attrtuple->attstorage = newstorage;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s can only have storage PLAIN",
						format_type_be(attrtuple->atttypid))));

	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);

	heap_freetuple(tuple);

	/*
	 * Apply the change to indexes as well (only for simple index columns,
	 * matching behavior of index.c ConstructTupleDescriptor()).
	 */
	SetIndexStorageProperties(rel, attrelation, attnum,
							  true, newstorage,
							  false, 0,
							  lockmode);

	table_close(attrelation, RowExclusiveLock);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * ALTER TABLE ALTER COLUMN SET COMPRESSION
 *
 * Return value is the address of the modified column
 */
static ObjectAddress
ATExecSetCompression(AlteredTableInfo *tab,
					 Relation rel,
					 const char *column,
					 Node *newValue,
					 LOCKMODE lockmode)
{
	Relation	attrel;
	HeapTuple	tuple;
	Form_pg_attribute atttableform;
	AttrNumber	attnum;
	char	   *compression;
	char		cmethod;
	ObjectAddress address;

	Assert(IsA(newValue, String));
	compression = strVal(newValue);

	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* copy the cache entry so we can scribble on it below */
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), column);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						column, RelationGetRelationName(rel))));

	/* prevent them from altering a system attribute */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", column)));

	/*
	 * Check that column type is compressible, then get the attribute
	 * compression method code
	 */
	cmethod = GetAttributeCompression(atttableform->atttypid, compression);

	/* update pg_attribute entry */
	atttableform->attcompression = cmethod;
	CatalogTupleUpdate(attrel, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attnum);

	/*
	 * Apply the change to indexes as well (only for simple index columns,
	 * matching behavior of index.c ConstructTupleDescriptor()).
	 */
	SetIndexStorageProperties(rel, attrel, attnum,
							  false, 0,
							  true, cmethod,
							  lockmode);

	heap_freetuple(tuple);

	table_close(attrel, RowExclusiveLock);

	/* make changes visible */
	CommandCounterIncrement();

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * ALTER TABLE DROP COLUMN
 *
 * DROP COLUMN cannot use the normal ALTER TABLE recursion mechanism,
 * because we have to decide at runtime whether to recurse or not depending
 * on whether attinhcount goes to zero or not.  (We can't check this in a
 * static pre-pass because it won't handle multiple inheritance situations
 * correctly.)
 */
static void
ATPrepDropColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				 AlterTableCmd *cmd, LOCKMODE lockmode,
				 AlterTableUtilityContext *context)
{
	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop column from typed table")));

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);

	if (recurse)
		cmd->subtype = AT_DropColumnRecurse;
}


/*
 * Drops column 'colName' from relation 'rel' and returns the address of the
 * dropped column.  The column is also dropped (or marked as no longer
 * inherited from relation) from the relation's inheritance children, if any.
 *
 * In the recursive invocations for inheritance child relations, instead of
 * dropping the column directly (if to be dropped at all), its object address
 * is added to 'addrs', which must be non-NULL in such invocations.  All
 * columns are dropped at the same time after all the children have been
 * checked recursively.
 */
static ObjectAddress
ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing,
				 bool missing_ok, LOCKMODE lockmode,
				 ObjectAddresses *addrs)
{
	HeapTuple	tuple;
	Form_pg_attribute targetatt;
	AttrNumber	attnum;
	List	   *children;
	ObjectAddress object;
	bool		is_expr;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/* Initialize addrs on the first invocation */
	Assert(!recursing || addrs != NULL);
	if (!recursing)
		addrs = new_object_addresses();

	/*
	 * get the number of the attribute
	 */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colName, RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" does not exist, skipping",
							colName, RelationGetRelationName(rel))));
			return InvalidObjectAddress;
		}
	}
	targetatt = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = targetatt->attnum;

	/* Can't drop a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot drop system column \"%s\"",
						colName)));

	/*
	 * Don't drop inherited columns, unless recursing (presumably from a drop
	 * of the parent column)
	 */
	if (targetatt->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop inherited column \"%s\"",
						colName)));

	/*
	 * Don't drop columns used in the partition key, either.  (If we let this
	 * go through, the key column's dependencies would cause a cascaded drop
	 * of the whole table, which is surely not what the user expected.)
	 */
	if (has_partition_attrs(rel,
							bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
							&is_expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop column \"%s\" because it is part of the partition key of relation \"%s\"",
						colName, RelationGetRelationName(rel))));

	ReleaseSysCache(tuple);

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	if (children)
	{
		Relation	attr_rel;
		ListCell   *child;

		/*
		 * In case of a partitioned table, the column must be dropped from the
		 * partitions as well.
		 */
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE && !recurse)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop column from only the partitioned table when partitions exist"),
					 errhint("Do not specify the ONLY keyword.")));

		attr_rel = table_open(AttributeRelationId, RowExclusiveLock);
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;
			Form_pg_attribute childatt;

			/* find_inheritance_children already got lock */
			childrel = table_open(childrelid, NoLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");

			tuple = SearchSysCacheCopyAttName(childrelid, colName);
			if (!HeapTupleIsValid(tuple))	/* shouldn't happen */
				elog(ERROR, "cache lookup failed for attribute \"%s\" of relation %u",
					 colName, childrelid);
			childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (childatt->attinhcount <= 0) /* shouldn't happen */
				elog(ERROR, "relation %u has non-inherited attribute \"%s\"",
					 childrelid, colName);

			if (recurse)
			{
				/*
				 * If the child column has other definition sources, just
				 * decrement its inheritance count; if not, recurse to delete
				 * it.
				 */
				if (childatt->attinhcount == 1 && !childatt->attislocal)
				{
					/* Time to delete this child column, too */
					ATExecDropColumn(wqueue, childrel, colName,
									 behavior, true, true,
									 false, lockmode, addrs);
				}
				else
				{
					/* Child column must survive my deletion */
					childatt->attinhcount--;

					CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

					/* Make update visible */
					CommandCounterIncrement();
				}
			}
			else
			{
				/*
				 * If we were told to drop ONLY in this table (no recursion),
				 * we need to mark the inheritors' attributes as locally
				 * defined rather than inherited.
				 */
				childatt->attinhcount--;
				childatt->attislocal = true;

				CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

				/* Make update visible */
				CommandCounterIncrement();
			}

			heap_freetuple(tuple);

			table_close(childrel, NoLock);
		}
		table_close(attr_rel, RowExclusiveLock);
	}

	/* Add object to delete */
	object.classId = RelationRelationId;
	object.objectId = RelationGetRelid(rel);
	object.objectSubId = attnum;
	add_exact_object_address(&object, addrs);

	if (!recursing)
	{
		/* Recursion has ended, drop everything that was collected */
		performMultipleDeletions(addrs, behavior, 0);
		free_object_addresses(addrs);
	}

	return object;
}


/*
 * ALTER TABLE ADD INDEX
 *
 * There is no such command in the grammar, but parse_utilcmd.c converts
 * UNIQUE and PRIMARY KEY constraints into AT_AddIndex subcommands.  This lets
 * us schedule creation of the index at the appropriate time during ALTER.
 *
 * Return value is the address of the new index.
 */
static ObjectAddress
ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild, LOCKMODE lockmode)
{
	bool		check_rights;
	bool		skip_build;
	bool		quiet;
	ObjectAddress address;

	Assert(IsA(stmt, IndexStmt));
	Assert(!stmt->concurrent);

	/* The IndexStmt has already been through transformIndexStmt */
	Assert(stmt->transformed);

	/* suppress schema rights check when rebuilding existing index */
	check_rights = !is_rebuild;
	/* skip index build if phase 3 will do it or we're reusing an old one */
	skip_build = tab->rewrite > 0 || OidIsValid(stmt->oldNode);
	/* suppress notices when rebuilding existing index */
	quiet = is_rebuild;

	address = DefineIndex(RelationGetRelid(rel),
						  stmt,
						  InvalidOid,	/* no predefined OID */
						  InvalidOid,	/* no parent index */
						  InvalidOid,	/* no parent constraint */
						  true, /* is_alter_table */
						  check_rights,
						  false,	/* check_not_in_use - we did it already */
						  skip_build,
						  quiet);

	/*
	 * If TryReuseIndex() stashed a relfilenode for us, we used it for the new
	 * index instead of building from scratch.  Restore associated fields.
	 * This may store InvalidSubTransactionId in both fields, in which case
	 * relcache.c will assume it can rebuild the relcache entry.  Hence, do
	 * this after the CCI that made catalog rows visible to any rebuild.  The
	 * DROP of the old edition of this index will have scheduled the storage
	 * for deletion at commit, so cancel that pending deletion.
	 */
	if (OidIsValid(stmt->oldNode))
	{
		Relation	irel = index_open(address.objectId, NoLock);

		irel->rd_createSubid = stmt->oldCreateSubid;
		irel->rd_firstRelfilenodeSubid = stmt->oldFirstRelfilenodeSubid;
		RelationPreserveStorage(irel->rd_node, true);
		index_close(irel, NoLock);
	}

	return address;
}


/*
 * ALTER TABLE ADD STATISTICS
 *
 * This is no such command in the grammar, but we use this internally to add
 * AT_ReAddStatistics subcommands to rebuild extended statistics after a table
 * column type change.
 */
static ObjectAddress
ATExecAddStatistics(AlteredTableInfo *tab, Relation rel,
					CreateStatsStmt *stmt, bool is_rebuild, LOCKMODE lockmode)
{
	ObjectAddress address;

	Assert(IsA(stmt, CreateStatsStmt));

	/* The CreateStatsStmt has already been through transformStatsStmt */
	Assert(stmt->transformed);

	address = CreateStatistics(stmt);

	return address;
}


/*
 * ALTER TABLE ADD CONSTRAINT
 *
 * Return value is the address of the new constraint; if no constraint was
 * added, InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecAddConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					Constraint *newConstraint, bool recurse, bool is_readd,
					LOCKMODE lockmode)
{
	ObjectAddress address = InvalidObjectAddress;

	Assert(IsA(newConstraint, Constraint));

	/*
	 * Currently, we only expect to see CONSTR_CHECK and CONSTR_FOREIGN nodes
	 * arriving here (see the preprocessing done in parse_utilcmd.c).  Use a
	 * switch anyway to make it easier to add more code later.
	 */
	switch (newConstraint->contype)
	{
		case CONSTR_CHECK:
			address =
				ATAddCheckConstraint(wqueue, tab, rel,
									 newConstraint, recurse, false, is_readd,
									 lockmode);
			break;

		case CONSTR_FOREIGN:

			/*
			 * Assign or validate constraint name
			 */
			if (newConstraint->conname)
			{
				if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
										 RelationGetRelid(rel),
										 newConstraint->conname))
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("constraint \"%s\" for relation \"%s\" already exists",
									newConstraint->conname,
									RelationGetRelationName(rel))));
			}
			else
				newConstraint->conname =
					ChooseConstraintName(RelationGetRelationName(rel),
										 ChooseForeignKeyConstraintNameAddition(newConstraint->fk_attrs),
										 "fkey",
										 RelationGetNamespace(rel),
										 NIL);

			address = ATAddForeignKeyConstraint(wqueue, tab, rel,
												newConstraint,
												recurse, false,
												lockmode);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 (int) newConstraint->contype);
	}

	return address;
}


/*
 * ALTER TABLE ADD CONSTRAINT USING INDEX
 *
 * Returns the address of the new constraint.
 */
static ObjectAddress
ATExecAddIndexConstraint(AlteredTableInfo *tab, Relation rel,
						 IndexStmt *stmt, LOCKMODE lockmode)
{
	Oid			index_oid = stmt->indexOid;
	Relation	indexRel;
	char	   *indexName;
	IndexInfo  *indexInfo;
	char	   *constraintName;
	char		constraintType;
	ObjectAddress address;
	bits16		flags;

	Assert(IsA(stmt, IndexStmt));
	Assert(OidIsValid(index_oid));
	Assert(stmt->isconstraint);

	/*
	 * Doing this on partitioned tables is not a simple feature to implement,
	 * so let's punt for now.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX is not supported on partitioned tables")));

	indexRel = index_open(index_oid, AccessShareLock);

	indexName = pstrdup(RelationGetRelationName(indexRel));

	indexInfo = BuildIndexInfo(indexRel);

	/* this should have been checked at parse time */
	if (!indexInfo->ii_Unique)
		elog(ERROR, "index \"%s\" is not unique", indexName);

	/*
	 * Determine name to assign to constraint.  We require a constraint to
	 * have the same name as the underlying index; therefore, use the index's
	 * existing name as the default constraint name, and if the user
	 * explicitly gives some other name for the constraint, rename the index
	 * to match.
	 */
	constraintName = stmt->idxname;
	if (constraintName == NULL)
		constraintName = indexName;
	else if (strcmp(constraintName, indexName) != 0)
	{
		ereport(NOTICE,
				(errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX will rename index \"%s\" to \"%s\"",
						indexName, constraintName)));
		RenameRelationInternal(index_oid, constraintName, false, true);
	}

	/* Extra checks needed if making primary key */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, true, stmt);

	/* Note we currently don't support EXCLUSION constraints here */
	if (stmt->primary)
		constraintType = CONSTRAINT_PRIMARY;
	else
		constraintType = CONSTRAINT_UNIQUE;

	/* Create the catalog entries for the constraint */
	flags = INDEX_CONSTR_CREATE_UPDATE_INDEX |
		INDEX_CONSTR_CREATE_REMOVE_OLD_DEPS |
		(stmt->initdeferred ? INDEX_CONSTR_CREATE_INIT_DEFERRED : 0) |
		(stmt->deferrable ? INDEX_CONSTR_CREATE_DEFERRABLE : 0) |
		(stmt->primary ? INDEX_CONSTR_CREATE_MARK_AS_PRIMARY : 0);

	address = index_constraint_create(rel,
									  index_oid,
									  InvalidOid,
									  indexInfo,
									  constraintName,
									  constraintType,
									  flags,
									  allowSystemTableMods,
									  false);	/* is_internal */

	index_close(indexRel, NoLock);

	return address;
}


/*
 * ALTER TABLE ALTER CONSTRAINT
 *
 * Update the attributes of a constraint.
 *
 * Currently only works for Foreign Key constraints.
 *
 * If the constraint is modified, returns its address; otherwise, return
 * InvalidObjectAddress.
 */
static ObjectAddress
ATExecAlterConstraint(Relation rel, AlterTableCmd *cmd, bool recurse,
					  bool recursing, LOCKMODE lockmode)
{
	Constraint *cmdcon;
	Relation	conrel;
	Relation	tgrel;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	contuple;
	Form_pg_constraint currcon;
	ObjectAddress address;
	List	   *otherrelids = NIL;
	ListCell   *lc;

	cmdcon = castNode(Constraint, cmd->def);

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Find and check the target constraint
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(cmdcon->conname));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* There can be at most one matching row */
	if (!HeapTupleIsValid(contuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						cmdcon->conname, RelationGetRelationName(rel))));

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	if (currcon->contype != CONSTRAINT_FOREIGN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of relation \"%s\" is not a foreign key constraint",
						cmdcon->conname, RelationGetRelationName(rel))));

	/*
	 * If it's not the topmost constraint, raise an error.
	 *
	 * Altering a non-topmost constraint leaves some triggers untouched, since
	 * they are not directly connected to this constraint; also, pg_dump would
	 * ignore the deferrability status of the individual constraint, since it
	 * only dumps topmost constraints.  Avoid these problems by refusing this
	 * operation and telling the user to alter the parent constraint instead.
	 */
	if (OidIsValid(currcon->conparentid))
	{
		HeapTuple	tp;
		Oid			parent = currcon->conparentid;
		char	   *ancestorname = NULL;
		char	   *ancestortable = NULL;

		/* Loop to find the topmost constraint */
		while (HeapTupleIsValid(tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(parent))))
		{
			Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);

			/* If no parent, this is the constraint we want */
			if (!OidIsValid(contup->conparentid))
			{
				ancestorname = pstrdup(NameStr(contup->conname));
				ancestortable = get_rel_name(contup->conrelid);
				ReleaseSysCache(tp);
				break;
			}

			parent = contup->conparentid;
			ReleaseSysCache(tp);
		}

		ereport(ERROR,
				(errmsg("cannot alter constraint \"%s\" on relation \"%s\"",
						cmdcon->conname, RelationGetRelationName(rel)),
				 ancestorname && ancestortable ?
				 errdetail("Constraint \"%s\" is derived from constraint \"%s\" of relation \"%s\".",
						   cmdcon->conname, ancestorname, ancestortable) : 0,
				 errhint("You may alter the constraint it derives from, instead.")));
	}

	/*
	 * Do the actual catalog work.  We can skip changing if already in the
	 * desired state, but not if a partitioned table: partitions need to be
	 * processed regardless, in case they had the constraint locally changed.
	 */
	address = InvalidObjectAddress;
	if (currcon->condeferrable != cmdcon->deferrable ||
		currcon->condeferred != cmdcon->initdeferred ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		if (ATExecAlterConstrRecurse(cmdcon, conrel, tgrel, rel, contuple,
									 &otherrelids, lockmode))
			ObjectAddressSet(address, ConstraintRelationId, currcon->oid);
	}

	/*
	 * ATExecConstrRecurse already invalidated relcache for the relations
	 * having the constraint itself; here we also invalidate for relations
	 * that have any triggers that are part of the constraint.
	 */
	foreach(lc, otherrelids)
		CacheInvalidateRelcacheByRelid(lfirst_oid(lc));

	systable_endscan(scan);

	table_close(tgrel, RowExclusiveLock);
	table_close(conrel, RowExclusiveLock);

	return address;
}


/*
 * Recursive subroutine of ATExecAlterConstraint.  Returns true if the
 * constraint is altered.
 *
 * *otherrelids is appended OIDs of relations containing affected triggers.
 *
 * Note that we must recurse even when the values are correct, in case
 * indirect descendants have had their constraints altered locally.
 * (This could be avoided if we forbade altering constraints in partitions
 * but existing releases don't do that.)
 */
static bool
ATExecAlterConstrRecurse(Constraint *cmdcon, Relation conrel, Relation tgrel,
						 Relation rel, HeapTuple contuple, List **otherrelids,
						 LOCKMODE lockmode)
{
	Form_pg_constraint currcon;
	Oid			conoid;
	Oid			refrelid;
	bool		changed = false;

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	conoid = currcon->oid;
	refrelid = currcon->confrelid;

	/*
	 * Update pg_constraint with the flags from cmdcon.
	 *
	 * If called to modify a constraint that's already in the desired state,
	 * silently do nothing.
	 */
	if (currcon->condeferrable != cmdcon->deferrable ||
		currcon->condeferred != cmdcon->initdeferred)
	{
		HeapTuple	copyTuple;
		Form_pg_constraint copy_con;
		HeapTuple	tgtuple;
		ScanKeyData tgkey;
		SysScanDesc tgscan;

		copyTuple = heap_copytuple(contuple);
		copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
		copy_con->condeferrable = cmdcon->deferrable;
		copy_con->condeferred = cmdcon->initdeferred;
		CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

		InvokeObjectPostAlterHook(ConstraintRelationId,
								  conoid, 0);

		heap_freetuple(copyTuple);
		changed = true;

		/* Make new constraint flags visible to others */
		CacheInvalidateRelcache(rel);

		/*
		 * Now we need to update the multiple entries in pg_trigger that
		 * implement the constraint.
		 */
		ScanKeyInit(&tgkey,
					Anum_pg_trigger_tgconstraint,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(conoid));
		tgscan = systable_beginscan(tgrel, TriggerConstraintIndexId, true,
									NULL, 1, &tgkey);
		while (HeapTupleIsValid(tgtuple = systable_getnext(tgscan)))
		{
			Form_pg_trigger tgform = (Form_pg_trigger) GETSTRUCT(tgtuple);
			Form_pg_trigger copy_tg;
			HeapTuple	copyTuple;

			/*
			 * Remember OIDs of other relation(s) involved in FK constraint.
			 * (Note: it's likely that we could skip forcing a relcache inval
			 * for other rels that don't have a trigger whose properties
			 * change, but let's be conservative.)
			 */
			if (tgform->tgrelid != RelationGetRelid(rel))
				*otherrelids = list_append_unique_oid(*otherrelids,
													  tgform->tgrelid);

			/*
			 * Update deferrability of RI_FKey_noaction_del,
			 * RI_FKey_noaction_upd, RI_FKey_check_ins and RI_FKey_check_upd
			 * triggers, but not others; see createForeignKeyActionTriggers
			 * and CreateFKCheckTrigger.
			 */
			if (tgform->tgfoid != F_RI_FKEY_NOACTION_DEL &&
				tgform->tgfoid != F_RI_FKEY_NOACTION_UPD &&
				tgform->tgfoid != F_RI_FKEY_CHECK_INS &&
				tgform->tgfoid != F_RI_FKEY_CHECK_UPD)
				continue;

			copyTuple = heap_copytuple(tgtuple);
			copy_tg = (Form_pg_trigger) GETSTRUCT(copyTuple);

			copy_tg->tgdeferrable = cmdcon->deferrable;
			copy_tg->tginitdeferred = cmdcon->initdeferred;
			CatalogTupleUpdate(tgrel, &copyTuple->t_self, copyTuple);

			InvokeObjectPostAlterHook(TriggerRelationId, tgform->oid, 0);

			heap_freetuple(copyTuple);
		}

		systable_endscan(tgscan);
	}

	/*
	 * If the table at either end of the constraint is partitioned, we need to
	 * recurse and handle every constraint that is a child of this one.
	 *
	 * (This assumes that the recurse flag is forcibly set for partitioned
	 * tables, and not set for legacy inheritance, though we don't check for
	 * that here.)
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
		get_rel_relkind(refrelid) == RELKIND_PARTITIONED_TABLE)
	{
		ScanKeyData pkey;
		SysScanDesc pscan;
		HeapTuple	childtup;

		ScanKeyInit(&pkey,
					Anum_pg_constraint_conparentid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(conoid));

		pscan = systable_beginscan(conrel, ConstraintParentIndexId,
								   true, NULL, 1, &pkey);

		while (HeapTupleIsValid(childtup = systable_getnext(pscan)))
		{
			Form_pg_constraint childcon = (Form_pg_constraint) GETSTRUCT(childtup);
			Relation	childrel;

			childrel = table_open(childcon->conrelid, lockmode);
			ATExecAlterConstrRecurse(cmdcon, conrel, tgrel, childrel, childtup,
									 otherrelids, lockmode);
			table_close(childrel, NoLock);
		}

		systable_endscan(pscan);
	}

	return changed;
}


/*
 * ALTER TABLE VALIDATE CONSTRAINT
 *
 * XXX The reason we handle recursion here rather than at Phase 1 is because
 * there's no good way to skip recursing when handling foreign keys: there is
 * no need to lock children in that case, yet we wouldn't be able to avoid
 * doing so at that level.
 *
 * Return value is the address of the validated constraint.  If the constraint
 * was already validated, InvalidObjectAddress is returned.
 */
static ObjectAddress
ATExecValidateConstraint(List **wqueue, Relation rel, char *constrName,
						 bool recurse, bool recursing, LOCKMODE lockmode)
{
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	tuple;
	Form_pg_constraint con;
	ObjectAddress address;

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and check the target constraint
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* There can be at most one matching row */
	if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						constrName, RelationGetRelationName(rel))));

	con = (Form_pg_constraint) GETSTRUCT(tuple);
	if (con->contype != CONSTRAINT_FOREIGN &&
		con->contype != CONSTRAINT_CHECK)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of relation \"%s\" is not a foreign key or check constraint",
						constrName, RelationGetRelationName(rel))));

	if (!con->convalidated)
	{
		AlteredTableInfo *tab;
		HeapTuple	copyTuple;
		Form_pg_constraint copy_con;

		if (con->contype == CONSTRAINT_FOREIGN)
		{
			NewConstraint *newcon;
			Constraint *fkconstraint;

			/* Queue validation for phase 3 */
			fkconstraint = makeNode(Constraint);
			/* for now this is all we need */
			fkconstraint->conname = constrName;

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = constrName;
			newcon->contype = CONSTR_FOREIGN;
			newcon->refrelid = con->confrelid;
			newcon->refindid = con->conindid;
			newcon->conid = con->oid;
			newcon->qual = (Node *) fkconstraint;

			/* Find or create work queue entry for this table */
			tab = ATGetQueueEntry(wqueue, rel);
			tab->constraints = lappend(tab->constraints, newcon);

			/*
			 * We disallow creating invalid foreign keys to or from
			 * partitioned tables, so ignoring the recursion bit is okay.
			 */
		}
		else if (con->contype == CONSTRAINT_CHECK)
		{
			List	   *children = NIL;
			ListCell   *child;
			NewConstraint *newcon;
			bool		isnull;
			Datum		val;
			char	   *conbin;

			/*
			 * If we're recursing, the parent has already done this, so skip
			 * it.  Also, if the constraint is a NO INHERIT constraint, we
			 * shouldn't try to look for it in the children.
			 */
			if (!recursing && !con->connoinherit)
				children = find_all_inheritors(RelationGetRelid(rel),
											   lockmode, NULL);

			/*
			 * For CHECK constraints, we must ensure that we only mark the
			 * constraint as validated on the parent if it's already validated
			 * on the children.
			 *
			 * We recurse before validating on the parent, to reduce risk of
			 * deadlocks.
			 */
			foreach(child, children)
			{
				Oid			childoid = lfirst_oid(child);
				Relation	childrel;

				if (childoid == RelationGetRelid(rel))
					continue;

				/*
				 * If we are told not to recurse, there had better not be any
				 * child tables, because we can't mark the constraint on the
				 * parent valid unless it is valid for all child tables.
				 */
				if (!recurse)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("constraint must be validated on child tables too")));

				/* find_all_inheritors already got lock */
				childrel = table_open(childoid, NoLock);

				ATExecValidateConstraint(wqueue, childrel, constrName, false,
										 true, lockmode);
				table_close(childrel, NoLock);
			}

			/* Queue validation for phase 3 */
			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = constrName;
			newcon->contype = CONSTR_CHECK;
			newcon->refrelid = InvalidOid;
			newcon->refindid = InvalidOid;
			newcon->conid = con->oid;

			val = SysCacheGetAttr(CONSTROID, tuple,
								  Anum_pg_constraint_conbin, &isnull);
			if (isnull)
				elog(ERROR, "null conbin for constraint %u", con->oid);

			conbin = TextDatumGetCString(val);
			newcon->qual = (Node *) stringToNode(conbin);

			/* Find or create work queue entry for this table */
			tab = ATGetQueueEntry(wqueue, rel);
			tab->constraints = lappend(tab->constraints, newcon);

			/*
			 * Invalidate relcache so that others see the new validated
			 * constraint.
			 */
			CacheInvalidateRelcache(rel);
		}

		/*
		 * Now update the catalog, while we have the door open.
		 */
		copyTuple = heap_copytuple(tuple);
		copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
		copy_con->convalidated = true;
		CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

		InvokeObjectPostAlterHook(ConstraintRelationId, con->oid, 0);

		heap_freetuple(copyTuple);

		ObjectAddressSet(address, ConstraintRelationId, con->oid);
	}
	else
		address = InvalidObjectAddress; /* already validated */

	systable_endscan(scan);

	table_close(conrel, RowExclusiveLock);

	return address;
}


/*
 * ALTER TABLE DROP CONSTRAINT
 *
 * Like DROP COLUMN, we can't use the normal ALTER TABLE recursion mechanism.
 */
static void
ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior,
					 bool recurse, bool recursing,
					 bool missing_ok, LOCKMODE lockmode)
{
	List	   *children;
	ListCell   *child;
	Relation	conrel;
	Form_pg_constraint con;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	tuple;
    AttrNumber autoIncAttrNum;
    List *autoIncAttrIndexs;
	bool		found = false;
	bool		is_no_inherit_constraint = false;
	char		contype;
    char *tempConstrName;

    autoIncAttrIndexs = NIL;
    autoIncAttrNum = mysGetTableAutoIncColNum(rel);
    if (autoIncAttrNum > 0)
    {
        autoIncAttrIndexs = getColumnIndexs(rel, get_attname(rel->rd_id, autoIncAttrNum, false));
    }

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

    tempConstrName = mysCheckConstraintName(rel->rd_id, (char *)constrName);

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and drop the target constraint
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tempConstrName));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* There can be at most one matching row */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress conobj;

		con = (Form_pg_constraint) GETSTRUCT(tuple);

		/* Don't drop inherited constraints */
		if (con->coninhcount > 0 && !recursing)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop inherited constraint \"%s\" of relation \"%s\"",
							constrName, RelationGetRelationName(rel))));

		is_no_inherit_constraint = con->connoinherit;
		contype = con->contype;

		/*
		 * If it's a foreign-key constraint, we'd better lock the referenced
		 * table and check that that's not in use, just as we've already done
		 * for the constrained table (else we might, eg, be dropping a trigger
		 * that has unfired events).  But we can/must skip that in the
		 * self-referential case.
		 */
		if (contype == CONSTRAINT_FOREIGN &&
			con->confrelid != RelationGetRelid(rel))
		{
			Relation	frel;

			/* Must match lock taken by RemoveTriggerById: */
			frel = table_open(con->confrelid, AccessExclusiveLock);
			CheckTableNotInUse(frel, "ALTER TABLE");
			table_close(frel, NoLock);
		}

        if (contype == CONSTRAINT_PRIMARY || contype == CONSTRAINT_UNIQUE)
        {
            Datum arrayDatum;
            ArrayType *array;
            int numkeys;
            AttrNumber *conkeys;
            bool isNull;

            if (autoIncAttrNum > 0)
            {
                arrayDatum = SysCacheGetAttr(CONSTROID, tuple, Anum_pg_constraint_conkey, &isNull);

                if (!isNull)
                {
                    int i = 0;

                    array = DatumGetArrayTypeP(arrayDatum);

                    if (ARR_NDIM(array) != 1 ||
                        ARR_HASNULL(array) ||
                        ARR_ELEMTYPE(array) != INT2OID)
                    {
                        elog(ERROR, "conkey is not a 1-D smallint array");
                    }

                    numkeys = ARR_DIMS(array)[0];
                    if (numkeys <= 0 || numkeys > INDEX_MAX_KEYS)
                    {
                        elog(ERROR, "constraint \"%s\" cannot have %d columns", constrName, numkeys);
                    }

                    conkeys = (AttrNumber *)ARR_DATA_PTR(array);
                    for (i = 0; i < numkeys; i++)
                    {
                        if (conkeys[i] == autoIncAttrNum)
                        {
                            if (list_length(autoIncAttrIndexs) <= 1)
                            {
                                elog(ERROR, 
                                    "Incorrect table definition; the auto increment column \"%s\" of table \"%s\" must be defined as a key",
                                    get_attname(rel->rd_id, autoIncAttrNum, false), RelationGetRelationName(rel));
                            }
                            else
                            {
                                break;
                            }
                        }
                    }
                }
            }
        }

		/*
		 * Perform the actual constraint deletion
		 */
		conobj.classId = ConstraintRelationId;
		conobj.objectId = con->oid;
		conobj.objectSubId = 0;

		performDeletion(&conobj, behavior, 0);

		found = true;
	}

	systable_endscan(scan);

	if (!found)
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
							constrName, RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("constraint \"%s\" of relation \"%s\" does not exist, skipping",
							constrName, RelationGetRelationName(rel))));
			table_close(conrel, RowExclusiveLock);
			return;
		}
	}

	/*
	 * For partitioned tables, non-CHECK inherited constraints are dropped via
	 * the dependency mechanism, so we're done here.
	 */
	if (contype != CONSTRAINT_CHECK &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		table_close(conrel, RowExclusiveLock);
		return;
	}

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	if (!is_no_inherit_constraint)
		children = find_inheritance_children(RelationGetRelid(rel), lockmode);
	else
		children = NIL;

	/*
	 * For a partitioned table, if partitions exist and we are told not to
	 * recurse, it's a user error.  It doesn't make sense to have a constraint
	 * be defined only on the parent, especially if it's a partitioned table.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
		children != NIL && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot remove constraint from only the partitioned table when partitions exist"),
				 errhint("Do not specify the ONLY keyword.")));

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		HeapTuple	copy_tuple;

		/* find_inheritance_children already got lock */
		childrel = table_open(childrelid, NoLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");

		ScanKeyInit(&skey[0],
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(childrelid));
		ScanKeyInit(&skey[1],
					Anum_pg_constraint_contypid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(InvalidOid));
		ScanKeyInit(&skey[2],
					Anum_pg_constraint_conname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(tempConstrName));
		scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
								  true, NULL, 3, skey);

		/* There can be at most one matching row */
		if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
							constrName,
							RelationGetRelationName(childrel))));

		copy_tuple = heap_copytuple(tuple);

		systable_endscan(scan);

		con = (Form_pg_constraint) GETSTRUCT(copy_tuple);

		/* Right now only CHECK constraints can be inherited */
		if (con->contype != CONSTRAINT_CHECK)
			elog(ERROR, "inherited constraint is not a CHECK constraint");

		if (con->coninhcount <= 0)	/* shouldn't happen */
			elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
				 childrelid, constrName);

		if (recurse)
		{
			/*
			 * If the child constraint has other definition sources, just
			 * decrement its inheritance count; if not, recurse to delete it.
			 */
			if (con->coninhcount == 1 && !con->conislocal)
			{
				/* Time to delete this child constraint, too */
				ATExecDropConstraint(childrel, tempConstrName, behavior,
									 true, true,
									 false, lockmode);
			}
			else
			{
				/* Child constraint must survive my deletion */
				con->coninhcount--;
				CatalogTupleUpdate(conrel, &copy_tuple->t_self, copy_tuple);

				/* Make update visible */
				CommandCounterIncrement();
			}
		}
		else
		{
			/*
			 * If we were told to drop ONLY in this table (no recursion), we
			 * need to mark the inheritors' constraints as locally defined
			 * rather than inherited.
			 */
			con->coninhcount--;
			con->conislocal = true;

			CatalogTupleUpdate(conrel, &copy_tuple->t_self, copy_tuple);

			/* Make update visible */
			CommandCounterIncrement();
		}

		heap_freetuple(copy_tuple);

		table_close(childrel, NoLock);
	}

	table_close(conrel, RowExclusiveLock);
}


/*
 * ALTER TABLE DROP PRIMARY KEY
 *
 * Like DROP CONSTRAINT.
 *      1. 继承表：子表当前只会继承父表的check, not null，在父表新建或删除check,not null，也会操作子表
 *      2. 分区表：删除父表的Primary key, 可以根据依赖关系删除子表的一些对象
 */
static void
ATExecDropPrimaryKey(Relation rel, DropBehavior behavior, bool missing_ok, LOCKMODE lockmode)
{
	Relation conrel;
	Form_pg_constraint con;
	SysScanDesc scan;
	ScanKeyData skey[2];
	HeapTuple tuple;
    AttrNumber autoIncAttrNum;
    List *autoIncAttrIndexs;
	bool found = false;

    autoIncAttrIndexs = NIL;
    autoIncAttrNum = mysGetTableAutoIncColNum(rel);
    if (autoIncAttrNum > 0)
    {
        autoIncAttrIndexs = getColumnIndexs(rel, get_attname(rel->rd_id, autoIncAttrNum, false));
    }

    conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));

    scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 2, skey);
    
    /* There can be at most one matching row(表最多只能有一个primary key) */
    while (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        ObjectAddress conobj;
        Datum arrayDatum;
        ArrayType *array;
        int numkeys;
        AttrNumber *conkeys;
        bool isNull;

		con = (Form_pg_constraint) GETSTRUCT(tuple);

        if (con->contype != CONSTRAINT_PRIMARY)
            continue;

        if (con->coninhcount > 0)
            ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop inherited primary key of relation \"%s\"",
							RelationGetRelationName(rel))));
        
        if (autoIncAttrNum > 0)
        {
            arrayDatum = SysCacheGetAttr(CONSTROID, tuple, Anum_pg_constraint_conkey, &isNull);

            if (!isNull)
		    {
                int i = 0;

                array = DatumGetArrayTypeP(arrayDatum);

                if (ARR_NDIM(array) != 1 ||
                    ARR_HASNULL(array) ||
                    ARR_ELEMTYPE(array) != INT2OID)
                {
                    elog(ERROR, "conkey is not a 1-D smallint array");
                }

                numkeys = ARR_DIMS(array)[0];
	            if (numkeys <= 0 || numkeys > INDEX_MAX_KEYS)
                {
                    elog(ERROR, "primary key constraint cannot have %d columns", numkeys);
                }

                conkeys = (AttrNumber *)ARR_DATA_PTR(array);
                for (i = 0; i < numkeys; i++)
                {
                    if (conkeys[i] == autoIncAttrNum)
                    {
                        if (list_length(autoIncAttrIndexs) <= 1)
                        {
                            elog(ERROR, 
                                 "Incorrect table definition; the auto increment column \"%s\" of table \"%s\" must be defined as a key",
                                 get_attname(rel->rd_id, autoIncAttrNum, false), RelationGetRelationName(rel));
                        }
                        else
                        {
                            break;
                        }
                    }
                }
            }
        }
        
        /*
		 * Perform the actual constraint deletion
		 */
		conobj.classId = ConstraintRelationId;
		conobj.objectId = con->oid;
		conobj.objectSubId = 0;

		performDeletion(&conobj, behavior, 0);

		found = true;
    }

    systable_endscan(scan);

    if (!found)
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint primary key of relation \"%s\" does not exist",
							RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("constraint primary key of relation \"%s\" does not exist, skipping",
							RelationGetRelationName(rel))));
			table_close(conrel, RowExclusiveLock);
			return;
		}
	}

    table_close(conrel, RowExclusiveLock);
	return;
}


/*
 * ALTER TABLE DROP INDEX
 *
 * Mysql: 可以删除primary key, unique key, 还有一般的Index
 * 
 * Halo: 索引与约束是不同的对象，部分约束使用索引实现(使用索引实现的约束，两者名字相同)
 *      1.继承表只能继承检查约束与null/not null，此处不用处理
 *      2.分区表继承的各种属性，由依赖关系删除
 * 
 */
static void
ATExecDropIndex(Relation rel, char *indexName, DropBehavior behavior, bool missing_ok, LOCKMODE lockmode)
{
    HeapTuple indexClassTuple;
    Form_pg_class indexClassForm;
    Oid indexOid;
    Oid conOid;
    ObjectAddress dropObj;
    AttrNumber autoIncAttrNum;
    List *autoIncAttrIndexs;
    char *tempIndexName;

    autoIncAttrIndexs = NIL;
    autoIncAttrNum = mysGetTableAutoIncColNum(rel);
    if (autoIncAttrNum > 0)
    {
        autoIncAttrIndexs = getColumnIndexs(rel, get_attname(rel->rd_id, autoIncAttrNum, false));
    }

    tempIndexName = mysCheckIndexName(RelationGetRelid(rel), indexName);
    /* Look for the index in the same schema as the table */
    indexClassTuple = SearchSysCache2(RELNAMENSP, CStringGetDatum(tempIndexName), ObjectIdGetDatum(RelationGetNamespace(rel)));

    if (!HeapTupleIsValid(indexClassTuple))
    {
        if (missing_ok)
        {
            ereport(NOTICE,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("index \"%s\" does not exist", indexName)
                    ));
            return ;
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("index \"%s\" does not exist", indexName)
                    ));
        }
    }

    indexClassForm = (Form_pg_class) GETSTRUCT(indexClassTuple);

    if (indexClassForm->relkind != RELKIND_INDEX && indexClassForm->relkind != RELKIND_PARTITIONED_INDEX)
        ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", indexName)));

    indexOid = indexClassForm->oid;

    ReleaseSysCache(indexClassTuple);

    // TODO: 检查该索引所属表是否是此次操作的目标表
    if (IndexGetRelation(indexOid, true) != RelationGetRelid(rel))
    {
        if (missing_ok)
        {
            ereport(NOTICE,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("index \"%s\" on table \"%s\" does not exist", indexName, RelationGetRelationName(rel))
                    ));
            return ;
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("index \"%s\" on table \"%s\" does not exist", indexName, RelationGetRelationName(rel))
                    ));
        }
    }

    if (autoIncAttrNum > 0)
    {
        Relation indexRel;
        int i = 0;

        indexRel = index_open(indexOid, AccessShareLock);
        
        for (i = 0; i < indexRel->rd_index->indnatts; i++)
        {
            if (autoIncAttrNum == indexRel->rd_index->indkey.values[i])
            {
                if (list_length(autoIncAttrIndexs) <= 1)
                {
                    elog(ERROR, 
                         "Incorrect table definition; the auto increment column \"%s\" of table \"%s\" must be defined as a key",
                         get_attname(rel->rd_id, autoIncAttrNum, false), RelationGetRelationName(rel));
                }
                else
                {
                    break;
                }
            }
        }

        index_close(indexRel, AccessShareLock);
    }

    if (OidIsValid(conOid = get_index_constraint(indexOid)))
    {
        dropObj.classId = ConstraintRelationId;
		dropObj.objectId = conOid;
		dropObj.objectSubId = 0;
    }
    else
    {
        dropObj.classId = RelationRelationId;
		dropObj.objectId = indexOid;
		dropObj.objectSubId = 0;
    }

    /* 
     * 1. for inherit table, Index can't be inherited
     * 2. for partition table, inherited indexs are dropped via the dependency mechanism
     */
    performDeletion(&dropObj, behavior, 0);

    return;
}


static void
ATExecDropForeignKey(Relation rel, char *fkeyName, DropBehavior behavior, bool missing_ok, LOCKMODE lockmode)
{
    Relation conrel;
    HeapTuple conTuple;
    Form_pg_constraint con;
    SysScanDesc scan;
	ScanKeyData skey[3];
    bool found = false;

    conrel = table_open(ConstraintRelationId, RowExclusiveLock);

    ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(fkeyName));
    
    scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);
    
    if (HeapTupleIsValid(conTuple = systable_getnext(scan)))
    {
        ObjectAddress conobj;

        con = (Form_pg_constraint) GETSTRUCT(conTuple);

        if (con->contype != CONSTRAINT_FOREIGN)
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("\"%s\" is not a foreign key", fkeyName)));
        
        if (con->coninhcount > 0)
            ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop inherited foreign key of relation \"%s\"",
							RelationGetRelationName(rel))));
        
        if (con->confrelid != RelationGetRelid(rel))
		{
			Relation frel;

			/* Must match lock taken by RemoveTriggerById: */
			frel = table_open(con->confrelid, AccessExclusiveLock);
			CheckTableNotInUse(frel, "ALTER TABLE");
			table_close(frel, NoLock);
		}

        conobj.classId = ConstraintRelationId;
        conobj.objectId = con->oid;
        conobj.objectSubId = 0;

        performDeletion(&conobj, behavior, 0);

        found = true;
    }

    systable_endscan(scan);

    if (!found)
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint foreign key of relation \"%s\" does not exist",
							RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("constraint foreign key of relation \"%s\" does not exist, skipping",
							RelationGetRelationName(rel))));
			table_close(conrel, RowExclusiveLock);
			return;
		}
	}

    table_close(conrel, RowExclusiveLock);
	return;
}


static void
ATExecDropCheck(Relation rel, char *checkName, DropBehavior behavior, bool recurse, bool recursing,
                bool missing_ok, LOCKMODE lockmode)
{
    List *children;
	ListCell *child;
	Relation conrel;
	Form_pg_constraint con;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple tuple;
	bool found = false;
	bool is_no_inherit_constraint = false;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and drop the target constraint
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(checkName));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* There can be at most one matching row */
    if (HeapTupleIsValid(tuple = systable_getnext(scan)))
    {
        ObjectAddress conobj;
        con = (Form_pg_constraint) GETSTRUCT(tuple);

        if (con->contype != CONSTRAINT_CHECK)
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("\"%s\" is not a check constraint", checkName)));

        /* Don't drop inherited constraints */
		if (con->coninhcount > 0 && !recursing)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop inherited check constraint \"%s\" of relation \"%s\"",
							checkName, RelationGetRelationName(rel))));

		is_no_inherit_constraint = con->connoinherit;

		/*
		 * Perform the actual constraint deletion
		 */
		conobj.classId = ConstraintRelationId;
		conobj.objectId = con->oid;
		conobj.objectSubId = 0;

		performDeletion(&conobj, behavior, 0);

		found = true;
    }

    systable_endscan(scan);

    if (!found)
    {
        if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("check constraint of relation \"%s\" does not exist",
							RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("check constraint of relation \"%s\" does not exist, skipping",
							RelationGetRelationName(rel))));
			table_close(conrel, RowExclusiveLock);
			return;
		}
    }

    /*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	if (!is_no_inherit_constraint)
		children = find_inheritance_children(RelationGetRelid(rel), lockmode);
	else
		children = NIL;

	/*
	 * For a partitioned table, if partitions exist and we are told not to
	 * recurse, it's a user error.  It doesn't make sense to have a constraint
	 * be defined only on the parent, especially if it's a partitioned table.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
		children != NIL && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot remove check constraint from only the partitioned table when partitions exist"),
				 errhint("Do not specify the ONLY keyword.")));

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		HeapTuple	copy_tuple;

		/* find_inheritance_children already got lock */
		childrel = table_open(childrelid, NoLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");

		ScanKeyInit(&skey[0],
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(childrelid));
		ScanKeyInit(&skey[1],
					Anum_pg_constraint_contypid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(InvalidOid));
		ScanKeyInit(&skey[2],
					Anum_pg_constraint_conname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(checkName));
		scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
								  true, NULL, 3, skey);

		/* There can be at most one matching row */
		if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("check constraint \"%s\" of relation \"%s\" does not exist",
							checkName,
							RelationGetRelationName(childrel))));

		copy_tuple = heap_copytuple(tuple);

		systable_endscan(scan);

		con = (Form_pg_constraint) GETSTRUCT(copy_tuple);

		/* Right now only CHECK constraints can be inherited */
		if (con->contype != CONSTRAINT_CHECK)
			elog(ERROR, "inherited constraint is not a CHECK constraint");

		if (con->coninhcount <= 0)	/* shouldn't happen */
			elog(ERROR, "relation %u has non-inherited check constraint \"%s\"",
				 childrelid, checkName);

		if (recurse)
		{
			/*
			 * If the child constraint has other definition sources, just
			 * decrement its inheritance count; if not, recurse to delete it.
			 */
			if (con->coninhcount == 1 && !con->conislocal)
			{
				/* Time to delete this child constraint, too */
				ATExecDropCheck(childrel, checkName, behavior,
                                true, true,
                                false, lockmode);
			}
			else
			{
				/* Child constraint must survive my deletion */
				con->coninhcount--;
				CatalogTupleUpdate(conrel, &copy_tuple->t_self, copy_tuple);

				/* Make update visible */
				CommandCounterIncrement();
			}
		}
		else
		{
			/*
			 * If we were told to drop ONLY in this table (no recursion), we
			 * need to mark the inheritors' constraints as locally defined
			 * rather than inherited.
			 */
			con->coninhcount--;
			con->conislocal = true;

			CatalogTupleUpdate(conrel, &copy_tuple->t_self, copy_tuple);

			/* Make update visible */
			CommandCounterIncrement();
		}

		heap_freetuple(copy_tuple);

		table_close(childrel, NoLock);
	}

	table_close(conrel, RowExclusiveLock);
}


/*
 * ALTER COLUMN .. OPTIONS ( ... )
 *
 * Returns the address of the modified column
 */
static ObjectAddress
ATExecAlterColumnGenericOptions(Relation rel,
								const char *colName,
								List *options,
								LOCKMODE lockmode)
{
	Relation	ftrel;
	Relation	attrel;
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	bool		isnull;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];
	Datum		datum;
	Form_pg_foreign_table fttableform;
	Form_pg_attribute atttableform;
	AttrNumber	attnum;
	ObjectAddress address;

	if (options == NIL)
		return InvalidObjectAddress;

	/* First, determine FDW validator associated to the foreign table. */
	ftrel = table_open(ForeignTableRelationId, AccessShareLock);
	tuple = SearchSysCache1(FOREIGNTABLEREL, rel->rd_id);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign table \"%s\" does not exist",
						RelationGetRelationName(rel))));
	fttableform = (Form_pg_foreign_table) GETSTRUCT(tuple);
	server = GetForeignServer(fttableform->ftserver);
	fdw = GetForeignDataWrapper(server->fdwid);

	table_close(ftrel, AccessShareLock);
	ReleaseSysCache(tuple);

	attrel = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* Prevent them from altering a system attribute */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", colName)));


	/* Initialize buffers for new tuple values */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Extract the current options */
	datum = SysCacheGetAttr(ATTNAME,
							tuple,
							Anum_pg_attribute_attfdwoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* Transform the options */
	datum = transformGenericOptions(AttributeRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_attribute_attfdwoptions - 1] = datum;
	else
		repl_null[Anum_pg_attribute_attfdwoptions - 1] = true;

	repl_repl[Anum_pg_attribute_attfdwoptions - 1] = true;

	/* Everything looks good - update the tuple */

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrel),
								 repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(attrel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  atttableform->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	ReleaseSysCache(tuple);

	table_close(attrel, RowExclusiveLock);

	heap_freetuple(newtuple);

	return address;
}


/*
 * ALTER TABLE CLUSTER ON
 *
 * The only thing we have to do is to change the indisclustered bits.
 *
 * Return the address of the new clustering index.
 */
static ObjectAddress
ATExecClusterOn(Relation rel, const char *indexName, LOCKMODE lockmode)
{
	Oid			indexOid;
	ObjectAddress address;

	indexOid = get_relname_relid(indexName, rel->rd_rel->relnamespace);

	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						indexName, RelationGetRelationName(rel))));

	/* Check index is valid to cluster on */
	check_index_is_clusterable(rel, indexOid, false, lockmode);

	/* And do the work */
	mark_index_clustered(rel, indexOid, false);

	ObjectAddressSet(address,
					 RelationRelationId, indexOid);

	return address;
}


/*
 * ALTER TABLE SET WITHOUT CLUSTER
 *
 * We have to find any indexes on the table that have indisclustered bit
 * set and turn it off.
 */
static void
ATExecDropCluster(Relation rel, LOCKMODE lockmode)
{
	mark_index_clustered(rel, InvalidOid, false);
}


/*
 * Special handling of ALTER TABLE SET TABLESPACE for relations with no
 * storage that have an interest in preserving tablespace.
 *
 * Since these have no storage the tablespace can be updated with a simple
 * metadata only operation to update the tablespace.
 */
static void
ATExecSetTableSpaceNoStorage(Relation rel, Oid newTableSpace)
{
	/*
	 * Shouldn't be called on relations having storage; these are processed in
	 * phase 3.
	 */
	Assert(!RELKIND_HAS_STORAGE(rel->rd_rel->relkind));

	/* check if relation can be moved to its new tablespace */
	if (!CheckRelationTableSpaceMove(rel, newTableSpace))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel),
								  0);
		return;
	}

	/* Update can be done, so change reltablespace */
	SetRelationTableSpace(rel, newTableSpace, InvalidOid);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	/* Make sure the reltablespace change is visible */
	CommandCounterIncrement();
}


/*
 * Set, reset, or replace reloptions.
 */
static void
ATExecSetRelOptions(Relation rel, List *defList, AlterTableType operation,
					LOCKMODE lockmode)
{
	Oid			relid;
	Relation	pgclass;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		datum;
	bool		isnull;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_class];
	bool		repl_null[Natts_pg_class];
	bool		repl_repl[Natts_pg_class];
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;

	if (defList == NIL && operation != AT_ReplaceRelOptions)
		return;					/* nothing to do */

	pgclass = table_open(RelationRelationId, RowExclusiveLock);

	/* Fetch heap tuple */
	relid = RelationGetRelid(rel);
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	if (operation == AT_ReplaceRelOptions)
	{
		/*
		 * If we're supposed to replace the reloptions list, we just pretend
		 * there were none before.
		 */
		datum = (Datum) 0;
		isnull = true;
	}
	else
	{
		/* Get the old reloptions */
		datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								&isnull);
	}

	/* Generate new proposed reloptions (text array) */
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 defList, NULL, validnsps, false,
									 operation == AT_ResetRelOptions);

	/* Validate */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_MATVIEW:
			(void) heap_reloptions(rel->rd_rel->relkind, newOptions, true);
			break;
		case RELKIND_PARTITIONED_TABLE:
			(void) partitioned_table_reloptions(newOptions, true);
			break;
		case RELKIND_VIEW:
			(void) view_reloptions(newOptions, true);
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			(void) index_reloptions(rel->rd_indam->amoptions, newOptions, true);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a table, view, materialized view, index, or TOAST table",
							RelationGetRelationName(rel))));
			break;
	}

	/* Special-case validation of view options */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		Query	   *view_query = get_view_query(rel);
		List	   *view_options = untransformRelOptions(newOptions);
		ListCell   *cell;
		bool		check_option = false;

		foreach(cell, view_options)
		{
			DefElem    *defel = (DefElem *) lfirst(cell);

			if (strcmp(defel->defname, "check_option") == 0)
				check_option = true;
		}

		/*
		 * If the check option is specified, look to see if the view is
		 * actually auto-updatable or not.
		 */
		if (check_option)
		{
			const char *view_updatable_error =
			view_query_is_auto_updatable(view_query, true);

			if (view_updatable_error)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("WITH CHECK OPTION is supported only on automatically updatable views"),
						 errhint("%s", _(view_updatable_error))));
		}
	}

	/*
	 * All we need do here is update the pg_class row; the new options will be
	 * propagated into relcaches during post-commit cache inval.
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_class_reloptions - 1] = newOptions;
	else
		repl_null[Anum_pg_class_reloptions - 1] = true;

	repl_repl[Anum_pg_class_reloptions - 1] = true;

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
								 repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(pgclass, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	/* repeat the whole exercise for the toast table, if there's one */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
	{
		Relation	toastrel;
		Oid			toastid = rel->rd_rel->reltoastrelid;

		toastrel = table_open(toastid, lockmode);

		/* Fetch heap tuple */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);

		if (operation == AT_ReplaceRelOptions)
		{
			/*
			 * If we're supposed to replace the reloptions list, we just
			 * pretend there were none before.
			 */
			datum = (Datum) 0;
			isnull = true;
		}
		else
		{
			/* Get the old reloptions */
			datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									&isnull);
		}

		newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
										 defList, "toast", validnsps, false,
										 operation == AT_ResetRelOptions);

		(void) heap_reloptions(RELKIND_TOASTVALUE, newOptions, true);

		memset(repl_val, 0, sizeof(repl_val));
		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		if (newOptions != (Datum) 0)
			repl_val[Anum_pg_class_reloptions - 1] = newOptions;
		else
			repl_null[Anum_pg_class_reloptions - 1] = true;

		repl_repl[Anum_pg_class_reloptions - 1] = true;

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
									 repl_val, repl_null, repl_repl);

		CatalogTupleUpdate(pgclass, &newtuple->t_self, newtuple);

		InvokeObjectPostAlterHookArg(RelationRelationId,
									 RelationGetRelid(toastrel), 0,
									 InvalidOid, true);

		heap_freetuple(newtuple);

		ReleaseSysCache(tuple);

		table_close(toastrel, NoLock);
	}

	table_close(pgclass, RowExclusiveLock);
}


/*
 * ALTER TABLE ENABLE/DISABLE TRIGGER
 *
 * We just pass this off to trigger.c.
 */
static void
ATExecEnableDisableTrigger(Relation rel, const char *trigname,
						   char fires_when, bool skip_system, bool recurse,
						   LOCKMODE lockmode)
{
	EnableDisableTriggerNew(rel, trigname, fires_when, skip_system, recurse,
							lockmode);
}


/*
 * ALTER TABLE ENABLE/DISABLE RULE
 *
 * We just pass this off to rewriteDefine.c.
 */
static void
ATExecEnableDisableRule(Relation rel, const char *rulename,
						char fires_when, LOCKMODE lockmode)
{
	EnableDisableRule(rel, rulename, fires_when);
}


/*
 * Return the address of the new parent relation.
 */
static ObjectAddress
ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode)
{
	Relation	parent_rel;
	List	   *children;
	ObjectAddress address;
	const char *trigger_name;

	/*
	 * A self-exclusive lock is needed here.  See the similar case in
	 * MergeAttributes() for a full explanation.
	 */
	parent_rel = table_openrv(parent, ShareUpdateExclusiveLock);

	/*
	 * Must be owner of both parent and child -- child was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 */
	ATSimplePermissions(parent_rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/* Permanent rels cannot inherit from temporary ones */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		child_rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation \"%s\"",
						RelationGetRelationName(parent_rel))));

	/* If parent rel is temp, it must belong to this session */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!parent_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation of another session")));

	/* Ditto for the child */
	if (child_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!child_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit to temporary relation of another session")));

	/* Prevent partitioned tables from becoming inheritance parents */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from partitioned table \"%s\"",
						parent->relname)));

	/* Likewise for partitions */
	if (parent_rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from a partition")));

	/*
	 * Prevent circularity by seeing if proposed parent inherits from child.
	 * (In particular, this disallows making a rel inherit from itself.)
	 *
	 * This is not completely bulletproof because of race conditions: in
	 * multi-level inheritance trees, someone else could concurrently be
	 * making another inheritance link that closes the loop but does not join
	 * either of the rels we have locked.  Preventing that seems to require
	 * exclusive locks on the entire inheritance tree, which is a cure worse
	 * than the disease.  find_all_inheritors() will cope with circularity
	 * anyway, so don't sweat it too much.
	 *
	 * We use weakest lock we can on child's children, namely AccessShareLock.
	 */
	children = find_all_inheritors(RelationGetRelid(child_rel),
								   AccessShareLock, NULL);

	if (list_member_oid(children, RelationGetRelid(parent_rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   parent->relname,
						   RelationGetRelationName(child_rel))));

	/*
	 * If child_rel has row-level triggers with transition tables, we
	 * currently don't allow it to become an inheritance child.  See also
	 * prohibitions in ATExecAttachPartition() and CreateTrigger().
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(child_rel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming an inheritance child",
						trigger_name, RelationGetRelationName(child_rel)),
				 errdetail("ROW triggers with transition tables are not supported in inheritance hierarchies.")));

	/* OK to create inheritance */
	CreateInheritance(child_rel, parent_rel);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* keep our lock on the parent relation until commit */
	table_close(parent_rel, NoLock);

	return address;
}


/*
 * CreateInheritance
 *		Catalog manipulation portion of creating inheritance between a child
 *		table and a parent table.
 *
 * Common to ATExecAddInherit() and ATExecAttachPartition().
 */
static void
CreateInheritance(Relation child_rel, Relation parent_rel)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	int32		inhseqno;

	/* Note: get RowExclusiveLock because we will write pg_inherits below. */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);

	/*
	 * Check for duplicates in the list of parents, and determine the highest
	 * inhseqno already present; we'll use the next one for the new parent.
	 * Also, if proposed child is a partition, it cannot already be
	 * inheriting.
	 *
	 * Note: we do not reject the case where the child already inherits from
	 * the parent indirectly; CREATE TABLE doesn't reject comparable cases.
	 */
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);

	/* inhseqno sequences start at 1 */
	inhseqno = 0;
	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inheritsTuple);

		if (inh->inhparent == RelationGetRelid(parent_rel))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" would be inherited from more than once",
							RelationGetRelationName(parent_rel))));

		if (inh->inhseqno > inhseqno)
			inhseqno = inh->inhseqno;
	}
	systable_endscan(scan);

	/* Match up the columns and bump attinhcount as needed */
	MergeAttributesIntoExisting(child_rel, parent_rel);

	/* Match up the constraints and bump coninhcount as needed */
	MergeConstraintsIntoExisting(child_rel, parent_rel);

	/*
	 * OK, it looks valid.  Make the catalog entries that show inheritance.
	 */
	StoreCatalogInheritance1(RelationGetRelid(child_rel),
							 RelationGetRelid(parent_rel),
							 inhseqno + 1,
							 catalogRelation,
							 parent_rel->rd_rel->relkind ==
							 RELKIND_PARTITIONED_TABLE);

	/* Now we're done with pg_inherits */
	table_close(catalogRelation, RowExclusiveLock);
}


/*
 * ALTER TABLE NO INHERIT
 *
 * Return value is the address of the relation that is no longer parent.
 */
static ObjectAddress
ATExecDropInherit(Relation rel, RangeVar *parent, LOCKMODE lockmode)
{
	ObjectAddress address;
	Relation	parent_rel;

	if (rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of a partition")));

	/*
	 * AccessShareLock on the parent is probably enough, seeing that DROP
	 * TABLE doesn't lock parent tables at all.  We need some lock since we'll
	 * be inspecting the parent's schema.
	 */
	parent_rel = table_openrv(parent, AccessShareLock);

	/*
	 * We don't bother to check ownership of the parent table --- ownership of
	 * the child is presumed enough rights.
	 */

	/* Off to RemoveInheritance() where most of the work happens */
	RemoveInheritance(rel, parent_rel, false);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* keep our lock on the parent relation until commit */
	table_close(parent_rel, NoLock);

	return address;
}


/*
 * Drop the dependency created by StoreCatalogInheritance1 (CREATE TABLE
 * INHERITS/ALTER TABLE INHERIT -- refclassid will be RelationRelationId) or
 * heap_create_with_catalog (CREATE TABLE OF/ALTER TABLE OF -- refclassid will
 * be TypeRelationId).  There's no convenient way to do this, so go trawling
 * through pg_depend.
 */
static void
drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid,
					   DependencyType deptype)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	depTuple;

	catalogRelation = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(catalogRelation, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(depTuple);

		if (dep->refclassid == refclassid &&
			dep->refobjid == refobjid &&
			dep->refobjsubid == 0 &&
			dep->deptype == deptype)
			CatalogTupleDelete(catalogRelation, &depTuple->t_self);
	}

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);
}


/*
 * ALTER TABLE OF
 *
 * Attach a table to a composite type, as though it had been created with CREATE
 * TABLE OF.  All attname, atttypid, atttypmod and attcollation must match.  The
 * subject table must not have inheritance parents.  These restrictions ensure
 * that you cannot create a configuration impossible with CREATE TABLE OF alone.
 *
 * The address of the type is returned.
 */
static ObjectAddress
ATExecAddOf(Relation rel, const TypeName *ofTypename, LOCKMODE lockmode)
{
	Oid			relid = RelationGetRelid(rel);
	Type		typetuple;
	Form_pg_type typeform;
	Oid			typeid;
	Relation	inheritsRelation,
				relationRelation;
	SysScanDesc scan;
	ScanKeyData key;
	AttrNumber	table_attno,
				type_attno;
	TupleDesc	typeTupleDesc,
				tableTupleDesc;
	ObjectAddress tableobj,
				typeobj;
	HeapTuple	classtuple;

	/* Validate the type. */
	typetuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(typetuple);
	typeform = (Form_pg_type) GETSTRUCT(typetuple);
	typeid = typeform->oid;

	/* Fail if the table has any inheritance parents. */
	inheritsRelation = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(inheritsRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);
	if (HeapTupleIsValid(systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("typed tables cannot inherit")));
	systable_endscan(scan);
	table_close(inheritsRelation, AccessShareLock);

	/*
	 * Check the tuple descriptors for compatibility.  Unlike inheritance, we
	 * require that the order also match.  However, attnotnull need not match.
	 */
	typeTupleDesc = lookup_rowtype_tupdesc(typeid, -1);
	tableTupleDesc = RelationGetDescr(rel);
	table_attno = 1;
	for (type_attno = 1; type_attno <= typeTupleDesc->natts; type_attno++)
	{
		Form_pg_attribute type_attr,
					table_attr;
		const char *type_attname,
				   *table_attname;

		/* Get the next non-dropped type attribute. */
		type_attr = TupleDescAttr(typeTupleDesc, type_attno - 1);
		if (type_attr->attisdropped)
			continue;
		type_attname = NameStr(type_attr->attname);

		/* Get the next non-dropped table attribute. */
		do
		{
			if (table_attno > tableTupleDesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table is missing column \"%s\"",
								type_attname)));
			table_attr = TupleDescAttr(tableTupleDesc, table_attno - 1);
			table_attno++;
		} while (table_attr->attisdropped);
		table_attname = NameStr(table_attr->attname);

		/* Compare name. */
		if (strncmp(table_attname, type_attname, NAMEDATALEN) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table has column \"%s\" where type requires \"%s\"",
							table_attname, type_attname)));

		/* Compare type. */
		if (table_attr->atttypid != type_attr->atttypid ||
			table_attr->atttypmod != type_attr->atttypmod ||
			table_attr->attcollation != type_attr->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table \"%s\" has different type for column \"%s\"",
							RelationGetRelationName(rel), type_attname)));
	}
	DecrTupleDescRefCount(typeTupleDesc);

	/* Any remaining columns at the end of the table had better be dropped. */
	for (; table_attno <= tableTupleDesc->natts; table_attno++)
	{
		Form_pg_attribute table_attr = TupleDescAttr(tableTupleDesc,
													 table_attno - 1);

		if (!table_attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table has extra column \"%s\"",
							NameStr(table_attr->attname))));
	}

	/* If the table was already typed, drop the existing dependency. */
	if (rel->rd_rel->reloftype)
		drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype,
							   DEPENDENCY_NORMAL);

	/* Record a dependency on the new type. */
	tableobj.classId = RelationRelationId;
	tableobj.objectId = relid;
	tableobj.objectSubId = 0;
	typeobj.classId = TypeRelationId;
	typeobj.objectId = typeid;
	typeobj.objectSubId = 0;
	recordDependencyOn(&tableobj, &typeobj, DEPENDENCY_NORMAL);

	/* Update pg_class.reloftype */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	classtuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(classtuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	((Form_pg_class) GETSTRUCT(classtuple))->reloftype = typeid;
	CatalogTupleUpdate(relationRelation, &classtuple->t_self, classtuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	heap_freetuple(classtuple);
	table_close(relationRelation, RowExclusiveLock);

	ReleaseSysCache(typetuple);

	return typeobj;
}


/*
 * ALTER TABLE NOT OF
 *
 * Detach a typed table from its originating type.  Just clear reloftype and
 * remove the dependency.
 */
static void
ATExecDropOf(Relation rel, LOCKMODE lockmode)
{
	Oid			relid = RelationGetRelid(rel);
	Relation	relationRelation;
	HeapTuple	tuple;

	if (!OidIsValid(rel->rd_rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a typed table",
						RelationGetRelationName(rel))));

	/*
	 * We don't bother to check ownership of the type --- ownership of the
	 * table is presumed enough rights.  No lock required on the type, either.
	 */

	drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype,
						   DEPENDENCY_NORMAL);

	/* Clear pg_class.reloftype */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	((Form_pg_class) GETSTRUCT(tuple))->reloftype = InvalidOid;
	CatalogTupleUpdate(relationRelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	heap_freetuple(tuple);
	table_close(relationRelation, RowExclusiveLock);
}


/*
 * relation_mark_replica_identity: Update a table's replica identity
 *
 * Iff ri_type = REPLICA_IDENTITY_INDEX, indexOid must be the Oid of a suitable
 * index. Otherwise, it must be InvalidOid.
 *
 * Caller had better hold an exclusive lock on the relation, as the results
 * of running two of these concurrently wouldn't be pretty.
 */
static void
relation_mark_replica_identity(Relation rel, char ri_type, Oid indexOid,
							   bool is_internal)
{
	Relation	pg_index;
	Relation	pg_class;
	HeapTuple	pg_class_tuple;
	HeapTuple	pg_index_tuple;
	Form_pg_class pg_class_form;
	Form_pg_index pg_index_form;
	ListCell   *index;

	/*
	 * Check whether relreplident has changed, and update it if so.
	 */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	pg_class_tuple = SearchSysCacheCopy1(RELOID,
										 ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(pg_class_tuple))
		elog(ERROR, "cache lookup failed for relation \"%s\"",
			 RelationGetRelationName(rel));
	pg_class_form = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	if (pg_class_form->relreplident != ri_type)
	{
		pg_class_form->relreplident = ri_type;
		CatalogTupleUpdate(pg_class, &pg_class_tuple->t_self, pg_class_tuple);
	}
	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(pg_class_tuple);

	/*
	 * Update the per-index indisreplident flags correctly.
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);
	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);
		bool		dirty = false;

		pg_index_tuple = SearchSysCacheCopy1(INDEXRELID,
											 ObjectIdGetDatum(thisIndexOid));
		if (!HeapTupleIsValid(pg_index_tuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		pg_index_form = (Form_pg_index) GETSTRUCT(pg_index_tuple);

		if (thisIndexOid == indexOid)
		{
			/* Set the bit if not already set. */
			if (!pg_index_form->indisreplident)
			{
				dirty = true;
				pg_index_form->indisreplident = true;
			}
		}
		else
		{
			/* Unset the bit if set. */
			if (pg_index_form->indisreplident)
			{
				dirty = true;
				pg_index_form->indisreplident = false;
			}
		}

		if (dirty)
		{
			CatalogTupleUpdate(pg_index, &pg_index_tuple->t_self, pg_index_tuple);
			InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
										 InvalidOid, is_internal);
			/*
			 * Invalidate the relcache for the table, so that after we commit
			 * all sessions will refresh the table's replica identity index
			 * before attempting any UPDATE or DELETE on the table.  (If we
			 * changed the table's pg_class row above, then a relcache inval
			 * is already queued due to that; but we might not have.)
			 */
			CacheInvalidateRelcache(rel);
		}
		heap_freetuple(pg_index_tuple);
	}

	table_close(pg_index, RowExclusiveLock);
}


/*
 * ALTER TABLE <name> REPLICA IDENTITY ...
 */
static void
ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt *stmt, LOCKMODE lockmode)
{
	Oid			indexOid;
	Relation	indexRel;
	int			key;

	if (stmt->identity_type == REPLICA_IDENTITY_DEFAULT)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_FULL)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_NOTHING)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_INDEX)
	{
		 /* fallthrough */ ;
	}
	else
		elog(ERROR, "unexpected identity type %u", stmt->identity_type);


	/* Check that the index exists */
	indexOid = get_relname_relid(stmt->name, rel->rd_rel->relnamespace);
	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						stmt->name, RelationGetRelationName(rel))));

	indexRel = index_open(indexOid, ShareLock);

	/* Check that the index is on the relation we're altering. */
	if (indexRel->rd_index == NULL ||
		indexRel->rd_index->indrelid != RelationGetRelid(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(indexRel),
						RelationGetRelationName(rel))));
	/* The AM must support uniqueness, and the index must in fact be unique. */
	if (!indexRel->rd_indam->amcanunique ||
		!indexRel->rd_index->indisunique)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot use non-unique index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Deferred indexes are not guaranteed to be always unique. */
	if (!indexRel->rd_index->indimmediate)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use non-immediate index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Expression indexes aren't supported. */
	if (RelationGetIndexExpressions(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use expression index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Predicate indexes aren't supported. */
	if (RelationGetIndexPredicate(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use partial index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));

	/* Check index for nullable columns. */
	for (key = 0; key < IndexRelationGetNumberOfKeyAttributes(indexRel); key++)
	{
		int16		attno = indexRel->rd_index->indkey.values[key];
		Form_pg_attribute attr;

		/*
		 * Reject any other system columns.  (Going forward, we'll disallow
		 * indexes containing such columns in the first place, but they might
		 * exist in older branches.)
		 */
		if (attno <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("index \"%s\" cannot be used as replica identity because column %d is a system column",
							RelationGetRelationName(indexRel), attno)));

		attr = TupleDescAttr(rel->rd_att, attno - 1);
		if (!attr->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" cannot be used as replica identity because column \"%s\" is nullable",
							RelationGetRelationName(indexRel),
							NameStr(attr->attname))));
	}

	/* This index is suitable for use as a replica identity. Mark it. */
	relation_mark_replica_identity(rel, stmt->identity_type, indexOid, true);

	index_close(indexRel, NoLock);
}


/*
 * ALTER TABLE ENABLE/DISABLE ROW LEVEL SECURITY
 */
static void
ATExecSetRowSecurity(Relation rel, bool rls)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	/* Pull the record for this relation and update it */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relrowsecurity = rls;
	CatalogTupleUpdate(pg_class, &tuple->t_self, tuple);

	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}


/*
 * ALTER TABLE FORCE/NO FORCE ROW LEVEL SECURITY
 */
static void
ATExecForceNoForceRowSecurity(Relation rel, bool force_rls)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relforcerowsecurity = force_rls;
	CatalogTupleUpdate(pg_class, &tuple->t_self, tuple);

	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}


/*
 * ALTER FOREIGN TABLE <name> OPTIONS (...)
 */
static void
ATExecGenericOptions(Relation rel, List *options)
{
	Relation	ftrel;
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	HeapTuple	tuple;
	bool		isnull;
	Datum		repl_val[Natts_pg_foreign_table];
	bool		repl_null[Natts_pg_foreign_table];
	bool		repl_repl[Natts_pg_foreign_table];
	Datum		datum;
	Form_pg_foreign_table tableform;

	if (options == NIL)
		return;

	ftrel = table_open(ForeignTableRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(FOREIGNTABLEREL, rel->rd_id);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign table \"%s\" does not exist",
						RelationGetRelationName(rel))));
	tableform = (Form_pg_foreign_table) GETSTRUCT(tuple);
	server = GetForeignServer(tableform->ftserver);
	fdw = GetForeignDataWrapper(server->fdwid);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Extract the current options */
	datum = SysCacheGetAttr(FOREIGNTABLEREL,
							tuple,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* Transform the options */
	datum = transformGenericOptions(ForeignTableRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_foreign_table_ftoptions - 1] = datum;
	else
		repl_null[Anum_pg_foreign_table_ftoptions - 1] = true;

	repl_repl[Anum_pg_foreign_table_ftoptions - 1] = true;

	/* Everything looks good - update the tuple */

	tuple = heap_modify_tuple(tuple, RelationGetDescr(ftrel),
							  repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(ftrel, &tuple->t_self, tuple);

	/*
	 * Invalidate relcache so that all sessions will refresh any cached plans
	 * that might depend on the old options.
	 */
	CacheInvalidateRelcache(rel);

	InvokeObjectPostAlterHook(ForeignTableRelationId,
							  RelationGetRelid(rel), 0);

	table_close(ftrel, RowExclusiveLock);

	heap_freetuple(tuple);
}


/*
 * ALTER TABLE <name> ATTACH PARTITION <partition-name> FOR VALUES
 *
 * Return the address of the newly attached partition.
 */
static ObjectAddress
ATExecAttachPartition(List **wqueue, Relation rel, PartitionCmd *cmd,
					  AlterTableUtilityContext *context)
{
	Relation	attachrel,
				catalog;
	List	   *attachrel_children;
	List	   *partConstraint;
	SysScanDesc scan;
	ScanKeyData skey;
	AttrNumber	attno;
	int			natts;
	TupleDesc	tupleDesc;
	ObjectAddress address;
	const char *trigger_name;
	Oid			defaultPartOid;
	List	   *partBoundConstraint;
	ParseState *pstate = make_parsestate(NULL);

	pstate->p_sourcetext = context->queryString;

	/*
	 * We must lock the default partition if one exists, because attaching a
	 * new partition will change its partition constraint.
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));
	if (OidIsValid(defaultPartOid))
		LockRelationOid(defaultPartOid, AccessExclusiveLock);

	attachrel = table_openrv(cmd->name, AccessExclusiveLock);

	/*
	 * XXX I think it'd be a good idea to grab locks on all tables referenced
	 * by FKs at this point also.
	 */

	/*
	 * Must be owner of both parent and source table -- parent was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 */
	ATSimplePermissions(attachrel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/* A partition can only have one parent */
	if (attachrel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is already a partition",
						RelationGetRelationName(attachrel))));

	if (OidIsValid(attachrel->rd_rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a typed table as partition")));

	/*
	 * Table being attached should not already be part of inheritance; either
	 * as a child table...
	 */
	catalog = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(attachrel)));
	scan = systable_beginscan(catalog, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach inheritance child as partition")));
	systable_endscan(scan);

	/* ...or as a parent table (except the case when it is partitioned) */
	ScanKeyInit(&skey,
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(attachrel)));
	scan = systable_beginscan(catalog, InheritsParentIndexId, true, NULL,
							  1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan)) &&
		attachrel->rd_rel->relkind == RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach inheritance parent as partition")));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);

	/*
	 * Prevent circularity by seeing if rel is a partition of attachrel. (In
	 * particular, this disallows making a rel a partition of itself.)
	 *
	 * We do that by checking if rel is a member of the list of attachrel's
	 * partitions provided the latter is partitioned at all.  We want to avoid
	 * having to construct this list again, so we request the strongest lock
	 * on all partitions.  We need the strongest lock, because we may decide
	 * to scan them if we find out that the table being attached (or its leaf
	 * partitions) may contain rows that violate the partition constraint. If
	 * the table has a constraint that would prevent such rows, which by
	 * definition is present in all the partitions, we need not scan the
	 * table, nor its partitions.  But we cannot risk a deadlock by taking a
	 * weaker lock now and the stronger one only when needed.
	 */
	attachrel_children = find_all_inheritors(RelationGetRelid(attachrel),
											 AccessExclusiveLock, NULL);
	if (list_member_oid(attachrel_children, RelationGetRelid(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   RelationGetRelationName(rel),
						   RelationGetRelationName(attachrel))));

	/* If the parent is permanent, so must be all of its partitions. */
	if (rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a temporary relation as partition of permanent relation \"%s\"",
						RelationGetRelationName(rel))));

	/* Temp parent cannot have a partition that is itself not a temp */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a permanent relation as partition of temporary relation \"%s\"",
						RelationGetRelationName(rel))));

	/* If the parent is temp, it must belong to this session */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach as partition of temporary relation of another session")));

	/* Ditto for the partition */
	if (attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!attachrel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach temporary relation of another session as partition")));

	/* Check if there are any columns in attachrel that aren't in the parent */
	tupleDesc = RelationGetDescr(attachrel);
	natts = tupleDesc->natts;
	for (attno = 1; attno <= natts; attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc, attno - 1);
		char	   *attributeName = NameStr(attribute->attname);

		/* Ignore dropped */
		if (attribute->attisdropped)
			continue;

		/* Try to find the column in parent (matching on column name) */
		if (!SearchSysCacheExists2(ATTNAME,
								   ObjectIdGetDatum(RelationGetRelid(rel)),
								   CStringGetDatum(attributeName)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table \"%s\" contains column \"%s\" not found in parent \"%s\"",
							RelationGetRelationName(attachrel), attributeName,
							RelationGetRelationName(rel)),
					 errdetail("The new partition may contain only the columns present in parent.")));
	}

	/*
	 * If child_rel has row-level triggers with transition tables, we
	 * currently don't allow it to become a partition.  See also prohibitions
	 * in ATExecAddInherit() and CreateTrigger().
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(attachrel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming a partition",
						trigger_name, RelationGetRelationName(attachrel)),
				 errdetail("ROW triggers with transition tables are not supported on partitions")));

	/*
	 * Check that the new partition's bound is valid and does not overlap any
	 * of existing partitions of the parent - note that it does not return on
	 * error.
	 */
	check_new_partition_bound(RelationGetRelationName(attachrel), rel,
							  cmd->bound, pstate);

	/* OK to create inheritance.  Rest of the checks performed there */
	CreateInheritance(attachrel, rel);

	/* Update the pg_class entry. */
	StorePartitionBound(attachrel, rel, cmd->bound);

	/* Ensure there exists a correct set of indexes in the partition. */
	AttachPartitionEnsureIndexes(rel, attachrel);

	/* and triggers */
	CloneRowTriggersToPartition(rel, attachrel);

	/*
	 * Clone foreign key constraints.  Callee is responsible for setting up
	 * for phase 3 constraint verification.
	 */
	CloneForeignKeyConstraints(wqueue, rel, attachrel);

	/*
	 * Generate partition constraint from the partition bound specification.
	 * If the parent itself is a partition, make sure to include its
	 * constraint as well.
	 */
	partBoundConstraint = get_qual_from_partbound(attachrel, rel, cmd->bound);
	partConstraint = list_concat(partBoundConstraint,
								 RelationGetPartitionQual(rel));

	/* Skip validation if there are no constraints to validate. */
	if (partConstraint)
	{
		/*
		 * Run the partition quals through const-simplification similar to
		 * check constraints.  We skip canonicalize_qual, though, because
		 * partition quals should be in canonical form already.
		 */
		partConstraint =
			(List *) eval_const_expressions(NULL,
											(Node *) partConstraint);

		/* XXX this sure looks wrong */
		partConstraint = list_make1(make_ands_explicit(partConstraint));

		/*
		 * Adjust the generated constraint to match this partition's attribute
		 * numbers.
		 */
		partConstraint = map_partition_varattnos(partConstraint, 1, attachrel,
												 rel);

		/* Validate partition constraints against the table being attached. */
		QueuePartitionConstraintValidation(wqueue, attachrel, partConstraint,
										   false);
	}

	/*
	 * If we're attaching a partition other than the default partition and a
	 * default one exists, then that partition's partition constraint changes,
	 * so add an entry to the work queue to validate it, too.  (We must not do
	 * this when the partition being attached is the default one; we already
	 * did it above!)
	 */
	if (OidIsValid(defaultPartOid))
	{
		Relation	defaultrel;
		List	   *defPartConstraint;

		Assert(!cmd->bound->is_default);

		/* we already hold a lock on the default partition */
		defaultrel = table_open(defaultPartOid, NoLock);
		defPartConstraint =
			get_proposed_default_constraint(partBoundConstraint);

		/*
		 * Map the Vars in the constraint expression from rel's attnos to
		 * defaultrel's.
		 */
		defPartConstraint =
			map_partition_varattnos(defPartConstraint,
									1, defaultrel, rel);
		QueuePartitionConstraintValidation(wqueue, defaultrel,
										   defPartConstraint, true);

		/* keep our lock until commit. */
		table_close(defaultrel, NoLock);
	}

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(attachrel));

	/*
	 * If the partition we just attached is partitioned itself, invalidate
	 * relcache for all descendent partitions too to ensure that their
	 * rd_partcheck expression trees are rebuilt; partitions already locked
	 * at the beginning of this function.
	 */
	if (attachrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		ListCell *l;

		foreach(l, attachrel_children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(l));
		}
	}

	/* keep our lock until commit */
	table_close(attachrel, NoLock);

	return address;
}


/*
 * AttachPartitionEnsureIndexes
 *		subroutine for ATExecAttachPartition to create/match indexes
 *
 * Enforce the indexing rule for partitioned tables during ALTER TABLE / ATTACH
 * PARTITION: every partition must have an index attached to each index on the
 * partitioned table.
 */
static void
AttachPartitionEnsureIndexes(Relation rel, Relation attachrel)
{
	List	   *idxes;
	List	   *attachRelIdxs;
	Relation   *attachrelIdxRels;
	IndexInfo **attachInfos;
	int			i;
	ListCell   *cell;
	MemoryContext cxt;
	MemoryContext oldcxt;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"AttachPartitionEnsureIndexes",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	idxes = RelationGetIndexList(rel);
	attachRelIdxs = RelationGetIndexList(attachrel);
	attachrelIdxRels = palloc(sizeof(Relation) * list_length(attachRelIdxs));
	attachInfos = palloc(sizeof(IndexInfo *) * list_length(attachRelIdxs));

	/* Build arrays of all existing indexes and their IndexInfos */
	i = 0;
	foreach(cell, attachRelIdxs)
	{
		Oid			cldIdxId = lfirst_oid(cell);

		attachrelIdxRels[i] = index_open(cldIdxId, AccessShareLock);
		attachInfos[i] = BuildIndexInfo(attachrelIdxRels[i]);
		i++;
	}

	/*
	 * If we're attaching a foreign table, we must fail if any of the indexes
	 * is a constraint index; otherwise, there's nothing to do here.  Do this
	 * before starting work, to avoid wasting the effort of building a few
	 * non-unique indexes before coming across a unique one.
	 */
	if (attachrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		foreach(cell, idxes)
		{
			Oid			idx = lfirst_oid(cell);
			Relation	idxRel = index_open(idx, AccessShareLock);

			if (idxRel->rd_index->indisunique ||
				idxRel->rd_index->indisprimary)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot attach foreign table \"%s\" as partition of partitioned table \"%s\"",
								RelationGetRelationName(attachrel),
								RelationGetRelationName(rel)),
						 errdetail("Partitioned table \"%s\" contains unique indexes.",
								   RelationGetRelationName(rel))));
			index_close(idxRel, AccessShareLock);
		}

		goto out;
	}

	/*
	 * For each index on the partitioned table, find a matching one in the
	 * partition-to-be; if one is not found, create one.
	 */
	foreach(cell, idxes)
	{
		Oid			idx = lfirst_oid(cell);
		Relation	idxRel = index_open(idx, AccessShareLock);
		IndexInfo  *info;
		AttrMap    *attmap;
		bool		found = false;
		Oid			constraintOid;

		/*
		 * Ignore indexes in the partitioned table other than partitioned
		 * indexes.
		 */
		if (idxRel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			index_close(idxRel, AccessShareLock);
			continue;
		}

		/* construct an indexinfo to compare existing indexes against */
		info = BuildIndexInfo(idxRel);
		attmap = build_attrmap_by_name(RelationGetDescr(attachrel),
									   RelationGetDescr(rel));
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(rel), idx);

		/*
		 * Scan the list of existing indexes in the partition-to-be, and mark
		 * the first matching, unattached one we find, if any, as partition of
		 * the parent index.  If we find one, we're done.
		 */
		for (i = 0; i < list_length(attachRelIdxs); i++)
		{
			Oid			cldIdxId = RelationGetRelid(attachrelIdxRels[i]);
			Oid			cldConstrOid = InvalidOid;

			/* does this index have a parent?  if so, can't use it */
			if (attachrelIdxRels[i]->rd_rel->relispartition)
				continue;

			if (CompareIndexInfo(attachInfos[i], info,
								 attachrelIdxRels[i]->rd_indcollation,
								 idxRel->rd_indcollation,
								 attachrelIdxRels[i]->rd_opfamily,
								 idxRel->rd_opfamily,
								 attmap))
			{
				/*
				 * If this index is being created in the parent because of a
				 * constraint, then the child needs to have a constraint also,
				 * so look for one.  If there is no such constraint, this
				 * index is no good, so keep looking.
				 */
				if (OidIsValid(constraintOid))
				{
					cldConstrOid =
						get_relation_idx_constraint_oid(RelationGetRelid(attachrel),
														cldIdxId);
					/* no dice */
					if (!OidIsValid(cldConstrOid))
						continue;
				}

				/* bingo. */
				IndexSetParentIndex(attachrelIdxRels[i], idx);
				if (OidIsValid(constraintOid))
					ConstraintSetParentConstraint(cldConstrOid, constraintOid,
												  RelationGetRelid(attachrel));
				found = true;

				CommandCounterIncrement();
				break;
			}
		}

		/*
		 * If no suitable index was found in the partition-to-be, create one
		 * now.
		 */
		if (!found)
		{
			IndexStmt  *stmt;
			Oid			constraintOid;

			stmt = generateClonedIndexStmt(NULL,
										   idxRel, attmap,
										   &constraintOid);
			DefineIndex(RelationGetRelid(attachrel), stmt, InvalidOid,
						RelationGetRelid(idxRel),
						constraintOid,
						true, false, false, false, false);
		}

		index_close(idxRel, AccessShareLock);
	}

out:
	/* Clean up. */
	for (i = 0; i < list_length(attachRelIdxs); i++)
		index_close(attachrelIdxRels[i], AccessShareLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}


/*
 * CloneRowTriggersToPartition
 *		subroutine for ATExecAttachPartition/DefineRelation to create row
 *		triggers on partitions
 */
static void
CloneRowTriggersToPartition(Relation parent, Relation partition)
{
	Relation	pg_trigger;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	MemoryContext perTupCxt;

	ScanKeyInit(&key, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parent)));
	pg_trigger = table_open(TriggerRelationId, RowExclusiveLock);
	scan = systable_beginscan(pg_trigger, TriggerRelidNameIndexId,
							  true, NULL, 1, &key);

	perTupCxt = AllocSetContextCreate(CurrentMemoryContext,
									  "clone trig", ALLOCSET_SMALL_SIZES);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_trigger trigForm = (Form_pg_trigger) GETSTRUCT(tuple);
		CreateTrigStmt *trigStmt;
		Node	   *qual = NULL;
		Datum		value;
		bool		isnull;
		List	   *cols = NIL;
		List	   *trigargs = NIL;
		MemoryContext oldcxt;

		/*
		 * Ignore statement-level triggers; those are not cloned.
		 */
		if (!TRIGGER_FOR_ROW(trigForm->tgtype))
			continue;

		/*
		 * Internal triggers require careful examination.  Ideally, we don't
		 * clone them.  However, if our parent is itself a partition, there
		 * might be internal triggers that must not be skipped; for example,
		 * triggers on our parent that are in turn clones from its parent (our
		 * grandparent) are marked internal, yet they are to be cloned.
		 *
		 * Note we dare not verify that the other trigger belongs to an
		 * ancestor relation of our parent, because that creates deadlock
		 * opportunities.
		 */
		if (trigForm->tgisinternal &&
			(!parent->rd_rel->relispartition ||
			 !OidIsValid(trigForm->tgparentid)))
			continue;

		/*
		 * Complain if we find an unexpected trigger type.
		 */
		if (!TRIGGER_FOR_BEFORE(trigForm->tgtype) &&
			!TRIGGER_FOR_AFTER(trigForm->tgtype))
			elog(ERROR, "unexpected trigger \"%s\" found",
				 NameStr(trigForm->tgname));

		/* Use short-lived context for CREATE TRIGGER */
		oldcxt = MemoryContextSwitchTo(perTupCxt);

		/*
		 * If there is a WHEN clause, generate a 'cooked' version of it that's
		 * appropriate for the partition.
		 */
		value = heap_getattr(tuple, Anum_pg_trigger_tgqual,
							 RelationGetDescr(pg_trigger), &isnull);
		if (!isnull)
		{
			qual = stringToNode(TextDatumGetCString(value));
			qual = (Node *) map_partition_varattnos((List *) qual, PRS2_OLD_VARNO,
													partition, parent);
			qual = (Node *) map_partition_varattnos((List *) qual, PRS2_NEW_VARNO,
													partition, parent);
		}

		/*
		 * If there is a column list, transform it to a list of column names.
		 * Note we don't need to map this list in any way ...
		 */
		if (trigForm->tgattr.dim1 > 0)
		{
			int			i;

			for (i = 0; i < trigForm->tgattr.dim1; i++)
			{
				Form_pg_attribute col;

				col = TupleDescAttr(parent->rd_att,
									trigForm->tgattr.values[i] - 1);
				cols = lappend(cols,
							   makeString(pstrdup(NameStr(col->attname))));
			}
		}

		/* Reconstruct trigger arguments list. */
		if (trigForm->tgnargs > 0)
		{
			char	   *p;

			value = heap_getattr(tuple, Anum_pg_trigger_tgargs,
								 RelationGetDescr(pg_trigger), &isnull);
			if (isnull)
				elog(ERROR, "tgargs is null for trigger \"%s\" in partition \"%s\"",
					 NameStr(trigForm->tgname), RelationGetRelationName(partition));

			p = (char *) VARDATA_ANY(DatumGetByteaPP(value));

			for (int i = 0; i < trigForm->tgnargs; i++)
			{
				trigargs = lappend(trigargs, makeString(pstrdup(p)));
				p += strlen(p) + 1;
			}
		}

		trigStmt = makeNode(CreateTrigStmt);
		trigStmt->replace = false;
		trigStmt->isconstraint = OidIsValid(trigForm->tgconstraint);
		trigStmt->trigname = NameStr(trigForm->tgname);
		trigStmt->relation = NULL;
		trigStmt->funcname = NULL;	/* passed separately */
		trigStmt->args = trigargs;
		trigStmt->row = true;
		trigStmt->timing = trigForm->tgtype & TRIGGER_TYPE_TIMING_MASK;
		trigStmt->events = trigForm->tgtype & TRIGGER_TYPE_EVENT_MASK;
		trigStmt->columns = cols;
		trigStmt->whenClause = NULL;	/* passed separately */
		trigStmt->transitionRels = NIL; /* not supported at present */
		trigStmt->deferrable = trigForm->tgdeferrable;
		trigStmt->initdeferred = trigForm->tginitdeferred;
		trigStmt->constrrel = NULL; /* passed separately */

		CreateTriggerFiringOn(trigStmt, NULL, RelationGetRelid(partition),
							  trigForm->tgconstrrelid, InvalidOid, InvalidOid,
							  trigForm->tgfoid, trigForm->oid, qual,
							  false, true, trigForm->tgenabled);

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(perTupCxt);
	}

	MemoryContextDelete(perTupCxt);

	systable_endscan(scan);
	table_close(pg_trigger, RowExclusiveLock);
}


/*
 * Before acquiring lock on an index, acquire the same lock on the owning
 * table.
 */
struct AttachIndexCallbackState
{
	Oid			partitionOid;
	Oid			parentTblOid;
	bool		lockedParentTbl;
};


static void
RangeVarCallbackForAttachIndex(const RangeVar *rv, Oid relOid, Oid oldRelOid,
							   void *arg)
{
	struct AttachIndexCallbackState *state;
	Form_pg_class classform;
	HeapTuple	tuple;

	state = (struct AttachIndexCallbackState *) arg;

	if (!state->lockedParentTbl)
	{
		LockRelationOid(state->parentTblOid, AccessShareLock);
		state->lockedParentTbl = true;
	}

	/*
	 * If we previously locked some other heap, and the name we're looking up
	 * no longer refers to an index on that relation, release the now-useless
	 * lock.  XXX maybe we should do *after* we verify whether the index does
	 * not actually belong to the same relation ...
	 */
	if (relOid != oldRelOid && OidIsValid(state->partitionOid))
	{
		UnlockRelationOid(state->partitionOid, AccessShareLock);
		state->partitionOid = InvalidOid;
	}

	/* Didn't find a relation, so no need for locking or permission checks. */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped, so nothing to do */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	if (classform->relkind != RELKIND_PARTITIONED_INDEX &&
		classform->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("\"%s\" is not an index", rv->relname)));
	ReleaseSysCache(tuple);

	/*
	 * Since we need only examine the heap's tupledesc, an access share lock
	 * on it (preventing any DDL) is sufficient.
	 */
	state->partitionOid = IndexGetRelation(relOid, false);
	LockRelationOid(state->partitionOid, AccessShareLock);
}


/*
 * ALTER INDEX i1 ATTACH PARTITION i2
 */
static ObjectAddress
ATExecAttachPartitionIdx(List **wqueue, Relation parentIdx, RangeVar *name)
{
	Relation	partIdx;
	Relation	partTbl;
	Relation	parentTbl;
	ObjectAddress address;
	Oid			partIdxId;
	Oid			currParent;
	struct AttachIndexCallbackState state;

	/*
	 * We need to obtain lock on the index 'name' to modify it, but we also
	 * need to read its owning table's tuple descriptor -- so we need to lock
	 * both.  To avoid deadlocks, obtain lock on the table before doing so on
	 * the index.  Furthermore, we need to examine the parent table of the
	 * partition, so lock that one too.
	 */
	state.partitionOid = InvalidOid;
	state.parentTblOid = parentIdx->rd_index->indrelid;
	state.lockedParentTbl = false;
	partIdxId =
		RangeVarGetRelidExtended(name, AccessExclusiveLock, 0,
								 RangeVarCallbackForAttachIndex,
								 (void *) &state);
	/* Not there? */
	if (!OidIsValid(partIdxId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", name->relname)));

	/* no deadlock risk: RangeVarGetRelidExtended already acquired the lock */
	partIdx = relation_open(partIdxId, AccessExclusiveLock);

	/* we already hold locks on both tables, so this is safe: */
	parentTbl = relation_open(parentIdx->rd_index->indrelid, AccessShareLock);
	partTbl = relation_open(partIdx->rd_index->indrelid, NoLock);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partIdx));

	/* Silently do nothing if already in the right state */
	currParent = partIdx->rd_rel->relispartition ?
		get_partition_parent(partIdxId, false) : InvalidOid;
	if (currParent != RelationGetRelid(parentIdx))
	{
		IndexInfo  *childInfo;
		IndexInfo  *parentInfo;
		AttrMap    *attmap;
		bool		found;
		int			i;
		PartitionDesc partDesc;
		Oid			constraintOid,
					cldConstrId = InvalidOid;

		/*
		 * If this partition already has an index attached, refuse the
		 * operation.
		 */
		refuseDupeIndexAttach(parentIdx, partIdx, partTbl);

		if (OidIsValid(currParent))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("Index \"%s\" is already attached to another index.",
							   RelationGetRelationName(partIdx))));

		/* Make sure it indexes a partition of the other index's table */
		partDesc = RelationGetPartitionDesc(parentTbl, true);
		found = false;
		for (i = 0; i < partDesc->nparts; i++)
		{
			if (partDesc->oids[i] == state.partitionOid)
			{
				found = true;
				break;
			}
		}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("Index \"%s\" is not an index on any partition of table \"%s\".",
							   RelationGetRelationName(partIdx),
							   RelationGetRelationName(parentTbl))));

		/* Ensure the indexes are compatible */
		childInfo = BuildIndexInfo(partIdx);
		parentInfo = BuildIndexInfo(parentIdx);
		attmap = build_attrmap_by_name(RelationGetDescr(partTbl),
									   RelationGetDescr(parentTbl));
		if (!CompareIndexInfo(childInfo, parentInfo,
							  partIdx->rd_indcollation,
							  parentIdx->rd_indcollation,
							  partIdx->rd_opfamily,
							  parentIdx->rd_opfamily,
							  attmap))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("The index definitions do not match.")));

		/*
		 * If there is a constraint in the parent, make sure there is one in
		 * the child too.
		 */
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(parentTbl),
														RelationGetRelid(parentIdx));

		if (OidIsValid(constraintOid))
		{
			cldConstrId = get_relation_idx_constraint_oid(RelationGetRelid(partTbl),
														  partIdxId);
			if (!OidIsValid(cldConstrId))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
								RelationGetRelationName(partIdx),
								RelationGetRelationName(parentIdx)),
						 errdetail("The index \"%s\" belongs to a constraint in table \"%s\" but no constraint exists for index \"%s\".",
								   RelationGetRelationName(parentIdx),
								   RelationGetRelationName(parentTbl),
								   RelationGetRelationName(partIdx))));
		}

		/* All good -- do it */
		IndexSetParentIndex(partIdx, RelationGetRelid(parentIdx));
		if (OidIsValid(constraintOid))
			ConstraintSetParentConstraint(cldConstrId, constraintOid,
										  RelationGetRelid(partTbl));

		free_attrmap(attmap);

		validatePartitionedIndex(parentIdx, parentTbl);
	}

	relation_close(parentTbl, AccessShareLock);
	/* keep these locks till commit */
	relation_close(partTbl, NoLock);
	relation_close(partIdx, NoLock);

	return address;
}


/*
 * Verify whether the given partition already contains an index attached
 * to the given partitioned index.  If so, raise an error.
 */
static void
refuseDupeIndexAttach(Relation parentIdx, Relation partIdx, Relation partitionTbl)
{
	Oid			existingIdx;

	existingIdx = index_get_partition(partitionTbl,
									  RelationGetRelid(parentIdx));
	if (OidIsValid(existingIdx))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
						RelationGetRelationName(partIdx),
						RelationGetRelationName(parentIdx)),
				 errdetail("Another index is already attached for partition \"%s\".",
						   RelationGetRelationName(partitionTbl))));
}


/*
 * Verify whether the set of attached partition indexes to a parent index on
 * a partitioned table is complete.  If it is, mark the parent index valid.
 *
 * This should be called each time a partition index is attached.
 */
static void
validatePartitionedIndex(Relation partedIdx, Relation partedTbl)
{
	Relation	inheritsRel;
	SysScanDesc scan;
	ScanKeyData key;
	int			tuples = 0;
	HeapTuple	inhTup;
	bool		updated = false;

	Assert(partedIdx->rd_rel->relkind == RELKIND_PARTITIONED_INDEX);

	/*
	 * Scan pg_inherits for this parent index.  Count each valid index we find
	 * (verifying the pg_index entry for each), and if we reach the total
	 * amount we expect, we can mark this parent index as valid.
	 */
	inheritsRel = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(partedIdx)));
	scan = systable_beginscan(inheritsRel, InheritsParentIndexId, true,
							  NULL, 1, &key);
	while ((inhTup = systable_getnext(scan)) != NULL)
	{
		Form_pg_inherits inhForm = (Form_pg_inherits) GETSTRUCT(inhTup);
		HeapTuple	indTup;
		Form_pg_index indexForm;

		indTup = SearchSysCache1(INDEXRELID,
								 ObjectIdGetDatum(inhForm->inhrelid));
		if (!HeapTupleIsValid(indTup))
			elog(ERROR, "cache lookup failed for index %u", inhForm->inhrelid);
		indexForm = (Form_pg_index) GETSTRUCT(indTup);
		if (indexForm->indisvalid)
			tuples += 1;
		ReleaseSysCache(indTup);
	}

	/* Done with pg_inherits */
	systable_endscan(scan);
	table_close(inheritsRel, AccessShareLock);

	/*
	 * If we found as many inherited indexes as the partitioned table has
	 * partitions, we're good; update pg_index to set indisvalid.
	 */
	if (tuples == RelationGetPartitionDesc(partedTbl, true)->nparts)
	{
		Relation	idxRel;
		HeapTuple	newtup;

		idxRel = table_open(IndexRelationId, RowExclusiveLock);

		newtup = heap_copytuple(partedIdx->rd_indextuple);
		((Form_pg_index) GETSTRUCT(newtup))->indisvalid = true;
		updated = true;

		CatalogTupleUpdate(idxRel, &partedIdx->rd_indextuple->t_self, newtup);

		table_close(idxRel, RowExclusiveLock);
	}

	/*
	 * If this index is in turn a partition of a larger index, validating it
	 * might cause the parent to become valid also.  Try that.
	 */
	if (updated && partedIdx->rd_rel->relispartition)
	{
		Oid			parentIdxId,
					parentTblId;
		Relation	parentIdx,
					parentTbl;

		/* make sure we see the validation we just did */
		CommandCounterIncrement();

		parentIdxId = get_partition_parent(RelationGetRelid(partedIdx), false);
		parentTblId = get_partition_parent(RelationGetRelid(partedTbl), false);
		parentIdx = relation_open(parentIdxId, AccessExclusiveLock);
		parentTbl = relation_open(parentTblId, AccessExclusiveLock);
		Assert(!parentIdx->rd_index->indisvalid);

		validatePartitionedIndex(parentIdx, parentTbl);

		relation_close(parentIdx, AccessExclusiveLock);
		relation_close(parentTbl, AccessExclusiveLock);
	}
}


/*
 * ALTER TABLE DETACH PARTITION
 *
 * Return the address of the relation that is no longer a partition of rel.
 *
 * If concurrent mode is requested, we run in two transactions.  A side-
 * effect is that this command cannot run in a multi-part ALTER TABLE.
 * Currently, that's enforced by the grammar.
 *
 * The strategy for concurrency is to first modify the partition's
 * pg_inherit catalog row to make it visible to everyone that the
 * partition is detached, lock the partition against writes, and commit
 * the transaction; anyone who requests the partition descriptor from
 * that point onwards has to ignore such a partition.  In a second
 * transaction, we wait until all transactions that could have seen the
 * partition as attached are gone, then we remove the rest of partition
 * metadata (pg_inherits and pg_class.relpartbounds).
 */
static ObjectAddress
ATExecDetachPartition(List **wqueue, AlteredTableInfo *tab, Relation rel,
					  RangeVar *name, bool concurrent)
{
	Relation	partRel;
	ObjectAddress address;
	Oid			defaultPartOid;

	/*
	 * We must lock the default partition, because detaching this partition
	 * will change its partition constraint.
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));
	if (OidIsValid(defaultPartOid))
	{
		/*
		 * Concurrent detaching when a default partition exists is not
		 * supported. The main problem is that the default partition
		 * constraint would change.  And there's a definitional problem: what
		 * should happen to the tuples that are being inserted that belong to
		 * the partition being detached?  Putting them on the partition being
		 * detached would be wrong, since they'd become "lost" after the
		 * detaching completes but we cannot put them in the default partition
		 * either until we alter its partition constraint.
		 *
		 * I think we could solve this problem if we effected the constraint
		 * change before committing the first transaction.  But the lock would
		 * have to remain AEL and it would cause concurrent query planning to
		 * be blocked, so changing it that way would be even worse.
		 */
		if (concurrent)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot detach partitions concurrently when a default partition exists")));
		LockRelationOid(defaultPartOid, AccessExclusiveLock);
	}

	/*
	 * In concurrent mode, the partition is locked with share-update-exclusive
	 * in the first transaction.  This allows concurrent transactions to be
	 * doing DML to the partition.
	 */
	partRel = table_openrv(name, concurrent ? ShareUpdateExclusiveLock :
						   AccessExclusiveLock);

	/*
	 * Check inheritance conditions and either delete the pg_inherits row (in
	 * non-concurrent mode) or just set the inhdetachpending flag.
	 */
	if (!concurrent)
		RemoveInheritance(partRel, rel, false);
	else
		MarkInheritDetached(partRel, rel);

	/*
	 * Ensure that foreign keys still hold after this detach.  This keeps
	 * locks on the referencing tables, which prevents concurrent transactions
	 * from adding rows that we wouldn't see.  For this to work in concurrent
	 * mode, it is critical that the partition appears as no longer attached
	 * for the RI queries as soon as the first transaction commits.
	 */
	ATDetachCheckNoForeignKeyRefs(partRel);

	/*
	 * Concurrent mode has to work harder; first we add a new constraint to
	 * the partition that matches the partition constraint.  Then we close our
	 * existing transaction, and in a new one wait for all processes to catch
	 * up on the catalog updates we've done so far; at that point we can
	 * complete the operation.
	 */
	if (concurrent)
	{
		Oid			partrelid,
					parentrelid;
		LOCKTAG		tag;
		char	   *parentrelname;
		char	   *partrelname;

		/*
		 * Add a new constraint to the partition being detached, which
		 * supplants the partition constraint (unless there is one already).
		 */
		DetachAddConstraintIfNeeded(wqueue, partRel);

		/*
		 * We're almost done now; the only traces that remain are the
		 * pg_inherits tuple and the partition's relpartbounds.  Before we can
		 * remove those, we need to wait until all transactions that know that
		 * this is a partition are gone.
		 */

		/*
		 * Remember relation OIDs to re-acquire them later; and relation names
		 * too, for error messages if something is dropped in between.
		 */
		partrelid = RelationGetRelid(partRel);
		parentrelid = RelationGetRelid(rel);
		parentrelname = MemoryContextStrdup(PortalContext,
											RelationGetRelationName(rel));
		partrelname = MemoryContextStrdup(PortalContext,
										  RelationGetRelationName(partRel));

		/* Invalidate relcache entries for the parent -- must be before close */
		CacheInvalidateRelcache(rel);

		table_close(partRel, NoLock);
		table_close(rel, NoLock);
		tab->rel = NULL;

		/* Make updated catalog entry visible */
		PopActiveSnapshot();
		CommitTransactionCommand();

		StartTransactionCommand();

		/*
		 * Now wait.  This ensures that all queries that were planned
		 * including the partition are finished before we remove the rest of
		 * catalog entries.  We don't need or indeed want to acquire this
		 * lock, though -- that would block later queries.
		 *
		 * We don't need to concern ourselves with waiting for a lock on the
		 * partition itself, since we will acquire AccessExclusiveLock below.
		 */
		SET_LOCKTAG_RELATION(tag, MyDatabaseId, parentrelid);
		WaitForLockersMultiple(list_make1(&tag), AccessExclusiveLock, false);

		/*
		 * Now acquire locks in both relations again.  Note they may have been
		 * removed in the meantime, so care is required.
		 */
		rel = try_relation_open(parentrelid, ShareUpdateExclusiveLock);
		partRel = try_relation_open(partrelid, AccessExclusiveLock);

		/* If the relations aren't there, something bad happened; bail out */
		if (rel == NULL)
		{
			if (partRel != NULL)	/* shouldn't happen */
				elog(WARNING, "dangling partition \"%s\" remains, can't fix",
					 partrelname);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partitioned table \"%s\" was removed concurrently",
							parentrelname)));
		}
		if (partRel == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partition \"%s\" was removed concurrently", partrelname)));

		tab->rel = rel;
	}

	/* Do the final part of detaching */
	DetachPartitionFinalize(rel, partRel, concurrent, defaultPartOid);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	/* keep our lock until commit */
	table_close(partRel, NoLock);

	return address;
}


/*
 * Second part of ALTER TABLE .. DETACH.
 *
 * This is separate so that it can be run independently when the second
 * transaction of the concurrent algorithm fails (crash or abort).
 */
static void
DetachPartitionFinalize(Relation rel, Relation partRel, bool concurrent,
						Oid defaultPartOid)
{
	Relation	classRel;
	List	   *fks;
	ListCell   *cell;
	List	   *indexes;
	Datum		new_val[Natts_pg_class];
	bool		new_null[Natts_pg_class],
				new_repl[Natts_pg_class];
	HeapTuple	tuple,
				newtuple;

	if (concurrent)
	{
		/*
		 * We can remove the pg_inherits row now. (In the non-concurrent case,
		 * this was already done).
		 */
		RemoveInheritance(partRel, rel, true);
	}

	/* Drop any triggers that were cloned on creation/attach. */
	DropClonedTriggersFromPartition(RelationGetRelid(partRel));

	/*
	 * Detach any foreign keys that are inherited.  This includes creating
	 * additional action triggers.
	 */
	fks = copyObject(RelationGetFKeyList(partRel));
	foreach(cell, fks)
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);
		HeapTuple	contup;
		Form_pg_constraint conform;
		Constraint *fkconstraint;

		contup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(fk->conoid));
		if (!HeapTupleIsValid(contup))
			elog(ERROR, "cache lookup failed for constraint %u", fk->conoid);
		conform = (Form_pg_constraint) GETSTRUCT(contup);

		/* consider only the inherited foreign keys */
		if (conform->contype != CONSTRAINT_FOREIGN ||
			!OidIsValid(conform->conparentid))
		{
			ReleaseSysCache(contup);
			continue;
		}

		/* unset conparentid and adjust conislocal, coninhcount, etc. */
		ConstraintSetParentConstraint(fk->conoid, InvalidOid, InvalidOid);

		/*
		 * Make the action triggers on the referenced relation.  When this was
		 * a partition the action triggers pointed to the parent rel (they
		 * still do), but now we need separate ones of our own.
		 */
		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		fkconstraint->conname = pstrdup(NameStr(conform->conname));
		fkconstraint->deferrable = conform->condeferrable;
		fkconstraint->initdeferred = conform->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		fkconstraint->fk_attrs = NIL;
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = conform->confmatchtype;
		fkconstraint->fk_upd_action = conform->confupdtype;
		fkconstraint->fk_del_action = conform->confdeltype;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = true;

		createForeignKeyActionTriggers(partRel, conform->confrelid,
									   fkconstraint, fk->conoid,
									   conform->conindid);

		ReleaseSysCache(contup);
	}
	list_free_deep(fks);

	/*
	 * Any sub-constraints that are in the referenced-side of a larger
	 * constraint have to be removed.  This partition is no longer part of the
	 * key space of the constraint.
	 */
	foreach(cell, GetParentedForeignKeyRefs(partRel))
	{
		Oid			constrOid = lfirst_oid(cell);
		ObjectAddress constraint;

		ConstraintSetParentConstraint(constrOid, InvalidOid, InvalidOid);
		deleteDependencyRecordsForClass(ConstraintRelationId,
										constrOid,
										ConstraintRelationId,
										DEPENDENCY_INTERNAL);
		CommandCounterIncrement();

		ObjectAddressSet(constraint, ConstraintRelationId, constrOid);
		performDeletion(&constraint, DROP_RESTRICT, 0);
	}

	/* Now we can detach indexes */
	indexes = RelationGetIndexList(partRel);
	foreach(cell, indexes)
	{
		Oid			idxid = lfirst_oid(cell);
		Relation	idx;
		Oid			constrOid;

		if (!has_superclass(idxid))
			continue;

		Assert((IndexGetRelation(get_partition_parent(idxid, false), false) ==
				RelationGetRelid(rel)));

		idx = index_open(idxid, AccessExclusiveLock);
		IndexSetParentIndex(idx, InvalidOid);

		/* If there's a constraint associated with the index, detach it too */
		constrOid = get_relation_idx_constraint_oid(RelationGetRelid(partRel),
													idxid);
		if (OidIsValid(constrOid))
			ConstraintSetParentConstraint(constrOid, InvalidOid, InvalidOid);

		index_close(idx, NoLock);
	}

	/* Update pg_class tuple */
	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(partRel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(partRel));
	Assert(((Form_pg_class) GETSTRUCT(tuple))->relispartition);

	/* Clear relpartbound and reset relispartition */
	memset(new_val, 0, sizeof(new_val));
	memset(new_null, false, sizeof(new_null));
	memset(new_repl, false, sizeof(new_repl));
	new_val[Anum_pg_class_relpartbound - 1] = (Datum) 0;
	new_null[Anum_pg_class_relpartbound - 1] = true;
	new_repl[Anum_pg_class_relpartbound - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(classRel),
								 new_val, new_null, new_repl);

	((Form_pg_class) GETSTRUCT(newtuple))->relispartition = false;
	CatalogTupleUpdate(classRel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	table_close(classRel, RowExclusiveLock);

	if (OidIsValid(defaultPartOid))
	{
		/*
		 * If the relation being detached is the default partition itself,
		 * remove it from the parent's pg_partitioned_table entry.
		 *
		 * If not, we must invalidate default partition's relcache entry, as
		 * in StorePartitionBound: its partition constraint depends on every
		 * other partition's partition constraint.
		 */
		if (RelationGetRelid(partRel) == defaultPartOid)
			update_default_partition_oid(RelationGetRelid(rel), InvalidOid);
		else
			CacheInvalidateRelcacheByRelid(defaultPartOid);
	}

	/*
	 * Invalidate the parent's relcache so that the partition is no longer
	 * included in its partition descriptor.
	 */
	CacheInvalidateRelcache(rel);

	/*
	 * If the partition we just detached is partitioned itself, invalidate
	 * relcache for all descendent partitions too to ensure that their
	 * rd_partcheck expression trees are rebuilt; must lock partitions
	 * before doing so, using the same lockmode as what partRel has been
	 * locked with by the caller.
	 */
	if (partRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List   *children;

		children = find_all_inheritors(RelationGetRelid(partRel),
									   AccessExclusiveLock, NULL);
		foreach(cell, children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(cell));
		}
	}
}


/*
 * ALTER TABLE ... DETACH PARTITION ... FINALIZE
 *
 * To use when a DETACH PARTITION command previously did not run to
 * completion; this completes the detaching process.
 */
static ObjectAddress
ATExecDetachPartitionFinalize(Relation rel, RangeVar *name)
{
	Relation	partRel;
	ObjectAddress address;
	Snapshot	snap = GetActiveSnapshot();

	partRel = table_openrv(name, AccessExclusiveLock);

	/*
	 * Wait until existing snapshots are gone.  This is important if the
	 * second transaction of DETACH PARTITION CONCURRENTLY is canceled: the
	 * user could immediately run DETACH FINALIZE without actually waiting for
	 * existing transactions.  We must not complete the detach action until
	 * all such queries are complete (otherwise we would present them with an
	 * inconsistent view of catalogs).
	 */
	WaitForOlderSnapshots(snap->xmin, false);

	DetachPartitionFinalize(rel, partRel, true, InvalidOid);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	table_close(partRel, NoLock);

	return address;
}


/*
 * Obtain list of partitions of the given table, locking them all at the given
 * lockmode and ensuring that they all pass CheckTableNotInUse.
 *
 * This function is a no-op if the given relation is not a partitioned table;
 * in particular, nothing is done if it's a legacy inheritance parent.
 */
static void
ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode)
{
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *inh;
		ListCell   *cell;

		inh = find_all_inheritors(RelationGetRelid(rel), lockmode, NULL);
		/* first element is the parent rel; must ignore it */
		for_each_from(cell, inh, 1)
		{
			Relation	childrel;

			/* find_all_inheritors already got lock */
			childrel = table_open(lfirst_oid(cell), NoLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");
			table_close(childrel, NoLock);
		}
		list_free(inh);
	}
}


/*
 * ALTER COLUMN TYPE
 *
 * Unlike other subcommand types, we do parse transformation for ALTER COLUMN
 * TYPE during phase 1 --- the AlterTableCmd passed in here is already
 * transformed (and must be, because we rely on some transformed fields).
 *
 * The point of this is that the execution of all ALTER COLUMN TYPEs for a
 * table will be done "in parallel" during phase 3, so all the USING
 * expressions should be parsed assuming the original column types.  Also,
 * this allows a USING expression to refer to a field that will be dropped.
 *
 * To make this work safely, AT_PASS_DROP then AT_PASS_ALTER_TYPE must be
 * the first two execution steps in phase 2; they must not see the effects
 * of any other subcommand types, since the USING expressions are parsed
 * against the unmodified table's state.
 */
static void
ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd, LOCKMODE lockmode,
					  AlterTableUtilityContext *context)
{
	char	   *colName = cmd->name;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	TypeName   *typeName = def->typeName;
	Node	   *transform = def->cooked_default;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollid;
	NewColumnValue *newval;
	ParseState *pstate = make_parsestate(NULL);
	AclResult	aclresult;
	bool		is_expr;

	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter column type of typed table")));

	/* lookup the attribute so we can check inheritance status */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Can't alter a system attribute */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Don't alter inherited columns.  At outer level, there had better not be
	 * any inherited definition; when recursing, we assume this was checked at
	 * the parent level (see below).
	 */
	if (attTup->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter inherited column \"%s\"",
						colName)));

	/* Don't alter columns used in the partition key */
	if (has_partition_attrs(rel,
							bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
							&is_expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter column \"%s\" because it is part of the partition key of relation \"%s\"",
						colName, RelationGetRelationName(rel))));

	/* Look up the target type */
	typenameTypeIdAndMod(NULL, typeName, &targettype, &targettypmod);

	aclresult = pg_type_aclcheck(targettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, targettype);

	/* And the collation */
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/* make sure datatype is legal for a column */
	CheckAttributeType(colName, targettype, targetcollid,
					   list_make1_oid(rel->rd_rel->reltype),
					   0);

	if (tab->relkind == RELKIND_RELATION ||
		tab->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * Set up an expression to transform the old data value to the new
		 * type. If a USING option was given, use the expression as
		 * transformed by transformAlterTableStmt, else just take the old
		 * value and try to coerce it.  We do this first so that type
		 * incompatibility can be detected before we waste effort, and because
		 * we need the expression to be parsed against the original table row
		 * type.
		 */
		if (!transform)
		{
			transform = (Node *) makeVar(1, attnum,
										 attTup->atttypid, attTup->atttypmod,
										 attTup->attcollation,
										 0);
		}

		transform = coerce_to_target_type(pstate,
										  transform, exprType(transform),
										  targettype, targettypmod,
										  COERCION_ASSIGNMENT,
										  COERCE_IMPLICIT_CAST,
										  -1);
		if (transform == NULL)
		{
			/* error text depends on whether USING was specified or not */
			if (def->cooked_default != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("result of USING clause for column \"%s\""
								" cannot be cast automatically to type %s",
								colName, format_type_be(targettype)),
						 errhint("You might need to add an explicit cast.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype)),
				/* translator: USING is SQL, don't translate it */
						 errhint("You might need to specify \"USING %s::%s\".",
								 quote_identifier(colName),
								 format_type_with_typemod(targettype,
														  targettypmod))));
		}

		/* Fix collations after all else */
		assign_expr_collations(pstate, transform);

		/* Plan the expr now so we can accurately assess the need to rewrite. */
		transform = (Node *) expression_planner((Expr *) transform);

		/*
		 * Add a work queue item to make ATRewriteTable update the column
		 * contents.
		 */
		newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
		newval->attnum = attnum;
		newval->expr = (Expr *) transform;
		newval->is_generated = false;

		tab->newvals = lappend(tab->newvals, newval);
		if (ATColumnChangeRequiresRewrite(transform, attnum))
			tab->rewrite |= AT_REWRITE_COLUMN_REWRITE;
	}
	else if (transform)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (!RELKIND_HAS_STORAGE(tab->relkind))
	{
		/*
		 * For relations without storage, do this check now.  Regular tables
		 * will check it later when the table is being rewritten.
		 */
		find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
	}

	ReleaseSysCache(tuple);

	/*
	 * Recurse manually by queueing a new command for each child, if
	 * necessary. We cannot apply ATSimpleRecursion here because we need to
	 * remap attribute numbers in the USING expression, if any.
	 *
	 * If we are told not to recurse, there had better not be any child
	 * tables; else the alter would put them out of step.
	 */
	if (recurse)
	{
		Oid			relid = RelationGetRelid(rel);
		List	   *child_oids,
				   *child_numparents;
		ListCell   *lo,
				   *li;

		child_oids = find_all_inheritors(relid, lockmode,
										 &child_numparents);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 */
		forboth(lo, child_oids, li, child_numparents)
		{
			Oid			childrelid = lfirst_oid(lo);
			int			numparents = lfirst_int(li);
			Relation	childrel;
			HeapTuple	childtuple;
			Form_pg_attribute childattTup;

			if (childrelid == relid)
				continue;

			/* find_all_inheritors already got lock */
			childrel = relation_open(childrelid, NoLock);
			CheckTableNotInUse(childrel, "ALTER TABLE");

			/*
			 * Verify that the child doesn't have any inherited definitions of
			 * this column that came from outside this inheritance hierarchy.
			 * (renameatt makes a similar test, though in a different way
			 * because of its different recursion mechanism.)
			 */
			childtuple = SearchSysCacheAttName(RelationGetRelid(childrel),
											   colName);
			if (!HeapTupleIsValid(childtuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								colName, RelationGetRelationName(childrel))));
			childattTup = (Form_pg_attribute) GETSTRUCT(childtuple);

			if (childattTup->attinhcount > numparents)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot alter inherited column \"%s\" of relation \"%s\"",
								colName, RelationGetRelationName(childrel))));

			ReleaseSysCache(childtuple);

			/*
			 * Remap the attribute numbers.  If no USING expression was
			 * specified, there is no need for this step.
			 */
			if (def->cooked_default)
			{
				AttrMap    *attmap;
				bool		found_whole_row;

				/* create a copy to scribble on */
				cmd = copyObject(cmd);

				attmap = build_attrmap_by_name(RelationGetDescr(childrel),
											   RelationGetDescr(rel));
				((ColumnDef *) cmd->def)->cooked_default =
					map_variable_attnos(def->cooked_default,
										1, 0,
										attmap,
										InvalidOid, &found_whole_row);
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot convert whole-row table reference"),
							 errdetail("USING expression contains a whole-row table reference.")));
				pfree(attmap);
			}
			ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode, context);
			relation_close(childrel, NoLock);
		}
	}
	else if (!recursing &&
			 find_inheritance_children(RelationGetRelid(rel), NoLock) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("type of inherited column \"%s\" must be changed in child tables too",
						colName)));

	if (tab->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);
}


/*
 * ALTER COLUMN .. SET DATA TYPE
 *
 * Return the address of the modified column.
 */
static ObjectAddress
ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  AlterTableCmd *cmd, LOCKMODE lockmode)
{
	char	   *colName = cmd->name;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	TypeName   *typeName = def->typeName;
	HeapTuple	heapTup;
	Form_pg_attribute attTup,
				attOldTup;
	AttrNumber	attnum;
	HeapTuple	typeTuple;
	Form_pg_type tform;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollid;
	Node	   *defaultexpr;
	Relation	attrelation;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	depTup;
	ObjectAddress address;

	/*
	 * Clear all the missing values if we're rewriting the table, since this
	 * renders them pointless.
	 */
	if (tab->rewrite)
	{
		Relation	newrel;

		newrel = table_open(RelationGetRelid(rel), NoLock);
		RelationClearMissing(newrel);
		relation_close(newrel, NoLock);
		/* make sure we don't conflict with later attribute modifications */
		CommandCounterIncrement();
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	/* Look up the target column */
	heapTup = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(heapTup)) /* shouldn't happen */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
	attnum = attTup->attnum;
	attOldTup = TupleDescAttr(tab->oldDesc, attnum - 1);

	/* Check for multiple ALTER TYPE on same column --- can't cope */
	if (attTup->atttypid != attOldTup->atttypid ||
		attTup->atttypmod != attOldTup->atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter type of column \"%s\" twice",
						colName)));

	/* Look up the target type (should not fail, since prep found it) */
	typeTuple = typenameType(NULL, typeName, &targettypmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	targettype = tform->oid;
	/* And the collation */
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/*
	 * If there is a default expression for the column, get it and ensure we
	 * can coerce it to the new datatype.  (We must do this before changing
	 * the column type, because build_column_default itself will try to
	 * coerce, and will not issue the error message we want if it fails.)
	 *
	 * We remove any implicit coercion steps at the top level of the old
	 * default expression; this has been agreed to satisfy the principle of
	 * least surprise.  (The conversion to the new column type should act like
	 * it started from what the user sees as the stored expression, and the
	 * implicit coercions aren't going to be shown.)
	 */
	if (attTup->atthasdef)
	{
		defaultexpr = build_column_default(rel, attnum);
		Assert(defaultexpr);
		defaultexpr = strip_implicit_coercions(defaultexpr);
		defaultexpr = coerce_to_target_type(NULL,	/* no UNKNOWN params */
											defaultexpr, exprType(defaultexpr),
											targettype, targettypmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
		if (defaultexpr == NULL)
		{
			if (attTup->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("generation expression for column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("default for column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype))));
		}
	}
	else
		defaultexpr = NULL;

	/*
	 * Find everything that depends on the column (constraints, indexes, etc),
	 * and record enough information to let us recreate the objects.
	 *
	 * The actual recreation does not happen here, but only after we have
	 * performed all the individual ALTER TYPE operations.  We have to save
	 * the info before executing ALTER TYPE, though, else the deparser will
	 * get confused.
	 */
	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress foundObject;

		/* We don't expect any PIN dependencies on columns */
		if (foundDep->deptype == DEPENDENCY_PIN)
			elog(ERROR, "cannot alter type of a pinned column");

		foundObject.classId = foundDep->classid;
		foundObject.objectId = foundDep->objid;
		foundObject.objectSubId = foundDep->objsubid;

		switch (getObjectClass(&foundObject))
		{
			case OCLASS_CLASS:
				{
					char		relKind = get_rel_relkind(foundObject.objectId);

					if (relKind == RELKIND_INDEX ||
						relKind == RELKIND_PARTITIONED_INDEX)
					{
						Assert(foundObject.objectSubId == 0);
						RememberIndexForRebuilding(foundObject.objectId, tab);
					}
					else if (relKind == RELKIND_SEQUENCE)
					{
						/*
						 * This must be a SERIAL column's sequence.  We need
						 * not do anything to it.
						 */
						Assert(foundObject.objectSubId == 0);
					}
					else if (relKind == RELKIND_RELATION &&
							 foundObject.objectSubId != 0 &&
							 get_attgenerated(foundObject.objectId, foundObject.objectSubId))
					{
						/*
						 * Changing the type of a column that is used by a
						 * generated column is not allowed by SQL standard. It
						 * might be doable with some thinking and effort.
						 */
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("cannot alter type of a column used by a generated column"),
								 errdetail("Column \"%s\" is used by generated column \"%s\".",
										   colName, get_attname(foundObject.objectId, foundObject.objectSubId, false))));
					}
					else
					{
						/* Not expecting any other direct dependencies... */
						elog(ERROR, "unexpected object depending on column: %s",
							 getObjectDescription(&foundObject, false));
					}
					break;
				}

			case OCLASS_CONSTRAINT:
				Assert(foundObject.objectSubId == 0);
				RememberConstraintForRebuilding(foundObject.objectId, tab);
				break;

			case OCLASS_REWRITE:
				/* XXX someday see if we can cope with revising views */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used by a view or rule"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject, false),
								   colName)));
				break;

			case OCLASS_TRIGGER:

				/*
				 * A trigger can depend on a column because the column is
				 * specified as an update target, or because the column is
				 * used in the trigger's WHEN condition.  The first case would
				 * not require any extra work, but the second case would
				 * require updating the WHEN expression, which will take a
				 * significant amount of new code.  Since we can't easily tell
				 * which case applies, we punt for both.  FIXME someday.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used in a trigger definition"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject, false),
								   colName)));
				break;

			case OCLASS_POLICY:

				/*
				 * A policy can depend on a column because the column is
				 * specified in the policy's USING or WITH CHECK qual
				 * expressions.  It might be possible to rewrite and recheck
				 * the policy expression, but punt for now.  It's certainly
				 * easy enough to remove and recreate the policy; still, FIXME
				 * someday.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type of a column used in a policy definition"),
						 errdetail("%s depends on column \"%s\"",
								   getObjectDescription(&foundObject, false),
								   colName)));
				break;

			case OCLASS_DEFAULT:

				/*
				 * Ignore the column's default expression, since we will fix
				 * it below.
				 */
				Assert(defaultexpr);
				break;

			case OCLASS_STATISTIC_EXT:

				/*
				 * Give the extended-stats machinery a chance to fix anything
				 * that this column type change would break.
				 */
				RememberStatisticsForRebuilding(foundObject.objectId, tab);
				break;

			case OCLASS_PROC:
            case OCLASS_TYPE:
			case OCLASS_CAST:
			case OCLASS_COLLATION:
			case OCLASS_CONVERSION:
			case OCLASS_LANGUAGE:
			case OCLASS_LARGEOBJECT:
			case OCLASS_OPERATOR:
			case OCLASS_OPCLASS:
			case OCLASS_OPFAMILY:
			case OCLASS_AM:
			case OCLASS_AMOP:
			case OCLASS_AMPROC:
			case OCLASS_SCHEMA:
			case OCLASS_TSPARSER:
			case OCLASS_TSDICT:
			case OCLASS_TSTEMPLATE:
			case OCLASS_TSCONFIG:
			case OCLASS_ROLE:
			case OCLASS_DATABASE:
			case OCLASS_TBLSPACE:
			case OCLASS_FDW:
			case OCLASS_FOREIGN_SERVER:
			case OCLASS_USER_MAPPING:
			case OCLASS_DEFACL:
			case OCLASS_EXTENSION:
			case OCLASS_EVENT_TRIGGER:
			case OCLASS_PUBLICATION:
			case OCLASS_PUBLICATION_REL:
			case OCLASS_SUBSCRIPTION:
			case OCLASS_TRANSFORM:

				/*
				 * We don't expect any of these sorts of objects to depend on
				 * a column.
				 */
				elog(ERROR, "unexpected object depending on column: %s",
					 getObjectDescription(&foundObject, false));
				break;

				/*
				 * There's intentionally no default: case here; we want the
				 * compiler to warn if a new OCLASS hasn't been handled above.
				 */
		}
	}

	systable_endscan(scan);

	/*
	 * Now scan for dependencies of this column on other things.  The only
	 * thing we should find is the dependency on the column datatype, which we
	 * want to remove, possibly a collation dependency, and dependencies on
	 * other columns if it is a generated column.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress foundObject;

		foundObject.classId = foundDep->refclassid;
		foundObject.objectId = foundDep->refobjid;
		foundObject.objectSubId = foundDep->refobjsubid;

		if (foundDep->deptype != DEPENDENCY_NORMAL &&
			foundDep->deptype != DEPENDENCY_AUTO)
			elog(ERROR, "found unexpected dependency type '%c'",
				 foundDep->deptype);
		if (!(foundDep->refclassid == TypeRelationId &&
			  foundDep->refobjid == attTup->atttypid) &&
			!(foundDep->refclassid == CollationRelationId &&
			  foundDep->refobjid == attTup->attcollation) &&
			!(foundDep->refclassid == RelationRelationId &&
			  foundDep->refobjid == RelationGetRelid(rel) &&
			  foundDep->refobjsubid != 0)
			)
			elog(ERROR, "found unexpected dependency for column: %s",
				 getObjectDescription(&foundObject, false));

		CatalogTupleDelete(depRel, &depTup->t_self);
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	/*
	 * Here we go --- change the recorded column type and collation.  (Note
	 * heapTup is a copy of the syscache entry, so okay to scribble on.) First
	 * fix up the missing value if any.
	 */
	if (attTup->atthasmissing)
	{
		Datum		missingval;
		bool		missingNull;

		/* if rewrite is true the missing value should already be cleared */
		Assert(tab->rewrite == 0);

		/* Get the missing value datum */
		missingval = heap_getattr(heapTup,
								  Anum_pg_attribute_attmissingval,
								  attrelation->rd_att,
								  &missingNull);

		/* if it's a null array there is nothing to do */

		if (!missingNull)
		{
			/*
			 * Get the datum out of the array and repack it in a new array
			 * built with the new type data. We assume that since the table
			 * doesn't need rewriting, the actual Datum doesn't need to be
			 * changed, only the array metadata.
			 */

			int			one = 1;
			bool		isNull;
			Datum		valuesAtt[Natts_pg_attribute];
			bool		nullsAtt[Natts_pg_attribute];
			bool		replacesAtt[Natts_pg_attribute];
			HeapTuple	newTup;

			MemSet(valuesAtt, 0, sizeof(valuesAtt));
			MemSet(nullsAtt, false, sizeof(nullsAtt));
			MemSet(replacesAtt, false, sizeof(replacesAtt));

			missingval = array_get_element(missingval,
										   1,
										   &one,
										   0,
										   attTup->attlen,
										   attTup->attbyval,
										   attTup->attalign,
										   &isNull);
			missingval = PointerGetDatum(construct_array(&missingval,
														 1,
														 targettype,
														 tform->typlen,
														 tform->typbyval,
														 tform->typalign));

			valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
			replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;
			nullsAtt[Anum_pg_attribute_attmissingval - 1] = false;

			newTup = heap_modify_tuple(heapTup, RelationGetDescr(attrelation),
									   valuesAtt, nullsAtt, replacesAtt);
			heap_freetuple(heapTup);
			heapTup = newTup;
			attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
		}
	}

	attTup->atttypid = targettype;
	attTup->atttypmod = targettypmod;
	attTup->attcollation = targetcollid;
	attTup->attndims = list_length(typeName->arrayBounds);
	attTup->attlen = tform->typlen;
	attTup->attbyval = tform->typbyval;
	attTup->attalign = tform->typalign;
	attTup->attstorage = tform->typstorage;
	attTup->attcompression = InvalidCompressionMethod;

	ReleaseSysCache(typeTuple);

	CatalogTupleUpdate(attrelation, &heapTup->t_self, heapTup);

	table_close(attrelation, RowExclusiveLock);

	/* Install dependencies on new datatype and collation */
	add_column_datatype_dependency(RelationGetRelid(rel), attnum, targettype);
	add_column_collation_dependency(RelationGetRelid(rel), attnum, targetcollid);

	/*
	 * Drop any pg_statistic entry for the column, since it's now wrong type
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	/*
	 * Update the default, if present, by brute force --- remove and re-add
	 * the default.  Probably unsafe to take shortcuts, since the new version
	 * may well have additional dependencies.  (It's okay to do this now,
	 * rather than after other ALTER TYPE commands, since the default won't
	 * depend on other column types.)
	 */
	if (defaultexpr)
	{
		/* Must make new row visible since it will be updated again */
		CommandCounterIncrement();

		/*
		 * We use RESTRICT here for safety, but at present we do not expect
		 * anything to depend on the default.
		 */
		RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, true,
						  true);

		StoreAttrDefault(rel, attnum, defaultexpr, true, false);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	/* Cleanup */
	heap_freetuple(heapTup);

	return address;
}


/*
 * Execute ALTER TABLE SET TABLESPACE for cases where there is no tuple
 * rewriting to be done, so we just want to copy the data as fast as possible.
 */
static void
ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode)
{
	Relation	rel;
	Oid			reltoastrelid;
	Oid			newrelfilenode;
	RelFileNode newrnode;
	List	   *reltoastidxids = NIL;
	ListCell   *lc;

	/*
	 * Need lock here in case we are recursing to toast table or index
	 */
	rel = relation_open(tableOid, lockmode);

	/* Check first if relation can be moved to new tablespace */
	if (!CheckRelationTableSpaceMove(rel, newTableSpace))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel), 0);
		relation_close(rel, NoLock);
		return;
	}

	reltoastrelid = rel->rd_rel->reltoastrelid;
	/* Fetch the list of indexes on toast relation if necessary */
	if (OidIsValid(reltoastrelid))
	{
		Relation	toastRel = relation_open(reltoastrelid, lockmode);

		reltoastidxids = RelationGetIndexList(toastRel);
		relation_close(toastRel, lockmode);
	}

	/*
	 * Relfilenodes are not unique in databases across tablespaces, so we need
	 * to allocate a new one in the new tablespace.
	 */
	newrelfilenode = GetNewRelFileNode(newTableSpace, NULL,
									   rel->rd_rel->relpersistence);

	/* Open old and new relation */
	newrnode = rel->rd_node;
	newrnode.relNode = newrelfilenode;
	newrnode.spcNode = newTableSpace;

	/* hand off to AM to actually create the new filenode and copy the data */
	if (rel->rd_rel->relkind == RELKIND_INDEX)
	{
		index_copy_data(rel, newrnode);
	}
	else
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_MATVIEW ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		table_relation_copy_data(rel, &newrnode);
	}

	/*
	 * Update the pg_class row.
	 *
	 * NB: This wouldn't work if ATExecSetTableSpace() were allowed to be
	 * executed on pg_class or its indexes (the above copy wouldn't contain
	 * the updated pg_class entry), but that's forbidden with
	 * CheckRelationTableSpaceMove().
	 */
	SetRelationTableSpace(rel, newTableSpace, newrelfilenode);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	RelationAssumeNewRelfilenode(rel);

	relation_close(rel, NoLock);

	/* Make sure the reltablespace change is visible */
	CommandCounterIncrement();

	/* Move associated toast relation and/or indexes, too */
	if (OidIsValid(reltoastrelid))
		ATExecSetTableSpace(reltoastrelid, newTableSpace, lockmode);
	foreach(lc, reltoastidxids)
		ATExecSetTableSpace(lfirst_oid(lc), newTableSpace, lockmode);

	/* Clean up */
	list_free(reltoastidxids);
}


/*
 * Scan the existing rows in a table to verify they meet a proposed FK
 * constraint.
 *
 * Caller must have opened and locked both relations appropriately.
 */
static void
validateForeignKeyConstraint(char *conname,
							 Relation rel,
							 Relation pkrel,
							 Oid pkindOid,
							 Oid constraintOid)
{
	TupleTableSlot *slot;
	TableScanDesc scan;
	Trigger		trig;
	Snapshot	snapshot;
	MemoryContext oldcxt;
	MemoryContext perTupCxt;

	ereport(DEBUG1,
			(errmsg_internal("validating foreign key constraint \"%s\"", conname)));

	/*
	 * Build a trigger call structure; we'll need it either way.
	 */
	MemSet(&trig, 0, sizeof(trig));
	trig.tgoid = InvalidOid;
	trig.tgname = conname;
	trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
	trig.tgisinternal = true;
	trig.tgconstrrelid = RelationGetRelid(pkrel);
	trig.tgconstrindid = pkindOid;
	trig.tgconstraint = constraintOid;
	trig.tgdeferrable = false;
	trig.tginitdeferred = false;
	/* we needn't fill in remaining fields */

	/*
	 * See if we can do it with a single LEFT JOIN query.  A false result
	 * indicates we must proceed with the fire-the-trigger method.
	 */
	if (RI_Initial_Check(&trig, rel, pkrel))
		return;

	/*
	 * Scan through each tuple, calling RI_FKey_check_ins (insert trigger) as
	 * if that tuple had just been inserted.  If any of those fail, it should
	 * ereport(ERROR) and that's that.
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL);

	perTupCxt = AllocSetContextCreate(CurrentMemoryContext,
									  "validateForeignKeyConstraint",
									  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(perTupCxt);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		LOCAL_FCINFO(fcinfo, 0);
		TriggerData trigdata = {0};

		CHECK_FOR_INTERRUPTS();

		/*
		 * Make a call to the trigger function
		 *
		 * No parameters are passed, but we do set a context
		 */
		MemSet(fcinfo, 0, SizeForFunctionCallInfo(0));

		/*
		 * We assume RI_FKey_check_ins won't look at flinfo...
		 */
		trigdata.type = T_TriggerData;
		trigdata.tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
		trigdata.tg_relation = rel;
		trigdata.tg_trigtuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		trigdata.tg_trigslot = slot;
		trigdata.tg_trigger = &trig;

		fcinfo->context = (Node *) &trigdata;

		RI_FKey_check_ins(fcinfo);

		MemoryContextReset(perTupCxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(perTupCxt);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
}


/*
 * Preparation phase for SET LOGGED/UNLOGGED
 *
 * This verifies that we're not trying to change a temp table.  Also,
 * existing foreign key constraints are checked to avoid ending up with
 * permanent tables referencing unlogged tables.
 *
 * Return value is false if the operation is a no-op (in which case the
 * checks are skipped), otherwise true.
 */
static bool
ATPrepChangePersistence(Relation rel, bool toLogged)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];

	/*
	 * Disallow changing status for a temp table.  Also verify whether we can
	 * get away with doing nothing; in such cases we don't need to run the
	 * checks below, either.
	 */
	switch (rel->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change logged status of table \"%s\" because it is temporary",
							RelationGetRelationName(rel)),
					 errtable(rel)));
			break;
		case RELPERSISTENCE_PERMANENT:
			if (toLogged)
				/* nothing to do */
				return false;
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (!toLogged)
				/* nothing to do */
				return false;
			break;
	}

	/*
	 * Check that the table is not part any publication when changing to
	 * UNLOGGED as UNLOGGED tables can't be published.
	 */
	if (!toLogged &&
		list_length(GetRelationPublications(RelationGetRelid(rel))) > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot change table \"%s\" to unlogged because it is part of a publication",
						RelationGetRelationName(rel)),
				 errdetail("Unlogged relations cannot be replicated.")));

	/*
	 * Check existing foreign key constraints to preserve the invariant that
	 * permanent tables cannot reference unlogged ones.  Self-referencing
	 * foreign keys can safely be ignored.
	 */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	/*
	 * Scan conrelid if changing to permanent, else confrelid.  This also
	 * determines whether a useful index exists.
	 */
	ScanKeyInit(&skey[0],
				toLogged ? Anum_pg_constraint_conrelid :
				Anum_pg_constraint_confrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(pg_constraint,
							  toLogged ? ConstraintRelidTypidNameIndexId : InvalidOid,
							  true, NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		if (con->contype == CONSTRAINT_FOREIGN)
		{
			Oid			foreignrelid;
			Relation	foreignrel;

			/* the opposite end of what we used as scankey */
			foreignrelid = toLogged ? con->confrelid : con->conrelid;

			/* ignore if self-referencing */
			if (RelationGetRelid(rel) == foreignrelid)
				continue;

			foreignrel = relation_open(foreignrelid, AccessShareLock);

			if (toLogged)
			{
				if (!RelationIsPermanent(foreignrel))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not change table \"%s\" to logged because it references unlogged table \"%s\"",
									RelationGetRelationName(rel),
									RelationGetRelationName(foreignrel)),
							 errtableconstraint(rel, NameStr(con->conname))));
			}
			else
			{
				if (RelationIsPermanent(foreignrel))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not change table \"%s\" to unlogged because it references logged table \"%s\"",
									RelationGetRelationName(rel),
									RelationGetRelationName(foreignrel)),
							 errtableconstraint(rel, NameStr(con->conname))));
			}

			relation_close(foreignrel, AccessShareLock);
		}
	}

	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);

	return true;
}


/*
 * ALTER TABLE SET TABLESPACE
 */
static void
ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel, const char *tablespacename, LOCKMODE lockmode)
{
	Oid			tablespaceId;

	/* Check that the tablespace exists */
	tablespaceId = get_tablespace_oid(tablespacename, false);

	/* Check permissions except when moving to database's default */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE, tablespacename);
	}

	/* Save info for Phase 3 to do the real work */
	if (OidIsValid(tab->newTableSpace))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot have multiple SET TABLESPACE subcommands")));

	tab->newTableSpace = tablespaceId;
}


/*
 * ALTER TABLE INHERIT
 *
 * Add a parent to the child's parents. This verifies that all the columns and
 * check constraints of the parent appear in the child and that they have the
 * same data types and expressions.
 */
static void
ATPrepAddInherit(Relation child_rel)
{
	if (child_rel->rd_rel->reloftype)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of typed table")));

	if (child_rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of a partition")));

	if (child_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of partitioned table")));
}


/*
 * When the data type of a column is changed, a rewrite might not be required
 * if the new type is sufficiently identical to the old one, and the USING
 * clause isn't trying to insert some other value.  It's safe to skip the
 * rewrite in these cases:
 *
 * - the old type is binary coercible to the new type
 * - the new type is an unconstrained domain over the old type
 * - {NEW,OLD} or {OLD,NEW} is {timestamptz,timestamp} and the timezone is UTC
 *
 * In the case of a constrained domain, we could get by with scanning the
 * table and checking the constraint rather than actually rewriting it, but we
 * don't currently try to do that.
 */
static bool
ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno)
{
	Assert(expr != NULL);

	for (;;)
	{
		/* only one varno, so no need to check that */
		if (IsA(expr, Var) && ((Var *) expr)->varattno == varattno)
			return false;
		else if (IsA(expr, RelabelType))
			expr = (Node *) ((RelabelType *) expr)->arg;
		else if (IsA(expr, CoerceToDomain))
		{
			CoerceToDomain *d = (CoerceToDomain *) expr;

			if (DomainHasConstraints(d->resulttype))
				return true;
			expr = (Node *) d->arg;
		}
		else if (IsA(expr, FuncExpr))
		{
			FuncExpr   *f = (FuncExpr *) expr;

			switch (f->funcid)
			{
				case F_TIMESTAMPTZ_TIMESTAMP:
				case F_TIMESTAMP_TIMESTAMPTZ:
					if (TimestampTimestampTzRequiresRewrite())
						return true;
					else
						expr = linitial(f->args);
					break;
				default:
					return true;
			}
		}
		else
			return true;
	}
}


/*
 * Cleanup after we've finished all the ALTER TYPE operations for a
 * particular relation.  We have to drop and recreate all the indexes
 * and constraints that depend on the altered columns.  We do the
 * actual dropping here, but re-creation is managed by adding work
 * queue entries to do those steps later.
 */
static void
ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab, LOCKMODE lockmode)
{
	ObjectAddress obj;
	ObjectAddresses *objects;
	ListCell   *def_item;
	ListCell   *oid_item;

	/*
	 * Collect all the constraints and indexes to drop so we can process them
	 * in a single call.  That way we don't have to worry about dependencies
	 * among them.
	 */
	objects = new_object_addresses();

	/*
	 * Re-parse the index and constraint definitions, and attach them to the
	 * appropriate work queue entries.  We do this before dropping because in
	 * the case of a FOREIGN KEY constraint, we might not yet have exclusive
	 * lock on the table the constraint is attached to, and we need to get
	 * that before reparsing/dropping.
	 *
	 * We can't rely on the output of deparsing to tell us which relation to
	 * operate on, because concurrent activity might have made the name
	 * resolve differently.  Instead, we've got to use the OID of the
	 * constraint or index we're processing to figure out which relation to
	 * operate on.
	 */
	forboth(oid_item, tab->changedConstraintOids,
			def_item, tab->changedConstraintDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		HeapTuple	tup;
		Form_pg_constraint con;
		Oid			relid;
		Oid			confrelid;
		char		contype;
		bool		conislocal;

		tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
		if (!HeapTupleIsValid(tup)) /* should not happen */
			elog(ERROR, "cache lookup failed for constraint %u", oldId);
		con = (Form_pg_constraint) GETSTRUCT(tup);
		if (OidIsValid(con->conrelid))
			relid = con->conrelid;
		else
		{
			/* must be a domain constraint */
			relid = get_typ_typrelid(getBaseType(con->contypid));
			if (!OidIsValid(relid))
				elog(ERROR, "could not identify relation associated with constraint %u", oldId);
		}
		confrelid = con->confrelid;
		contype = con->contype;
		conislocal = con->conislocal;
		ReleaseSysCache(tup);

		ObjectAddressSet(obj, ConstraintRelationId, oldId);
		add_exact_object_address(&obj, objects);

		/*
		 * If the constraint is inherited (only), we don't want to inject a
		 * new definition here; it'll get recreated when ATAddCheckConstraint
		 * recurses from adding the parent table's constraint.  But we had to
		 * carry the info this far so that we can drop the constraint below.
		 */
		if (!conislocal)
			continue;

		/*
		 * When rebuilding an FK constraint that references the table we're
		 * modifying, we might not yet have any lock on the FK's table, so get
		 * one now.  We'll need AccessExclusiveLock for the DROP CONSTRAINT
		 * step, so there's no value in asking for anything weaker.
		 */
		if (relid != tab->relid && contype == CONSTRAINT_FOREIGN)
			LockRelationOid(relid, AccessExclusiveLock);

		ATPostAlterTypeParse(oldId, relid, confrelid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);
	}
	forboth(oid_item, tab->changedIndexOids,
			def_item, tab->changedIndexDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		Oid			relid;

		relid = IndexGetRelation(oldId, false);
		ATPostAlterTypeParse(oldId, relid, InvalidOid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);

		ObjectAddressSet(obj, RelationRelationId, oldId);
		add_exact_object_address(&obj, objects);
	}

	/* add dependencies for new statistics */
	forboth(oid_item, tab->changedStatisticsOids,
			def_item, tab->changedStatisticsDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		Oid			relid;

		relid = StatisticsGetRelation(oldId, false);
		ATPostAlterTypeParse(oldId, relid, InvalidOid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);

		ObjectAddressSet(obj, StatisticExtRelationId, oldId);
		add_exact_object_address(&obj, objects);
	}

	/*
	 * Queue up command to restore replica identity index marking
	 */
	if (tab->replicaIdentityIndex)
	{
		AlterTableCmd *cmd = makeNode(AlterTableCmd);
		ReplicaIdentityStmt *subcmd = makeNode(ReplicaIdentityStmt);

		subcmd->identity_type = REPLICA_IDENTITY_INDEX;
		subcmd->name = tab->replicaIdentityIndex;
		cmd->subtype = AT_ReplicaIdentity;
		cmd->def = (Node *) subcmd;

		/* do it after indexes and constraints */
		tab->subcmds[AT_PASS_OLD_CONSTR] =
			lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
	}

	/*
	 * Queue up command to restore marking of index used for cluster.
	 */
	if (tab->clusterOnIndex)
	{
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_ClusterOn;
		cmd->name = tab->clusterOnIndex;

		/* do it after indexes and constraints */
		tab->subcmds[AT_PASS_OLD_CONSTR] =
			lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
	}

	/*
	 * It should be okay to use DROP_RESTRICT here, since nothing else should
	 * be depending on these objects.
	 */
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	free_object_addresses(objects);

	/*
	 * The objects will get recreated during subsequent passes over the work
	 * queue.
	 */
}


/*
 * Parse the previously-saved definition string for a constraint, index or
 * statistics object against the newly-established column data type(s), and
 * queue up the resulting command parsetrees for execution.
 *
 * This might fail if, for example, you have a WHERE clause that uses an
 * operator that's not available for the new column type.
 */
static void
ATPostAlterTypeParse(Oid oldId, Oid oldRelId, Oid refRelId, char *cmd,
					 List **wqueue, LOCKMODE lockmode, bool rewrite)
{
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *list_item;
	Relation	rel;

	/*
	 * We expect that we will get only ALTER TABLE and CREATE INDEX
	 * statements. Hence, there is no need to pass them through
	 * parse_analyze() or the rewriter, but instead we need to pass them
	 * through parse_utilcmd.c to make them ready for execution.
	 */
	raw_parsetree_list = raw_parser(cmd, RAW_PARSE_DEFAULT);
	querytree_list = NIL;
	foreach(list_item, raw_parsetree_list)
	{
		RawStmt    *rs = lfirst_node(RawStmt, list_item);
		Node	   *stmt = rs->stmt;

		if (IsA(stmt, IndexStmt))
			querytree_list = lappend(querytree_list,
									 transformIndexStmt(oldRelId,
														(IndexStmt *) stmt,
														cmd));
		else if (IsA(stmt, AlterTableStmt))
		{
			List	   *beforeStmts;
			List	   *afterStmts;

			stmt = (Node *) transformAlterTableStmt(oldRelId,
													(AlterTableStmt *) stmt,
													cmd,
													&beforeStmts,
													&afterStmts);
			querytree_list = list_concat(querytree_list, beforeStmts);
			querytree_list = lappend(querytree_list, stmt);
			querytree_list = list_concat(querytree_list, afterStmts);
		}
		else if (IsA(stmt, CreateStatsStmt))
			querytree_list = lappend(querytree_list,
									 transformStatsStmt(oldRelId,
														(CreateStatsStmt *) stmt,
														cmd));
		else
			querytree_list = lappend(querytree_list, stmt);
	}

	/* Caller should already have acquired whatever lock we need. */
	rel = relation_open(oldRelId, NoLock);

	/*
	 * Attach each generated command to the proper place in the work queue.
	 * Note this could result in creation of entirely new work-queue entries.
	 *
	 * Also note that we have to tweak the command subtypes, because it turns
	 * out that re-creation of indexes and constraints has to act a bit
	 * differently from initial creation.
	 */
	foreach(list_item, querytree_list)
	{
		Node	   *stm = (Node *) lfirst(list_item);
		AlteredTableInfo *tab;

		tab = ATGetQueueEntry(wqueue, rel);

		if (IsA(stm, IndexStmt))
		{
			IndexStmt  *stmt = (IndexStmt *) stm;
			AlterTableCmd *newcmd;

			if (!rewrite)
				TryReuseIndex(oldId, stmt);
			stmt->reset_default_tblspc = true;
			/* keep the index's comment */
			stmt->idxcomment = GetComment(oldId, RelationRelationId, 0);

			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = AT_ReAddIndex;
			newcmd->def = (Node *) stmt;
			tab->subcmds[AT_PASS_OLD_INDEX] =
				lappend(tab->subcmds[AT_PASS_OLD_INDEX], newcmd);
		}
		else if (IsA(stm, AlterTableStmt))
		{
			AlterTableStmt *stmt = (AlterTableStmt *) stm;
			ListCell   *lcmd;

			foreach(lcmd, stmt->cmds)
			{
				AlterTableCmd *cmd = castNode(AlterTableCmd, lfirst(lcmd));

				if (cmd->subtype == AT_AddIndex)
				{
					IndexStmt  *indstmt;
					Oid			indoid;

					indstmt = castNode(IndexStmt, cmd->def);
					indoid = get_constraint_index(oldId);

					if (!rewrite)
						TryReuseIndex(indoid, indstmt);
					/* keep any comment on the index */
					indstmt->idxcomment = GetComment(indoid,
													 RelationRelationId, 0);
					indstmt->reset_default_tblspc = true;

					cmd->subtype = AT_ReAddIndex;
					tab->subcmds[AT_PASS_OLD_INDEX] =
						lappend(tab->subcmds[AT_PASS_OLD_INDEX], cmd);

					/* recreate any comment on the constraint */
					RebuildConstraintComment(tab,
											 AT_PASS_OLD_INDEX,
											 oldId,
											 rel,
											 NIL,
											 indstmt->idxname);
				}
				else if (cmd->subtype == AT_AddConstraint)
				{
					Constraint *con = castNode(Constraint, cmd->def);

					con->old_pktable_oid = refRelId;
					/* rewriting neither side of a FK */
					if (con->contype == CONSTR_FOREIGN &&
						!rewrite && tab->rewrite == 0)
						TryReuseForeignKey(oldId, con);
					con->reset_default_tblspc = true;
					cmd->subtype = AT_ReAddConstraint;
					tab->subcmds[AT_PASS_OLD_CONSTR] =
						lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

					/* recreate any comment on the constraint */
					RebuildConstraintComment(tab,
											 AT_PASS_OLD_CONSTR,
											 oldId,
											 rel,
											 NIL,
											 con->conname);
				}
				else if (cmd->subtype == AT_SetNotNull)
				{
					/*
					 * The parser will create AT_SetNotNull subcommands for
					 * columns of PRIMARY KEY indexes/constraints, but we need
					 * not do anything with them here, because the columns'
					 * NOT NULL marks will already have been propagated into
					 * the new table definition.
					 */
				}
				else
					elog(ERROR, "unexpected statement subtype: %d",
						 (int) cmd->subtype);
			}
		}
		else if (IsA(stm, AlterDomainStmt))
		{
			AlterDomainStmt *stmt = (AlterDomainStmt *) stm;

			if (stmt->subtype == 'C')	/* ADD CONSTRAINT */
			{
				Constraint *con = castNode(Constraint, stmt->def);
				AlterTableCmd *cmd = makeNode(AlterTableCmd);

				cmd->subtype = AT_ReAddDomainConstraint;
				cmd->def = (Node *) stmt;
				tab->subcmds[AT_PASS_OLD_CONSTR] =
					lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

				/* recreate any comment on the constraint */
				RebuildConstraintComment(tab,
										 AT_PASS_OLD_CONSTR,
										 oldId,
										 NULL,
										 stmt->typeName,
										 con->conname);
			}
			else
				elog(ERROR, "unexpected statement subtype: %d",
					 (int) stmt->subtype);
		}
		else if (IsA(stm, CreateStatsStmt))
		{
			CreateStatsStmt *stmt = (CreateStatsStmt *) stm;
			AlterTableCmd *newcmd;

			/* keep the statistics object's comment */
			stmt->stxcomment = GetComment(oldId, StatisticExtRelationId, 0);

			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = AT_ReAddStatistics;
			newcmd->def = (Node *) stmt;
			tab->subcmds[AT_PASS_MISC] =
				lappend(tab->subcmds[AT_PASS_MISC], newcmd);
		}
		else
			elog(ERROR, "unexpected statement type: %d",
				 (int) nodeTag(stm));
	}

	relation_close(rel, NoLock);
}


/*
 * Subroutine for ATPostAlterTypeParse() to recreate any existing comment
 * for a table or domain constraint that is being rebuilt.
 *
 * objid is the OID of the constraint.
 * Pass "rel" for a table constraint, or "domname" (domain's qualified name
 * as a string list) for a domain constraint.
 * (We could dig that info, as well as the conname, out of the pg_constraint
 * entry; but callers already have them so might as well pass them.)
 */
static void
RebuildConstraintComment(AlteredTableInfo *tab, int pass, Oid objid,
						 Relation rel, List *domname,
						 const char *conname)
{
	CommentStmt *cmd;
	char	   *comment_str;
	AlterTableCmd *newcmd;

	/* Look for comment for object wanted, and leave if none */
	comment_str = GetComment(objid, ConstraintRelationId, 0);
	if (comment_str == NULL)
		return;

	/* Build CommentStmt node, copying all input data for safety */
	cmd = makeNode(CommentStmt);
	if (rel)
	{
		cmd->objtype = OBJECT_TABCONSTRAINT;
		cmd->object = (Node *)
			list_make3(makeString(get_namespace_name(RelationGetNamespace(rel))),
					   makeString(pstrdup(RelationGetRelationName(rel))),
					   makeString(pstrdup(conname)));
	}
	else
	{
		cmd->objtype = OBJECT_DOMCONSTRAINT;
		cmd->object = (Node *)
			list_make2(makeTypeNameFromNameList(copyObject(domname)),
					   makeString(pstrdup(conname)));
	}
	cmd->comment = comment_str;

	/* Append it to list of commands */
	newcmd = makeNode(AlterTableCmd);
	newcmd->subtype = AT_ReAddComment;
	newcmd->def = (Node *) cmd;
	tab->subcmds[pass] = lappend(tab->subcmds[pass], newcmd);
}


/*
 * Subroutine for ATPostAlterTypeParse().  Calls out to CheckIndexCompatible()
 * for the real analysis, then mutates the IndexStmt based on that verdict.
 */
static void
TryReuseIndex(Oid oldId, IndexStmt *stmt)
{
	if (CheckIndexCompatible(oldId,
							 stmt->accessMethod,
							 stmt->indexParams,
							 stmt->excludeOpNames))
	{
		Relation	irel = index_open(oldId, NoLock);

		/* If it's a partitioned index, there is no storage to share. */
		if (irel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			stmt->oldNode = irel->rd_node.relNode;
			stmt->oldCreateSubid = irel->rd_createSubid;
			stmt->oldFirstRelfilenodeSubid = irel->rd_firstRelfilenodeSubid;
		}
		index_close(irel, NoLock);
	}
}


/*
 * Subroutine for ATPostAlterTypeParse().
 *
 * Stash the old P-F equality operator into the Constraint node, for possible
 * use by ATAddForeignKeyConstraint() in determining whether revalidation of
 * this constraint can be skipped.
 */
static void
TryReuseForeignKey(Oid oldId, Constraint *con)
{
	HeapTuple	tup;
	Datum		adatum;
	bool		isNull;
	ArrayType  *arr;
	Oid		   *rawarr;
	int			numkeys;
	int			i;

	Assert(con->contype == CONSTR_FOREIGN);
	Assert(con->old_conpfeqop == NIL);	/* already prepared this node */

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for constraint %u", oldId);

	adatum = SysCacheGetAttr(CONSTROID, tup,
							 Anum_pg_constraint_conpfeqop, &isNull);
	if (isNull)
		elog(ERROR, "null conpfeqop for constraint %u", oldId);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	numkeys = ARR_DIMS(arr)[0];
	/* test follows the one in ri_FetchConstraintInfo() */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "conpfeqop is not a 1-D Oid array");
	rawarr = (Oid *) ARR_DATA_PTR(arr);

	/* stash a List of the operator Oids in our Constraint node */
	for (i = 0; i < numkeys; i++)
		con->old_conpfeqop = lappend_oid(con->old_conpfeqop, rawarr[i]);

	ReleaseSysCache(tup);
}


/*
 * If a new or renamed column will collide with the name of an existing
 * column and if_not_exists is false then error out, else do nothing.
 */
static bool
check_for_column_name_collision(Relation rel, const char *colname,
								bool if_not_exists)
{
	HeapTuple	attTuple;
	int			attnum;

	/*
	 * this test is deliberately not attisdropped-aware, since if one tries to
	 * add a column matching a dropped column name, it's gonna fail anyway.
	 */
	attTuple = SearchSysCache2(ATTNAME,
							   ObjectIdGetDatum(RelationGetRelid(rel)),
							   PointerGetDatum(colname));
	if (!HeapTupleIsValid(attTuple))
		return true;

	attnum = ((Form_pg_attribute) GETSTRUCT(attTuple))->attnum;
	ReleaseSysCache(attTuple);

	/*
	 * We throw a different error message for conflicts with system column
	 * names, since they are normally not shown and the user might otherwise
	 * be confused about the reason for the conflict.
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column name \"%s\" conflicts with a system column name",
						colname)));
	else
	{
		if (if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" already exists, skipping",
							colname, RelationGetRelationName(rel))));
			return false;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" already exists",
						colname, RelationGetRelationName(rel))));
	}

	return true;
}


/*
 * Install a column's dependency on its datatype.
 */
static void
add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid)
{
	ObjectAddress myself,
				referenced;

	myself.classId = RelationRelationId;
	myself.objectId = relid;
	myself.objectSubId = attnum;
	referenced.classId = TypeRelationId;
	referenced.objectId = typid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * Install a column's dependency on its collation.
 */
static void
add_column_collation_dependency(Oid relid, int32 attnum, Oid collid)
{
	ObjectAddress myself,
				referenced;

	/* We know the default collation is pinned, so don't bother recording it */
	if (OidIsValid(collid) && collid != DEFAULT_COLLATION_OID)
	{
		myself.classId = RelationRelationId;
		myself.objectId = relid;
		myself.objectSubId = attnum;
		referenced.classId = CollationRelationId;
		referenced.objectId = collid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
}


/*
 * Subroutine for ATExecAlterColumnType: remember that an index needs
 * to be rebuilt (which we might already know).
 */
static void
RememberIndexForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: we
	 * mustn't try to recreate the same index twice, and if an index depends
	 * on more than one column whose type is to be altered, we must capture
	 * its definition string before applying any of the column type changes.
	 * ruleutils.c will get confused if we ask again later.
	 */
	if (!list_member_oid(tab->changedIndexOids, indoid))
	{
		/*
		 * Before adding it as an index-to-rebuild, we'd better see if it
		 * belongs to a constraint, and if so rebuild the constraint instead.
		 * Typically this check fails, because constraint indexes normally
		 * have only dependencies on their constraint.  But it's possible for
		 * such an index to also have direct dependencies on table columns,
		 * for example with a partial exclusion constraint.
		 */
		Oid			conoid = get_index_constraint(indoid);

		if (OidIsValid(conoid))
		{
			RememberConstraintForRebuilding(conoid, tab);
		}
		else
		{
			/* OK, capture the index's existing definition string */
			char	   *defstring = pg_get_indexdef_string(indoid);

			tab->changedIndexOids = lappend_oid(tab->changedIndexOids,
												indoid);
			tab->changedIndexDefs = lappend(tab->changedIndexDefs,
											defstring);

			/*
			 * Remember if this index is used for the table's replica identity
			 * or if it is a clustered index, so that ATPostAlterTypeCleanup()
			 * can queue up commands necessary to restore those properties.
			 */
			RememberReplicaIdentityForRebuilding(indoid, tab);
			RememberClusterOnForRebuilding(indoid, tab);
		}
	}
}


/*
 * Subroutine for ATExecAlterColumnType: remember that a replica identity
 * needs to be reset.
 */
static void
RememberReplicaIdentityForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	if (!get_index_isreplident(indoid))
		return;

	if (tab->replicaIdentityIndex)
		elog(ERROR, "relation %u has multiple indexes marked as replica identity", tab->relid);

	tab->replicaIdentityIndex = get_rel_name(indoid);
}


/*
 * Subroutine for ATExecAlterColumnType: remember any clustered index.
 */
static void
RememberClusterOnForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	if (!get_index_isclustered(indoid))
		return;

	if (tab->clusterOnIndex)
		elog(ERROR, "relation %u has multiple clustered indexes", tab->relid);

	tab->clusterOnIndex = get_rel_name(indoid);
}


/*
 * Subroutine for ATExecAlterColumnType: remember that a constraint needs
 * to be rebuilt (which we might already know).
 */
static void
RememberConstraintForRebuilding(Oid conoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: we
	 * mustn't try to recreate the same constraint twice, and if a constraint
	 * depends on more than one column whose type is to be altered, we must
	 * capture its definition string before applying any of the column type
	 * changes.  ruleutils.c will get confused if we ask again later.
	 */
	if (!list_member_oid(tab->changedConstraintOids, conoid))
	{
		/* OK, capture the constraint's existing definition string */
		char	   *defstring = pg_get_constraintdef_command(conoid);
		Oid			indoid;

		tab->changedConstraintOids = lappend_oid(tab->changedConstraintOids,
												 conoid);
		tab->changedConstraintDefs = lappend(tab->changedConstraintDefs,
											 defstring);

		/*
		 * For the index of a constraint, if any, remember if it is used for
		 * the table's replica identity or if it is a clustered index, so that
		 * ATPostAlterTypeCleanup() can queue up commands necessary to restore
		 * those properties.
		 */
		indoid = get_constraint_index(conoid);
		if (OidIsValid(indoid))
		{
			RememberReplicaIdentityForRebuilding(indoid, tab);
			RememberClusterOnForRebuilding(indoid, tab);
		}
	}
}


/*
 * Subroutine for ATExecAlterColumnType: remember that a statistics object
 * needs to be rebuilt (which we might already know).
 */
static void
RememberStatisticsForRebuilding(Oid stxoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: we
	 * mustn't try to recreate the same statistics object twice, and if the
	 * statistics object depends on more than one column whose type is to be
	 * altered, we must capture its definition string before applying any of
	 * the type changes. ruleutils.c will get confused if we ask again later.
	 */
	if (!list_member_oid(tab->changedStatisticsOids, stxoid))
	{
		/* OK, capture the statistics object's existing definition string */
		char	   *defstring = pg_get_statisticsobjdef_string(stxoid);

		tab->changedStatisticsOids = lappend_oid(tab->changedStatisticsOids,
												 stxoid);
		tab->changedStatisticsDefs = lappend(tab->changedStatisticsDefs,
											 defstring);
	}
}


/* 获取字段默认值依赖的序列Oid */
Oid
getColumnDefaultSeq(Relation rel, const char *colName)
{
    HeapTuple attTuple;
    Oid seqOid = InvalidOid;

    attTuple = SearchSysCacheCopyAttName(rel->rd_id, colName);
    if (!HeapTupleIsValid(attTuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    if (((Form_pg_attribute)GETSTRUCT(attTuple))->atthasdef)
    {
        Relation attrdefRel;
        Relation depRel;
        ScanKeyData keys[2];
        SysScanDesc scan;
        HeapTuple attrdefTuple;
        Oid columnDefaultOid;
        HeapTuple depTuple;

        attrdefRel = table_open(AttrDefaultRelationId, AccessShareLock);
        depRel = table_open(DependRelationId, AccessShareLock);

        ScanKeyInit(&keys[0],
                    Anum_pg_attrdef_adrelid,
                    BTEqualStrategyNumber,
                    F_OIDEQ,
                    ObjectIdGetDatum(RelationGetRelid(rel)));
        ScanKeyInit(&keys[1],
                    Anum_pg_attrdef_adnum,
                    BTEqualStrategyNumber,
                    F_INT2EQ,
                    Int16GetDatum(((Form_pg_attribute)GETSTRUCT(attTuple))->attnum));
        scan = systable_beginscan(attrdefRel, AttrDefaultIndexId, true,
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
                if (!OidIsValid(seqOid))
                    seqOid = foundDep->refobjid;
                else
                {
                    systable_endscan(scan);
                    elog(ERROR, "more than one sequence be dependent on column \"%s\" of relation \"%s\"",
                         colName, RelationGetRelationName(rel));
                }
            }
        }
        systable_endscan(scan);

        table_close(attrdefRel, AccessShareLock);
        table_close(depRel, AccessShareLock);
    }

    heap_freetuple(attTuple);

    return seqOid;
}

/* 获取表的自增字段序号 */
static AttrNumber
mysGetTableAutoIncColNum(Relation rel)
{
    int i;
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


static Oid
mysGetTableAutoIncSeqOid(Relation rel)
{
    int i;
    Oid AutoIncSeqOid = InvalidOid;
    TupleDesc columnsDesc = rel->rd_att;

    if (columnsDesc->constr == NULL || (columnsDesc->constr != NULL && columnsDesc->constr->num_defval <= 0))
        return AutoIncSeqOid;
    
    for (i = 0; i < columnsDesc->constr->num_defval; i++)
    {
        AttrDefault attrDef = columnsDesc->constr->defval[i];
        Form_pg_attribute attrForm = &(columnsDesc->attrs[attrDef.adnum - 1]);
        
        if (!OidIsValid(AutoIncSeqOid))
        {
            AutoIncSeqOid = getColumnDefaultSeq(rel, NameStr(attrForm->attname));
        }
        else
        {
            if (getColumnDefaultSeq(rel, NameStr(attrForm->attname)))
            {
                // 不应该发生
                elog(ERROR, "Incorrect table definition; there can be only one auto increment column");
            }
        }
    }

    return AutoIncSeqOid;
}


static List *
getTableOnUpdateNowColsNum(Relation rel)
{
    int i;
    List *OnUpdateNowCols = NIL;
    TupleDesc columnsDesc = rel->rd_att;
    
    for (i = 0; i < columnsDesc->natts; i++)
    {
        Oid onUpdateNowTrigOid;
        Form_pg_attribute attrForm = &(columnsDesc->attrs[i]);
        
        if (!OidIsValid(onUpdateNowTrigOid = mysGetColumnOnUpdateNowTrig(rel, NameStr(attrForm->attname))))
        {
            continue;
        }

        OnUpdateNowCols = lappend_int(OnUpdateNowCols, attrForm->attnum);
    }

    return OnUpdateNowCols;
}


/* 获取表中使用该字段作为键列的所有索引 */
static List *
getColumnIndexs(Relation rel, char *colName)
{
    AttrNumber colAttrNum;
    List *columnIndexs;
    List *tableAllIndexs;
    ListCell *lc;

    columnIndexs = NIL;
    tableAllIndexs = NIL;
    colAttrNum = get_attnum(rel->rd_id, colName);
    tableAllIndexs = RelationGetIndexList(rel);

    foreach(lc, tableAllIndexs)
    {
        Oid indexOid = lfirst_oid(lc);
        Relation indexRel = index_open(indexOid, AccessShareLock);
        int i = 0;

        for (i = 0; i < indexRel->rd_index->indnatts; i++)
        {
            if (colAttrNum == indexRel->rd_index->indkey.values[i])
            {
                columnIndexs = lappend_oid(columnIndexs, indexOid);
                break;
            }
        }

        index_close(indexRel, AccessShareLock);
    }

    return columnIndexs;
}


/*
 * NotNullImpliedByRelConstraints
 *		Does rel's existing constraints imply NOT NULL for the given attribute?
 */
static bool
NotNullImpliedByRelConstraints(Relation rel, Form_pg_attribute attr)
{
	NullTest   *nnulltest = makeNode(NullTest);

	nnulltest->arg = (Expr *) makeVar(1,
									  attr->attnum,
									  attr->atttypid,
									  attr->atttypmod,
									  attr->attcollation,
									  0);
	nnulltest->nulltesttype = IS_NOT_NULL;

	/*
	 * argisrow = false is correct even for a composite column, because
	 * attnotnull does not represent a SQL-spec IS NOT NULL test in such a
	 * case, just IS DISTINCT FROM NULL.
	 */
	nnulltest->argisrow = false;
	nnulltest->location = -1;

	if (ConstraintImpliedByRelConstraint(rel, list_make1(nnulltest), NIL))
	{
		ereport(DEBUG1,
				(errmsg_internal("existing constraints on column \"%s.%s\" are sufficient to prove that it does not contain nulls",
								 RelationGetRelationName(rel), NameStr(attr->attname))));
		return true;
	}

	return false;
}


/*
 * Helper function for ATExecSetStorage and ATExecSetCompression
 *
 * Set the attstorage and/or attcompression fields for index columns
 * associated with the specified table column.
 */
static void
SetIndexStorageProperties(Relation rel, Relation attrelation,
						  AttrNumber attnum,
						  bool setstorage, char newstorage,
						  bool setcompression, char newcompression,
						  LOCKMODE lockmode)
{
	ListCell   *lc;

	foreach(lc, RelationGetIndexList(rel))
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	indrel;
		AttrNumber	indattnum = 0;
		HeapTuple	tuple;

		indrel = index_open(indexoid, lockmode);

		for (int i = 0; i < indrel->rd_index->indnatts; i++)
		{
			if (indrel->rd_index->indkey.values[i] == attnum)
			{
				indattnum = i + 1;
				break;
			}
		}

		if (indattnum == 0)
		{
			index_close(indrel, lockmode);
			continue;
		}

		tuple = SearchSysCacheCopyAttNum(RelationGetRelid(indrel), indattnum);

		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

			if (setstorage)
				attrtuple->attstorage = newstorage;

			if (setcompression)
				attrtuple->attcompression = newcompression;

			CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

			InvokeObjectPostAlterHook(RelationRelationId,
									  RelationGetRelid(rel),
									  attrtuple->attnum);

			heap_freetuple(tuple);
		}

		index_close(indrel, lockmode);
	}
}


/*
 * Add a check constraint to a single table and its children.  Returns the
 * address of the constraint added to the parent relation, if one gets added,
 * or InvalidObjectAddress otherwise.
 *
 * Subroutine for ATExecAddConstraint.
 *
 * We must recurse to child tables during execution, rather than using
 * ALTER TABLE's normal prep-time recursion.  The reason is that all the
 * constraints *must* be given the same name, else they won't be seen as
 * related later.  If the user didn't explicitly specify a name, then
 * AddRelationNewConstraints would normally assign different names to the
 * child constraints.  To fix that, we must capture the name assigned at
 * the parent table and pass that down.
 */
static ObjectAddress
ATAddCheckConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					 Constraint *constr, bool recurse, bool recursing,
					 bool is_readd, LOCKMODE lockmode)
{
	List	   *newcons;
	ListCell   *lcon;
	List	   *children;
	ListCell   *child;
	ObjectAddress address = InvalidObjectAddress;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	if (recursing)
		ATSimplePermissions(rel, ATT_ORDINARY_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/*
	 * Call AddRelationNewConstraints to do the work, making sure it works on
	 * a copy of the Constraint so transformExpr can't modify the original. It
	 * returns a list of cooked constraints.
	 *
	 * If the constraint ends up getting merged with a pre-existing one, it's
	 * omitted from the returned list, which is what we want: we do not need
	 * to do any validation work.  That can only happen at child tables,
	 * though, since we disallow merging at the top level.
	 */
	newcons = AddRelationNewConstraints(rel, NIL,
										list_make1(copyObject(constr)),
										recursing | is_readd,	/* allow_merge */
										!recursing, /* is_local */
										is_readd,	/* is_internal */
										NULL);	/* queryString not available
												 * here */

	/* we don't expect more than one constraint here */
	Assert(list_length(newcons) <= 1);

	/* Add each to-be-validated constraint to Phase 3's queue */
	foreach(lcon, newcons)
	{
		CookedConstraint *ccon = (CookedConstraint *) lfirst(lcon);

		if (!ccon->skip_validation)
		{
			NewConstraint *newcon;

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = ccon->name;
			newcon->contype = ccon->contype;
			newcon->qual = ccon->expr;

			tab->constraints = lappend(tab->constraints, newcon);
		}

		/* Save the actually assigned name if it was defaulted */
		if (constr->conname == NULL)
			constr->conname = ccon->name;

		ObjectAddressSet(address, ConstraintRelationId, ccon->conoid);
	}

	/* At this point we must have a locked-down name to use */
	Assert(constr->conname != NULL);

	/* Advance command counter in case same table is visited multiple times */
	CommandCounterIncrement();

	/*
	 * If the constraint got merged with an existing constraint, we're done.
	 * We mustn't recurse to child tables in this case, because they've
	 * already got the constraint, and visiting them again would lead to an
	 * incorrect value for coninhcount.
	 */
	if (newcons == NIL)
		return address;

	/*
	 * If adding a NO INHERIT constraint, no need to find our children.
	 */
	if (constr->is_no_inherit)
		return address;

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * Check if ONLY was specified with ALTER TABLE.  If so, allow the
	 * constraint creation only if there are no children currently.  Error out
	 * otherwise.
	 */
	if (!recurse && children != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("constraint must be added to child tables too")));

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/* find_inheritance_children already got lock */
		childrel = table_open(childrelid, NoLock);
		CheckTableNotInUse(childrel, "ALTER TABLE");

		/* Find or create work queue entry for this table */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* Recurse to child */
		ATAddCheckConstraint(wqueue, childtab, childrel,
							 constr, recurse, true, is_readd, lockmode);

		table_close(childrel, NoLock);
	}

	return address;
}


/*
 * Add a foreign-key constraint to a single table; return the new constraint's
 * address.
 *
 * Subroutine for ATExecAddConstraint.  Must already hold exclusive
 * lock on the rel, and have done appropriate validity checks for it.
 * We do permissions checks here, however.
 *
 * When the referenced or referencing tables (or both) are partitioned,
 * multiple pg_constraint rows are required -- one for each partitioned table
 * and each partition on each side (fortunately, not one for every combination
 * thereof).  We also need action triggers on each leaf partition on the
 * referenced side, and check triggers on each leaf partition on the
 * referencing side.
 */
static ObjectAddress
ATAddForeignKeyConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
						  Constraint *fkconstraint,
						  bool recurse, bool recursing, LOCKMODE lockmode)
{
	Relation	pkrel;
	int16		pkattnum[INDEX_MAX_KEYS];
	int16		fkattnum[INDEX_MAX_KEYS];
	Oid			pktypoid[INDEX_MAX_KEYS];
	Oid			fktypoid[INDEX_MAX_KEYS];
	Oid			opclasses[INDEX_MAX_KEYS];
	Oid			pfeqoperators[INDEX_MAX_KEYS];
	Oid			ppeqoperators[INDEX_MAX_KEYS];
	Oid			ffeqoperators[INDEX_MAX_KEYS];
	int			i;
	int			numfks,
				numpks;
	Oid			indexOid;
	bool		old_check_ok;
	ObjectAddress address;
	ListCell   *old_pfeqop_item = list_head(fkconstraint->old_conpfeqop);

	/*
	 * Grab ShareRowExclusiveLock on the pk table, so that someone doesn't
	 * delete rows out from under us.
	 */
	if (OidIsValid(fkconstraint->old_pktable_oid))
		pkrel = table_open(fkconstraint->old_pktable_oid, ShareRowExclusiveLock);
	else
		pkrel = table_openrv(fkconstraint->pktable, ShareRowExclusiveLock);

	/*
	 * Validity checks (permission checks wait till we have the column
	 * numbers)
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		if (!recurse)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot use ONLY for foreign key on partitioned table \"%s\" referencing relation \"%s\"",
							RelationGetRelationName(rel),
							RelationGetRelationName(pkrel))));
		if (fkconstraint->skip_validation && !fkconstraint->initially_valid)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot add NOT VALID foreign key on partitioned table \"%s\" referencing relation \"%s\"",
							RelationGetRelationName(rel),
							RelationGetRelationName(pkrel)),
					 errdetail("This feature is not yet supported on partitioned tables.")));
	}

	if (pkrel->rd_rel->relkind != RELKIND_RELATION &&
		pkrel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	if (!allowSystemTableMods && IsSystemRelation(pkrel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(pkrel))));

	/*
	 * References from permanent or unlogged tables to temp tables, and from
	 * permanent tables to unlogged tables, are disallowed because the
	 * referenced data can vanish out from under us.  References from temp
	 * tables to any other table type are also disallowed, because other
	 * backends might need to run the RI triggers on the perm table, but they
	 * can't reliably see tuples in the local buffers of other backends.
	 */
	switch (rel->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_PERMANENT:
			if (!RelationIsPermanent(pkrel))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on permanent tables may reference only permanent tables")));
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (!RelationIsPermanent(pkrel)
				&& pkrel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on unlogged tables may reference only permanent or unlogged tables")));
			break;
		case RELPERSISTENCE_TEMP:
			if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on temporary tables may reference only temporary tables")));
			if (!pkrel->rd_islocaltemp || !rel->rd_islocaltemp)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on temporary tables must involve temporary tables of this session")));
			break;
	}

	/*
	 * Look up the referencing attributes to make sure they exist, and record
	 * their attnums and type OIDs.
	 */
	MemSet(pkattnum, 0, sizeof(pkattnum));
	MemSet(fkattnum, 0, sizeof(fkattnum));
	MemSet(pktypoid, 0, sizeof(pktypoid));
	MemSet(fktypoid, 0, sizeof(fktypoid));
	MemSet(opclasses, 0, sizeof(opclasses));
	MemSet(pfeqoperators, 0, sizeof(pfeqoperators));
	MemSet(ppeqoperators, 0, sizeof(ppeqoperators));
	MemSet(ffeqoperators, 0, sizeof(ffeqoperators));

	numfks = transformColumnNameList(RelationGetRelid(rel),
									 fkconstraint->fk_attrs,
									 fkattnum, fktypoid);

	/*
	 * If the attribute list for the referenced table was omitted, lookup the
	 * definition of the primary key and use it.  Otherwise, validate the
	 * supplied attribute list.  In either case, discover the index OID and
	 * index opclasses, and the attnums and type OIDs of the attributes.
	 */
	if (fkconstraint->pk_attrs == NIL)
	{
		numpks = transformFkeyGetPrimaryKey(pkrel, &indexOid,
											&fkconstraint->pk_attrs,
											pkattnum, pktypoid,
											opclasses);
	}
	else
	{
		numpks = transformColumnNameList(RelationGetRelid(pkrel),
										 fkconstraint->pk_attrs,
										 pkattnum, pktypoid);
		/* Look for an index matching the column list */
		indexOid = transformFkeyCheckAttrs(pkrel, numpks, pkattnum,
										   opclasses);
	}

	/*
	 * Now we can check permissions.
	 */
	checkFkeyPermissions(pkrel, pkattnum, numpks);

	/*
	 * Check some things for generated columns.
	 */
	for (i = 0; i < numfks; i++)
	{
		char		attgenerated = TupleDescAttr(RelationGetDescr(rel), fkattnum[i] - 1)->attgenerated;

		if (attgenerated)
		{
			/*
			 * Check restrictions on UPDATE/DELETE actions, per SQL standard
			 */
			if (fkconstraint->fk_upd_action == FKCONSTR_ACTION_SETNULL ||
				fkconstraint->fk_upd_action == FKCONSTR_ACTION_SETDEFAULT ||
				fkconstraint->fk_upd_action == FKCONSTR_ACTION_CASCADE)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid %s action for foreign key constraint containing generated column",
								"ON UPDATE")));
			if (fkconstraint->fk_del_action == FKCONSTR_ACTION_SETNULL ||
				fkconstraint->fk_del_action == FKCONSTR_ACTION_SETDEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid %s action for foreign key constraint containing generated column",
								"ON DELETE")));
		}
	}

	/*
	 * Look up the equality operators to use in the constraint.
	 *
	 * Note that we have to be careful about the difference between the actual
	 * PK column type and the opclass' declared input type, which might be
	 * only binary-compatible with it.  The declared opcintype is the right
	 * thing to probe pg_amop with.
	 */
	if (numfks != numpks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree")));

	/*
	 * On the strength of a previous constraint, we might avoid scanning
	 * tables to validate this one.  See below.
	 */
	old_check_ok = (fkconstraint->old_conpfeqop != NIL);
	Assert(!old_check_ok || numfks == list_length(fkconstraint->old_conpfeqop));

	for (i = 0; i < numpks; i++)
	{
		Oid			pktype = pktypoid[i];
		Oid			fktype = fktypoid[i];
		Oid			fktyped;
		HeapTuple	cla_ht;
		Form_pg_opclass cla_tup;
		Oid			amid;
		Oid			opfamily;
		Oid			opcintype;
		Oid			pfeqop;
		Oid			ppeqop;
		Oid			ffeqop;
		int16		eqstrategy;
		Oid			pfeqop_right;

		/* We need several fields out of the pg_opclass entry */
		cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclasses[i]));
		if (!HeapTupleIsValid(cla_ht))
			elog(ERROR, "cache lookup failed for opclass %u", opclasses[i]);
		cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
		amid = cla_tup->opcmethod;
		opfamily = cla_tup->opcfamily;
		opcintype = cla_tup->opcintype;
		ReleaseSysCache(cla_ht);

		/*
		 * Check it's a btree; currently this can never fail since no other
		 * index AMs support unique indexes.  If we ever did have other types
		 * of unique indexes, we'd need a way to determine which operator
		 * strategy number is equality.  (Is it reasonable to insist that
		 * every such index AM use btree's number for equality?)
		 */
		if (amid != BTREE_AM_OID)
			elog(ERROR, "only b-tree indexes are supported for foreign keys");
		eqstrategy = BTEqualStrategyNumber;

		/*
		 * There had better be a primary equality operator for the index.
		 * We'll use it for PK = PK comparisons.
		 */
		ppeqop = get_opfamily_member(opfamily, opcintype, opcintype,
									 eqstrategy);

		if (!OidIsValid(ppeqop))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 eqstrategy, opcintype, opcintype, opfamily);

		/*
		 * Are there equality operators that take exactly the FK type? Assume
		 * we should look through any domain here.
		 */
		fktyped = getBaseType(fktype);

		pfeqop = get_opfamily_member(opfamily, opcintype, fktyped,
									 eqstrategy);
		if (OidIsValid(pfeqop))
		{
			pfeqop_right = fktyped;
			ffeqop = get_opfamily_member(opfamily, fktyped, fktyped,
										 eqstrategy);
		}
		else
		{
			/* keep compiler quiet */
			pfeqop_right = InvalidOid;
			ffeqop = InvalidOid;
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
		{
			/*
			 * Otherwise, look for an implicit cast from the FK type to the
			 * opcintype, and if found, use the primary equality operator.
			 * This is a bit tricky because opcintype might be a polymorphic
			 * type such as ANYARRAY or ANYENUM; so what we have to test is
			 * whether the two actual column types can be concurrently cast to
			 * that type.  (Otherwise, we'd fail to reject combinations such
			 * as int[] and point[].)
			 */
			Oid			input_typeids[2];
			Oid			target_typeids[2];

			input_typeids[0] = pktype;
			input_typeids[1] = fktype;
			target_typeids[0] = opcintype;
			target_typeids[1] = opcintype;
			if (can_coerce_type(2, input_typeids, target_typeids,
								COERCION_IMPLICIT))
			{
				pfeqop = ffeqop = ppeqop;
				pfeqop_right = opcintype;
			}
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("foreign key constraint \"%s\" cannot be implemented",
							fkconstraint->conname),
					 errdetail("Key columns \"%s\" and \"%s\" "
							   "are of incompatible types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktype),
							   format_type_be(pktype))));

		if (old_check_ok)
		{
			/*
			 * When a pfeqop changes, revalidate the constraint.  We could
			 * permit intra-opfamily changes, but that adds subtle complexity
			 * without any concrete benefit for core types.  We need not
			 * assess ppeqop or ffeqop, which RI_Initial_Check() does not use.
			 */
			old_check_ok = (pfeqop == lfirst_oid(old_pfeqop_item));
			old_pfeqop_item = lnext(fkconstraint->old_conpfeqop,
									old_pfeqop_item);
		}
		if (old_check_ok)
		{
			Oid			old_fktype;
			Oid			new_fktype;
			CoercionPathType old_pathtype;
			CoercionPathType new_pathtype;
			Oid			old_castfunc;
			Oid			new_castfunc;
			Form_pg_attribute attr = TupleDescAttr(tab->oldDesc,
												   fkattnum[i] - 1);

			/*
			 * Identify coercion pathways from each of the old and new FK-side
			 * column types to the right (foreign) operand type of the pfeqop.
			 * We may assume that pg_constraint.conkey is not changing.
			 */
			old_fktype = attr->atttypid;
			new_fktype = fktype;
			old_pathtype = findFkeyCast(pfeqop_right, old_fktype,
										&old_castfunc);
			new_pathtype = findFkeyCast(pfeqop_right, new_fktype,
										&new_castfunc);

			/*
			 * Upon a change to the cast from the FK column to its pfeqop
			 * operand, revalidate the constraint.  For this evaluation, a
			 * binary coercion cast is equivalent to no cast at all.  While
			 * type implementors should design implicit casts with an eye
			 * toward consistency of operations like equality, we cannot
			 * assume here that they have done so.
			 *
			 * A function with a polymorphic argument could change behavior
			 * arbitrarily in response to get_fn_expr_argtype().  Therefore,
			 * when the cast destination is polymorphic, we only avoid
			 * revalidation if the input type has not changed at all.  Given
			 * just the core data types and operator classes, this requirement
			 * prevents no would-be optimizations.
			 *
			 * If the cast converts from a base type to a domain thereon, then
			 * that domain type must be the opcintype of the unique index.
			 * Necessarily, the primary key column must then be of the domain
			 * type.  Since the constraint was previously valid, all values on
			 * the foreign side necessarily exist on the primary side and in
			 * turn conform to the domain.  Consequently, we need not treat
			 * domains specially here.
			 *
			 * Since we require that all collations share the same notion of
			 * equality (which they do, because texteq reduces to bitwise
			 * equality), we don't compare collation here.
			 *
			 * We need not directly consider the PK type.  It's necessarily
			 * binary coercible to the opcintype of the unique index column,
			 * and ri_triggers.c will only deal with PK datums in terms of
			 * that opcintype.  Changing the opcintype also changes pfeqop.
			 */
			old_check_ok = (new_pathtype == old_pathtype &&
							new_castfunc == old_castfunc &&
							(!IsPolymorphicType(pfeqop_right) ||
							 new_fktype == old_fktype));
		}

		pfeqoperators[i] = pfeqop;
		ppeqoperators[i] = ppeqop;
		ffeqoperators[i] = ffeqop;
	}

	/*
	 * Create all the constraint and trigger objects, recursing to partitions
	 * as necessary.  First handle the referenced side.
	 */
	address = addFkRecurseReferenced(wqueue, fkconstraint, rel, pkrel,
									 indexOid,
									 InvalidOid,	/* no parent constraint */
									 numfks,
									 pkattnum,
									 fkattnum,
									 pfeqoperators,
									 ppeqoperators,
									 ffeqoperators,
									 old_check_ok);

	/* Now handle the referencing side. */
	addFkRecurseReferencing(wqueue, fkconstraint, rel, pkrel,
							indexOid,
							address.objectId,
							numfks,
							pkattnum,
							fkattnum,
							pfeqoperators,
							ppeqoperators,
							ffeqoperators,
							old_check_ok,
							lockmode);

	/*
	 * Done.  Close pk table, but keep lock until we've committed.
	 */
	table_close(pkrel, NoLock);

	return address;
}


/*
 * Check columns in child table match up with columns in parent, and increment
 * their attinhcount.
 *
 * Called by CreateInheritance
 *
 * Currently all parent columns must be found in child. Missing columns are an
 * error.  One day we might consider creating new columns like CREATE TABLE
 * does.  However, that is widely unpopular --- in the common use case of
 * partitioned tables it's a foot-gun.
 *
 * The data type must match exactly. If the parent column is NOT NULL then
 * the child must be as well. Defaults are not compared, however.
 */
static void
MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	attrrel;
	AttrNumber	parent_attno;
	int			parent_natts;
	TupleDesc	tupleDesc;
	HeapTuple	tuple;
	bool		child_is_partition = false;

	attrrel = table_open(AttributeRelationId, RowExclusiveLock);

	tupleDesc = RelationGetDescr(parent_rel);
	parent_natts = tupleDesc->natts;

	/* If parent_rel is a partitioned table, child_rel must be a partition */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		child_is_partition = true;

	for (parent_attno = 1; parent_attno <= parent_natts; parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
													parent_attno - 1);
		char	   *attributeName = NameStr(attribute->attname);

		/* Ignore dropped columns in the parent. */
		if (attribute->attisdropped)
			continue;

		/* Find same column in child (matching on column name). */
		tuple = SearchSysCacheCopyAttName(RelationGetRelid(child_rel),
										  attributeName);
		if (HeapTupleIsValid(tuple))
		{
			/* Check they are same type, typmod, and collation */
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (attribute->atttypid != childatt->atttypid ||
				attribute->atttypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(child_rel),
								attributeName)));

			if (attribute->attcollation != childatt->attcollation)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("child table \"%s\" has different collation for column \"%s\"",
								RelationGetRelationName(child_rel),
								attributeName)));

			/*
			 * Check child doesn't discard NOT NULL property.  (Other
			 * constraints are checked elsewhere.)
			 */
			if (attribute->attnotnull && !childatt->attnotnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" in child table must be marked NOT NULL",
								attributeName)));

			/*
			 * If parent column is generated, child column must be, too.
			 */
			if (attribute->attgenerated && !childatt->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" in child table must be a generated column",
								attributeName)));

			/*
			 * Check that both generation expressions match.
			 *
			 * The test we apply is to see whether they reverse-compile to the
			 * same source string.  This insulates us from issues like whether
			 * attributes have the same physical column numbers in parent and
			 * child relations.  (See also constraints_equivalent().)
			 */
			if (attribute->attgenerated && childatt->attgenerated)
			{
				TupleConstr *child_constr = child_rel->rd_att->constr;
				TupleConstr *parent_constr = parent_rel->rd_att->constr;
				char	   *child_expr = NULL;
				char	   *parent_expr = NULL;

				Assert(child_constr != NULL);
				Assert(parent_constr != NULL);

				for (int i = 0; i < child_constr->num_defval; i++)
				{
					if (child_constr->defval[i].adnum == childatt->attnum)
					{
						child_expr =
							TextDatumGetCString(DirectFunctionCall2(pg_get_expr,
																	CStringGetTextDatum(child_constr->defval[i].adbin),
																	ObjectIdGetDatum(child_rel->rd_id)));
						break;
					}
				}
				Assert(child_expr != NULL);

				for (int i = 0; i < parent_constr->num_defval; i++)
				{
					if (parent_constr->defval[i].adnum == attribute->attnum)
					{
						parent_expr =
							TextDatumGetCString(DirectFunctionCall2(pg_get_expr,
																	CStringGetTextDatum(parent_constr->defval[i].adbin),
																	ObjectIdGetDatum(parent_rel->rd_id)));
						break;
					}
				}
				Assert(parent_expr != NULL);

				if (strcmp(child_expr, parent_expr) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("column \"%s\" in child table has a conflicting generation expression",
									attributeName)));
			}

			/*
			 * OK, bump the child column's inheritance count.  (If we fail
			 * later on, this change will just roll back.)
			 */
			childatt->attinhcount++;

			/*
			 * In case of partitions, we must enforce that value of attislocal
			 * is same in all partitions. (Note: there are only inherited
			 * attributes in partitions)
			 */
			if (child_is_partition)
			{
				Assert(childatt->attinhcount == 1);
				childatt->attislocal = false;
			}

			CatalogTupleUpdate(attrrel, &tuple->t_self, tuple);
			heap_freetuple(tuple);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing column \"%s\"",
							attributeName)));
		}
	}

	table_close(attrrel, RowExclusiveLock);
}

/*
 * Check constraints in child table match up with constraints in parent,
 * and increment their coninhcount.
 *
 * Constraints that are marked ONLY in the parent are ignored.
 *
 * Called by CreateInheritance
 *
 * Currently all constraints in parent must be present in the child. One day we
 * may consider adding new constraints like CREATE TABLE does.
 *
 * XXX This is O(N^2) which may be an issue with tables with hundreds of
 * constraints. As long as tables have more like 10 constraints it shouldn't be
 * a problem though. Even 100 constraints ought not be the end of the world.
 *
 * XXX See MergeWithExistingConstraint too if you change this code.
 */
static void
MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	catalog_relation;
	TupleDesc	tuple_desc;
	SysScanDesc parent_scan;
	ScanKeyData parent_key;
	HeapTuple	parent_tuple;
	bool		child_is_partition = false;

	catalog_relation = table_open(ConstraintRelationId, RowExclusiveLock);
	tuple_desc = RelationGetDescr(catalog_relation);

	/* If parent_rel is a partitioned table, child_rel must be a partition */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		child_is_partition = true;

	/* Outer loop scans through the parent's constraint definitions */
	ScanKeyInit(&parent_key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	parent_scan = systable_beginscan(catalog_relation, ConstraintRelidTypidNameIndexId,
									 true, NULL, 1, &parent_key);

	while (HeapTupleIsValid(parent_tuple = systable_getnext(parent_scan)))
	{
		Form_pg_constraint parent_con = (Form_pg_constraint) GETSTRUCT(parent_tuple);
		SysScanDesc child_scan;
		ScanKeyData child_key;
		HeapTuple	child_tuple;
		bool		found = false;

		if (parent_con->contype != CONSTRAINT_CHECK)
			continue;

		/* if the parent's constraint is marked NO INHERIT, it's not inherited */
		if (parent_con->connoinherit)
			continue;

		/* Search for a child constraint matching this one */
		ScanKeyInit(&child_key,
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(child_rel)));
		child_scan = systable_beginscan(catalog_relation, ConstraintRelidTypidNameIndexId,
										true, NULL, 1, &child_key);

		while (HeapTupleIsValid(child_tuple = systable_getnext(child_scan)))
		{
			Form_pg_constraint child_con = (Form_pg_constraint) GETSTRUCT(child_tuple);
			HeapTuple	child_copy;

			if (child_con->contype != CONSTRAINT_CHECK)
				continue;

			if (strcmp(NameStr(parent_con->conname),
					   NameStr(child_con->conname)) != 0)
				continue;

			if (!constraints_equivalent(parent_tuple, child_tuple, tuple_desc))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different definition for check constraint \"%s\"",
								RelationGetRelationName(child_rel),
								NameStr(parent_con->conname))));

			/* If the child constraint is "no inherit" then cannot merge */
			if (child_con->connoinherit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with non-inherited constraint on child table \"%s\"",
								NameStr(child_con->conname),
								RelationGetRelationName(child_rel))));

			/*
			 * If the child constraint is "not valid" then cannot merge with a
			 * valid parent constraint
			 */
			if (parent_con->convalidated && !child_con->convalidated)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with NOT VALID constraint on child table \"%s\"",
								NameStr(child_con->conname),
								RelationGetRelationName(child_rel))));

			/*
			 * OK, bump the child constraint's inheritance count.  (If we fail
			 * later on, this change will just roll back.)
			 */
			child_copy = heap_copytuple(child_tuple);
			child_con = (Form_pg_constraint) GETSTRUCT(child_copy);
			child_con->coninhcount++;

			/*
			 * In case of partitions, an inherited constraint must be
			 * inherited only once since it cannot have multiple parents and
			 * it is never considered local.
			 */
			if (child_is_partition)
			{
				Assert(child_con->coninhcount == 1);
				child_con->conislocal = false;
			}

			CatalogTupleUpdate(catalog_relation, &child_copy->t_self, child_copy);
			heap_freetuple(child_copy);

			found = true;
			break;
		}

		systable_endscan(child_scan);

		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing constraint \"%s\"",
							NameStr(parent_con->conname))));
	}

	systable_endscan(parent_scan);
	table_close(catalog_relation, RowExclusiveLock);
}


/*
 * Make catalog entries showing relationId as being an inheritance child
 * of parentOid.  inhRelation is the already-opened pg_inherits catalog.
 */
static void
StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int32 seqNumber, Relation inhRelation,
						 bool child_is_partition)
{
	ObjectAddress childobject,
				parentobject;

	/* store the pg_inherits row */
	StoreSingleInheritance(relationId, parentOid, seqNumber);

	/*
	 * Store a dependency too
	 */
	parentobject.classId = RelationRelationId;
	parentobject.objectId = parentOid;
	parentobject.objectSubId = 0;
	childobject.classId = RelationRelationId;
	childobject.objectId = relationId;
	childobject.objectSubId = 0;

	recordDependencyOn(&childobject, &parentobject,
					   child_dependency_type(child_is_partition));

	/*
	 * Post creation hook of this inheritance. Since object_access_hook
	 * doesn't take multiple object identifiers, we relay oid of parent
	 * relation using auxiliary_id argument.
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 relationId, 0,
								 parentOid, false);

	/*
	 * Mark the parent as having subclasses.
	 */
	SetRelationHasSubclass(parentOid, true);
}


/*
 * RemoveInheritance
 *
 * Drop a parent from the child's parents. This just adjusts the attinhcount
 * and attislocal of the columns and removes the pg_inherit and pg_depend
 * entries.  expect_detached is passed down to DeleteInheritsTuple, q.v..
 *
 * If attinhcount goes to 0 then attislocal gets set to true. If it goes back
 * up attislocal stays true, which means if a child is ever removed from a
 * parent then its columns will never be automatically dropped which may
 * surprise. But at least we'll never surprise by dropping columns someone
 * isn't expecting to be dropped which would actually mean data loss.
 *
 * coninhcount and conislocal for inherited constraints are adjusted in
 * exactly the same way.
 *
 * Common to ATExecDropInherit() and ATExecDetachPartition().
 */
static void
RemoveInheritance(Relation child_rel, Relation parent_rel, bool expect_detached)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	attributeTuple,
				constraintTuple;
	List	   *connames;
	bool		found;
	bool		child_is_partition = false;

	/* If parent_rel is a partitioned table, child_rel must be a partition */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		child_is_partition = true;

	found = DeleteInheritsTuple(RelationGetRelid(child_rel),
								RelationGetRelid(parent_rel),
								expect_detached,
								RelationGetRelationName(child_rel));
	if (!found)
	{
		if (child_is_partition)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" is not a partition of relation \"%s\"",
							RelationGetRelationName(child_rel),
							RelationGetRelationName(parent_rel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" is not a parent of relation \"%s\"",
							RelationGetRelationName(parent_rel),
							RelationGetRelationName(child_rel))));
	}

	/*
	 * Search through child columns looking for ones matching parent rel
	 */
	catalogRelation = table_open(AttributeRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, AttributeRelidNumIndexId,
							  true, NULL, 1, key);
	while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);

		/* Ignore if dropped or not inherited */
		if (att->attisdropped)
			continue;
		if (att->attinhcount <= 0)
			continue;

		if (SearchSysCacheExistsAttName(RelationGetRelid(parent_rel),
										NameStr(att->attname)))
		{
			/* Decrement inhcount and possibly set islocal to true */
			HeapTuple	copyTuple = heap_copytuple(attributeTuple);
			Form_pg_attribute copy_att = (Form_pg_attribute) GETSTRUCT(copyTuple);

			copy_att->attinhcount--;
			if (copy_att->attinhcount == 0)
				copy_att->attislocal = true;

			CatalogTupleUpdate(catalogRelation, &copyTuple->t_self, copyTuple);
			heap_freetuple(copyTuple);
		}
	}
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	/*
	 * Likewise, find inherited check constraints and disinherit them. To do
	 * this, we first need a list of the names of the parent's check
	 * constraints.  (We cheat a bit by only checking for name matches,
	 * assuming that the expressions will match.)
	 */
	catalogRelation = table_open(ConstraintRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, key);

	connames = NIL;

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);

		if (con->contype == CONSTRAINT_CHECK)
			connames = lappend(connames, pstrdup(NameStr(con->conname)));
	}

	systable_endscan(scan);

	/* Now scan the child's constraints */
	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);
		bool		match;
		ListCell   *lc;

		if (con->contype != CONSTRAINT_CHECK)
			continue;

		match = false;
		foreach(lc, connames)
		{
			if (strcmp(NameStr(con->conname), (char *) lfirst(lc)) == 0)
			{
				match = true;
				break;
			}
		}

		if (match)
		{
			/* Decrement inhcount and possibly set islocal to true */
			HeapTuple	copyTuple = heap_copytuple(constraintTuple);
			Form_pg_constraint copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);

			if (copy_con->coninhcount <= 0) /* shouldn't happen */
				elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
					 RelationGetRelid(child_rel), NameStr(copy_con->conname));

			copy_con->coninhcount--;
			if (copy_con->coninhcount == 0)
				copy_con->conislocal = true;

			CatalogTupleUpdate(catalogRelation, &copyTuple->t_self, copyTuple);
			heap_freetuple(copyTuple);
		}
	}

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	drop_parent_dependency(RelationGetRelid(child_rel),
						   RelationRelationId,
						   RelationGetRelid(parent_rel),
						   child_dependency_type(child_is_partition));

	/*
	 * Post alter hook of this inherits. Since object_access_hook doesn't take
	 * multiple object identifiers, we relay oid of parent relation using
	 * auxiliary_id argument.
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 RelationGetRelid(child_rel), 0,
								 RelationGetRelid(parent_rel), false);
}


/*
 * CloneForeignKeyConstraints
 *		Clone foreign keys from a partitioned table to a newly acquired
 *		partition.
 *
 * partitionRel is a partition of parentRel, so we can be certain that it has
 * the same columns with the same datatypes.  The columns may be in different
 * order, though.
 *
 * wqueue must be passed to set up phase 3 constraint checking, unless the
 * referencing-side partition is known to be empty (such as in CREATE TABLE /
 * PARTITION OF).
 */
static void
CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
						   Relation partitionRel)
{
	/* This only works for declarative partitioning */
	Assert(parentRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * Clone constraints for which the parent is on the referenced side.
	 */
	CloneFkReferenced(parentRel, partitionRel);

	/*
	 * Now clone constraints where the parent is on the referencing side.
	 */
	CloneFkReferencing(wqueue, parentRel, partitionRel);
}


/*
 * CloneFkReferenced
 *		Subroutine for CloneForeignKeyConstraints
 *
 * Find all the FKs that have the parent relation on the referenced side;
 * clone those constraints to the given partition.  This is to be called
 * when the partition is being created or attached.
 *
 * This ignores self-referencing FKs; those are handled by CloneFkReferencing.
 *
 * This recurses to partitions, if the relation being attached is partitioned.
 * Recursion is done by calling addFkRecurseReferenced.
 */
static void
CloneFkReferenced(Relation parentRel, Relation partitionRel)
{
	Relation	pg_constraint;
	AttrMap    *attmap;
	ListCell   *cell;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tuple;
	List	   *clone = NIL;

	/*
	 * Search for any constraints where this partition's parent is in the
	 * referenced side.  However, we must not clone any constraint whose
	 * parent constraint is also going to be cloned, to avoid duplicates.  So
	 * do it in two steps: first construct the list of constraints to clone,
	 * then go over that list cloning those whose parents are not in the list.
	 * (We must not rely on the parent being seen first, since the catalog
	 * scan could return children first.)
	 */
	pg_constraint = table_open(ConstraintRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parentRel)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));
	/* This is a seqscan, as we don't have a usable index ... */
	scan = systable_beginscan(pg_constraint, InvalidOid, true,
							  NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		clone = lappend_oid(clone, constrForm->oid);
	}
	systable_endscan(scan);
	table_close(pg_constraint, RowShareLock);

	attmap = build_attrmap_by_name(RelationGetDescr(partitionRel),
								   RelationGetDescr(parentRel));
	foreach(cell, clone)
	{
		Oid			constrOid = lfirst_oid(cell);
		Form_pg_constraint constrForm;
		Relation	fkRel;
		Oid			indexOid;
		Oid			partIndexId;
		int			numfks;
		AttrNumber	conkey[INDEX_MAX_KEYS];
		AttrNumber	mapped_confkey[INDEX_MAX_KEYS];
		AttrNumber	confkey[INDEX_MAX_KEYS];
		Oid			conpfeqop[INDEX_MAX_KEYS];
		Oid			conppeqop[INDEX_MAX_KEYS];
		Oid			conffeqop[INDEX_MAX_KEYS];
		Constraint *fkconstraint;

		tuple = SearchSysCache1(CONSTROID, constrOid);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * As explained above: don't try to clone a constraint for which we're
		 * going to clone the parent.
		 */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/*
		 * Don't clone self-referencing foreign keys, which can be in the
		 * partitioned table or in the partition-to-be.
		 */
		if (constrForm->conrelid == RelationGetRelid(parentRel) ||
			constrForm->conrelid == RelationGetRelid(partitionRel))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/*
		 * Because we're only expanding the key space at the referenced side,
		 * we don't need to prevent any operation in the referencing table, so
		 * AccessShareLock suffices (assumes that dropping the constraint
		 * acquires AEL).
		 */
		fkRel = table_open(constrForm->conrelid, AccessShareLock);

		indexOid = constrForm->conindid;
		DeconstructFkConstraintRow(tuple,
								   &numfks,
								   conkey,
								   confkey,
								   conpfeqop,
								   conppeqop,
								   conffeqop);

		for (int i = 0; i < numfks; i++)
			mapped_confkey[i] = attmap->attnums[confkey[i] - 1];

		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		fkconstraint->conname = NameStr(constrForm->conname);
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs determined below */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = true;

		/* set up colnames that are used to generate the constraint name */
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(fkRel),
								conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}

		/*
		 * Add the new foreign key constraint pointing to the new partition.
		 * Because this new partition appears in the referenced side of the
		 * constraint, we don't need to set up for Phase 3 check.
		 */
		partIndexId = index_get_partition(partitionRel, indexOid);
		if (!OidIsValid(partIndexId))
			elog(ERROR, "index for %u not found in partition %s",
				 indexOid, RelationGetRelationName(partitionRel));
		addFkRecurseReferenced(NULL,
							   fkconstraint,
							   fkRel,
							   partitionRel,
							   partIndexId,
							   constrOid,
							   numfks,
							   mapped_confkey,
							   conkey,
							   conpfeqop,
							   conppeqop,
							   conffeqop,
							   true);

		table_close(fkRel, NoLock);
		ReleaseSysCache(tuple);
	}
}

/*
 * CloneFkReferencing
 *		Subroutine for CloneForeignKeyConstraints
 *
 * For each FK constraint of the parent relation in the given list, find an
 * equivalent constraint in its partition relation that can be reparented;
 * if one cannot be found, create a new constraint in the partition as its
 * child.
 *
 * If wqueue is given, it is used to set up phase-3 verification for each
 * cloned constraint; if omitted, we assume that such verification is not
 * needed (example: the partition is being created anew).
 */
static void
CloneFkReferencing(List **wqueue, Relation parentRel, Relation partRel)
{
	AttrMap    *attmap;
	List	   *partFKs;
	List	   *clone = NIL;
	ListCell   *cell;

	/* obtain a list of constraints that we need to clone */
	foreach(cell, RelationGetFKeyList(parentRel))
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);

		clone = lappend_oid(clone, fk->conoid);
	}

	/*
	 * Silently do nothing if there's nothing to do.  In particular, this
	 * avoids throwing a spurious error for foreign tables.
	 */
	if (clone == NIL)
		return;

	if (partRel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("foreign key constraints are not supported on foreign tables")));

	/*
	 * The constraint key may differ, if the columns in the partition are
	 * different.  This map is used to convert them.
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(partRel),
								   RelationGetDescr(parentRel));

	partFKs = copyObject(RelationGetFKeyList(partRel));

	foreach(cell, clone)
	{
		Oid			parentConstrOid = lfirst_oid(cell);
		Form_pg_constraint constrForm;
		Relation	pkrel;
		HeapTuple	tuple;
		int			numfks;
		AttrNumber	conkey[INDEX_MAX_KEYS];
		AttrNumber	mapped_conkey[INDEX_MAX_KEYS];
		AttrNumber	confkey[INDEX_MAX_KEYS];
		Oid			conpfeqop[INDEX_MAX_KEYS];
		Oid			conppeqop[INDEX_MAX_KEYS];
		Oid			conffeqop[INDEX_MAX_KEYS];
		Constraint *fkconstraint;
		bool		attached;
		Oid			indexOid;
		Oid			constrOid;
		ObjectAddress address,
					referenced;
		ListCell   *cell;

		tuple = SearchSysCache1(CONSTROID, parentConstrOid);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u",
				 parentConstrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/* Don't clone constraints whose parents are being cloned */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/*
		 * Need to prevent concurrent deletions.  If pkrel is a partitioned
		 * relation, that means to lock all partitions.
		 */
		pkrel = table_open(constrForm->confrelid, ShareRowExclusiveLock);
		if (pkrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			(void) find_all_inheritors(RelationGetRelid(pkrel),
									   ShareRowExclusiveLock, NULL);

		DeconstructFkConstraintRow(tuple, &numfks, conkey, confkey,
								   conpfeqop, conppeqop, conffeqop);
		for (int i = 0; i < numfks; i++)
			mapped_conkey[i] = attmap->attnums[conkey[i] - 1];

		/*
		 * Before creating a new constraint, see whether any existing FKs are
		 * fit for the purpose.  If one is, attach the parent constraint to
		 * it, and don't clone anything.  This way we avoid the expensive
		 * verification step and don't end up with a duplicate FK, and we
		 * don't need to recurse to partitions for this constraint.
		 */
		attached = false;
		foreach(cell, partFKs)
		{
			ForeignKeyCacheInfo *fk = lfirst_node(ForeignKeyCacheInfo, cell);

			if (tryAttachPartitionForeignKey(fk,
											 RelationGetRelid(partRel),
											 parentConstrOid,
											 numfks,
											 mapped_conkey,
											 confkey,
											 conpfeqop))
			{
				attached = true;
				table_close(pkrel, NoLock);
				break;
			}
		}
		if (attached)
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/* No dice.  Set up to create our own constraint */
		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		/* ->conname determined below */
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs determined below */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = true;
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(partRel),
								mapped_conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}
		if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
								 RelationGetRelid(partRel),
								 NameStr(constrForm->conname)))
			fkconstraint->conname =
				ChooseConstraintName(RelationGetRelationName(partRel),
									 ChooseForeignKeyConstraintNameAddition(fkconstraint->fk_attrs),
									 "fkey",
									 RelationGetNamespace(partRel), NIL);
		else
			fkconstraint->conname = pstrdup(NameStr(constrForm->conname));

		indexOid = constrForm->conindid;
		constrOid =
			CreateConstraintEntry(fkconstraint->conname,
								  constrForm->connamespace,
								  CONSTRAINT_FOREIGN,
								  fkconstraint->deferrable,
								  fkconstraint->initdeferred,
								  constrForm->convalidated,
								  parentConstrOid,
								  RelationGetRelid(partRel),
								  mapped_conkey,
								  numfks,
								  numfks,
								  InvalidOid,	/* not a domain constraint */
								  indexOid,
								  constrForm->confrelid,	/* same foreign rel */
								  confkey,
								  conpfeqop,
								  conppeqop,
								  conffeqop,
								  numfks,
								  fkconstraint->fk_upd_action,
								  fkconstraint->fk_del_action,
								  fkconstraint->fk_matchtype,
								  NULL,
								  NULL,
								  NULL,
								  false,	/* islocal */
								  1,	/* inhcount */
								  false,	/* conNoInherit */
								  true);

		/* Set up partition dependencies for the new constraint */
		ObjectAddressSet(address, ConstraintRelationId, constrOid);
		ObjectAddressSet(referenced, ConstraintRelationId, parentConstrOid);
		recordDependencyOn(&address, &referenced, DEPENDENCY_PARTITION_PRI);
		ObjectAddressSet(referenced, RelationRelationId,
						 RelationGetRelid(partRel));
		recordDependencyOn(&address, &referenced, DEPENDENCY_PARTITION_SEC);

		/* Done with the cloned constraint's tuple */
		ReleaseSysCache(tuple);

		/* Make all this visible before recursing */
		CommandCounterIncrement();

		addFkRecurseReferencing(wqueue,
								fkconstraint,
								partRel,
								pkrel,
								indexOid,
								constrOid,
								numfks,
								confkey,
								mapped_conkey,
								conpfeqop,
								conppeqop,
								conffeqop,
								false,	/* no old check exists */
								AccessExclusiveLock);
		table_close(pkrel, NoLock);
	}
}


/*
 * When the parent of a partition receives [the referencing side of] a foreign
 * key, we must propagate that foreign key to the partition.  However, the
 * partition might already have an equivalent foreign key; this routine
 * compares the given ForeignKeyCacheInfo (in the partition) to the FK defined
 * by the other parameters.  If they are equivalent, create the link between
 * the two constraints and return true.
 *
 * If the given FK does not match the one defined by rest of the params,
 * return false.
 */
static bool
tryAttachPartitionForeignKey(ForeignKeyCacheInfo *fk,
							 Oid partRelid,
							 Oid parentConstrOid,
							 int numfks,
							 AttrNumber *mapped_conkey,
							 AttrNumber *confkey,
							 Oid *conpfeqop)
{
	HeapTuple	parentConstrTup;
	Form_pg_constraint parentConstr;
	HeapTuple	partcontup;
	Form_pg_constraint partConstr;
	Relation	trigrel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	parentConstrTup = SearchSysCache1(CONSTROID,
									  ObjectIdGetDatum(parentConstrOid));
	if (!HeapTupleIsValid(parentConstrTup))
		elog(ERROR, "cache lookup failed for constraint %u", parentConstrOid);
	parentConstr = (Form_pg_constraint) GETSTRUCT(parentConstrTup);

	/*
	 * Do some quick & easy initial checks.  If any of these fail, we cannot
	 * use this constraint.
	 */
	if (fk->confrelid != parentConstr->confrelid || fk->nkeys != numfks)
	{
		ReleaseSysCache(parentConstrTup);
		return false;
	}
	for (int i = 0; i < numfks; i++)
	{
		if (fk->conkey[i] != mapped_conkey[i] ||
			fk->confkey[i] != confkey[i] ||
			fk->conpfeqop[i] != conpfeqop[i])
		{
			ReleaseSysCache(parentConstrTup);
			return false;
		}
	}

	/*
	 * Looks good so far; do some more extensive checks.  Presumably the check
	 * for 'convalidated' could be dropped, since we don't really care about
	 * that, but let's be careful for now.
	 */
	partcontup = SearchSysCache1(CONSTROID,
								 ObjectIdGetDatum(fk->conoid));
	if (!HeapTupleIsValid(partcontup))
		elog(ERROR, "cache lookup failed for constraint %u", fk->conoid);
	partConstr = (Form_pg_constraint) GETSTRUCT(partcontup);
	if (OidIsValid(partConstr->conparentid) ||
		!partConstr->convalidated ||
		partConstr->condeferrable != parentConstr->condeferrable ||
		partConstr->condeferred != parentConstr->condeferred ||
		partConstr->confupdtype != parentConstr->confupdtype ||
		partConstr->confdeltype != parentConstr->confdeltype ||
		partConstr->confmatchtype != parentConstr->confmatchtype)
	{
		ReleaseSysCache(parentConstrTup);
		ReleaseSysCache(partcontup);
		return false;
	}

	ReleaseSysCache(partcontup);
	ReleaseSysCache(parentConstrTup);

	/*
	 * Looks good!  Attach this constraint.  The action triggers in the new
	 * partition become redundant -- the parent table already has equivalent
	 * ones, and those will be able to reach the partition.  Remove the ones
	 * in the partition.  We identify them because they have our constraint
	 * OID, as well as being on the referenced rel.
	 */
	trigrel = table_open(TriggerRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(fk->conoid));

	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);
		ObjectAddress trigger;

		if (trgform->tgconstrrelid != fk->conrelid)
			continue;
		if (trgform->tgrelid != fk->confrelid)
			continue;

		/*
		 * The constraint is originally set up to contain this trigger as an
		 * implementation object, so there's a dependency record that links
		 * the two; however, since the trigger is no longer needed, we remove
		 * the dependency link in order to be able to drop the trigger while
		 * keeping the constraint intact.
		 */
		deleteDependencyRecordsFor(TriggerRelationId,
								   trgform->oid,
								   false);
		/* make dependency deletion visible to performDeletion */
		CommandCounterIncrement();
		ObjectAddressSet(trigger, TriggerRelationId,
						 trgform->oid);
		performDeletion(&trigger, DROP_RESTRICT, 0);
		/* make trigger drop visible, in case the loop iterates */
		CommandCounterIncrement();
	}

	systable_endscan(scan);
	table_close(trigrel, RowExclusiveLock);

	ConstraintSetParentConstraint(fk->conoid, parentConstrOid, partRelid);
	CommandCounterIncrement();
	return true;
}


/*
 * QueuePartitionConstraintValidation
 *
 * Add an entry to wqueue to have the given partition constraint validated by
 * Phase 3, for the given relation, and all its children.
 *
 * We first verify whether the given constraint is implied by pre-existing
 * relation constraints; if it is, there's no need to scan the table to
 * validate, so don't queue in that case.
 */
static void
QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
								   List *partConstraint,
								   bool validate_default)
{
	/*
	 * Based on the table's existing constraints, determine whether or not we
	 * may skip scanning the table.
	 */
	if (PartConstraintImpliedByRelConstraint(scanrel, partConstraint))
	{
		if (!validate_default)
			ereport(DEBUG1,
					(errmsg_internal("partition constraint for table \"%s\" is implied by existing constraints",
									 RelationGetRelationName(scanrel))));
		else
			ereport(DEBUG1,
					(errmsg_internal("updated partition constraint for default partition \"%s\" is implied by existing constraints",
									 RelationGetRelationName(scanrel))));
		return;
	}

	/*
	 * Constraints proved insufficient. For plain relations, queue a
	 * validation item now; for partitioned tables, recurse to process each
	 * partition.
	 */
	if (scanrel->rd_rel->relkind == RELKIND_RELATION)
	{
		AlteredTableInfo *tab;

		/* Grab a work queue entry. */
		tab = ATGetQueueEntry(wqueue, scanrel);
		Assert(tab->partition_constraint == NULL);
		tab->partition_constraint = (Expr *) linitial(partConstraint);
		tab->validate_default = validate_default;
	}
	else if (scanrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(scanrel, true);
		int			i;

		for (i = 0; i < partdesc->nparts; i++)
		{
			Relation	part_rel;
			List	   *thisPartConstraint;

			/*
			 * This is the minimum lock we need to prevent deadlocks.
			 */
			part_rel = table_open(partdesc->oids[i], AccessExclusiveLock);

			/*
			 * Adjust the constraint for scanrel so that it matches this
			 * partition's attribute numbers.
			 */
			thisPartConstraint =
				map_partition_varattnos(partConstraint, 1,
										part_rel, scanrel);

			QueuePartitionConstraintValidation(wqueue, part_rel,
											   thisPartConstraint,
											   validate_default);
			table_close(part_rel, NoLock);	/* keep lock till commit */
		}
	}
}


/*
 * MarkInheritDetached
 *
 * Set inhdetachpending for a partition, for ATExecDetachPartition
 * in concurrent mode.  While at it, verify that no other partition is
 * already pending detach.
 */
static void
MarkInheritDetached(Relation child_rel, Relation parent_rel)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	bool		found = false;

	Assert(parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * Find pg_inherits entries by inhparent.  (We need to scan them all in
	 * order to verify that no other partition is pending detach.)
	 */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, InheritsParentIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inhForm;

		inhForm = (Form_pg_inherits) GETSTRUCT(inheritsTuple);
		if (inhForm->inhdetachpending)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("partition \"%s\" already pending detach in partitioned table \"%s.%s\"",
						   get_rel_name(inhForm->inhrelid),
						   get_namespace_name(parent_rel->rd_rel->relnamespace),
						   RelationGetRelationName(parent_rel)),
					errhint("Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation."));

		if (inhForm->inhrelid == RelationGetRelid(child_rel))
		{
			HeapTuple	newtup;

			newtup = heap_copytuple(inheritsTuple);
			((Form_pg_inherits) GETSTRUCT(newtup))->inhdetachpending = true;

			CatalogTupleUpdate(catalogRelation,
							   &inheritsTuple->t_self,
							   newtup);
			found = true;
			heap_freetuple(newtup);
			/* keep looking, to ensure we catch others pending detach */
		}
	}

	/* Done */
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" is not a partition of relation \"%s\"",
						RelationGetRelationName(child_rel),
						RelationGetRelationName(parent_rel))));
}


/*
 * During DETACH PARTITION, verify that any foreign keys pointing to the
 * partitioned table would not become invalid.  An error is raised if any
 * referenced values exist.
 */
static void
ATDetachCheckNoForeignKeyRefs(Relation partition)
{
	List	   *constraints;
	ListCell   *cell;

	constraints = GetParentedForeignKeyRefs(partition);

	foreach(cell, constraints)
	{
		Oid			constrOid = lfirst_oid(cell);
		HeapTuple	tuple;
		Form_pg_constraint constrForm;
		Relation	rel;
		Trigger		trig;

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		Assert(OidIsValid(constrForm->conparentid));
		Assert(constrForm->confrelid == RelationGetRelid(partition));

		/* prevent data changes into the referencing table until commit */
		rel = table_open(constrForm->conrelid, ShareLock);

		MemSet(&trig, 0, sizeof(trig));
		trig.tgoid = InvalidOid;
		trig.tgname = NameStr(constrForm->conname);
		trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
		trig.tgisinternal = true;
		trig.tgconstrrelid = RelationGetRelid(partition);
		trig.tgconstrindid = constrForm->conindid;
		trig.tgconstraint = constrForm->oid;
		trig.tgdeferrable = false;
		trig.tginitdeferred = false;
		/* we needn't fill in remaining fields */

		RI_PartitionRemove_Check(&trig, rel, partition);

		ReleaseSysCache(tuple);

		table_close(rel, NoLock);
	}
}


/*
 * DetachAddConstraintIfNeeded
 *		Subroutine for ATExecDetachPartition.  Create a constraint that
 *		takes the place of the partition constraint, but avoid creating
 *		a dupe if an constraint already exists which implies the needed
 *		constraint.
 */
static void
DetachAddConstraintIfNeeded(List **wqueue, Relation partRel)
{
	List	   *constraintExpr;

	constraintExpr = RelationGetPartitionQual(partRel);
	constraintExpr = (List *) eval_const_expressions(NULL, (Node *) constraintExpr);

	/*
	 * Avoid adding a new constraint if the needed constraint is implied by an
	 * existing constraint
	 */
	if (!PartConstraintImpliedByRelConstraint(partRel, constraintExpr))
	{
		AlteredTableInfo *tab;
		Constraint *n;

		tab = ATGetQueueEntry(wqueue, partRel);

		/* Add constraint on partition, equivalent to the partition constraint */
		n = makeNode(Constraint);
		n->contype = CONSTR_CHECK;
		n->conname = NULL;
		n->location = -1;
		n->is_no_inherit = false;
		n->raw_expr = NULL;
		n->cooked_expr = nodeToString(make_ands_explicit(constraintExpr));
		n->initially_valid = true;
		n->skip_validation = true;
		/* It's a re-add, since it nominally already exists */
		ATAddCheckConstraint(wqueue, tab, partRel, n,
							 true, false, true, ShareUpdateExclusiveLock);
	}
}


/*
 * DropClonedTriggersFromPartition
 *		subroutine for ATExecDetachPartition to remove any triggers that were
 *		cloned to the partition when it was created-as-partition or attached.
 *		This undoes what CloneRowTriggersToPartition did.
 */
static void
DropClonedTriggersFromPartition(Oid partitionId)
{
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	trigtup;
	Relation	tgrel;
	ObjectAddresses *objects;

	objects = new_object_addresses();

	/*
	 * Scan pg_trigger to search for all triggers on this rel.
	 */
	ScanKeyInit(&skey, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(partitionId));
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);
	scan = systable_beginscan(tgrel, TriggerRelidNameIndexId,
							  true, NULL, 1, &skey);
	while (HeapTupleIsValid(trigtup = systable_getnext(scan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(trigtup);
		ObjectAddress trig;

		/* Ignore triggers that weren't cloned */
		if (!OidIsValid(pg_trigger->tgparentid))
			continue;

		/*
		 * This is ugly, but necessary: remove the dependency markings on the
		 * trigger so that it can be removed.
		 */
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										TriggerRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);

		/* remember this trigger to remove it below */
		ObjectAddressSet(trig, TriggerRelationId, pg_trigger->oid);
		add_exact_object_address(&trig, objects);
	}

	/* make the dependency removal visible to the deletion below */
	CommandCounterIncrement();
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* done */
	free_object_addresses(objects);
	systable_endscan(scan);
	table_close(tgrel, RowExclusiveLock);
}


/*
 * createForeignKeyActionTriggers
 *		Create the referenced-side "action" triggers that implement a foreign
 *		key.
 */
static void
createForeignKeyActionTriggers(Relation rel, Oid refRelOid, Constraint *fkconstraint,
							   Oid constraintOid, Oid indexOid)
{
	CreateTrigStmt *fk_trigger;

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * DELETE action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_a";
	fk_trigger->relation = NULL;
	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->events = TRIGGER_TYPE_DELETE;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->constrrel = NULL;
	switch (fkconstraint->fk_del_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_del");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_del");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_del");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_del");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_del");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_del_action);
			break;
	}

	(void) CreateTrigger(fk_trigger, NULL, refRelOid, RelationGetRelid(rel),
						 constraintOid,
						 indexOid, InvalidOid, InvalidOid, NULL, true, false);

	/* Make changes-so-far visible */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * UPDATE action on the referenced table.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_a";
	fk_trigger->relation = NULL;
	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->events = TRIGGER_TYPE_UPDATE;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->constrrel = NULL;
	switch (fkconstraint->fk_upd_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_upd");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_upd");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_upd");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_upd");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_upd");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_upd_action);
			break;
	}

	(void) CreateTrigger(fk_trigger, NULL, refRelOid, RelationGetRelid(rel),
						 constraintOid,
						 indexOid, InvalidOid, InvalidOid, NULL, true, false);
}


/*
 * Return an OID list of constraints that reference the given relation
 * that are marked as having a parent constraints.
 */
static List *
GetParentedForeignKeyRefs(Relation partition)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData key[2];
	List	   *constraints = NIL;

	/*
	 * If no indexes, or no columns are referenceable by FKs, we can avoid the
	 * scan.
	 */
	if (RelationGetIndexList(partition) == NIL ||
		bms_is_empty(RelationGetIndexAttrBitmap(partition,
												INDEX_ATTR_BITMAP_KEY)))
		return NIL;

	/* Search for constraints referencing this table */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(partition)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));

	/* XXX This is a seqscan, as we don't have a usable index */
	scan = systable_beginscan(pg_constraint, InvalidOid, true, NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * We only need to process constraints that are part of larger ones.
		 */
		if (!OidIsValid(constrForm->conparentid))
			continue;

		constraints = lappend_oid(constraints, constrForm->oid);
	}

	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);

	return constraints;
}


/*
 * ConstraintImpliedByRelConstraint
 *		Do scanrel's existing constraints imply the given constraint?
 *
 * testConstraint is the constraint to validate. provenConstraint is a
 * caller-provided list of conditions which this function may assume
 * to be true. Both provenConstraint and testConstraint must be in
 * implicit-AND form, must only contain immutable clauses, and must
 * contain only Vars with varno = 1.
 */
static bool
ConstraintImpliedByRelConstraint(Relation scanrel, List *testConstraint, List *provenConstraint)
{
	List	   *existConstraint = list_copy(provenConstraint);
	TupleConstr *constr = RelationGetDescr(scanrel)->constr;
	int			num_check,
				i;

	num_check = (constr != NULL) ? constr->num_check : 0;
	for (i = 0; i < num_check; i++)
	{
		Node	   *cexpr;

		/*
		 * If this constraint hasn't been fully validated yet, we must ignore
		 * it here.
		 */
		if (!constr->check[i].ccvalid)
			continue;

		cexpr = stringToNode(constr->check[i].ccbin);

		/*
		 * Run each expression through const-simplification and
		 * canonicalization.  It is necessary, because we will be comparing it
		 * to similarly-processed partition constraint expressions, and may
		 * fail to detect valid matches without this.
		 */
		cexpr = eval_const_expressions(NULL, cexpr);
		cexpr = (Node *) canonicalize_qual((Expr *) cexpr, true);

		existConstraint = list_concat(existConstraint,
									  make_ands_implicit((Expr *) cexpr));
	}

	/*
	 * Try to make the proof.  Since we are comparing CHECK constraints, we
	 * need to use weak implication, i.e., we assume existConstraint is
	 * not-false and try to prove the same for testConstraint.
	 *
	 * Note that predicate_implied_by assumes its first argument is known
	 * immutable.  That should always be true for both NOT NULL and partition
	 * constraints, so we don't test it here.
	 */
	return predicate_implied_by(testConstraint, existConstraint, true);
}


/*
 * transformColumnNameList - transform list of column names
 *
 * Lookup each name and return its attnum and type OID
 */
static int
transformColumnNameList(Oid relId, List *colList,
						int16 *attnums, Oid *atttypids)
{
	ListCell   *l;
	int			attnum;

	attnum = 0;
	foreach(l, colList)
	{
		char	   *attname = strVal(lfirst(l));
		HeapTuple	atttuple;

		atttuple = SearchSysCacheAttName(relId, attname);
		if (!HeapTupleIsValid(atttuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" referenced in foreign key constraint does not exist",
							attname)));
		if (attnum >= INDEX_MAX_KEYS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot have more than %d keys in a foreign key",
							INDEX_MAX_KEYS)));
		attnums[attnum] = ((Form_pg_attribute) GETSTRUCT(atttuple))->attnum;
		atttypids[attnum] = ((Form_pg_attribute) GETSTRUCT(atttuple))->atttypid;
		ReleaseSysCache(atttuple);
		attnum++;
	}

	return attnum;
}


/*
 * transformFkeyGetPrimaryKey -
 *
 *	Look up the names, attnums, and types of the primary key attributes
 *	for the pkrel.  Also return the index OID and index opclasses of the
 *	index supporting the primary key.
 *
 *	All parameters except pkrel are output parameters.  Also, the function
 *	return value is the number of attributes in the primary key.
 *
 *	Used when the column list in the REFERENCES specification is omitted.
 */
static int
transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
						   List **attnamelist,
						   int16 *attnums, Oid *atttypids,
						   Oid *opclasses)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	HeapTuple	indexTuple = NULL;
	Form_pg_index indexStruct = NULL;
	Datum		indclassDatum;
	bool		isnull;
	oidvector  *indclass;
	int			i;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache until we find one marked primary key
	 * (hopefully there isn't more than one such).  Insist it's valid, too.
	 */
	*indexOid = InvalidOid;

	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);
		if (indexStruct->indisprimary && indexStruct->indisvalid)
		{
			/*
			 * Refuse to use a deferrable primary key.  This is per SQL spec,
			 * and there would be a lot of interesting semantic problems if we
			 * tried to allow it.
			 */
			if (!indexStruct->indimmediate)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot use a deferrable primary key for referenced table \"%s\"",
								RelationGetRelationName(pkrel))));

			*indexOid = indexoid;
			break;
		}
		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/*
	 * Check that we found it
	 */
	if (!OidIsValid(*indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no primary key for referenced table \"%s\"",
						RelationGetRelationName(pkrel))));

	/* Must get indclass the hard way */
	indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
									Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	/*
	 * Now build the list of PK attributes from the indkey definition (we
	 * assume a primary key cannot have expressional elements)
	 */
	*attnamelist = NIL;
	for (i = 0; i < indexStruct->indnkeyatts; i++)
	{
		int			pkattno = indexStruct->indkey.values[i];

		attnums[i] = pkattno;
		atttypids[i] = attnumTypeId(pkrel, pkattno);
		opclasses[i] = indclass->values[i];
		*attnamelist = lappend(*attnamelist,
							   makeString(pstrdup(NameStr(*attnumAttName(pkrel, pkattno)))));
	}

	ReleaseSysCache(indexTuple);

	return i;
}


/*
 * transformFkeyCheckAttrs -
 *
 *	Make sure that the attributes of a referenced table belong to a unique
 *	(or primary key) constraint.  Return the OID of the index supporting
 *	the constraint, as well as the opclasses associated with the index
 *	columns.
 */
static Oid
transformFkeyCheckAttrs(Relation pkrel,
						int numattrs, int16 *attnums,
						Oid *opclasses) /* output parameter */
{
	Oid			indexoid = InvalidOid;
	bool		found = false;
	bool		found_deferrable = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int			i,
				j;

	/*
	 * Reject duplicate appearances of columns in the referenced-columns list.
	 * Such a case is forbidden by the SQL standard, and even if we thought it
	 * useful to allow it, there would be ambiguity about how to match the
	 * list to unique indexes (in particular, it'd be unclear which index
	 * opclass goes with which FK column).
	 */
	for (i = 0; i < numattrs; i++)
	{
		for (j = i + 1; j < numattrs; j++)
		{
			if (attnums[i] == attnums[j])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FOREIGN_KEY),
						 errmsg("foreign key referenced-columns list must not contain duplicates")));
		}
	}

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache, and match unique indexes to the list
	 * of attnums we are given.
	 */
	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;

		indexoid = lfirst_oid(indexoidscan);
		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Must have the right number of columns; must be unique and not a
		 * partial index; forget it if there are any expressions, too. Invalid
		 * indexes are out as well.
		 */
		if (indexStruct->indnkeyatts == numattrs &&
			indexStruct->indisunique &&
			indexStruct->indisvalid &&
			heap_attisnull(indexTuple, Anum_pg_index_indpred, NULL) &&
			heap_attisnull(indexTuple, Anum_pg_index_indexprs, NULL))
		{
			Datum		indclassDatum;
			bool		isnull;
			oidvector  *indclass;

			/* Must get indclass the hard way */
			indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple,
											Anum_pg_index_indclass, &isnull);
			Assert(!isnull);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/*
			 * The given attnum list may match the index columns in any order.
			 * Check for a match, and extract the appropriate opclasses while
			 * we're at it.
			 *
			 * We know that attnums[] is duplicate-free per the test at the
			 * start of this function, and we checked above that the number of
			 * index columns agrees, so if we find a match for each attnums[]
			 * entry then we must have a one-to-one match in some order.
			 */
			for (i = 0; i < numattrs; i++)
			{
				found = false;
				for (j = 0; j < numattrs; j++)
				{
					if (attnums[i] == indexStruct->indkey.values[j])
					{
						opclasses[i] = indclass->values[j];
						found = true;
						break;
					}
				}
				if (!found)
					break;
			}

			/*
			 * Refuse to use a deferrable unique/primary key.  This is per SQL
			 * spec, and there would be a lot of interesting semantic problems
			 * if we tried to allow it.
			 */
			if (found && !indexStruct->indimmediate)
			{
				/*
				 * Remember that we found an otherwise matching index, so that
				 * we can generate a more appropriate error message.
				 */
				found_deferrable = true;
				found = false;
			}
		}
		ReleaseSysCache(indexTuple);
		if (found)
			break;
	}

	if (!found)
	{
		if (found_deferrable)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot use a deferrable unique constraint for referenced table \"%s\"",
							RelationGetRelationName(pkrel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("there is no unique constraint matching given keys for referenced table \"%s\"",
							RelationGetRelationName(pkrel))));
	}

	list_free(indexoidlist);

	return indexoid;
}


/*
 * Permissions checks on the referenced table for ADD FOREIGN KEY
 *
 * Note: we have already checked that the user owns the referencing table,
 * else we'd have failed much earlier; no additional checks are needed for it.
 */
static void
checkFkeyPermissions(Relation rel, int16 *attnums, int natts)
{
	Oid			roleid = GetUserId();
	AclResult	aclresult;
	int			i;

	/* Okay if we have relation-level REFERENCES permission */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), roleid,
								  ACL_REFERENCES);
	if (aclresult == ACLCHECK_OK)
		return;
	/* Else we must have REFERENCES on each column */
	for (i = 0; i < natts; i++)
	{
		aclresult = pg_attribute_aclcheck(RelationGetRelid(rel), attnums[i],
										  roleid, ACL_REFERENCES);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));
	}
}


/*
 * findFkeyCast -
 *
 *	Wrapper around find_coercion_pathway() for ATAddForeignKeyConstraint().
 *	Caller has equal regard for binary coercibility and for an exact match.
*/
static CoercionPathType
findFkeyCast(Oid targetTypeId, Oid sourceTypeId, Oid *funcid)
{
	CoercionPathType ret;

	if (targetTypeId == sourceTypeId)
	{
		ret = COERCION_PATH_RELABELTYPE;
		*funcid = InvalidOid;
	}
	else
	{
		ret = find_coercion_pathway(targetTypeId, sourceTypeId,
									COERCION_IMPLICIT, funcid);
		if (ret == COERCION_PATH_NONE)
			/* A previously-relied-upon cast is now gone. */
			elog(ERROR, "could not find cast from %u to %u",
				 sourceTypeId, targetTypeId);
	}

	return ret;
}


/*
 * addFkRecurseReferenced
 *		subroutine for ATAddForeignKeyConstraint; recurses on the referenced
 *		side of the constraint
 *
 * Create pg_constraint rows for the referenced side of the constraint,
 * referencing the parent of the referencing side; also create action triggers
 * on leaf partitions.  If the table is partitioned, recurse to handle each
 * partition.
 *
 * wqueue is the ALTER TABLE work queue; can be NULL when not running as part
 * of an ALTER TABLE sequence.
 * fkconstraint is the constraint being added.
 * rel is the root referencing relation.
 * pkrel is the referenced relation; might be a partition, if recursing.
 * indexOid is the OID of the index (on pkrel) implementing this constraint.
 * parentConstr is the OID of a parent constraint; InvalidOid if this is a
 * top-level constraint.
 * numfks is the number of columns in the foreign key
 * pkattnum is the attnum array of referenced attributes.
 * fkattnum is the attnum array of referencing attributes.
 * pf/pp/ffeqoperators are OID array of operators between columns.
 * old_check_ok signals that this constraint replaces an existing one that
 * was already validated (thus this one doesn't need validation).
 */
static ObjectAddress
addFkRecurseReferenced(List **wqueue, Constraint *fkconstraint, Relation rel,
					   Relation pkrel, Oid indexOid, Oid parentConstr,
					   int numfks,
					   int16 *pkattnum, int16 *fkattnum, Oid *pfeqoperators,
					   Oid *ppeqoperators, Oid *ffeqoperators, bool old_check_ok)
{
	ObjectAddress address;
	Oid			constrOid;
	char	   *conname;
	bool		conislocal;
	int			coninhcount;
	bool		connoinherit;

	/*
	 * Verify relkind for each referenced partition.  At the top level, this
	 * is redundant with a previous check, but we need it when recursing.
	 */
	if (pkrel->rd_rel->relkind != RELKIND_RELATION &&
		pkrel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	/*
	 * Caller supplies us with a constraint name; however, it may be used in
	 * this partition, so come up with a different one in that case.
	 */
	if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
							 RelationGetRelid(rel),
							 fkconstraint->conname))
		conname = ChooseConstraintName(RelationGetRelationName(rel),
									   ChooseForeignKeyConstraintNameAddition(fkconstraint->fk_attrs),
									   "fkey",
									   RelationGetNamespace(rel), NIL);
	else
		conname = fkconstraint->conname;

	if (OidIsValid(parentConstr))
	{
		conislocal = false;
		coninhcount = 1;
		connoinherit = false;
	}
	else
	{
		conislocal = true;
		coninhcount = 0;

		/*
		 * always inherit for partitioned tables, never for legacy inheritance
		 */
		connoinherit = rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE;
	}

	/*
	 * Record the FK constraint in pg_constraint.
	 */
	constrOid = CreateConstraintEntry(conname,
									  RelationGetNamespace(rel),
									  CONSTRAINT_FOREIGN,
									  fkconstraint->deferrable,
									  fkconstraint->initdeferred,
									  fkconstraint->initially_valid,
									  parentConstr,
									  RelationGetRelid(rel),
									  fkattnum,
									  numfks,
									  numfks,
									  InvalidOid,	/* not a domain constraint */
									  indexOid,
									  RelationGetRelid(pkrel),
									  pkattnum,
									  pfeqoperators,
									  ppeqoperators,
									  ffeqoperators,
									  numfks,
									  fkconstraint->fk_upd_action,
									  fkconstraint->fk_del_action,
									  fkconstraint->fk_matchtype,
									  NULL, /* no exclusion constraint */
									  NULL, /* no check constraint */
									  NULL,
									  conislocal,	/* islocal */
									  coninhcount,	/* inhcount */
									  connoinherit, /* conNoInherit */
									  false);	/* is_internal */

	ObjectAddressSet(address, ConstraintRelationId, constrOid);

	/*
	 * Mark the child constraint as part of the parent constraint; it must not
	 * be dropped on its own.  (This constraint is deleted when the partition
	 * is detached, but a special check needs to occur that the partition
	 * contains no referenced values.)
	 */
	if (OidIsValid(parentConstr))
	{
		ObjectAddress referenced;

		ObjectAddressSet(referenced, ConstraintRelationId, parentConstr);
		recordDependencyOn(&address, &referenced, DEPENDENCY_INTERNAL);
	}

	/* make new constraint visible, in case we add more */
	CommandCounterIncrement();

	/*
	 * If the referenced table is a plain relation, create the action triggers
	 * that enforce the constraint.
	 */
	if (pkrel->rd_rel->relkind == RELKIND_RELATION)
	{
		createForeignKeyActionTriggers(rel, RelationGetRelid(pkrel),
									   fkconstraint,
									   constrOid, indexOid);
	}

	/*
	 * If the referenced table is partitioned, recurse on ourselves to handle
	 * each partition.  We need one pg_constraint row created for each
	 * partition in addition to the pg_constraint row for the parent table.
	 */
	if (pkrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc pd = RelationGetPartitionDesc(pkrel, true);

		for (int i = 0; i < pd->nparts; i++)
		{
			Relation	partRel;
			AttrMap    *map;
			AttrNumber *mapped_pkattnum;
			Oid			partIndexId;

			partRel = table_open(pd->oids[i], ShareRowExclusiveLock);

			/*
			 * Map the attribute numbers in the referenced side of the FK
			 * definition to match the partition's column layout.
			 */
			map = build_attrmap_by_name_if_req(RelationGetDescr(partRel),
											   RelationGetDescr(pkrel));
			if (map)
			{
				mapped_pkattnum = palloc(sizeof(AttrNumber) * numfks);
				for (int j = 0; j < numfks; j++)
					mapped_pkattnum[j] = map->attnums[pkattnum[j] - 1];
			}
			else
				mapped_pkattnum = pkattnum;

			/* do the deed */
			partIndexId = index_get_partition(partRel, indexOid);
			if (!OidIsValid(partIndexId))
				elog(ERROR, "index for %u not found in partition %s",
					 indexOid, RelationGetRelationName(partRel));
			addFkRecurseReferenced(wqueue, fkconstraint, rel, partRel,
								   partIndexId, constrOid, numfks,
								   mapped_pkattnum, fkattnum,
								   pfeqoperators, ppeqoperators, ffeqoperators,
								   old_check_ok);

			/* Done -- clean up (but keep the lock) */
			table_close(partRel, NoLock);
			if (map)
			{
				pfree(mapped_pkattnum);
				free_attrmap(map);
			}
		}
	}

	return address;
}


/*
 * addFkRecurseReferencing
 *		subroutine for ATAddForeignKeyConstraint and CloneFkReferencing
 *
 * If the referencing relation is a plain relation, create the necessary check
 * triggers that implement the constraint, and set up for Phase 3 constraint
 * verification.  If the referencing relation is a partitioned table, then
 * we create a pg_constraint row for it and recurse on this routine for each
 * partition.
 *
 * We assume that the referenced relation is locked against concurrent
 * deletions.  If it's a partitioned relation, every partition must be so
 * locked.
 *
 * wqueue is the ALTER TABLE work queue; can be NULL when not running as part
 * of an ALTER TABLE sequence.
 * fkconstraint is the constraint being added.
 * rel is the referencing relation; might be a partition, if recursing.
 * pkrel is the root referenced relation.
 * indexOid is the OID of the index (on pkrel) implementing this constraint.
 * parentConstr is the OID of the parent constraint (there is always one).
 * numfks is the number of columns in the foreign key
 * pkattnum is the attnum array of referenced attributes.
 * fkattnum is the attnum array of referencing attributes.
 * pf/pp/ffeqoperators are OID array of operators between columns.
 * old_check_ok signals that this constraint replaces an existing one that
 *		was already validated (thus this one doesn't need validation).
 * lockmode is the lockmode to acquire on partitions when recursing.
 */
static void
addFkRecurseReferencing(List **wqueue, Constraint *fkconstraint, Relation rel,
						Relation pkrel, Oid indexOid, Oid parentConstr,
						int numfks, int16 *pkattnum, int16 *fkattnum,
						Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
						bool old_check_ok, LOCKMODE lockmode)
{
	AssertArg(OidIsValid(parentConstr));

	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("foreign key constraints are not supported on foreign tables")));

	/*
	 * If the referencing relation is a plain table, add the check triggers to
	 * it and, if necessary, schedule it to be checked in Phase 3.
	 *
	 * If the relation is partitioned, drill down to do it to its partitions.
	 */
	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		createForeignKeyCheckTriggers(RelationGetRelid(rel),
									  RelationGetRelid(pkrel),
									  fkconstraint,
									  parentConstr,
									  indexOid);

		/*
		 * Tell Phase 3 to check that the constraint is satisfied by existing
		 * rows. We can skip this during table creation, when requested
		 * explicitly by specifying NOT VALID in an ADD FOREIGN KEY command,
		 * and when we're recreating a constraint following a SET DATA TYPE
		 * operation that did not impugn its validity.
		 */
		if (wqueue && !old_check_ok && !fkconstraint->skip_validation)
		{
			NewConstraint *newcon;
			AlteredTableInfo *tab;

			tab = ATGetQueueEntry(wqueue, rel);

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = get_constraint_name(parentConstr);
			newcon->contype = CONSTR_FOREIGN;
			newcon->refrelid = RelationGetRelid(pkrel);
			newcon->refindid = indexOid;
			newcon->conid = parentConstr;
			newcon->qual = (Node *) fkconstraint;

			tab->constraints = lappend(tab->constraints, newcon);
		}
	}
	else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc pd = RelationGetPartitionDesc(rel, true);

		/*
		 * Recurse to take appropriate action on each partition; either we
		 * find an existing constraint to reparent to ours, or we create a new
		 * one.
		 */
		for (int i = 0; i < pd->nparts; i++)
		{
			Oid			partitionId = pd->oids[i];
			Relation	partition = table_open(partitionId, lockmode);
			List	   *partFKs;
			AttrMap    *attmap;
			AttrNumber	mapped_fkattnum[INDEX_MAX_KEYS];
			bool		attached;
			char	   *conname;
			Oid			constrOid;
			ObjectAddress address,
						referenced;
			ListCell   *cell;

			CheckTableNotInUse(partition, "ALTER TABLE");

			attmap = build_attrmap_by_name(RelationGetDescr(partition),
										   RelationGetDescr(rel));
			for (int j = 0; j < numfks; j++)
				mapped_fkattnum[j] = attmap->attnums[fkattnum[j] - 1];

			/* Check whether an existing constraint can be repurposed */
			partFKs = copyObject(RelationGetFKeyList(partition));
			attached = false;
			foreach(cell, partFKs)
			{
				ForeignKeyCacheInfo *fk;

				fk = lfirst_node(ForeignKeyCacheInfo, cell);
				if (tryAttachPartitionForeignKey(fk,
												 partitionId,
												 parentConstr,
												 numfks,
												 mapped_fkattnum,
												 pkattnum,
												 pfeqoperators))
				{
					attached = true;
					break;
				}
			}
			if (attached)
			{
				table_close(partition, NoLock);
				continue;
			}

			/*
			 * No luck finding a good constraint to reuse; create our own.
			 */
			if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
									 RelationGetRelid(partition),
									 fkconstraint->conname))
				conname = ChooseConstraintName(RelationGetRelationName(partition),
											   ChooseForeignKeyConstraintNameAddition(fkconstraint->fk_attrs),
											   "fkey",
											   RelationGetNamespace(partition), NIL);
			else
				conname = fkconstraint->conname;
			constrOid =
				CreateConstraintEntry(conname,
									  RelationGetNamespace(partition),
									  CONSTRAINT_FOREIGN,
									  fkconstraint->deferrable,
									  fkconstraint->initdeferred,
									  fkconstraint->initially_valid,
									  parentConstr,
									  partitionId,
									  mapped_fkattnum,
									  numfks,
									  numfks,
									  InvalidOid,
									  indexOid,
									  RelationGetRelid(pkrel),
									  pkattnum,
									  pfeqoperators,
									  ppeqoperators,
									  ffeqoperators,
									  numfks,
									  fkconstraint->fk_upd_action,
									  fkconstraint->fk_del_action,
									  fkconstraint->fk_matchtype,
									  NULL,
									  NULL,
									  NULL,
									  false,
									  1,
									  false,
									  false);

			/*
			 * Give this constraint partition-type dependencies on the parent
			 * constraint as well as the table.
			 */
			ObjectAddressSet(address, ConstraintRelationId, constrOid);
			ObjectAddressSet(referenced, ConstraintRelationId, parentConstr);
			recordDependencyOn(&address, &referenced, DEPENDENCY_PARTITION_PRI);
			ObjectAddressSet(referenced, RelationRelationId, partitionId);
			recordDependencyOn(&address, &referenced, DEPENDENCY_PARTITION_SEC);

			/* Make all this visible before recursing */
			CommandCounterIncrement();

			/* call ourselves to finalize the creation and we're done */
			addFkRecurseReferencing(wqueue, fkconstraint, partition, pkrel,
									indexOid,
									constrOid,
									numfks,
									pkattnum,
									mapped_fkattnum,
									pfeqoperators,
									ppeqoperators,
									ffeqoperators,
									old_check_ok,
									lockmode);

			table_close(partition, NoLock);
		}
	}
}


/*
 * Obtain the source-text form of the constraint expression for a check
 * constraint, given its pg_constraint tuple
 */
static char *
decompile_conbin(HeapTuple contup, TupleDesc tupdesc)
{
	Form_pg_constraint con;
	bool		isnull;
	Datum		attr;
	Datum		expr;

	con = (Form_pg_constraint) GETSTRUCT(contup);
	attr = heap_getattr(contup, Anum_pg_constraint_conbin, tupdesc, &isnull);
	if (isnull)
		elog(ERROR, "null conbin for constraint %u", con->oid);

	expr = DirectFunctionCall2(pg_get_expr, attr,
							   ObjectIdGetDatum(con->conrelid));
	return TextDatumGetCString(expr);
}


/*
 * Determine whether two check constraints are functionally equivalent
 *
 * The test we apply is to see whether they reverse-compile to the same
 * source string.  This insulates us from issues like whether attributes
 * have the same physical column numbers in parent and child relations.
 */
static bool
constraints_equivalent(HeapTuple a, HeapTuple b, TupleDesc tupleDesc)
{
	Form_pg_constraint acon = (Form_pg_constraint) GETSTRUCT(a);
	Form_pg_constraint bcon = (Form_pg_constraint) GETSTRUCT(b);

	if (acon->condeferrable != bcon->condeferrable ||
		acon->condeferred != bcon->condeferred ||
		strcmp(decompile_conbin(a, tupleDesc),
			   decompile_conbin(b, tupleDesc)) != 0)
		return false;
	else
		return true;
}


/*
 * Generate the column-name portion of the constraint name for a new foreign
 * key given the list of column names that reference the referenced
 * table.  This will be passed to ChooseConstraintName along with the parent
 * table name and the "fkey" suffix.
 *
 * We know that less than NAMEDATALEN characters will actually be used, so we
 * can truncate the result once we've generated that many.
 *
 * XXX see also ChooseExtendedStatisticNameAddition and
 * ChooseIndexNameAddition.
 */
static char *
ChooseForeignKeyConstraintNameAddition(List *colnames)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, colnames)
	{
		const char *name = strVal(lfirst(lc));

		if (buflen > 0)
			buf[buflen++] = '_';	/* insert _ between names */

		/*
		 * At this point we have buflen <= NAMEDATALEN.  name should be less
		 * than NAMEDATALEN already, but use strlcpy for paranoia.
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}


static void
index_copy_data(Relation rel, RelFileNode newrnode)
{
	SMgrRelation dstrel;

	dstrel = smgropen(newrnode, rel->rd_backend);

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	/*
	 * Create and copy all forks of the relation, and schedule unlinking of
	 * old physical files.
	 *
	 * NOTE: any conflict in relfilenode value will be caught in
	 * RelationCreateStorage().
	 */
	RelationCreateStorage(newrnode, rel->rd_rel->relpersistence);

	/* copy main fork */
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* copy those extra forks that exist */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or this is the
			 * init fork of an unlogged relation.
			 */
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(&newrnode, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}

	/* drop old relation, and close new one */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}


/*
 * createForeignKeyCheckTriggers
 *		Create the referencing-side "check" triggers that implement a foreign
 *		key.
 */
static void
createForeignKeyCheckTriggers(Oid myRelOid, Oid refRelOid,
							  Constraint *fkconstraint, Oid constraintOid,
							  Oid indexOid)
{
	CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint, constraintOid,
						 indexOid, true);
	CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint, constraintOid,
						 indexOid, false);
}


static void
CreateFKCheckTrigger(Oid myRelOid, Oid refRelOid, Constraint *fkconstraint,
					 Oid constraintOid, Oid indexOid, bool on_insert)
{
	CreateTrigStmt *fk_trigger;

	/*
	 * Note: for a self-referential FK (referencing and referenced tables are
	 * the same), it is important that the ON UPDATE action fires before the
	 * CHECK action, since both triggers will fire on the same row during an
	 * UPDATE event; otherwise the CHECK trigger will be checking a non-final
	 * state of the row.  Triggers fire in name order, so we ensure this by
	 * using names like "RI_ConstraintTrigger_a_NNNN" for the action triggers
	 * and "RI_ConstraintTrigger_c_NNNN" for the check triggers.
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_c";
	fk_trigger->relation = NULL;

	/* Either ON INSERT or ON UPDATE */
	if (on_insert)
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_ins");
		fk_trigger->events = TRIGGER_TYPE_INSERT;
	}
	else
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_upd");
		fk_trigger->events = TRIGGER_TYPE_UPDATE;
	}

	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->deferrable = fkconstraint->deferrable;
	fk_trigger->initdeferred = fkconstraint->initdeferred;
	fk_trigger->constrrel = NULL;

	(void) CreateTrigger(fk_trigger, NULL, myRelOid, refRelOid, constraintOid,
						 indexOid, InvalidOid, InvalidOid, NULL, true, false);

	/* Make changes-so-far visible */
	CommandCounterIncrement();
}


/*
 * resolve column compression specification to compression method.
 */
static char
GetAttributeCompression(Oid atttypid, char *compression)
{
	char		cmethod;

	if (compression == NULL || strcmp(compression, "default") == 0)
		return InvalidCompressionMethod;

	/*
	 * To specify a nondefault method, the column data type must be toastable.
	 * Note this says nothing about whether the column's attstorage setting
	 * permits compression; we intentionally allow attstorage and
	 * attcompression to be independent.  But with a non-toastable type,
	 * attstorage could not be set to a value that would permit compression.
	 *
	 * We don't actually need to enforce this, since nothing bad would happen
	 * if attcompression were non-default; it would never be consulted.  But
	 * it seems more user-friendly to complain about a certainly-useless
	 * attempt to set the property.
	 */
	if (!TypeIsToastable(atttypid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s does not support compression",
						format_type_be(atttypid))));

	cmethod = CompressionNameToMethod(compression);
	if (!CompressionMethodIsValid(cmethod))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid compression method \"%s\"", compression)));

	return cmethod;
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


/*
 *		renameatt_check			- basic sanity checks before attribute rename
 */
static void
renameatt_check(Oid myrelid, Form_pg_class classform, bool recursing)
{
	char		relkind = classform->relkind;

	if (classform->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot rename column of typed table")));

	/*
	 * Renaming the columns of sequences or toast tables doesn't actually
	 * break anything from the system's point of view, since internal
	 * references are by attnum.  But it doesn't seem right to allow users to
	 * change names that are hardcoded into the system, hence the following
	 * restriction.
	 */
	if (relkind != RELKIND_RELATION &&
		relkind != RELKIND_VIEW &&
		relkind != RELKIND_MATVIEW &&
		relkind != RELKIND_COMPOSITE_TYPE &&
		relkind != RELKIND_INDEX &&
		relkind != RELKIND_PARTITIONED_INDEX &&
		relkind != RELKIND_FOREIGN_TABLE &&
		relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, view, materialized view, composite type, index, or foreign table",
						NameStr(classform->relname))));

	/*
	 * permissions checking.  only the owner of a class can change its schema.
	 */
	if (!pg_class_ownercheck(myrelid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(myrelid)),
					   NameStr(classform->relname));
	if (!allowSystemTableMods && IsSystemClass(myrelid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						NameStr(classform->relname))));
}


/*
 * Perform permissions and integrity checks before acquiring a relation lock.
 */
static void
RangeVarCallbackForRenameAttribute(const RangeVar *rv, Oid relid, Oid oldrelid,
								   void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
	form = (Form_pg_class) GETSTRUCT(tuple);
	renameatt_check(relid, form, false);
	ReleaseSysCache(tuple);
}


static char *
mysCheckConstraintName(Oid relOid, char *userInputConstraintName)
{
    char *ret;

    if (get_relation_constraint_oid(relOid, userInputConstraintName, true) == InvalidOid)
    {
        // 假设该约束使用索引实现(primary key, unique key)， 该情况下约束名与索引名相同
        ret = mysBuildInternalIndexName(relOid, userInputConstraintName);
    }
    else
    {
        ret = pstrdup(userInputConstraintName);
    }

    return ret;
}


/*
 * 针对Mysql中同一个库下索引名可以相同的问题。
 *      返回一个在当前内存上下文下的字符串.
 */
char *
mysCheckIndexName(Oid relOid, char *oriIndexName)
{
    char *ret;
    HeapTuple indexClassTuple;
    bool useInternalIndexName = true;

    indexClassTuple = SearchSysCache2(RELNAMENSP, CStringGetDatum(oriIndexName), ObjectIdGetDatum(get_rel_namespace(relOid)));
    if (HeapTupleIsValid(indexClassTuple))
    {
        Oid tableOid = IndexGetRelation(((Form_pg_class)GETSTRUCT(indexClassTuple))->oid, true);
        if (OidIsValid(tableOid))
        {
            if (tableOid == relOid)
            {
                ret = pstrdup(oriIndexName);
                useInternalIndexName = false;
            }
        }
    }

    if (useInternalIndexName)
    {
        ret = mysBuildInternalIndexName(relOid, oriIndexName);
    }

    return ret;
}


char *
mysBuildInternalIndexName(Oid relOid, char *oriIndexName)
{
    char *ret;
    char buf[256];

    pg_snprintf(buf, 256, "_%u_", relOid);
    if (strncmp(oriIndexName, buf, strlen(buf)) == 0)
    {
        ret = pstrdup(oriIndexName);
    }
    else
    {
        pg_snprintf(buf, 256, "_%u_%s", relOid, oriIndexName);
        ret = pstrdup(buf);
    }

    return ret;
}


char *
mysBuildCheckNameForSet(void)
{
    return pstrdup("halo_set_check");
}


char *
mysBuildSeqName(char *tableName)
{
    char *seqName;

    /* 如果表名过长则截断 */
    seqName = palloc0(NAMEDATALEN);
    pg_snprintf(seqName, NAMEDATALEN, "seq%s", tableName);

    return seqName;
}


char *
mysBuildTrigNameForAutoInc(char *tableName)
{
    char *trigName;

    /* 如果表名过长则截断 */
    trigName = palloc0(NAMEDATALEN);
    pg_snprintf(trigName, NAMEDATALEN, "ait%s", tableName);

    return trigName;
}


char *
mysBuildTrigFuncNameForAutoInc(char *tableName)
{
    char *funcName;

    /* 如果表名过长则截断 */
    funcName = palloc0(NAMEDATALEN);
    pg_snprintf(funcName, NAMEDATALEN, "aitf%s", tableName);

    return funcName;
}


char *
mysBuildTrigNameForOnUpdateNow(char *tableName, char *colName)
{
    return tailor("aut", tableName, colName);
}


char *
mysBuildTrigFuncNameForOnUpdateNow(char *tableName, char *colName)
{
    return tailor("autf", tableName, colName);
}


char *
mysBuildSetDomainName(char *tablename, char *colname)
{
    return tailor("set_", tablename, colname);
}


char *
mysBuildEnumDomainName(char *tablename, char *colname)
{
    return tailor("enum_", tablename, colname);
}


char *
mysBuildPartitionTableName(char *tableName, char *pTableName)
{
    return tailor("", tableName, pTableName);
}


static char *
tailor(char *prefix, char *firstPart, char *secondPart)
{
    char *ret;
    int prefixLen;
    int firstPartLen;
    int secondPartLen;

    ret = palloc0(NAMEDATALEN);
    prefixLen = strlen(prefix);
    firstPartLen = strlen(firstPart);
    secondPartLen = strlen(secondPart);
    if ((prefixLen + firstPartLen + secondPartLen) <= (NAMEDATALEN - 1))
    {
        pg_snprintf(ret, NAMEDATALEN, "%s%s%s", prefix, firstPart, secondPart);
    }
    else 
    {
        int firstPartAllocatedSize;
        int secondPartAllocatedSize;

        firstPartAllocatedSize = (NAMEDATALEN - 1 - prefixLen) / 3 * 2;
        secondPartAllocatedSize = (NAMEDATALEN - 1 - prefixLen) - firstPartAllocatedSize;
        if (firstPartLen < firstPartAllocatedSize)
        {
            secondPartAllocatedSize = (NAMEDATALEN - 1 - prefixLen) - firstPartLen;
            firstPartAllocatedSize = firstPartLen;
        }
        if (secondPartLen < secondPartAllocatedSize)
        {
            firstPartAllocatedSize = (NAMEDATALEN - 1 - prefixLen) - secondPartLen;
            secondPartAllocatedSize = secondPartLen;
        }

        pg_snprintf(ret, (prefixLen + 1), "%s", prefix);
        pg_snprintf((ret + prefixLen), (firstPartAllocatedSize + 1), 
                    "%s", firstPart);
        pg_snprintf((ret + prefixLen + firstPartAllocatedSize), 
                    (secondPartAllocatedSize + 1), "%s", secondPart);
    }

    return ret;
}


/*
 * 返回一个经过语法的RenameStmt("alter sequence oldSeqName rename to newSeqName")
 * 
 * Note: 如果新序列名与旧序列名相同，返回NULL
 * 
 */
static RenameStmt *
renameSeq(Oid oldSeqOid, char *namespaceName, char *relName)
{
    RenameStmt *renameStmt;
    char *oldSeqName;
    char *newSeqName;

    oldSeqName = get_rel_name(oldSeqOid);
    newSeqName = mysBuildSeqName(relName);
    if (strcmp(oldSeqName, newSeqName) == 0)
    {
        return NULL;
    }

    renameStmt = makeNode(RenameStmt);
    renameStmt->renameType = OBJECT_SEQUENCE;
    renameStmt->relation = makeRangeVar(namespaceName, oldSeqName, -1);
    renameStmt->subname = NULL;
    renameStmt->newname = pstrdup(newSeqName);
    renameStmt->missing_ok = false;

    return renameStmt;
}


/*
 * 返回一个经过语法的RenameStmt("alter trigger oldTrigName rename to newTrigName")
 * 
 * Note: 如果新序列名与旧序列名相同，返回NULL
 * 
 */
static RenameStmt *
renameTrig(Oid oldTrigOid, char *namespaceName, char *oldRelName, char *newTrigName)
{
    RenameStmt *renameStmt;
    char *oldTrigName;

    Relation trigRel;
    ScanKeyData trigKey;
    SysScanDesc trigScan;
    HeapTuple trigTuple;

    trigRel = table_open(TriggerRelationId, AccessShareLock);
    ScanKeyInit(&trigKey, Anum_pg_trigger_oid, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(oldTrigOid));
    trigScan = systable_beginscan(trigRel, TriggerOidIndexId, true,
                                    NULL, 1, &trigKey);
    trigTuple = systable_getnext(trigScan);
    if (HeapTupleIsValid(trigTuple))
    {
        Form_pg_trigger trigInfo = (Form_pg_trigger) GETSTRUCT(trigTuple);

        oldTrigName = pstrdup(trigInfo->tgname.data);
        systable_endscan(trigScan);
        table_close(trigRel, AccessShareLock);
    }
    else
    {
        systable_endscan(trigScan);
        table_close(trigRel, AccessShareLock);
        elog(ERROR, "Oid为%d的触发器不存在。", oldTrigOid);
    }

    if (strcmp(oldTrigName, newTrigName) == 0)
    {
        return NULL;
    }
    
    renameStmt = makeNode(RenameStmt);
    renameStmt->renameType = OBJECT_TRIGGER;
    renameStmt->relation = makeRangeVar(namespaceName, oldRelName, -1);
    renameStmt->subname = pstrdup(oldTrigName);
    renameStmt->newname = pstrdup(newTrigName);
    renameStmt->missing_ok = false;

    return renameStmt;
}


/*
 * 返回一个经过语法的RenameStmt("alter function oldFuncName rename to newFuncName")
 * 
 * Note: 如果新序列名与旧序列名相同，返回NULL
 * 
 */
static RenameStmt *
renameFunc(Oid oldFuncOid, char *namespaceName, char *newFuncName)
{
    RenameStmt *renameStmt;
    char *oldFuncName;

    HeapTuple funcTuple;
    ObjectWithArgs *renameObject;

    funcTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(oldFuncOid));
    if (HeapTupleIsValid(funcTuple))
    {
        Form_pg_proc funcInfo = (Form_pg_proc) GETSTRUCT(funcTuple);
        oldFuncName = pstrdup(NameStr(funcInfo->proname));
        ReleaseSysCache(funcTuple);
    }
    else
    {
        elog(ERROR, "Oid为%d的函数不存在。", oldFuncOid);
    }

    if (strcmp(oldFuncName, newFuncName) == 0)
    {
        return NULL;
    }
    
    renameObject = makeNode(ObjectWithArgs);
    renameObject->objname = list_make2(makeString(namespaceName), 
                                       makeString(oldFuncName));
    renameObject->args_unspecified = true;
    renameStmt = makeNode(RenameStmt);
    renameStmt->renameType = OBJECT_FUNCTION;
    renameStmt->object = (Node *)renameObject;
    renameStmt->newname = pstrdup(newFuncName);
    renameStmt->missing_ok = false;

    return renameStmt;
}


static Oid
mysGetColumnAutoIncTrig(Relation rel, Oid seqOid, char *colName)
{
    Relation depRel;
    ScanKeyData depKeys[2];
    SysScanDesc depScan;
    HeapTuple depTuple;
    char *oldTrigName;
    Oid autoIncTrigOid = InvalidOid;

    oldTrigName = mysBuildTrigNameForAutoInc(RelationGetRelationName(rel));

    if (!SearchSysCacheExistsAttName(rel->rd_id, colName))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(seqOid)))
    {
        elog(ERROR, "Sequence don't exist.");
    }

    depRel = table_open(DependRelationId, AccessShareLock);

    ScanKeyInit(&depKeys[0],
                    Anum_pg_depend_refclassid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationRelationId));
    ScanKeyInit(&depKeys[1],
                Anum_pg_depend_refobjid,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(seqOid));

    depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
                                 NULL, 2, depKeys);
        
    while (HeapTupleIsValid(depTuple = systable_getnext(depScan)))
    {
        Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

        if (foundDep->classid == TriggerRelationId &&
            foundDep->objsubid == 0 &&
            foundDep->deptype == DEPENDENCY_INTERNAL)
        {
            autoIncTrigOid = foundDep->objid;
            break;
        }
    }
    systable_endscan(depScan);

    table_close(depRel, AccessShareLock);

    if (!OidIsValid(autoIncTrigOid))
    {
        autoIncTrigOid = get_trigger_oid(rel->rd_id, oldTrigName, true);

        if (OidIsValid(autoIncTrigOid))
        {
            ObjectAddress objAddr;
            ObjectAddress refObjAddr;

            objAddr.classId = TriggerRelationId;
            objAddr.objectId = autoIncTrigOid;
            objAddr.objectSubId = 0;

            refObjAddr.classId = RelationRelationId;
            refObjAddr.objectId = seqOid;
            refObjAddr.objectSubId = 0;

            recordDependencyOn(&objAddr, &refObjAddr, DEPENDENCY_INTERNAL);

            CommandCounterIncrement();
        }
    }

    if (OidIsValid(autoIncTrigOid))
    {
        char *trigName = get_trigger_name(autoIncTrigOid);

        if (strcmp(oldTrigName, trigName) != 0)
        {
            elog(NOTICE, "实现auto increment的触发器不符合命名规则.");
        }
    }
    else
    {
        // 不应该发生
        elog(ERROR, "Halo中自增字段必须有相应的触发器.");
    }

    return autoIncTrigOid;
}


static Oid
mysGetColumnAutoIncFunc(Relation rel, Oid autoIncTrigOid, char *colName)
{
    Relation depRel;
    ScanKeyData depKeys[2];
    SysScanDesc depScan;
    HeapTuple depTuple;
    char *oldFuncName;
    Oid autoIncFuncOid = InvalidOid;

    if (!OidIsValid(autoIncTrigOid))
        return InvalidOid;

    oldFuncName = mysBuildTrigFuncNameForAutoInc(RelationGetRelationName(rel));

    if (!SearchSysCacheExistsAttName(rel->rd_id, colName))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    depRel = table_open(DependRelationId, AccessShareLock);

    ScanKeyInit(&depKeys[0],
                Anum_pg_depend_refclassid,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(TriggerRelationId));
    ScanKeyInit(&depKeys[1],
                Anum_pg_depend_refobjid,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(autoIncTrigOid));
    
    depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
                                    NULL, 2, depKeys);
    
    while (HeapTupleIsValid(depTuple = systable_getnext(depScan)))
    {
        Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

        if (foundDep->classid == ProcedureRelationId &&
            foundDep->objsubid == 0 &&
            foundDep->deptype == DEPENDENCY_AUTO)
        {
            autoIncFuncOid = foundDep->objid;
            break;
        }
    }
    systable_endscan(depScan);

    table_close(depRel, AccessShareLock);

    if (!OidIsValid(autoIncFuncOid))
    {
        int i = 0;
        for (i = 0; i < rel->trigdesc->numtriggers; i++)
        {
            Trigger trig = rel->trigdesc->triggers[i];
            if (trig.tgoid == autoIncTrigOid)
            {
                autoIncFuncOid = trig.tgfoid;
                break;
            }
        }

        if (OidIsValid(autoIncFuncOid))
        {
            ObjectAddress objAddr;
            ObjectAddress refObjAddr;

            objAddr.classId = ProcedureRelationId;
            objAddr.objectId = autoIncFuncOid;
            objAddr.objectSubId = 0;

            refObjAddr.classId = TriggerRelationId;
            refObjAddr.objectId = autoIncTrigOid;
            refObjAddr.objectSubId = 0;

            recordDependencyOn(&objAddr, &refObjAddr, DEPENDENCY_AUTO);

            CommandCounterIncrement();
        }
    }

    if (OidIsValid(autoIncFuncOid))
    {
        char *funcName = get_func_name(autoIncFuncOid);
        if (strcmp(oldFuncName, funcName) != 0)
        {
            elog(NOTICE, "实现on update now的函数不符合命名规则.");
        }
    }
    else
    {
        // 不应该发生
    }

    return autoIncFuncOid;
}


Oid
mysGetColumnOnUpdateNowTrig(Relation rel, char *colName)
{
    HeapTuple attTuple;
    int attnum;
    char *oldTrigName;
    Oid onUpdateNowTrigOid = InvalidOid;
    Oid oriTypeOid = InvalidOid;
    Oid baseTypeOid = InvalidOid;

    oldTrigName = mysBuildTrigNameForOnUpdateNow(RelationGetRelationName(rel), colName);

    attTuple = SearchSysCacheCopyAttName(rel->rd_id, colName);
    if (!HeapTupleIsValid(attTuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    attnum = ((Form_pg_attribute)GETSTRUCT(attTuple))->attnum;
    oriTypeOid = ((Form_pg_attribute)GETSTRUCT(attTuple))->atttypid;
    baseTypeOid = getBaseType(oriTypeOid);

    if (baseTypeOid == TIMESTAMPOID || baseTypeOid == TIMESTAMPTZOID)
    {
        Relation depRel;
        ScanKeyData depKeys[3];
        SysScanDesc depScan;
        HeapTuple depTuple;

        depRel = table_open(DependRelationId, AccessShareLock);

        ScanKeyInit(&depKeys[0],
                    Anum_pg_depend_refclassid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationRelationId));
        ScanKeyInit(&depKeys[1],
                    Anum_pg_depend_refobjid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationGetRelid(rel)));
        ScanKeyInit(&depKeys[2],
                    Anum_pg_depend_refobjsubid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(attnum));
        
        depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
                                     NULL, 3, depKeys);
        
        while (HeapTupleIsValid(depTuple = systable_getnext(depScan)))
        {
            Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

            if (foundDep->classid == TriggerRelationId &&
                foundDep->objsubid == 0 &&
                foundDep->deptype == DEPENDENCY_INTERNAL)
            {
                onUpdateNowTrigOid = foundDep->objid;
                break;
            }
        }
        systable_endscan(depScan);

        table_close(depRel, AccessShareLock);

        if (!OidIsValid(onUpdateNowTrigOid))
        {
            onUpdateNowTrigOid = get_trigger_oid(rel->rd_id, oldTrigName, true);

            if (OidIsValid(onUpdateNowTrigOid))
            {
                ObjectAddress objAddr;
	            ObjectAddress refObjAddr;

                objAddr.classId = TriggerRelationId;
                objAddr.objectId = onUpdateNowTrigOid;
                objAddr.objectSubId = 0;

                refObjAddr.classId = RelationRelationId;
                refObjAddr.objectId = RelationGetRelid(rel);
                refObjAddr.objectSubId = attnum;

                recordDependencyOn(&objAddr, &refObjAddr, DEPENDENCY_INTERNAL);

                CommandCounterIncrement();
            }
        }

        if (OidIsValid(onUpdateNowTrigOid))
        {
            char *trigName = get_trigger_name(onUpdateNowTrigOid);

            if (strcmp(oldTrigName, trigName) != 0)
            {
                elog(NOTICE, "实现on update now的触发器不符合命名规则.");
            }
        }
    }

    return onUpdateNowTrigOid;
}


static Oid
mysGetColumnOnUpdateNowFunc(Relation rel, Oid onUpdateNowTrig, char *colName)
{
    HeapTuple attTuple;
    char *oldFuncName;
    Oid onUpdateNowFuncOid = InvalidOid;
    Oid oriTypeOid = InvalidOid;
    Oid baseTypeOid = InvalidOid;

    if (!OidIsValid(onUpdateNowTrig))
        return InvalidOid;

    oldFuncName = mysBuildTrigFuncNameForOnUpdateNow(RelationGetRelationName(rel), 
                                                     colName);

    attTuple = SearchSysCacheCopyAttName(rel->rd_id, colName);
    if (!HeapTupleIsValid(attTuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    oriTypeOid = ((Form_pg_attribute)GETSTRUCT(attTuple))->atttypid;
    baseTypeOid = getBaseType(oriTypeOid);

    if (baseTypeOid == TIMESTAMPOID || baseTypeOid == TIMESTAMPTZOID)
    {
        Relation depRel;
        ScanKeyData depKeys[2];
        SysScanDesc depScan;
        HeapTuple depTuple;

        depRel = table_open(DependRelationId, AccessShareLock);

        ScanKeyInit(&depKeys[0],
                    Anum_pg_depend_refclassid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(TriggerRelationId));
        ScanKeyInit(&depKeys[1],
                    Anum_pg_depend_refobjid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(onUpdateNowTrig));
        
        depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
                                     NULL, 2, depKeys);
        
        while (HeapTupleIsValid(depTuple = systable_getnext(depScan)))
        {
            Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

            if (foundDep->classid == ProcedureRelationId &&
                foundDep->objsubid == 0 &&
                foundDep->deptype == DEPENDENCY_AUTO)
            {
                onUpdateNowFuncOid = foundDep->objid;
                break;
            }
        }
        systable_endscan(depScan);

        table_close(depRel, AccessShareLock);

        if (!OidIsValid(onUpdateNowFuncOid))
        {
            int i = 0;
            for (i = 0; i < rel->trigdesc->numtriggers; i++)
            {
                Trigger trig = rel->trigdesc->triggers[i];
                if (trig.tgoid == onUpdateNowTrig)
                {
                    onUpdateNowFuncOid = trig.tgfoid;
                    break;
                }
            }

            if (OidIsValid(onUpdateNowFuncOid))
            {
                ObjectAddress objAddr;
	            ObjectAddress refObjAddr;

                objAddr.classId = ProcedureRelationId;
                objAddr.objectId = onUpdateNowFuncOid;
                objAddr.objectSubId = 0;

                refObjAddr.classId = TriggerRelationId;
                refObjAddr.objectId = onUpdateNowTrig;
                refObjAddr.objectSubId = 0;

                recordDependencyOn(&objAddr, &refObjAddr, DEPENDENCY_AUTO);

                CommandCounterIncrement();
            }
        }

        if (OidIsValid(onUpdateNowFuncOid))
        {
            char *funcName = get_func_name(onUpdateNowFuncOid);
            if (strcmp(oldFuncName, funcName) != 0)
            {
                elog(NOTICE, "实现on update now的函数不符合命名规则.");
            }
        }
        else
        {
            // 不应该发生
        }
    }

    return onUpdateNowFuncOid;
}


void
mysProcessAutoIncForRenameAtt(Relation targetRel, char *oldColName, 
                              char *newColName, List **stmts)
{
    Oid autoIncSeqOid;

    if (OidIsValid(autoIncSeqOid = getColumnDefaultSeq(targetRel, oldColName)))
    {        
        RenameStmt *renameSeqStmt;
        RenameStmt *renameTrigStmt;
        RenameStmt *renameFunStmt;
        CreateFunctionStmt *createFuncStmt;
        AlterObjectDependsStmt *bindStmt;
        char *namespaceName;
        char *relName;
        RangeVar *rangeVar;
        char *newTrigName;
        char *newFuncName;
        Oid oldTrigOid;
        Oid oldFuncOid;

        namespaceName = get_namespace_name(RelationGetNamespace(targetRel));
        relName = RelationGetRelationName(targetRel);

        renameSeqStmt = renameSeq(autoIncSeqOid, namespaceName, relName);
        if (renameSeqStmt != NULL)
            *stmts = lappend(*stmts, renameSeqStmt);

        oldTrigOid = mysGetColumnAutoIncTrig(targetRel, autoIncSeqOid, oldColName);
        newTrigName = mysBuildTrigNameForAutoInc(relName);
        renameTrigStmt = renameTrig(oldTrigOid, namespaceName, relName, newTrigName);
        if (renameTrigStmt != NULL)
        {
            *stmts = lappend(*stmts, renameTrigStmt);
        }
        
        oldFuncOid = mysGetColumnAutoIncFunc(targetRel, oldTrigOid, oldColName);
        newFuncName = mysBuildTrigFuncNameForAutoInc(relName);
        renameFunStmt = renameFunc(oldFuncOid, namespaceName, newFuncName);
        if (renameFunStmt != NULL)
            *stmts = lappend(*stmts, renameFunStmt);
        
        createFuncStmt = createAutoIncrementTriggerFunc(namespaceName, 
                                                        relName, 
                                                        newColName);
        *stmts = lappend(*stmts, createFuncStmt);

        rangeVar = makeRangeVar(namespaceName, relName, -1);
        bindStmt = bindTriggerFunctionToTrigger(createFuncStmt->funcname, 
                                                rangeVar,
                                                newTrigName);
        *stmts = lappend(*stmts, bindStmt);
    }
}


void
mysProcessSetEnumForRenameAtt(Relation targetRel, char *oldColName, 
                              char *newColName, List **stmts)
{
    char *namespaceName;
    char *tableName;
    HeapTuple attTuple;
    Form_pg_attribute attInfo;
    HeapTuple typTuple;
    char *oldTypeName;
    char *newTypeName;
    RenameStmt *renameStmt;

    namespaceName = get_namespace_name(RelationGetNamespace(targetRel));
    tableName = RelationGetRelationName(targetRel);
    attTuple = SearchSysCacheCopyAttName(targetRel->rd_id, oldColName);

    if (!HeapTupleIsValid(attTuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        oldColName, RelationGetRelationName(targetRel))));
    }

    attInfo = (Form_pg_attribute) GETSTRUCT(attTuple);
    typTuple = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(attInfo->atttypid));
    oldTypeName = NameStr(((Form_pg_type)GETSTRUCT(typTuple))->typname);

    if (strncasecmp(oldTypeName, "set_", 4) == 0)
    {
        newTypeName = mysBuildSetDomainName(tableName, newColName);
        renameStmt = makeNode(RenameStmt);
        renameStmt->renameType = OBJECT_DOMAIN;
        renameStmt->object = (Node*)list_make2(makeString(namespaceName), 
                                               makeString(pstrdup(oldTypeName)));
        renameStmt->subname = NULL;
        renameStmt->newname = pstrdup(newTypeName);
        renameStmt->missing_ok = false;
        *stmts = lappend(*stmts, renameStmt);
    }
    else if (strncasecmp(oldTypeName, "enum_", 5) == 0)
    {
        newTypeName = mysBuildEnumDomainName(tableName, newColName);
        renameStmt = makeNode(RenameStmt);
        renameStmt->renameType = OBJECT_TYPE;
        renameStmt->object = (Node*)list_make2(makeString(namespaceName), 
                                               makeString(pstrdup(oldTypeName)));
        renameStmt->subname = NULL;
        renameStmt->newname = pstrdup(newTypeName);
        renameStmt->missing_ok = false;
        *stmts = lappend(*stmts, renameStmt);
    }
    else 
    {
        // do nothing;
    }
}


static void
mysProcessOnUpdateNowForRenameAtt(Relation targetRel, char *oldColName, 
                                  char *newColName, List **stmts)
{
    Oid onUpdateNowTrigOid;

    onUpdateNowTrigOid = mysGetColumnOnUpdateNowTrig(targetRel, oldColName);
    if (OidIsValid(onUpdateNowTrigOid))
    {
        RenameStmt *renameTrigStmt;
        RenameStmt *renameFuncStmt;
        CreateFunctionStmt *createFuncStmt;
        AlterObjectDependsStmt *bindStmt;
        char *namespaceName;
        char *relName;
        RangeVar *rangeVar;
        char *newTrigName;
        char *newFuncName;
        Oid onUpdateNowFuncOid;

        namespaceName = get_namespace_name(RelationGetNamespace(targetRel));
        relName = NameStr(targetRel->rd_rel->relname);

        onUpdateNowFuncOid = mysGetColumnOnUpdateNowFunc(targetRel, 
                                                         onUpdateNowTrigOid, 
                                                         oldColName);
        Assert(OidIsValid(onUpdateNowFuncOid));

        newTrigName = mysBuildTrigNameForOnUpdateNow(relName, newColName);
        renameTrigStmt = renameTrig(onUpdateNowTrigOid, namespaceName, 
                                    relName, newTrigName);
        if (renameTrigStmt != NULL)
            *stmts = lappend(*stmts, renameTrigStmt);
        
        newFuncName = mysBuildTrigFuncNameForOnUpdateNow(relName, newColName);
        renameFuncStmt = renameFunc(onUpdateNowFuncOid, namespaceName, newFuncName);
        if (renameFuncStmt != NULL)
            *stmts = lappend(*stmts, renameFuncStmt);
        
        createFuncStmt = createAutoUpdateTimeStampTriggerFunc(namespaceName, 
                                                              relName, 
                                                              newColName);
        *stmts = lappend(*stmts, createFuncStmt);

        // create or replace function会删掉原有同名函数的依赖关系
        rangeVar = makeRangeVar(namespaceName, relName, -1);
        bindStmt = bindTriggerFunctionToTrigger(createFuncStmt->funcname, 
                                                rangeVar,
                                                newTrigName);
        *stmts = lappend(*stmts, bindStmt);
    }
}


static List *
mysPreProcessRenameAtt(RenameStmt *renameStmt)
{
    List *stmts;
    char *oldColName;
    char *newColName;
    Oid targetRelOid;
    Relation targetRel;

    stmts = NIL;
    oldColName = renameStmt->subname;
    newColName = renameStmt->newname;

    if (strcmp(oldColName, newColName) == 0)
    {
        return NIL;
    }

    targetRelOid = RangeVarGetRelidExtended(renameStmt->relation, NoLock,
                                            renameStmt->missing_ok ? RVR_MISSING_OK : 0,
                                            RangeVarCallbackForRenameAttribute,
                                            NULL);
    
    targetRel = table_open(targetRelOid, AccessShareLock);

    mysProcessAutoIncForRenameAtt(targetRel, oldColName, newColName, &stmts);

    mysProcessSetEnumForRenameAtt(targetRel, oldColName, newColName, &stmts);

    mysProcessOnUpdateNowForRenameAtt(targetRel, oldColName, newColName, &stmts);

    table_close(targetRel, AccessShareLock);

    return stmts;
}


static void
mysProcessAutoIncForRenameRelation(Relation targetRel, char *oldTableName, 
                                   char *newTableName, List **stmts)
{
    AttrNumber autoIncColNum;

    if ((autoIncColNum = mysGetTableAutoIncColNum(targetRel)) > 0)
    {
        char *autoIncColName;
        Oid autoIncSeqOid;

        autoIncColName = get_attname(targetRel->rd_id, autoIncColNum, false);
        if (OidIsValid(autoIncSeqOid = getColumnDefaultSeq(targetRel, 
                                                           autoIncColName)))
        {        
            RenameStmt *renameSeqStmt;
            RenameStmt *renameTrigStmt;
            RenameStmt *renameFuncStmt;
            CreateFunctionStmt *createFuncStmt;
            AlterObjectDependsStmt *bindStmt;
            char *namespaceName;
            RangeVar *rangeVar;
            char *newTrigName;
            char *newFuncName;
            Oid oldTriggerOid;
            Oid oldFuncOid;

            namespaceName = get_namespace_name(RelationGetNamespace(targetRel));

            renameSeqStmt = renameSeq(autoIncSeqOid, namespaceName, newTableName);
            if (renameSeqStmt != NULL)
                *stmts = lappend(*stmts, renameSeqStmt);

            oldTriggerOid = mysGetColumnAutoIncTrig(targetRel, autoIncSeqOid, 
                                                    autoIncColName);
            newTrigName = mysBuildTrigNameForAutoInc(newTableName);
            renameTrigStmt = renameTrig(oldTriggerOid, namespaceName, 
                                        oldTableName, newTrigName);
            if (renameTrigStmt != NULL)
                *stmts = lappend(*stmts, renameTrigStmt);
            
            oldFuncOid = mysGetColumnAutoIncFunc(targetRel, oldTriggerOid, 
                                                 autoIncColName);
            newFuncName = mysBuildTrigFuncNameForAutoInc(newTableName);
            renameFuncStmt = renameFunc(oldFuncOid, namespaceName, newFuncName);
            if (renameFuncStmt != NULL)
                *stmts = lappend(*stmts, renameFuncStmt);
            
            createFuncStmt = createAutoIncrementTriggerFunc(namespaceName, 
                                                            newTableName, 
                                                            autoIncColName);
            *stmts = lappend(*stmts, createFuncStmt);

            rangeVar = makeRangeVar(namespaceName, oldTableName, -1);
            bindStmt = bindTriggerFunctionToTrigger(createFuncStmt->funcname, 
                                                    rangeVar,
                                                    newTrigName);
            *stmts = lappend(*stmts, bindStmt);
        }
        else
        {
            elog(ERROR, "internal error in mysProcessAutoIncForRenameRelation.");
        }
    }
}


static void
mysProcessSetEnumForRenameRelation(Relation targetRel, char *oldTableName, 
                                   char *newTableName, List **stmts)
{
    char *namespaceName;
    namespaceName = get_namespace_name(RelationGetNamespace(targetRel));
    for (int i = 1; i < targetRel->rd_att->natts; i++)
    {
        RenameStmt *renameStmt;
        char *newTypeName;
        char* oldTypeName;
        char* attName;
        Form_pg_attribute attrForm = &(targetRel->rd_att->attrs[i]);
		HeapTuple heapTuple = 
            SearchSysCacheCopy1(TYPEOID, 
                  ObjectIdGetDatum(attrForm->atttypid));
        Form_pg_type formPgType = (Form_pg_type)GETSTRUCT(heapTuple);
        oldTypeName = NameStr(formPgType->typname);
        attName = NameStr(attrForm->attname);

        if (strncasecmp(oldTypeName, "set_", 4) == 0)
        {
            newTypeName = mysBuildSetDomainName(newTableName, attName);
            renameStmt = makeNode(RenameStmt);
            renameStmt->renameType = OBJECT_DOMAIN;
            renameStmt->object = 
                (Node*)list_make2(makeString(pstrdup(namespaceName)), 
                                  makeString(pstrdup(oldTypeName)));
            renameStmt->subname = NULL;
            renameStmt->newname = pstrdup(newTypeName);
            renameStmt->missing_ok = false;
            *stmts = lappend(*stmts, renameStmt);
        }
        else if (strncasecmp(oldTypeName, "enum_", 5) == 0)
        {
            newTypeName = mysBuildEnumDomainName(newTableName, attName);
            renameStmt = makeNode(RenameStmt);
            renameStmt->renameType = OBJECT_TYPE;
            renameStmt->object = 
                (Node*)list_make2(makeString(pstrdup(namespaceName)), 
                                  makeString(pstrdup(oldTypeName)));
            renameStmt->subname = NULL;
            renameStmt->newname = pstrdup(newTypeName);
            renameStmt->missing_ok = false;
            *stmts = lappend(*stmts, renameStmt);
        }
        else 
        {
            // do nothing;
        }
    }
}


static void
mysProcessOnUpdateNowColForRenameRelation(Relation targetRel, 
                                          char *oldTableName, 
                                          char *newTableName, 
                                          char *colName, 
                                          List **stmts)
{
    Oid onUpdateNowTrigOid;

    onUpdateNowTrigOid = mysGetColumnOnUpdateNowTrig(targetRel, colName);
    if (OidIsValid(onUpdateNowTrigOid))
    {
        RenameStmt *renameTrigStmt;
        RenameStmt *renameFuncStmt;
        Oid onUpdateNowFuncOid;
        char *namespaceName;
        char *newTrigName;
        char *newFuncName;

        onUpdateNowFuncOid = mysGetColumnOnUpdateNowFunc(targetRel, 
                                                         onUpdateNowTrigOid, 
                                                         colName);
        Assert(OidIsValid(onUpdateNowFuncOid));

        namespaceName = get_namespace_name(RelationGetNamespace(targetRel));

        newTrigName = mysBuildTrigNameForOnUpdateNow(newTableName, colName);
        renameTrigStmt = renameTrig(onUpdateNowTrigOid, namespaceName, 
                                    oldTableName, newTrigName);
        if (renameTrigStmt != NULL)
            *stmts = lappend(*stmts, renameTrigStmt);
        
        newFuncName = mysBuildTrigFuncNameForOnUpdateNow(newTableName, colName);
        renameFuncStmt = renameFunc(onUpdateNowFuncOid, namespaceName, newFuncName);
        if (renameFuncStmt != NULL)
            *stmts = lappend(*stmts, renameFuncStmt);
        
        // stmt = rebuildFuncForOnUpdateNow(get_func_namespace(onUpdateNowFuncOid), newFuncName, newColName);
        // *stmts = lappend(*stmts, stmt);

        // create or replace function会删掉原有同名函数的依赖关系
        // stmt = rebuildFuncDependForOnUpdateNow(targetRel, newTrigName, get_namespace_name(get_func_namespace(onUpdateNowFuncOid)), newFuncName);
        // *stmts = lappend(*stmts, stmt);
    }
}


static void
mysProcessOnUpdateNowColsForRenameRelation(Relation targetRel, 
                                           char *oldTableName, 
                                           char *newTableName, 
                                           List **stmts)
{
    List *onUpdateNowColsNum = NIL;

    if ((onUpdateNowColsNum = getTableOnUpdateNowColsNum(targetRel)) != NIL)
    {
        ListCell *lc;

        foreach(lc, onUpdateNowColsNum)
        {
            AttrNumber attNum = lfirst_int(lc);
            char *attName = get_attname(targetRel->rd_id, attNum, false);

            mysProcessOnUpdateNowColForRenameRelation(targetRel, 
                                                      oldTableName, 
                                                      newTableName, 
                                                      attName, 
                                                      stmts);
        }
    }
}


static List *
mysPreProcessRenameTable(RenameStmt *renameStmt)
{
    List *stmts;
    char *oldTableName;
    char *oldTableNamespace;
    char *newTableName;
    char *newTableNamespace;
    RangeVar *newTable;
    Oid targetRelOid;
    Relation targetRel;

    stmts = NIL;

    targetRelOid = RangeVarGetRelidExtended(renameStmt->relation, NoLock,
                                            renameStmt->missing_ok ? RVR_MISSING_OK : 0,
                                            RangeVarCallbackForRenameAttribute,
                                            NULL);
    
    targetRel = table_open(targetRelOid, AccessShareLock);

    oldTableName = pstrdup(RelationGetRelationName(targetRel));
    oldTableNamespace = get_namespace_name(RelationGetNamespace(targetRel));
    newTable = (RangeVar *)(renameStmt->object);
    newTableName = newTable->relname;
    newTableNamespace = newTable->schemaname;

    if (newTableNamespace != NULL)
    {
        if (strcmp(oldTableNamespace, newTableNamespace) != 0)
        {
            table_close(targetRel, AccessShareLock);
            elog(ERROR, "表%s.%s将被数据库%s所拥有，拒绝修改表名。",
                 oldTableNamespace,
                 oldTableName,
                 newTableNamespace);
        }
    }
    else
    {
        Oid currentSchemaOid = linitial_oid(fetch_search_path(false));

        if (RelationGetNamespace(targetRel) != currentSchemaOid)
        {
            table_close(targetRel, AccessShareLock);
            elog(ERROR, "表%s.%s将被数据库%s所拥有，拒绝修改表名。",
                 oldTableNamespace,
                 oldTableName,
                 get_namespace_name(currentSchemaOid));
        }
    }
    
    // 供后续实际的修改表名使用
    renameStmt->object = NULL;
    renameStmt->newname = newTableName;

    if (strcmp(oldTableName, newTableName) == 0)
    {
        table_close(targetRel, AccessShareLock);
        return NIL;
    }

    mysProcessAutoIncForRenameRelation(targetRel, oldTableName, newTableName, &stmts);

    mysProcessSetEnumForRenameRelation(targetRel, oldTableName, newTableName, &stmts);

    mysProcessOnUpdateNowColsForRenameRelation(targetRel, oldTableName, newTableName, &stmts);

    table_close(targetRel, AccessShareLock);

    return stmts;
}


static List *
mysPreProcessRenameIndex(RenameStmt *renameStmt)
{
    Oid relOid = RangeVarGetRelidExtended(renameStmt->relation, NoLock,
										  0,
										  RangeVarCallbackOwnsRelation,
										  NULL);
    char *oriIndexName = renameStmt->subname;

    renameStmt->relation->relname = mysCheckIndexName(relOid, oriIndexName);
    renameStmt->subname = NULL;
    renameStmt->newname = mysBuildInternalIndexName(relOid, renameStmt->newname);

    return NIL;
}


void
mysPreProcessDropStmt(DropStmt *dropStmt)
{
	if (dropStmt->removeType == OBJECT_INDEX)
	{
        ListCell   *lc;
        foreach(lc, dropStmt->objects)
        {
            char *newIndexName = NULL;
            Oid relOid;
            Oid schemaOid;
            char *schemaName = NULL;
            char *tableName = NULL;
            char *indexName = NULL;
            List *names = (List *)lfirst(lc);
            if (list_length(names) == 2)
            {
                tableName = pstrdup(strVal(linitial(names)));
                indexName = pstrdup(strVal(lsecond(names)));
            }
            else    
            {
                schemaName = pstrdup(strVal(linitial(names)));
                tableName = pstrdup(strVal(lsecond(names)));
                indexName = pstrdup(strVal(lthird(names)));
            }

            if (schemaName == NULL)
            {
                schemaOid = getCurrentNamespaceOid();
            }
            else 
            {
                schemaOid = get_namespace_oid(schemaName, false);
            }
            relOid = get_relname_relid(tableName, schemaOid);

            newIndexName = mysBuildInternalIndexName(relOid, indexName);
            if (list_length(names) == 2)
            {
                names = list_make1(makeString(newIndexName));
            }
            else    
            {
                schemaName = getCurrentNamespcaeName();
                names = list_make2(makeString(pstrdup(schemaName)), 
                                   makeString(pstrdup(newIndexName)));
            }
            dropStmt->objects = list_make1(names);

            break;
        }
    }
}


List *
mysPreProcessRenameStmt(RenameStmt *renameStmt)
{
    switch (renameStmt->renameType)
	{
        case OBJECT_INDEX:
            return mysPreProcessRenameIndex(renameStmt);
        case OBJECT_TABLE:
            return mysPreProcessRenameTable(renameStmt);
        case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:
            return mysPreProcessRenameAtt(renameStmt);
        default:
            return NIL;
    }
}


List *
mysPreProcessCreateViewStmt(ViewStmt *viewStmt)
{
    if (!viewStmt->replace)
    {
        return NULL;
    }
    else 
    {
        List *stmts = NIL;
        DropStmt *dropViewStmt = makeNode(DropStmt);
        dropViewStmt->objects = lappend(dropViewStmt->objects, 
                                        list_make1(makeString(viewStmt->view->relname)));
        dropViewStmt->removeType = OBJECT_VIEW;
        dropViewStmt->behavior = DROP_RESTRICT;
        dropViewStmt->missing_ok = true;
        dropViewStmt->concurrent = false;
        stmts = lappend(stmts, dropViewStmt);

        viewStmt->replace = false;

        return stmts;
    }
}

