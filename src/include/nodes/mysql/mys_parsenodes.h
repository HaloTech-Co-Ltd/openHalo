/*-------------------------------------------------------------------------
 *
 * mys_parsenodes.h
 *		Definitions for the "mys" parser (flex and bison phases only)
 *
 * Portions Copyright (c) 2021, Jet C.X. ZHANG
 *
 * src/include/parser/mysql/mys_parsenodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_PARSENODES_H
#define MYS_PARSENODES_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"


typedef struct UserVarRef
{
    NodeTag type;
    char *userVarName;
    int location;
} UserVarRef;


typedef struct UserVarAssign
{
    NodeTag type;
    char *userVarName;
    Node *expr;
    int location;
} UserVarAssign;


typedef struct SysVarRef
{
    NodeTag type;
    char *sysVarName;
    int location;
} SysVarRef;


typedef struct MysVariableSetStmt
{
    NodeTag type;
    Node *varSetStmt;   /* 目前是SelectStmt */
} MysVariableSetStmt;


typedef struct MysSelectIntoStmt
{
    NodeTag type;
    List *target;
    Node *expr;   /* 目前是SelectStmt */
} MysSelectIntoStmt;


#endif							/* MYS_PARSENODES_H */
