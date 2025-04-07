
/*-------------------------------------------------------------------------
 *
 * mys_parser.c
 *	  Main entry point/driver for MySQL COMPATIBLE grammar
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
 *	  src/backend/parser/mysql/mys_parser.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "parser/parsereng.h"
#include "parser/scansup.h"
#include "parser/mysql/mys_gramparse.h"
#include "parser/mysql/mys_parser.h"
#include "parser/mysql/mys_scanner.h"
#include "parser/mysql/mys_analyze.h"
#include "parser/mysql/mys_parse_clause.h"
#include "parser/mysql/mys_parse_expr.h"
#include "parser/mysql/mys_parse_func.h"
#include "parser/mysql/mys_parse_utilcmd.h"

const ParserRoutine *GetMysParserEngine(void);	/* Get MySQL Parser Engine */

static List *mys_raw_parser(const char *str, RawParseMode mode);
static bool check_uescapechar(unsigned char escape);
static unsigned int hexval(unsigned char c);
static void check_unicode_value(pg_wchar c);
static char *str_udeescape(const char *str, char escape,
						   int position, core_yyscan_t yyscanner);
static const ParserRoutine mys_parser_engine;

/* save previous token */
static int prev_token;

int
mys_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner)
{
	mys_yy_extra_type *yyextra = mys_yyget_extra(yyscanner);
	int			cur_token;
	int			next_token;
	int			cur_token_length;
	YYLTYPE		cur_yylloc;

	/* Get next token --- we might already have it */
	if (yyextra->have_lookahead)
	{
		cur_token = yyextra->lookahead_token;
		lvalp->core_yystype = yyextra->lookahead_yylval;
		*llocp = yyextra->lookahead_yylloc;
		if (yyextra->lookahead_end)
			*(yyextra->lookahead_end) = yyextra->lookahead_hold_char;
		yyextra->have_lookahead = false;
	}
	else
		cur_token = mys_core_yylex(&(lvalp->core_yystype), llocp, yyscanner);

    /*
     * If this token isn't one that requires lookahead, just return it.  If it
	 * does, determine the token length.  (We could get that via strlen(), but
	 * since we have such a small set of possibilities, hardwiring seems
	 * feasible and more efficient --- at least for the fixed-length cases.)
	 */
	switch (cur_token)
	{
		case NOT:
			cur_token_length = 3;
			break;
		case NULLS_P:
			cur_token_length = 5;
			break;
		case WITH:
			cur_token_length = 4;
			break;
		case UIDENT:
		case USCONST:
			cur_token_length = strlen(yyextra->core_yy_extra.scanbuf + *llocp);
			break;
		case BINARY:
		    cur_token_length = 6;
			break;

		default:
			prev_token = cur_token;
			return cur_token;
	}

	/*
	 * Identify end+1 of current token.  mys_core_yylex() has temporarily stored a
	 * '\0' here, and will undo that when we call it again.  We need to redo
	 * it to fully revert the lookahead call for error reporting purposes.
	 */
	yyextra->lookahead_end = yyextra->core_yy_extra.scanbuf +
		*llocp + cur_token_length;
	Assert(*(yyextra->lookahead_end) == '\0');

	/*
	 * Save and restore *llocp around the call.  It might look like we could
	 * avoid this by just passing &lookahead_yylloc to mys_core_yylex(), but that
	 * does not work because flex actually holds onto the last-passed pointer
	 * internally, and will use that for error reporting.  We need any error
	 * reports to point to the current token, not the next one.
	 */
	cur_yylloc = *llocp;

	/* Get next token, saving outputs into lookahead variables */
	next_token = mys_core_yylex(&(yyextra->lookahead_yylval), llocp, yyscanner);
	yyextra->lookahead_token = next_token;
	yyextra->lookahead_yylloc = *llocp;

	*llocp = cur_yylloc;

	/* Now revert the un-truncation of the current token */
	yyextra->lookahead_hold_char = *(yyextra->lookahead_end);
	*(yyextra->lookahead_end) = '\0';

	yyextra->have_lookahead = true;

	/* Replace cur_token if needed, based on lookahead */
	switch (cur_token)
	{
		case NOT:
			/* Replace NOT by NOT_LA if it's followed by BETWEEN, IN, etc */
			switch (next_token)
			{
				case BETWEEN:
				case IN_P:
				case LIKE:
				case ILIKE:
				case SIMILAR:
					cur_token = NOT_LA;
					break;
			}
			break;

		case NULLS_P:
			/* Replace NULLS_P by NULLS_LA if it's followed by FIRST or LAST */
			switch (next_token)
			{
				case FIRST_P:
				case LAST_P:
					cur_token = NULLS_LA;
					break;
			}
			break;

		case WITH:
			/* Replace WITH by WITH_LA if it's followed by TIME or ORDINALITY or ROLLUP */
			switch (next_token)
			{
				case TIME:
				case ORDINALITY:
				case ROLLUP:
					cur_token = WITH_LA;
					break;
			}
			break;

		case UIDENT:
		case USCONST:
			/* Look ahead for UESCAPE */
			if (next_token == UESCAPE)
			{
				/* Yup, so get third token, which had better be SCONST */
				const char *escstr;

				/* Again save and restore *llocp */
				cur_yylloc = *llocp;

				/* Un-truncate current token so errors point to third token */
				*(yyextra->lookahead_end) = yyextra->lookahead_hold_char;

				/* Get third token */
				next_token = mys_core_yylex(&(yyextra->lookahead_yylval),
										llocp, yyscanner);

				/* If we throw error here, it will point to third token */
				if (next_token != SCONST)
					scanner_yyerror("UESCAPE must be followed by a simple string literal",
									yyscanner);

				escstr = yyextra->lookahead_yylval.str;
				if (strlen(escstr) != 1 || !check_uescapechar(escstr[0]))
					scanner_yyerror("invalid Unicode escape character",
									yyscanner);

				/* Now restore *llocp; errors will point to first token */
				*llocp = cur_yylloc;

				/* Apply Unicode conversion */
				lvalp->core_yystype.str =
					str_udeescape(lvalp->core_yystype.str,
								  escstr[0],
								  *llocp,
								  yyscanner);

				/*
				 * We don't need to revert the un-truncation of UESCAPE.  What
				 * we do want to do is clear have_lookahead, thereby consuming
				 * all three tokens.
				 */
				yyextra->have_lookahead = false;
			}
			else
			{
				/* No UESCAPE, so convert using default escape character */
				lvalp->core_yystype.str =
					str_udeescape(lvalp->core_yystype.str,
								  '\\',
								  *llocp,
								  yyscanner);
			}

			if (cur_token == UIDENT)
			{
				/* It's an identifier, so truncate as appropriate */
				truncate_identifier(lvalp->core_yystype.str,
									strlen(lvalp->core_yystype.str),
									true);
				cur_token = IDENT;
			}
			else if (cur_token == USCONST)
			{
				cur_token = SCONST;
			}
			break;
		
		case BINARY:
			if (prev_token == REGEXP)
				cur_token = BINARY_LA;
			break;
	}

	prev_token = cur_token;
	return cur_token;
}

/*
 * MySQL Parser Engine
 */
static const ParserRoutine mys_parser_engine = {
    .type = T_ParserRoutine,
	.is_standard_parser = false,
	.need_standard_parser = true,

	.keywordlist = &MysScanKeywords,
	.keyword_tokens = MysScanKeywordTokens,
	.keyword_categories = MysScanKeywordCategories,
    .transformStmt = mys_transformStmt,
    .transformSelectStmt = mys_transformSelectStmt,
    .transformInsertStmt = mys_transformInsertStmt,
    .transformUpdateStmt = mys_transformUpdateStmt,
    .transformDeleteStmt = mys_transformDeleteStmt,
    .transformCallStmt = mys_transformCallStmt,
    .transformOptionalSelectInto = mys_transformOptionalSelectInto,
    .transformSetOperationStmt = mys_transformSetOperationStmt,
    .transformSetOperationTree = mys_transformSetOperationTree,
    .analyze_requires_snapshot = mys_analyze_requires_snapshot,
    .transformOnConflictArbiter = mys_transformOnConflictArbiter,
    .raw_parser = mys_raw_parser,
    .transformExpr = mys_transformExpr,
	.transformGroupClause = mys_transformGroupClause,
    .transformDistinctClause = mys_transformDistinctClause,
    .transformCreateStmt = mys_transformCreateStmt,
    .transformAlterTableStmt = mys_transformAlterTableStmt,
    .ParseFuncOrColumn = mys_ParseFuncOrColumn,
    .func_get_detail = mys_func_get_detail,
    .make_fn_arguments = mys_make_fn_arguments
};

const ParserRoutine *
GetMysParserEngine(void)
{
    return &mys_parser_engine;
}

static List *
mys_raw_parser(const char *str, RawParseMode mode)
{
    core_yyscan_t yyscanner;
	mys_yy_extra_type yyextra;
	int			yyresult;

	/* initialize the flex scanner */
	yyscanner = mys_scanner_init(str, &yyextra.core_yy_extra,
							 &MysScanKeywords, MysScanKeywordTokens);

	if (mode == RAW_PARSE_DEFAULT)
		yyextra.have_lookahead = false;
	else
	{
		/* this array is indexed by RawParseMode enum */
		static const int mode_token[] = {
			0,					/* RAW_PARSE_DEFAULT */
			MODE_TYPE_NAME,		/* RAW_PARSE_TYPE_NAME */
			MODE_PLPGSQL_EXPR,	/* RAW_PARSE_PLPGSQL_EXPR */
			MODE_PLPGSQL_ASSIGN1,	/* RAW_PARSE_PLPGSQL_ASSIGN1 */
			MODE_PLPGSQL_ASSIGN2,	/* RAW_PARSE_PLPGSQL_ASSIGN2 */
			MODE_PLPGSQL_ASSIGN3	/* RAW_PARSE_PLPGSQL_ASSIGN3 */
		};

		yyextra.have_lookahead = true;
		yyextra.lookahead_token = mode_token[mode];
		yyextra.lookahead_yylloc = 0;
		yyextra.lookahead_end = NULL;
	}

	/* initialize the bison parser */
	mys_parser_init(&yyextra);

	/* Parse! */
	yyresult = mys_yyparse(yyscanner);

	/* Clean up (release memory) */
	mys_scanner_finish(yyscanner);

	if (yyresult)				/* error */
		return NIL;

	return yyextra.parsetree;
}

static bool
check_uescapechar(unsigned char escape)
{
	if (isxdigit(escape)
		|| escape == '+'
		|| escape == '\''
		|| escape == '"'
		|| scanner_isspace(escape))
		return false;
	else
		return true;
}

static unsigned int
hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 0xA;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 0xA;
	elog(ERROR, "invalid hexadecimal digit");
	return 0;					/* not reached */
}

static void
check_unicode_value(pg_wchar c)
{
	if (!is_valid_unicode_codepoint(c))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid Unicode escape value")));
}

static char *
str_udeescape(const char *str, char escape,
			  int position, core_yyscan_t yyscanner)
{
	const char *in;
	char	   *new,
			   *out;
	size_t		new_len;
	pg_wchar	pair_first = 0;
	ScannerCallbackState scbstate;

	/*
	 * Guesstimate that result will be no longer than input, but allow enough
	 * padding for Unicode conversion.
	 */
	new_len = strlen(str) + MAX_UNICODE_EQUIVALENT_STRING + 1;
	new = palloc(new_len);

	in = str;
	out = new;
	while (*in)
	{
		/* Enlarge string if needed */
		size_t		out_dist = out - new;

		if (out_dist > new_len - (MAX_UNICODE_EQUIVALENT_STRING + 1))
		{
			new_len *= 2;
			new = repalloc(new, new_len);
			out = new + out_dist;
		}

		if (in[0] == escape)
		{
			/*
			 * Any errors reported while processing this escape sequence will
			 * have an error cursor pointing at the escape.
			 */
			setup_scanner_errposition_callback(&scbstate, yyscanner,
											   in - str + position + 3);	/* 3 for U&" */
			if (in[1] == escape)
			{
				if (pair_first)
					goto invalid_pair;
				*out++ = escape;
				in += 2;
			}
			else if (isxdigit((unsigned char) in[1]) &&
					 isxdigit((unsigned char) in[2]) &&
					 isxdigit((unsigned char) in[3]) &&
					 isxdigit((unsigned char) in[4]))
			{
				pg_wchar	unicode;

				unicode = (hexval(in[1]) << 12) +
					(hexval(in[2]) << 8) +
					(hexval(in[3]) << 4) +
					hexval(in[4]);
				check_unicode_value(unicode);
				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) out);
					out += strlen(out);
				}
				in += 5;
			}
			else if (in[1] == '+' &&
					 isxdigit((unsigned char) in[2]) &&
					 isxdigit((unsigned char) in[3]) &&
					 isxdigit((unsigned char) in[4]) &&
					 isxdigit((unsigned char) in[5]) &&
					 isxdigit((unsigned char) in[6]) &&
					 isxdigit((unsigned char) in[7]))
			{
				pg_wchar	unicode;

				unicode = (hexval(in[2]) << 20) +
					(hexval(in[3]) << 16) +
					(hexval(in[4]) << 12) +
					(hexval(in[5]) << 8) +
					(hexval(in[6]) << 4) +
					hexval(in[7]);
				check_unicode_value(unicode);
				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) out);
					out += strlen(out);
				}
				in += 8;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid Unicode escape"),
						 errhint("Unicode escapes must be \\XXXX or \\+XXXXXX.")));

			cancel_scanner_errposition_callback(&scbstate);
		}
		else
		{
			if (pair_first)
				goto invalid_pair;

			*out++ = *in++;
		}
	}

	/* unfinished surrogate pair? */
	if (pair_first)
		goto invalid_pair;

	*out = '\0';
	return new;

	/*
	 * We might get here with the error callback active, or not.  Call
	 * scanner_errposition to make sure an error cursor appears; if the
	 * callback is active, this is duplicative but harmless.
	 */
invalid_pair:
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid Unicode surrogate pair"),
			 scanner_errposition(in - str + position + 3,	/* 3 for U&" */
								 yyscanner)));
	return NULL;				/* keep compiler quiet */
}

