/*-------------------------------------------------------------------------
 *
 * parse_expr.h
 *	  handle expressions in parser
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_expr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_EXPR_H
#define PARSE_EXPR_H

#include "parser/parse_node.h"

/* GUC parameters */
extern bool Transform_null_equals;

extern Node *transformExpr(ParseState *pstate, Node *expr, ParseExprKind exprKind);

extern const char *ParseExprKindName(ParseExprKind exprKind);


extern Node *standard_transformExpr(ParseState *pstate, Node *expr, ParseExprKind exprKind);
extern Node *finalTransformColumnRef(ParseState *pstate, ColumnRef *cref);

#endif							/* PARSE_EXPR_H */
