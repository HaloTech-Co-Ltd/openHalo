/*-------------------------------------------------------------------------
 *
 * strfuncs.c
 *	  Extend strfuncs routines
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
 *	  src/backend/utils/ddsm/mysm/strfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <strings.h>
#include <math.h>

#include "access/detoast.h"
#include "access/toast_compression.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/unicode_norm.h"
#include "lib/hyperloglog.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "parser/scansup.h"
#include "port/pg_bswap.h"
#include "regex/regex.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/sortsupport.h"
#include "utils/varbit.h"
#include "utils/varlena.h"
#include "adapter/mysql/adapter.h"


typedef struct
{
	bool		is_multibyte;	/* T if multibyte encoding */
	bool		is_multibyte_char_in_char;	/* need to check char boundaries? */

	char	   *str1;			/* haystack string */
	char	   *str2;			/* needle string */
	int			len1;			/* string lengths in bytes */
	int			len2;

	/* Skip table for Boyer-Moore-Horspool search algorithm: */
	int			skiptablemask;	/* mask for ANDing with skiptable subscripts */
	int			skiptable[256]; /* skip distance for given mismatched char */

	char	   *last_match;		/* pointer to last match in 'str1' */

	/*
	 * Sometimes we need to convert the byte position of a match to a
	 * character position.  These store the last position that was converted,
	 * so that on the next call, we can continue from that point, rather than
	 * count characters from the very beginning.
	 */
	char	   *refpoint;		/* pointer within original haystack string */
	int			refpos;			/* 0-based character offset of the same point */
} TextPositionState;


static text *text_substring(Datum str, int32 start, int32 length, bool length_not_specified);
static text *concat_internal(const char *sepstr, int argidx, FunctionCallInfo fcinfo);
static text *concat_ws_internal(const char *sepstr, int argidx, FunctionCallInfo fcinfo);
static FmgrInfo *build_concat_foutcache(FunctionCallInfo fcinfo, int argidx);
static int	text_position(text *t1, text *t2, Oid collid);
static void text_position_setup(text *t1, text *t2, Oid collid, TextPositionState *state);
static bool text_position_next(Oid collid, TextPositionState *state);
static char * text_position_next_internal(Oid collid, char *start_ptr, TextPositionState *state);
static int text_position_get_match_pos(TextPositionState *state);
static void text_position_cleanup(TextPositionState *state);
static void check_collation_set(Oid collid);
static void binary_to_hex(char ch0, char ch1, char ch2, char ch3, char* ch);
static void convert_to_digit_text_for_mysql(char **str, int str_len);
static void convert_to_text_for_mysql(char **str, int str_len);
static bool lowerStrs(text *t1, text *t2, char **backupBuf1, char **backupBuf2);
static void restoreStrs(char *backupBuf1, char *backupBuf2, text *t1, text *t2);


PG_FUNCTION_INFO_V1(elt);
Datum
elt(PG_FUNCTION_ARGS)
{
    FmgrInfo *foutcache;
    Datum datum;
    char *value;
    Oid valtype;
    Oid valBaseType;
    int index;

    foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
    if (foutcache == NULL)
    {
        foutcache = build_concat_foutcache(fcinfo, 0);
    }

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    datum = PG_GETARG_DATUM(0);
    value = OutputFunctionCall(&foutcache[0], datum);
    index = atoi(value);
    if ((index < 1) || (PG_NARGS() < index))
    {
        PG_RETURN_NULL();
    }

    if (PG_ARGISNULL(index))
    {
        PG_RETURN_NULL();
    }

    datum = PG_GETARG_DATUM(index);
    valtype = get_fn_expr_argtype(fcinfo->flinfo, index);
    valBaseType = getBaseType(valtype);
    if (valBaseType != BYTEAOID)
    {
        value = OutputFunctionCall(&foutcache[index], datum);
    }
    else
    {
        bytea *vlena = DatumGetByteaPP(datum);
        value = VARDATA_ANY(vlena);
    }

    PG_RETURN_TEXT_P(cstring_to_text(value));
}


PG_FUNCTION_INFO_V1(text_substr);
/*
 * text_substr()
 * Return a substring starting at the specified position.
 * - thomas 1997-12-31
 *
 * Input:
 *	- string
 *	- starting position (is one-based)
 *	- string length
 *
 * If the starting position is zero or less, then return from the start of the string
 *	adjusting the length to be consistent with the "negative start" per SQL.
 * If the length is less than zero, return the remaining string.
 *
 * Added multibyte support.
 * - Tatsuo Ishii 1998-4-21
 * Changed behavior if starting position is less than one to conform to SQL behavior.
 * Formerly returned the entire string; now returns a portion.
 * - Thomas Lockhart 1998-12-10
 * Now uses faster TOAST-slicing interface
 * - John Gray 2002-02-22
 * Remove "#ifdef MULTIBYTE" and test for encoding_max_length instead. Change
 * behaviors conflicting with SQL to meet SQL (if E = S + L < S throw
 * error; if E < 1, return '', not entire string). Fixed MB related bug when
 * S > LC and < LC + 4 sometimes garbage characters are returned.
 * - Joe Conway 2002-08-10
 */
Datum
text_substr(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									PG_GETARG_INT32(1),
									PG_GETARG_INT32(2),
									false));
}

PG_FUNCTION_INFO_V1(text_substr_no_len);
/*
 * text_substr_no_len -
 *	  Wrapper to avoid opr_sanity failure due to
 *	  one function accepting a different number of args.
 */
Datum
text_substr_no_len(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									PG_GETARG_INT32(1),
									-1, 
                                    true));
}

PG_FUNCTION_INFO_V1(text_left);
Datum
text_left(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
                                    1,
									PG_GETARG_INT32(1),
									false));
}

PG_FUNCTION_INFO_V1(text_right);
Datum
text_right(PG_FUNCTION_ARGS)
{
    Datum arg1;
    Datum arg2;
    int len1;
    int rightLen;
    int startIndex;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }
    if (PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    arg1 = PG_GETARG_DATUM(0);
    arg2 = PG_GETARG_DATUM(1);
    //len1 = toast_raw_datum_size(arg1) - VARHDRSZ;
    len1 = DatumGetInt32(DirectFunctionCall1(textlen, arg1));
    rightLen = DatumGetInt32(arg2);

    if (rightLen <= 0)
    {
        PG_RETURN_TEXT_P(cstring_to_text(""));
    }

    if (rightLen < len1)
    {
        startIndex = len1 - rightLen + 1;
    }
    else
    {
        startIndex = 1;
    }

	PG_RETURN_TEXT_P(text_substring(arg1, startIndex, -1, true));
}

PG_FUNCTION_INFO_V1(check_set);
Datum
check_set(PG_FUNCTION_ARGS)
{
    text *txt1;
    char* str1;
    int str1Len;
    char items1[64][128];
    int items1Num;
    text *txt2;
    char* str2;
    int str2Len;
    int len;
    int i;
    int j;
    int k;
    int l;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_BOOL(false);
    }

    txt1 = PG_GETARG_TEXT_PP(0); 
    str1 = VARDATA_ANY(txt1);
    str1Len = VARSIZE_ANY_EXHDR(txt1);
    items1Num = 0;
    txt2 = PG_GETARG_TEXT_PP(1); 
    str2 = VARDATA_ANY(txt2);
    str2Len = VARSIZE_ANY_EXHDR(txt2);

    if (str2Len == 0)
    {
        PG_RETURN_BOOL(true);
    }

    i = 0;
    j = 0;
    k = 0;
    for (i = 0; i < str1Len; i++)
    {
        if (64 <= items1Num)
        {
            PG_RETURN_BOOL(false);
        }

        if (str1[i] != ' ')
        {
            if (str1[i] == ',')
            {
                if (j < i)
                {
                    len = i - j;
                    k = 1;
                    while (true)
                    {
                        if (len <= 0)
                        {
                            break;
                        }

                        if (str1[i - k] == ' ')
                        {
                            --len;
                            ++k;
                        }
                        else 
                        {
                            break;
                        }
                    }
                    if (128 <= len)
                    {
                        PG_RETURN_BOOL(false);
                    }

                    memcpy(items1[items1Num], (str1 + j), len);
                    items1[items1Num][len] = '\0';
                    ++items1Num;

                    j = i + 1;
                    k = 0;
                }
            }   // if (str1[i] == ',')
        }   // if (str1[i] != ' ')
    }   // for (i = 0; i < str1Len; i++)
    if (j < i)
    {
        if (64 <= items1Num)
        {
            PG_RETURN_BOOL(false);
        }

        len = i - j;
        k = 1;
        while (true)
        {
            if (len <= 0)
            {
                break;
            }

            if (str1[i - k] == ' ')
            {
                --len;
                ++k;
            }
            else 
            {
                break;
            }
        }
        if (128 <= len)
        {
            PG_RETURN_BOOL(false);
        }

        memcpy(items1[items1Num], (str1 + j), len);
        items1[items1Num][len] = '\0';
        ++items1Num;
    }

    i = 0;
    j = i;
    k = 0;
    l = 0;
    while (true)
    {
        if (str2Len <= i)
        {
            bool found = false;
            int item2Len = 0;
            k = i - 1;
            while (true)
            {
                if (k <= j)
                {
                    break;
                }

                if (str2[k] != ' ')
                {
                    break;
                }      
                else
                {
                    --k;
                }
            }

            item2Len = k - j + 1;
            for (l = 0; l < items1Num; l++)
            {
                int item1Len = strlen(items1[l]);
                if (item1Len == item2Len)
                {
                    if (strncmp((str2 + j), items1[l], item2Len) == 0)
                    {
                        found = true;
                        break;
                    }
                    else if (strncasecmp((str2 + j), items1[l], item2Len) == 0)
                    {
                        memcpy((str2 + j), items1[l], item2Len);
                        found = true;
                        break;
                    }
                    else 
                    {
                        continue;
                    }
                }
            }
            if (found)
            {
                PG_RETURN_BOOL(true);
            }
            {
                PG_RETURN_BOOL(false);
            }

            break;
        }

        if (str2[i] == ',')
        {
            bool found = false;
            int item2Len = i - j;
            for (l = 0; l < items1Num; l++)
            {
                int item1Len = strlen(items1[l]);
                if (item1Len == item2Len)
                {
                    if (strncmp((str2 + j), items1[l], item2Len) == 0)
                    {
                        found = true;
                        break;
                    }
                    else if (strncasecmp((str2 + j), items1[l], item2Len) == 0)
                    {
                        memcpy((str2 + j), items1[l], item2Len);
                        found = true;
                        break;
                    }
                    else 
                    {
                        continue;
                    }
                }
            }
            if (found)
            {
                j = i + 1;
            }
            else
            {
                PG_RETURN_BOOL(false);
            }
        }
        ++i;
    }

    PG_RETURN_BOOL(true);
}


PG_FUNCTION_INFO_V1(text_instr);
Datum
text_instr(PG_FUNCTION_ARGS)
{
    text   *str = PG_GETARG_TEXT_PP(0); 
    text   *search_str = PG_GETARG_TEXT_PP(1); 

    PG_RETURN_INT32((int32) text_position(str, search_str, PG_GET_COLLATION()));
}


PG_FUNCTION_INFO_V1(convert_time_text_to_numeric_for_mysql);
Datum
convert_time_text_to_numeric_for_mysql(PG_FUNCTION_ARGS)
{
    float8 ret;
    Datum txt;
    char* str;
    int strLen;
    int tmpSize;
    char tmp[64];
    int i;
    int j;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }

    txt = PG_GETARG_DATUM(0);
    str = TextDatumGetCString(txt);
    strLen = strlen(str);
    tmpSize = 64;
    if (strLen == 0)
    {
        PG_RETURN_NULL();
    }
    if (tmpSize <= strLen)
    {
        PG_RETURN_NULL();
    }

    i = 0;
    j = 0;
    for (i = 0; i < strLen; i++) 
    {
        // "2023-08-07 15:26:37.12345"
        if (isdigit(str[i]) || (str[i] == '.'))
        {
            tmp[j] = str[i];
            ++j;
        }
        else 
        {
            // do nothing;
        }
    }
    tmp[j] = '\0';

    ret = atof(tmp);
    PG_RETURN_FLOAT8(ret);
}

PG_FUNCTION_INFO_V1(text_locate2);
Datum
text_locate2(PG_FUNCTION_ARGS)
{
    text   *str = PG_GETARG_TEXT_PP(0); 
    text   *search_str = PG_GETARG_TEXT_PP(1); 

    PG_RETURN_INT32((int32) text_position(search_str, str, PG_GET_COLLATION()));
}

PG_FUNCTION_INFO_V1(text_locate3);
Datum
text_locate3(PG_FUNCTION_ARGS)
{
    Datum arg1;
    Datum arg2;
    Datum arg3;
    text *targ1;
    int iarg3;
    text *slice;
    int32 ret;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }
    if (PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }
    if (PG_ARGISNULL(2))
    {
        PG_RETURN_INT32(0);
    }

    arg1 = PG_GETARG_DATUM(0);
    arg2 = PG_GETARG_DATUM(1);
    arg3 = PG_GETARG_DATUM(2);
    targ1 = DatumGetTextPP(arg1);
    iarg3 = DatumGetInt32(arg3);

    if (iarg3 <= 0)
    {
        PG_RETURN_INT32(0);
    }

    slice = text_substring(arg2, iarg3, -1, true);
	ret = (int32) text_position(slice, targ1, PG_GET_COLLATION());
    if (0 < ret)
    {
        PG_RETURN_INT32(iarg3 -1 + ret);
    }
    else 
    {
        PG_RETURN_INT32(0);
    }
}

PG_FUNCTION_INFO_V1(convertTextToDouble);
Datum
convertTextToDouble(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        Datum txt = PG_GETARG_DATUM(0);
        char* str = TextDatumGetCString(txt);
        int len = strlen(str);
        if (1 <= len)
        {
            float8 ret;

            for (int i = (len - 1); i >= 0; i--)
            {
                if (str[i] != ' ')
                {
                    break;
                }
                else 
                {
                    str[i] = '\0';
                }
            }
            ret = strtod(str, &str);
            if (*str != '\0')
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("invalid input syntax for type %s: \"%s\"",
                                "double precision", str)));
            }

            PG_RETURN_FLOAT8(ret);
        }
        else 
        {
            PG_RETURN_FLOAT8(0.0);
        }
    }
    else
    {
        PG_RETURN_NULL();
    }
}

PG_FUNCTION_INFO_V1(convertTextToInt8);
Datum
convertTextToInt8(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        Datum txt = PG_GETARG_DATUM(0);
        char* str = TextDatumGetCString(txt);
        int len = strlen(str);
        if (1 <= len)
        {
            int64 ret;
            for (int i = (len - 1); i >= 0; i--)
            {
                if (str[i] != ' ')
                {
                    break;
                }
                else 
                {
                    str[i] = '\0';
                }
            }
            ret = atoll(str);
            PG_RETURN_INT64(ret);
        }
        else 
        {
            PG_RETURN_INT64(0);
        }
    }
    else
    {
        PG_RETURN_NULL();
    }
}


PG_FUNCTION_INFO_V1(convert_text_to_bigint_for_mysql);
Datum
convert_text_to_bigint_for_mysql(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        long ret = 0;
        Datum txt = PG_GETARG_DATUM(0);
        char* str = TextDatumGetCString(txt);
        int len = strlen(str);
        int digitNum = 0;
        int dotIndex = -1;
        int signIndex = -1;
        bool isNegative = false;
        bool started = false;
        for (int i = 0; i < len; i++)
        {
            if (str[i] != ' ')
            {
                if ('0' <= str[i] && str[i] <= '9')
                {
                    ++digitNum;
                }
                else 
                {
                    if (str[i] == '.') 
                    {
                        if (dotIndex == -1)
                        {
                            dotIndex = i;
                        }
                        else
                        {
                            PG_RETURN_NULL();
                        }
                    }
                    else if (str[i] == '+')
                    {
                        if (signIndex == -1)
                        {
                            signIndex = i;
                        }
                        else
                        {
                            PG_RETURN_NULL();
                        }
                    }
                    else if (str[i] == '-')
                    {
                        isNegative = true;
                        if (signIndex == -1)
                        {
                            signIndex = i;
                        }
                        else
                        {
                            PG_RETURN_NULL();
                        }
                    }
                    else 
                    {
                        PG_RETURN_NULL();
                    }
                }
                started = true;
            }
            else 
            {
                if (started)
                {
                    for (int j = (i + 1); j < len; j++)
                    {
                        if (str[j] != ' ')
                        {
                            PG_RETURN_NULL();
                        }
                    }
                }
            }
        }
        if (0 < digitNum)
        {
            if ((0 <= dotIndex) && (0 <= signIndex))
            {
                if (((dotIndex + 1) == signIndex) && 
                    ((dotIndex - 1) == signIndex))
                {
                    PG_RETURN_NULL();
                }
            }

            ret = atol(str);
            if (0 <= dotIndex)
            {
                if ('4' < str[dotIndex + 1])
                {
                    if (!isNegative)
                    {
                        ++ret;
                    }
                    else
                    {
                        --ret;
                    }
                }
            }
            PG_RETURN_INT64(ret);
        }
        else 
        {
            PG_RETURN_NULL();
        }
    }
    else
    {
        PG_RETURN_NULL();
    }
}


PG_FUNCTION_INFO_V1(mys_convertTextToFloat8);
Datum
mys_convertTextToFloat8(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        /* 未做数据的合法性检查 */
        Datum txt = PG_GETARG_DATUM(0);
        char* str = TextDatumGetCString(txt);
        double ret = atof(str);

        PG_RETURN_FLOAT8(ret);
    }
    else
    {
        PG_RETURN_NULL();
    }
}



PG_FUNCTION_INFO_V1(convertTextVarbitToBigint);
Datum
convertTextVarbitToBigint(PG_FUNCTION_ARGS)
{
    unsigned long long ret = 0;
    char *bitData = TextDatumGetCString(PG_GETARG_DATUM(0));
    int bitDataLen = strlen(bitData);
    for (int i = 0; i < bitDataLen; i++)
    {
        unsigned long long num = 1;
        if (bitData[i] == '1')
        {
            for (int j = 1; j <= (bitDataLen - i - 1); j++)
            {
                num = num * 2;
            }
            ret += num;
            if (9223372036854775807 < ret)
            {
                /* 
                 * halo中没有无符号整数，
                 * 可能导致转换后的十进制溢出
                 */
                PG_RETURN_NULL();
            }
        }
    }
    PG_RETURN_INT64(ret);
}


PG_FUNCTION_INFO_V1(convertVarbitToBigint);
Datum
convertVarbitToBigint(PG_FUNCTION_ARGS)
{
    unsigned long long ret = 0;
    VarBit *varBit = PG_GETARG_VARBIT_P(0);
    char *bitData = DatumGetCString(DirectFunctionCall1(bit_out, VarBitPGetDatum(varBit)));
    int bitDataLen = strlen(bitData);
    for (int i = 0; i < bitDataLen; i++)
    {
        unsigned long long num = 1;
        if (bitData[i] == '1')
        {
            for (int j = 1; j <= (bitDataLen - i - 1); j++)
            {
                num = num * 2;
            }
            ret += num;
            if (9223372036854775807 < ret)
            {
                /* 
                 * halo中没有无符号整数，
                 * 可能导致转换后的十进制溢出
                 */
                PG_RETURN_NULL();
            }
        }
    }
    PG_RETURN_INT64(ret);
}


PG_FUNCTION_INFO_V1(convertVarbitToInt);
Datum
convertVarbitToInt(PG_FUNCTION_ARGS)
{
    VarBit *varBit = PG_GETARG_VARBIT_P(0);
    char *bitData = DatumGetCString(DirectFunctionCall1(bit_out, VarBitPGetDatum(varBit)));
    int bitDataLen = strlen(bitData);
    unsigned long ret = 0;
    for (int i = 0; i < bitDataLen; i++)
    {
        unsigned long num = 1;
        if (bitData[i] == '1')
        {
            for (int j = 1; j <= (bitDataLen - i - 1); j++)
            {
                num = num * 2;
            }
            ret += num;
            if (2147483647 < ret)
            {
                /* 
                 * halo中没有无符号整数，
                 * 可能导致转换后的十进制溢出
                 */
                PG_RETURN_NULL();
            }
        }
    }
    PG_RETURN_INT32(ret);
}


PG_FUNCTION_INFO_V1(convertVarbitToSmallint);
Datum
convertVarbitToSmallint(PG_FUNCTION_ARGS)
{
    VarBit *varBit = PG_GETARG_VARBIT_P(0);
    char *bitData = DatumGetCString(DirectFunctionCall1(bit_out, VarBitPGetDatum(varBit)));
    int bitDataLen = strlen(bitData);
    unsigned int ret = 0;
    for (int i = 0; i < bitDataLen; i++)
    {
        unsigned int num = 1;
        if (bitData[i] == '1')
        {
            for (int j = 1; j <= (bitDataLen - i - 1); j++)
            {
                num = num * 2;
            }
            ret += num;
            if (32767 < ret)
            {
                /* 
                 * halo中没有无符号整数，
                 * 可能导致转换后的十进制溢出
                 */
                PG_RETURN_NULL();
            }
        }
    }
    PG_RETURN_INT16(ret);
}


PG_FUNCTION_INFO_V1(convertSmallintToVarbit);
Datum
convertSmallintToVarbit(PG_FUNCTION_ARGS)
{
	VarBit *ret;

    int32 arg;
	int32 typmod;
	int32 rlen;
	int32 destbitsleft;
    int32 srcbitsleft;
	bits8 *r;

    arg = PG_GETARG_INT16(0);
    if (0 <= arg)
    {
        typmod = 0;
        do
        {
            arg = arg / 2;
            typmod++;
        } while (arg != 0);
        arg = PG_GETARG_INT16(0);
    }
    else 
    {
        typmod = 64;
    }

    rlen = VARBITTOTALLEN(typmod);
    ret = (VarBit *) palloc(rlen);
    SET_VARSIZE(ret, rlen);
    VARBITLEN(ret) = typmod;
    r = VARBITS(ret);
	destbitsleft = typmod;
	srcbitsleft = 32;
	/* drop any input bits that don't fit */
	srcbitsleft = Min(srcbitsleft, destbitsleft);
	/* sign-fill any excess bytes in output */
	while (destbitsleft >= (srcbitsleft + 8))
	{
		*r++ = (bits8) ((arg < 0) ? BITMASK : 0);
		destbitsleft -= 8;
	}
	/* store first fractional byte */
	if (destbitsleft > srcbitsleft)
	{
		unsigned int val = (unsigned int) (arg >> (destbitsleft - 8));

		/* Force sign-fill in case the compiler implements >> as zero-fill */
		if (arg < 0)
        {
			val |= ((unsigned int) -1) << (srcbitsleft + 8 - destbitsleft);
        }
		*r++ = (bits8) (val & BITMASK);
		destbitsleft -= 8;
	}
	/* Now srcbitsleft and destbitsleft are the same, need not track both */
	/* store whole bytes */
	while (destbitsleft >= 8)
	{
		*r++ = (bits8) ((arg >> (destbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* store last fractional byte */
	if (destbitsleft > 0)
    {
		*r = (bits8) ((arg << (8 - destbitsleft)) & BITMASK);
    }

    PG_RETURN_VARBIT_P(ret);
}


PG_FUNCTION_INFO_V1(convertIntToVarbit);
Datum
convertIntToVarbit(PG_FUNCTION_ARGS)
{
	VarBit *ret;

    int32 arg;
	int32 typmod;
	int32 rlen;
	int32 destbitsleft;
    int32 srcbitsleft;
	bits8 *r;

    arg = PG_GETARG_INT32(0);
    if (0 <= arg)
    {
        typmod = 0;
        do
        {
            arg = arg / 2;
            typmod++;
        } while (arg != 0);
        arg = PG_GETARG_INT32(0);
    }
    else 
    {
        typmod = 64;
    }

    rlen = VARBITTOTALLEN(typmod);
    ret = (VarBit *) palloc(rlen);
    SET_VARSIZE(ret, rlen);
    VARBITLEN(ret) = typmod;
    r = VARBITS(ret);
	destbitsleft = typmod;
	srcbitsleft = 32;
	/* drop any input bits that don't fit */
	srcbitsleft = Min(srcbitsleft, destbitsleft);
	/* sign-fill any excess bytes in output */
	while (destbitsleft >= (srcbitsleft + 8))
	{
		*r++ = (bits8) ((arg < 0) ? BITMASK : 0);
		destbitsleft -= 8;
	}
	/* store first fractional byte */
	if (destbitsleft > srcbitsleft)
	{
		unsigned int val = (unsigned int) (arg >> (destbitsleft - 8));

		/* Force sign-fill in case the compiler implements >> as zero-fill */
		if (arg < 0)
        {
			val |= ((unsigned int) -1) << (srcbitsleft + 8 - destbitsleft);
        }
		*r++ = (bits8) (val & BITMASK);
		destbitsleft -= 8;
	}
	/* Now srcbitsleft and destbitsleft are the same, need not track both */
	/* store whole bytes */
	while (destbitsleft >= 8)
	{
		*r++ = (bits8) ((arg >> (destbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* store last fractional byte */
	if (destbitsleft > 0)
    {
		*r = (bits8) ((arg << (8 - destbitsleft)) & BITMASK);
    }

    PG_RETURN_VARBIT_P(ret);
}


PG_FUNCTION_INFO_V1(convertBigintToVarbit);
Datum
convertBigintToVarbit(PG_FUNCTION_ARGS)
{
	VarBit *ret;

    int64 arg;
	int32 typmod;
	int32 rlen;
	int32 destbitsleft;
    int32 srcbitsleft;
	bits8 *r;

    arg = PG_GETARG_INT64(0);
    if (0 <= arg)
    {
        typmod = 0;
        do
        {
            arg = arg / 2;
            typmod++;
        } while (arg != 0);
        arg = PG_GETARG_INT64(0);
    }
    else 
    {
        typmod = 64;
    }

    rlen = VARBITTOTALLEN(typmod);
    ret = (VarBit *) palloc(rlen);
    SET_VARSIZE(ret, rlen);
    VARBITLEN(ret) = typmod;
    r = VARBITS(ret);
	destbitsleft = typmod;
	srcbitsleft = 64;
	/* drop any input bits that don't fit */
	srcbitsleft = Min(srcbitsleft, destbitsleft);
	/* Now srcbitsleft and destbitsleft are the same, need not track both */
	/* store whole bytes */
	while (destbitsleft >= 8)
	{
		*r++ = (bits8) ((arg >> (destbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* store last fractional byte */
	if (destbitsleft > 0)
    {
		*r = (bits8) ((arg << (8 - destbitsleft)) & BITMASK);
    }

    PG_RETURN_VARBIT_P(ret);
}


PG_FUNCTION_INFO_V1(convert_text_to_digit_text_for_mysql);
Datum
convert_text_to_digit_text_for_mysql(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        Datum txt = PG_GETARG_DATUM(0);
        char* str = TextDatumGetCString(txt);
        int len = strlen(str);
        if (1 <= len)
        {
            convert_to_digit_text_for_mysql(&str, len);
            PG_RETURN_TEXT_P(cstring_to_text(str));
        }
        else 
        {
            PG_RETURN_TEXT_P(cstring_to_text("0"));
        }
    }
    else
    {
        PG_RETURN_NULL();
    }
}

PG_FUNCTION_INFO_V1(convert_digit_text_to_text_for_mysql);
Datum
convert_digit_text_to_text_for_mysql(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        Datum txt = PG_GETARG_DATUM(0);
        char* str = TextDatumGetCString(txt);
        int len = strlen(str);
        
        convert_to_text_for_mysql(&str, len);
        PG_RETURN_TEXT_P(cstring_to_text(str));
    }
    else
    {
        PG_RETURN_NULL();
    }
}

PG_FUNCTION_INFO_V1(ceil_for_mysql);
Datum
ceil_for_mysql(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        Datum txt = PG_GETARG_DATUM(0);
        char* str = TextDatumGetCString(txt);
        int len = strlen(str);
        if (1 <= len)
        {
            int64 ret;
            float8 tmp;
            convert_to_digit_text_for_mysql(&str, len);
            tmp = atof(str);
            ret = ceil(tmp);
            PG_RETURN_INT64(ret);
        }
        else 
        {
            PG_RETURN_INT64(0);
        }
    }
    else
    {
        PG_RETURN_NULL();
    }
}

PG_FUNCTION_INFO_V1(floor_for_mysql);
Datum
floor_for_mysql(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        Datum txt = PG_GETARG_DATUM(0);
        char* str = TextDatumGetCString(txt);
        int len = strlen(str);
        if (1 <= len)
        {
            int64 ret;
            float8 tmp;
            convert_to_digit_text_for_mysql(&str, len);
            tmp = atof(str);
            ret = floor(tmp);
            PG_RETURN_INT64(ret);
        }
        else 
        {
            PG_RETURN_INT64(0);
        }
    }
    else
    {
        PG_RETURN_NULL();
    }
}

//PG_FUNCTION_INFO_V1(mod_ti_for_mysql);
//Datum
//mod_ti_for_mysql(PG_FUNCTION_ARGS)
//{
//    if (!PG_ARGISNULL(0))
//    {
//        Datum txt = PG_GETARG_DATUM(0);
//        char* str = TextDatumGetCString(txt);
//        int len = strlen(str);
//        int64 divisor = PG_GETARG_INT64(1);
//        if (1 <= len)
//        {
//            if (divisor != 0)
//            {
//                float8 ret;
//                float8 dividend;
//                convert_to_digit_text_for_mysql(&str, len);
//                dividend = atof(str);
//                ret = dividend % (float8)divisor;
//                PG_RETURN_FLOAT8(ret);
//            }
//            else 
//            {
//                PG_RETURN_NULL();
//            }
//        }
//        else 
//        {
//            PG_RETURN_NULL();
//        }
//    }
//    else
//    {
//        PG_RETURN_NULL();
//    }
//}
//
//PG_FUNCTION_INFO_V1(mod_it_for_mysql);
//Datum
//mod_it_for_mysql(PG_FUNCTION_ARGS)
//{
//    if (!PG_ARGISNULL(0))
//    {
//        int64 dividend = PG_GETARG_INT64(0);
//        Datum txt = PG_GETARG_DATUM(1);
//        char* str = TextDatumGetCString(txt);
//        int len = strlen(str);
//        if (1 <= len)
//        {
//            float8 ret;
//            float8 divisor;
//            convert_to_digit_text_for_mysql(&str, len);
//            divisor = atof(str);
//            if (divisor != 0)
//            {
//                ret = dividend % divisor;
//                PG_RETURN_FLOAT8(ret);
//            }
//            else 
//            {
//                PG_RETURN_NULL();
//            }
//        }
//        else 
//        {
//            PG_RETURN_NULL();
//        }
//    }
//    else
//    {
//        PG_RETURN_NULL();
//    }
//}
//
//PG_FUNCTION_INFO_V1(mod_tn_for_mysql);
//Datum
//mod_tn_for_mysql(PG_FUNCTION_ARGS)
//{
//    if (!PG_ARGISNULL(0))
//    {
//        Datum txt = PG_GETARG_DATUM(0);
//        char* str = TextDatumGetCString(txt);
//        int len = strlen(str);
//        float8 divisor = PG_GETARG_FLOAT8(1);
//        if (1 <= len)
//        {
//            if (divisor != 0)
//            {
//                float8 ret;
//                float8 dividend;
//                convert_to_digit_text_for_mysql(&str, len);
//                dividend = atof(str);
//                ret = dividend % divisor;
//                PG_RETURN_FLOAT8(ret);
//            }
//            else 
//            {
//                PG_RETURN_NULL();
//            }
//        }
//        else 
//        {
//            PG_RETURN_NULL();
//        }
//    }
//    else
//    {
//        PG_RETURN_NULL();
//    }
//}
//
//PG_FUNCTION_INFO_V1(mod_nt_for_mysql);
//Datum
//mod_nt_for_mysql(PG_FUNCTION_ARGS)
//{
//    if (!PG_ARGISNULL(0))
//    {
//        float8 dividend = PG_GETARG_FLOAT8(0);
//        Datum txt = PG_GETARG_DATUM(1);
//        char* str = TextDatumGetCString(txt);
//        int len = strlen(str);
//        if (1 <= len)
//        {
//            float8 ret;
//            float8 divisor;
//            convert_to_digit_text_for_mysql(&str, len);
//            divisor = atof(str);
//            if (divisor != 0)
//            {
//                ret = dividend % divisor;
//                PG_RETURN_FLOAT8(ret);
//            }
//            else 
//            {
//                PG_RETURN_NULL();
//            }
//        }
//        else 
//        {
//            PG_RETURN_NULL();
//        }
//    }
//    else
//    {
//        PG_RETURN_NULL();
//    }
//}
//
//PG_FUNCTION_INFO_V1(mod_tt_for_mysql);
//Datum
//mod_tt_for_mysql(PG_FUNCTION_ARGS)
//{
//    if (!PG_ARGISNULL(0))
//    {
//        Datum txt1 = PG_GETARG_DATUM(0);
//        char* str1 = TextDatumGetCString(txt1);
//        int len1 = strlen(str1);
//        Datum txt2 = PG_GETARG_DATUM(1);
//        char* str2 = TextDatumGetCString(txt2);
//        int len2 = strlen(str2);
//        if ((1 <= len1) && (1 <= len2))
//        {
//            float8 dividend;
//            float8 divisor;
//            convert_to_digit_text_for_mysql(&str1, len1);
//            dividend = atof(str1);
//            convert_to_digit_text_for_mysql(&str2, len2);
//            divisor = atof(str2);
//
//            if (divisor != 0)
//            {
//                float8 ret;
//                ret = dividend % divisor;
//                PG_RETURN_FLOAT8(ret);
//            }
//            else 
//            {
//                PG_RETURN_NULL();
//            }
//        }
//        else 
//        {
//            PG_RETURN_NULL();
//        }
//    }
//    else
//    {
//        PG_RETURN_NULL();
//    }
//}


PG_FUNCTION_INFO_V1(mys_bit_to_text);
Datum
mys_bit_to_text(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        VarBit *s = PG_GETARG_VARBIT_P(0);
        char *bitData = DatumGetCString(DirectFunctionCall1(bit_out, VarBitPGetDatum(s)));
        int bitLen = VARBITLEN(s);
        int needPrePadBitNum = VARBITPAD(s);
        int byteLen = VARBITBYTES(s);
        text *result = (text *) palloc0(byteLen + VARHDRSZ);
        bits8 *textData;
        int i;

        SET_VARSIZE(result, byteLen + VARHDRSZ);
        textData = (bits8 *)VARDATA(result);

        if (bitLen != 0)
        {
            int j;
            bool saw_non_ascii = false;

            for (i = 0; i < (BITS_PER_BYTE - needPrePadBitNum); i++)
            {
                textData[0] = textData[0] * 2 + (bitData[i] - '0');
            }
            if (textData[0] == '\0' || IS_HIGHBIT_SET(textData[0]))
				saw_non_ascii = true;

            for (j = 1; j < byteLen; j++)
            {
                int end = i + BITS_PER_BYTE;
                for (; i < end; i++)
                {
                    textData[j] = textData[j] * 2 + (bitData[i] - '0');
                }
                if (textData[j] == '\0' || IS_HIGHBIT_SET(textData[j]))
				    saw_non_ascii = true;
            }
            if (saw_non_ascii)
            {
                pg_verifymbstr((const char *)textData, byteLen, false);
            }
        }
        PG_RETURN_TEXT_P(result);
    }
    else
    {
        PG_RETURN_NULL();
    }
}


PG_FUNCTION_INFO_V1(cast_bittext_to_bytea);
Datum
cast_bittext_to_bytea(PG_FUNCTION_ARGS)
{
    if (!PG_ARGISNULL(0))
    {
        bytea* result;
        int bc;

        VarBit *s = PG_GETARG_VARBIT_P(0);
        int len = VARBITLEN(s);

        //text *arg1 = PG_GETARG_TEXT_PP(0); 
        //int len = VARSIZE_ANY_EXHDR(arg1);
        if (0 < len)
        {
            // char* inputText = VARDATA_ANY(arg1);
            char *inputText = DatumGetCString(DirectFunctionCall1(bit_out, VarBitPGetDatum(s)));
            int i = 0;
            int j = 0;
            int len_mod_4 = len % 4;
            if (len_mod_4 == 0)
            {
                char tmp1 = ' ';
                binary_to_hex(inputText[0], inputText[1], inputText[2], inputText[3], &tmp1);
                inputText[j++] = '\\';
                inputText[j++] = 'x';
                inputText[j++] = tmp1;
                i += 4;
            }
            else if (len_mod_4 == 1)
            {
                char tmp1 = ' ';
                char tmp2 = ' ';
                binary_to_hex('0', '0', '0', inputText[0], &tmp1);
                binary_to_hex(inputText[1], inputText[2], inputText[3], inputText[4], &tmp2);
                inputText[j++] = '\\';
                inputText[j++] = 'x';
                inputText[j++] = tmp1;
                inputText[j++] = tmp2;
                i += 5;
            }
            else if (len_mod_4 == 2)
            {
                char tmp1 = ' ';
                char tmp2 = ' ';
                binary_to_hex('0', '0', inputText[0], inputText[1], &tmp1);
                binary_to_hex(inputText[2], inputText[3], inputText[4], inputText[5], &tmp2);
                inputText[j++] = '\\';
                inputText[j++] = 'x';
                inputText[j++] = tmp1;
                inputText[j++] = tmp2;
                i += 6;
            }
            else    // (len_mod_4 == 3)
            {
                char tmp1 = ' ';
                binary_to_hex('0', inputText[0], inputText[1], inputText[2], &tmp1);
                inputText[j++] = '\\';
                inputText[j++] = 'x';
                inputText[j++] = tmp1;
                i += 3;
            }

            for (; i < len; i += 4)
            {
                binary_to_hex(inputText[i], inputText[i + 1], inputText[i + 2], inputText[i + 3], 
                              &inputText[j++]);
            }   

            bc = (j - 2) / 2 + VARHDRSZ;	/* maximum possible length */
            result = palloc(bc);
            bc = hex_decode((inputText + 2), (j - 2), VARDATA(result));
            SET_VARSIZE(result, (bc + VARHDRSZ)); /* actual length */

            PG_RETURN_BYTEA_P(result);
        }
        else
        {
            bc = VARHDRSZ;	/* maximum possible length */
            result = palloc(bc);
            bc = hex_decode("", 0, VARDATA(result));
            SET_VARSIZE(result, (bc + VARHDRSZ)); /* actual length */
            PG_RETURN_BYTEA_P(result);
        }
    }
    else
    {
        PG_RETURN_NULL();
    }
}

/*
 * text_substring -
 *	Does the real work for text_substr() and text_substr_no_len()
 *
 *	This is broken out so it can be called directly by other string processing
 *	functions.  Note that the argument is passed as a Datum, to indicate that
 *	it may still be in compressed/toasted form.  We can avoid detoasting all
 *	of it in some cases.
 *
 *	The result is always a freshly palloc'd datum.
 */
static text *
text_substring(Datum str, int32 start, int32 length, bool length_not_specified)
{
    int32		eml = pg_database_encoding_max_length();
    int32		S = start;		/* start position */
	int32		S1;				/* adjusted start position */
	int32		L1;				/* adjusted substring length */
	int32		E;				/* end position */
    int len;

	/*
	 * SQL99 says S can be zero or negative, but we still must fetch from the
	 * start of the string.
	 */
	//S1 = Max(S, 1);

    len = DatumGetInt32(DirectFunctionCall1(textlen, str));

    if ((start == 0) || 
        ((start < 0) && (len < -start)))
    {
        return cstring_to_text("");
    }
    if (!length_not_specified && (length <= 0))
    {
        return cstring_to_text("");
    }
    //length = length < 0 ? 0 : length;

    if (start < 0)
    {
        S = len + 1 + start;
    }

    S1 = Max(S, 1);

	/* life is easy if the encoding max length is 1 */
	if (eml == 1)
	{
		if (length_not_specified)	/* special case - get length to end of
									 * string */
			L1 = -1;
		else if (length < 0)
		{
			/* SQL99 says to throw an error for E < S, i.e., negative length */
			ereport(ERROR,
					(errcode(ERRCODE_SUBSTRING_ERROR),
					 errmsg("negative substring length not allowed")));
			L1 = -1;			/* silence stupider compilers */
		}
		else if (pg_add_s32_overflow(S, length, &E))
		{
			/*
			 * L could be large enough for S + L to overflow, in which case
			 * the substring must run to end of string.
			 */
			L1 = -1;
		}
		else
		{
			/*
			 * A zero or negative value for the end position can happen if the
			 * start was negative or one. SQL99 says to return a zero-length
			 * string.
			 */
			if (E < 1)
				return cstring_to_text("");

			L1 = E - S1;
		}

		/*
		 * If the start position is past the end of the string, SQL99 says to
		 * return a zero-length string -- DatumGetTextPSlice() will do that
		 * for us.  We need only convert S1 to zero-based starting position.
		 */
		return DatumGetTextPSlice(str, S1 - 1, L1);
	}
	else if (eml > 1)
	{
		/*
		 * When encoding max length is > 1, we can't get LC without
		 * detoasting, so we'll grab a conservatively large slice now and go
		 * back later to do the right thing
		 */
		int32		slice_start;
		int32		slice_size;
		int32		slice_strlen;
		text	   *slice;
		int32		E1;
		int32		i;
		char	   *p;
		char	   *s;
		text	   *ret;

		/*
		 * We need to start at position zero because there is no way to know
		 * in advance which byte offset corresponds to the supplied start
		 * position.
		 */
		slice_start = 0;

		if (length_not_specified)	/* special case - get length to end of
									 * string */
			slice_size = L1 = -1;
		else if (length < 0)
		{
			/* SQL99 says to throw an error for E < S, i.e., negative length */
			ereport(ERROR,
					(errcode(ERRCODE_SUBSTRING_ERROR),
					 errmsg("negative substring length not allowed")));
			slice_size = L1 = -1;	/* silence stupider compilers */
		}
		else if (pg_add_s32_overflow(S, length, &E))
		{
			/*
			 * L could be large enough for S + L to overflow, in which case
			 * the substring must run to end of string.
			 */
			slice_size = L1 = -1;
		}
		else
		{
			/*
			 * A zero or negative value for the end position can happen if the
			 * start was negative or one. SQL99 says to return a zero-length
			 * string.
			 */
			if (E < 1)
				return cstring_to_text("");

			/*
			 * if E is past the end of the string, the tuple toaster will
			 * truncate the length for us
			 */
			L1 = E - S1;

			/*
			 * Total slice size in bytes can't be any longer than the start
			 * position plus substring length times the encoding max length.
			 * If that overflows, we can just use -1.
			 */
			if (pg_mul_s32_overflow(E, eml, &slice_size))
				slice_size = -1;
		}

		/*
		 * If we're working with an untoasted source, no need to do an extra
		 * copying step.
		 */
		if (VARATT_IS_COMPRESSED(DatumGetPointer(str)) ||
			VARATT_IS_EXTERNAL(DatumGetPointer(str)))
			slice = DatumGetTextPSlice(str, slice_start, slice_size);
		else
			slice = (text *) DatumGetPointer(str);

		/* see if we got back an empty string */
		if (VARSIZE_ANY_EXHDR(slice) == 0)
		{
			if (slice != (text *) DatumGetPointer(str))
				pfree(slice);
			return cstring_to_text("");
		}

		/* Now we can get the actual length of the slice in MB characters */
		slice_strlen = pg_mbstrlen_with_len(VARDATA_ANY(slice),
											VARSIZE_ANY_EXHDR(slice));

		/*
		 * Check that the start position wasn't > slice_strlen. If so, SQL99
		 * says to return a zero-length string.
		 */
		if (S1 > slice_strlen)
		{
			if (slice != (text *) DatumGetPointer(str))
				pfree(slice);
			return cstring_to_text("");
		}

		/*
		 * Adjust L1 and E1 now that we know the slice string length. Again
		 * remember that S1 is one based, and slice_start is zero based.
		 */
		if (L1 > -1)
			E1 = Min(S1 + L1, slice_start + 1 + slice_strlen);
		else
			E1 = slice_start + 1 + slice_strlen;

		/*
		 * Find the start position in the slice; remember S1 is not zero based
		 */
		p = VARDATA_ANY(slice);
		for (i = 0; i < S1 - 1; i++)
			p += pg_mblen(p);

		/* hang onto a pointer to our start position */
		s = p;

		/*
		 * Count the actual bytes used by the substring of the requested
		 * length.
		 */
		for (i = S1; i < E1; i++)
			p += pg_mblen(p);

		ret = (text *) palloc(VARHDRSZ + (p - s));
		SET_VARSIZE(ret, VARHDRSZ + (p - s));
		memcpy(VARDATA(ret), s, (p - s));

		if (slice != (text *) DatumGetPointer(str))
			pfree(slice);

		return ret;
	}
	else
		elog(ERROR, "invalid backend encoding: encoding max length < 1");

	/* not reached: suppress compiler warning */
	return NULL;
}

PG_FUNCTION_INFO_V1(text_concat);

Datum
text_concat(PG_FUNCTION_ARGS)
{
	text	   *result;

	result = concat_internal("", 0, fcinfo);
	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(text_concat_ws);

Datum
text_concat_ws(PG_FUNCTION_ARGS)
{
    char *sep;
	text *result;

    /* return NULL when separator is NULL */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	sep = text_to_cstring(PG_GETARG_TEXT_PP(0));

	result = concat_ws_internal(sep, 1, fcinfo);
	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

/*
 * Implementation of both concat() and concat_ws().
 *
 * sepstr is the separator string to place between values.
 * argidx identifies the first argument to concatenate (counting from zero);
 * note that this must be constant across any one series of calls.
 *
 * Returns NULL if result should be NULL, else text value.
 */
static text *
concat_internal(const char *sepstr, int argidx,
				FunctionCallInfo fcinfo)
{
	text	   *result;
	StringInfoData str;
	FmgrInfo   *foutcache;
	bool		first_arg = true;
	int			i;

	/* Normal case without explicit VARIADIC marker */
	initStringInfo(&str);

	/* Get output function info, building it if first time through */
	foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
	if (foutcache == NULL)
		foutcache = build_concat_foutcache(fcinfo, argidx);

	for (i = argidx; i < PG_NARGS(); i++)
	{
		if (!PG_ARGISNULL(i))
		{
			Datum value = PG_GETARG_DATUM(i);
            Oid valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
            Oid valBaseType = getBaseType(valtype);

			/* add separator if appropriate */
			if (first_arg)
				first_arg = false;
			else
				appendStringInfoString(&str, sepstr);

			/* call the appropriate type output function, append the result */
            if (valBaseType != BYTEAOID)
            {
                appendStringInfoString(&str,
                                       OutputFunctionCall(&foutcache[i], value));
            }
            else
            {
                bytea *vlena = DatumGetByteaPP(value);
                char *byte = VARDATA_ANY(vlena);
                size_t byteLen = VARSIZE_ANY_EXHDR(vlena);

                pg_verifymbstr(byte, byteLen, false);
                appendBinaryStringInfo(&str, byte, byteLen);
            }
		}
        else
        {
            pfree(str.data);
            return NULL;
        }
	}

	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	return result;
}

static text *
concat_ws_internal(const char *sepstr, int argidx, FunctionCallInfo fcinfo)
{
	text	   *result;
	StringInfoData str;
	FmgrInfo   *foutcache;
	bool		first_arg = true;
	int			i;

	/* Normal case without explicit VARIADIC marker */
	initStringInfo(&str);

	/* Get output function info, building it if first time through */
	foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
	if (foutcache == NULL)
		foutcache = build_concat_foutcache(fcinfo, argidx);

	for (i = argidx; i < PG_NARGS(); i++)
	{
		if (!PG_ARGISNULL(i))
		{
			Datum value = PG_GETARG_DATUM(i);
            Oid valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
            Oid valBaseType = getBaseType(valtype);

			/* add separator if appropriate */
			if (first_arg)
				first_arg = false;
			else
				appendStringInfoString(&str, sepstr);

			/* call the appropriate type output function, append the result */
            if (valBaseType != BYTEAOID)
            {
                appendStringInfoString(&str,
                                       OutputFunctionCall(&foutcache[i], value));
            }
            else
            {
                bytea *vlena = DatumGetByteaPP(value);
                char *byte = VARDATA_ANY(vlena);
                size_t byteLen = VARSIZE_ANY_EXHDR(vlena);

                pg_verifymbstr(byte, byteLen, false);
                appendBinaryStringInfo(&str, byte, byteLen);
            }
		}
        else
        {
            continue;
        }
	}

	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	return result;
}
/*
 * Prepare cache with fmgr info for the output functions of the datatypes of
 * the arguments of a concat-like function, beginning with argument "argidx".
 * (Arguments before that will have corresponding slots in the resulting
 * FmgrInfo array, but we don't fill those slots.)
 */
static FmgrInfo *
build_concat_foutcache(FunctionCallInfo fcinfo, int argidx)
{
	FmgrInfo   *foutcache;
	int			i;

	/* We keep the info in fn_mcxt so it survives across calls */
	foutcache = (FmgrInfo *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
												PG_NARGS() * sizeof(FmgrInfo));

	for (i = argidx; i < PG_NARGS(); i++)
	{
		Oid			valtype;
		Oid			typOutput;
		bool		typIsVarlena;

		valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
		if (!OidIsValid(valtype))
			elog(ERROR, "could not determine data type of concat() input");

		getTypeOutputInfo(valtype, &typOutput, &typIsVarlena);
		fmgr_info_cxt(typOutput, &foutcache[i], fcinfo->flinfo->fn_mcxt);
	}

	fcinfo->flinfo->fn_extra = foutcache;

	return foutcache;
}

static int
text_position(text *t1, text *t2, Oid collid)
{
	TextPositionState state;
	int			result;
    char *backupBuf1 = NULL;
    char *backupBuf2 = NULL;
    bool lowered = false;

	/* Empty needle always matches at position 1 */
	if (VARSIZE_ANY_EXHDR(t2) < 1)
		return 1;

	/* Otherwise, can't match if haystack is shorter than needle */
	if (VARSIZE_ANY_EXHDR(t1) < VARSIZE_ANY_EXHDR(t2))
		return 0;

    if (collid == caseInsensitiveId)
    {
        lowered = lowerStrs(t1, t2, &backupBuf1, &backupBuf2);
        collid = 100;   /* 临时修改collid，绕过nondeterministic collations are not supported for substring searches错误*/
    }
    else 
    {
        if (halo_mysql_ci_collation)
        {
            lowered = lowerStrs(t1, t2, &backupBuf1, &backupBuf2);
        }
    }

	text_position_setup(t1, t2, collid, &state);
	if (!text_position_next(collid, &state))
		result = 0;
	else
		result = text_position_get_match_pos(&state);

    if (lowered)
    {
        restoreStrs(backupBuf1, backupBuf2, t1, t2);
    }

    text_position_cleanup(&state);
    return result;
}


static void
text_position_setup(text *t1, text *t2, Oid collid, TextPositionState *state)
{
	int			len1 = VARSIZE_ANY_EXHDR(t1);
	int			len2 = VARSIZE_ANY_EXHDR(t2);
	pg_locale_t mylocale = 0;

	check_collation_set(collid);

	if (!lc_collate_is_c(collid) && collid != DEFAULT_COLLATION_OID)
		mylocale = pg_newlocale_from_collation(collid);

	if (mylocale && !mylocale->deterministic)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations are not supported for substring searches")));

	Assert(len1 > 0);
	Assert(len2 > 0);

	/*
	 * Even with a multi-byte encoding, we perform the search using the raw
	 * byte sequence, ignoring multibyte issues.  For UTF-8, that works fine,
	 * because in UTF-8 the byte sequence of one character cannot contain
	 * another character.  For other multi-byte encodings, we do the search
	 * initially as a simple byte search, ignoring multibyte issues, but
	 * verify afterwards that the match we found is at a character boundary,
	 * and continue the search if it was a false match.
	 */
	if (pg_database_encoding_max_length() == 1)
	{
		state->is_multibyte = false;
		state->is_multibyte_char_in_char = false;
	}
	else if (GetDatabaseEncoding() == PG_UTF8)
	{
		state->is_multibyte = true;
		state->is_multibyte_char_in_char = false;
	}
	else
	{
		state->is_multibyte = true;
		state->is_multibyte_char_in_char = true;
	}

	state->str1 = VARDATA_ANY(t1);
	state->str2 = VARDATA_ANY(t2);
	state->len1 = len1;
	state->len2 = len2;
	state->last_match = NULL;
	state->refpoint = state->str1;
	state->refpos = 0;
    state->skiptablemask = 255;

	/*
	 * Prepare the skip table for Boyer-Moore-Horspool searching.  In these
	 * notes we use the terminology that the "haystack" is the string to be
	 * searched (t1) and the "needle" is the pattern being sought (t2).
	 *
	 * If the needle is empty or bigger than the haystack then there is no
	 * point in wasting cycles initializing the table.  We also choose not to
	 * use B-M-H for needles of length 1, since the skip table can't possibly
	 * save anything in that case.
	 */
	if (len1 >= len2 && len2 > 1)
	{
		int			searchlength = len1 - len2;
		int			skiptablemask;
		int			last;
		int			i;
		const char *str2 = state->str2;

		/*
		 * First we must determine how much of the skip table to use.  The
		 * declaration of TextPositionState allows up to 256 elements, but for
		 * short search problems we don't really want to have to initialize so
		 * many elements --- it would take too long in comparison to the
		 * actual search time.  So we choose a useful skip table size based on
		 * the haystack length minus the needle length.  The closer the needle
		 * length is to the haystack length the less useful skipping becomes.
		 *
		 * Note: since we use bit-masking to select table elements, the skip
		 * table size MUST be a power of 2, and so the mask must be 2^N-1.
		 */
		if (searchlength < 16)
			skiptablemask = 3;
		else if (searchlength < 64)
			skiptablemask = 7;
		else if (searchlength < 128)
			skiptablemask = 15;
		else if (searchlength < 512)
			skiptablemask = 31;
		else if (searchlength < 2048)
			skiptablemask = 63;
		else if (searchlength < 4096)
			skiptablemask = 127;
		else
			skiptablemask = 255;
		state->skiptablemask = skiptablemask;

		/*
		 * Initialize the skip table.  We set all elements to the needle
		 * length, since this is the correct skip distance for any character
		 * not found in the needle.
		 */
		for (i = 0; i <= skiptablemask; i++)
			state->skiptable[i] = len2;

		/*
		 * Now examine the needle.  For each character except the last one,
		 * set the corresponding table element to the appropriate skip
		 * distance.  Note that when two characters share the same skip table
		 * entry, the one later in the needle must determine the skip
		 * distance.
		 */
		last = len2 - 1;

		for (i = 0; i < last; i++)
			state->skiptable[(unsigned char) str2[i] & skiptablemask] = last - i;
	}
}

/*
 * Advance to the next match, starting from the end of the previous match
 * (or the beginning of the string, on first call).  Returns true if a match
 * is found.
 *
 * Note that this refuses to match an empty-string needle.  Most callers
 * will have handled that case specially and we'll never see it here.
 */
static bool
text_position_next(Oid collid, TextPositionState *state)
{
	int			needle_len = state->len2;
	char	   *start_ptr;
	char	   *matchptr;

	if (needle_len <= 0)
		return false;			/* result for empty pattern */

	/* Start from the point right after the previous match. */
	if (state->last_match)
		start_ptr = state->last_match + needle_len;
	else
		start_ptr = state->str1;

retry:
	matchptr = text_position_next_internal(collid, start_ptr, state);

	if (!matchptr)
		return false;

	/*
	 * Found a match for the byte sequence.  If this is a multibyte encoding,
	 * where one character's byte sequence can appear inside a longer
	 * multi-byte character, we need to verify that the match was at a
	 * character boundary, not in the middle of a multi-byte character.
	 */
	if (state->is_multibyte_char_in_char)
	{
		/* Walk one character at a time, until we reach the match. */

		/* the search should never move backwards. */
		Assert(state->refpoint <= matchptr);

		while (state->refpoint < matchptr)
		{
			/* step to next character. */
			state->refpoint += pg_mblen(state->refpoint);
			state->refpos++;

			/*
			 * If we stepped over the match's start position, then it was a
			 * false positive, where the byte sequence appeared in the middle
			 * of a multi-byte character.  Skip it, and continue the search at
			 * the next character boundary.
			 */
			if (state->refpoint > matchptr)
			{
				start_ptr = state->refpoint;
				goto retry;
			}
		}
	}

	state->last_match = matchptr;
	return true;
}

/*
 * Subroutine of text_position_next().  This searches for the raw byte
 * sequence, ignoring any multi-byte encoding issues.  Returns the first
 * match starting at 'start_ptr', or NULL if no match is found.
 */
static char *
text_position_next_internal(Oid collid, char *start_ptr, TextPositionState *state)
{
	int			haystack_len = state->len1;
	int			needle_len = state->len2;
	int			skiptablemask = state->skiptablemask;
	const char *haystack = state->str1;
	const char *needle = state->str2;
	const char *haystack_end = &haystack[haystack_len];
	const char *hptr;

	Assert(start_ptr >= haystack && start_ptr <= haystack_end);

	if (needle_len == 1)
	{
		/* No point in using B-M-H for a one-character needle */
		char		nchar = *needle;

		hptr = start_ptr;
        if ((collid == caseInsensitiveId) || halo_mysql_ci_collation)
        {
            while (hptr < haystack_end)
            {
                if ((*hptr == nchar) || 
                    (('a' <= *hptr) && (*hptr <= 'z') && ((*hptr - 32 ) == nchar)) || 
                    (('A' <= *hptr) && (*hptr <= 'Z') && ((*hptr + 32 ) == nchar)))
                {
                    return (char *) hptr;
                }
                hptr++;
            }
        }
        else 
        {
            while (hptr < haystack_end)
            {
                if (*hptr == nchar)
                {
                    return (char *) hptr;
                }
                hptr++;
            }
        }
    }
    else
    {
        const char *needle_last = &needle[needle_len - 1];

        /* Start at startpos plus the length of the needle */
        hptr = start_ptr + needle_len - 1;
        if ((collid == caseInsensitiveId) || halo_mysql_ci_collation)
        {
            while (hptr < haystack_end)
            {
                /* Match the needle scanning *backward* */
                const char *nptr;
                const char *p;

                nptr = needle_last;
                p = hptr;
                while ((*nptr == *p) || 
                       (('a' <= *nptr) && (*nptr <= 'z') && ((*nptr - 32) == *p)) || 
                       (('A' <= *nptr) && (*nptr <= 'Z') && ((*nptr + 32) == *p)))
                {
                    /* Matched it all?	If so, return 1-based position */
                    if (nptr == needle)
                        return (char *) p;
                    nptr--, p--;
                }

                /*
                 * No match, so use the haystack char at hptr to decide how far to
                 * advance.  If the needle had any occurrence of that character
                 * (or more precisely, one sharing the same skiptable entry)
                 * before its last character, then we advance far enough to align
                 * the last such needle character with that haystack position.
                 * Otherwise we can advance by the whole needle length.
                 */
                hptr += state->skiptable[(unsigned char) *hptr & skiptablemask];
            }
        }
        else 
        {
            while (hptr < haystack_end)
            {
                /* Match the needle scanning *backward* */
                const char *nptr;
                const char *p;

                nptr = needle_last;
                p = hptr;
                while (*nptr == *p)
                {
                    /* Matched it all?	If so, return 1-based position */
                    if (nptr == needle)
                        return (char *) p;
                    nptr--, p--;
                }

                /*
                 * No match, so use the haystack char at hptr to decide how far to
                 * advance.  If the needle had any occurrence of that character
                 * (or more precisely, one sharing the same skiptable entry)
                 * before its last character, then we advance far enough to align
                 * the last such needle character with that haystack position.
                 * Otherwise we can advance by the whole needle length.
                 */
                hptr += state->skiptable[(unsigned char) *hptr & skiptablemask];
            }
        }
    }

    return 0;					/* not found */
}

static int
text_position_get_match_pos(TextPositionState *state)
{
	if (!state->is_multibyte)
		return state->last_match - state->str1 + 1;
	else
	{
		/* Convert the byte position to char position. */
		while (state->refpoint < state->last_match)
		{
			state->refpoint += pg_mblen(state->refpoint);
			state->refpos++;
		}
		Assert(state->refpoint == state->last_match);
		return state->refpos + 1;
	}
}

static void
text_position_cleanup(TextPositionState *state)
{
	/* no cleanup needed */
}

static void
check_collation_set(Oid collid)
{
	if (!OidIsValid(collid))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string comparison"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}
}

static void 
binary_to_hex(char ch0, char ch1, char ch2, char ch3, char* ch)
{
    if (ch0 == '0')     
    {
        if (ch1 == '0') 
        {
            if (ch2 == '0')
            {
                if (ch3 == '0')
                {
                    *ch = '0';
                }
                else 
                {
                    *ch = '1';
                }
            }
            else 
            {
                if (ch3 == '0')
                {
                    *ch = '2';
                }
                else 
                {
                    *ch = '3';
                }
            }
        }
        else
        {
            if (ch2 == '0')
            {
                if (ch3 == '0')
                {
                    *ch = '4';
                }
                else 
                {
                    *ch = '5';
                }
            }
            else 
            {
                if (ch3 == '0')
                {
                    *ch = '6';
                }
                else 
                {
                    *ch = '7';
                }
            }
        }
    }   
    else   
    {
        if (ch1 == '0')
        {
            if (ch2 == '0')
            {
                if (ch3 == '0')
                {
                    *ch = '8';
                }
                else 
                {
                    *ch = '9';
                }
            }
            else 
            {
                if (ch3 == '0')
                {
                    *ch = 'A';
                }
                else 
                {
                    *ch = 'B';
                }
            }
        }
        else
        {
            if (ch2 == '0')
            {
                if (ch3 == '0')
                {
                    *ch = 'C';
                }
                else 
                {
                    *ch = 'D';
                }
            }
            else 
            {
                if (ch3 == '0')
                {
                    *ch = 'E';
                }
                else 
                {
                    *ch = 'F';
                }
            }
        }
    }
}

//static void
//convert_to_int_text_for_mysql(char **str, int str_len)
//{
//    char* ptr = *str;
//    int i = 0;
//
//    if (str_len <= 0)
//    {
//        return;
//    }
//
//    for (i = 0; i < str_len; i++)
//    {
//        if (ptr[i] != ' ')
//        {
//            break;
//        }
//    }
//    if (0 < i)
//    {
//        ptr += i;
//        str_len -= i;
//        i = 0;
//    }
//
//    if (str_len <= 0)
//    {
//        (*str)[0] = '0';
//        (*str)[1] = '\0';
//    }
//    else 
//    {
//        if (isdigit(ptr[0]))
//        {
//            for (i = 0; i < str_len; i++)
//            {
//                if (ptr[i] != '0')
//                {
//                    break;
//                }
//            }
//            if (0 < i)
//            {
//                ptr += i;
//                str_len -= i;
//            }
//
//            for (i = 0; i < str_len; i++)
//            {
//                if (!isdigit(ptr[i]))
//                {
//                    ptr[i] = '\0';
//                    break;
//                }
//            }
//            if (i <= 0)
//            {
//                //  00(ptr)
//                //00(ptr)
//                (*str)[0] = '0';
//                (*str)[1] = '\0';
//            }
//            else 
//            {
//                //1
//                //1a
//                //  00(ptr)123
//                //00(ptr)123
//                *str = ptr;
//            }
//        }   // if (isdigit(ptr[0]))
//        else if (ptr[0] == '+')
//        {
//            ++ptr;
//            --str_len;
//
//            for (i = 0; i < str_len; i++)
//            {
//                if (ptr[i] != '0')
//                {
//                    break;
//                }
//            }
//            if (0 < i)
//            {
//                ptr += i;
//                str_len -= i;
//            }
//
//            for (i = 0; i < str_len; i++)
//            {
//                if (!isdigit(ptr[i]))
//                {
//                    ptr[i] = '\0';
//                    break;
//                }
//            }
//            if (i <= 0)
//            {
//                (*str)[0] = '0';
//                (*str)[1] = '\0';
//            }
//            else 
//            {
//                *str = ptr;
//            }
//        }   // else if (ptr[0] == '+')
//        else if (ptr[0] == '-')
//        {
//            for (i = 1; i < str_len; i++)
//            {
//                if (ptr[i] != '0')
//                {
//                    break;
//                }
//            }
//            if (i <= 1)
//            {
//                for (i = 1; i < str_len; i++)
//                {
//                    if (!isdigit(ptr[i]))
//                    {
//                        ptr[i] = '\0';
//                        break;
//                    }
//                }
//                if (1 < i)
//                {
//                    // -1ab
//                    // -123
//                    *str = ptr;
//                }
//                else 
//                {
//                    // -abc
//                    (*str)[0] = '0';
//                    (*str)[1] = '\0';
//                }
//            }
//            else
//            {
//                // -00abc
//                // -00123
//                // -001ab
//                // -00123abc
//                ptr += i;
//                for (i = 0; i < str_len; i++)
//                {
//                    if (!isdigit(ptr[i]))
//                    {
//                        ptr[i] = '\0';
//                        break;
//                    }
//                }
//                if (0 < i)
//                {
//                    // -00(ptr)123
//                    // -00(ptr)1ab
//                    // -00(ptr)123abc
//                    int j = 0;
//                    int k = 0;
//                    (*str)[j++] = '-';
//                    for (k = 0; k < i; k++)
//                    {
//                        (*str)[j++] = ptr[k];
//                    }
//                    (*str)[j] = '\0';
//                }
//                else 
//                {
//                    (*str)[0] = '0';
//                    (*str)[1] = '\0';
//                }
//            }
//        }   // else if (ptr[0] == '-')
//        else 
//        {
//            (*str)[0] = '0';
//            (*str)[1] = '\0';
//        }
//    }
//}


static void
convert_to_digit_text_for_mysql(char **str, int str_len)
{
    char* ptr = *str;
    int dotNum = 0;
    int i = 0;

    if (str_len <= 0)
    {
        return;
    }

    for (i = 0; i < str_len; i++)
    {
        if (ptr[i] != ' ')
        {
            break;
        }
    }
    if (0 < i)
    {
        ptr += i;
        str_len -= i;
        i = 0;
    }

    if (str_len <= 0)
    {
        (*str)[0] = '0';
        (*str)[1] = '\0';
    }
    else 
    {
        if (isdigit(ptr[0]))
        {
            for (i = 0; i < str_len; i++)
            {
                if (ptr[i] != '0')
                {
                    break;
                }
            }
            if (0 < i)
            {
                ptr += i;
                str_len -= i;
            }

            for (i = 0; i < str_len; i++)
            {
                if (!isdigit(ptr[i]))
                {
                    if (ptr[i] == '.')
                    {
                        ++dotNum;
                        if (1 < dotNum)
                        {
                            ptr[i] = '\0';
                            break;
                        }
                    }
                    else 
                    {
                        ptr[i] = '\0';
                        break;
                    }
                }
            }
            if (i <= 0)
            {
                (*str)[0] = '0';
                (*str)[1] = '\0';
            }
            else 
            {
                *str = ptr;
            }
        }   // if (isdigit(ptr[0]))
        else if (ptr[0] == '+')
        {
            ++ptr;
            --str_len;

            for (i = 0; i < str_len; i++)
            {
                if (ptr[i] != '0')
                {
                    break;
                }
            }
            if (0 < i)
            {
                ptr += i;
                str_len -= i;
            }

            for (i = 0; i < str_len; i++)
            {
                if (!isdigit(ptr[i]))
                {
                    if (ptr[i] == '.')
                    {
                        ++dotNum;
                        if (1 < dotNum)
                        {
                            ptr[i] = '\0';
                            break;
                        }
                    }
                    else 
                    {
                        ptr[i] = '\0';
                        break;
                    }
                }
            }
            if (i <= 0)
            {
                (*str)[0] = '0';
                (*str)[1] = '\0';
            }
            else 
            {
                *str = ptr;
            }
        }   // else if (ptr[0] == '+')
        else if (ptr[0] == '-')
        {
            for (i = 1; i < str_len; i++)
            {
                if (ptr[i] != '0')
                {
                    break;
                }
            }
            if (i <= 1)
            {
                for (i = 1; i < str_len; i++)
                {
                    if (!isdigit(ptr[i]))
                    {
                        if (ptr[i] == '.')
                        {
                            ++dotNum;
                            if (1 < dotNum)
                            {
                                ptr[i] = '\0';
                                break;
                            }
                        }
                        else 
                        {
                            ptr[i] = '\0';
                            break;
                        }
                    }
                }
                if (1 < i)
                {
                    *str = ptr;
                }
                else 
                {
                    (*str)[0] = '0';
                    (*str)[1] = '\0';
                }
            }
            else
            {
                ptr += i;
                for (i = 0; i < str_len; i++)
                {
                    if (!isdigit(ptr[i]))
                    {
                        if (ptr[i] == '.')
                        {
                            ++dotNum;
                            if (1 < dotNum)
                            {
                                ptr[i] = '\0';
                                break;
                            }
                        }
                        else 
                        {
                            ptr[i] = '\0';
                            break;
                        }
                    }
                }
                if (0 < i)
                {
                    int j = 0;
                    int k = 0;
                    (*str)[j++] = '-';
                    for (k = 0; k < i; k++)
                    {
                        (*str)[j++] = ptr[k];
                    }
                    (*str)[j] = '\0';
                }
                else 
                {
                    (*str)[0] = '0';
                    (*str)[1] = '\0';
                }
            }
        }   // else if (ptr[0] == '-')
        else if (ptr[0] == '.')
        {
            for (i = 1; i < str_len; i++)
            {
                if (!isdigit(ptr[i]))
                {
                    ptr[i] = '\0';
                    break;
                }
            }

            if (i <= 1)
            {
                (*str)[0] = '0';
                (*str)[1] = '\0';
            }
            else 
            {
                *str = ptr;
            }
        }
        else 
        {
            (*str)[0] = '0';
            (*str)[1] = '\0';
        }
    }
}


static void
convert_to_text_for_mysql(char **str, int str_len)
{
    char* ptr = *str;
    int i = 0;

    if (str_len <= 0)
    {
        return;
    }

    if (isdigit(ptr[0]))
    {
        for (i = 0; i < str_len; i++)
        {
            if (ptr[i] != '0')
            {
                break;
            }
        }

        ptr += i;
        str_len -= i;

        if (0 < str_len)
        {
            *str = ptr;
        }
        else
        {
            (*str)[0] = '0';
            (*str)[1] = '\0';
        }
    }   // if (isdigit(ptr[0]))
    else if (ptr[0] == '+')
    {
        ++ptr;
        --str_len;

        for (i = 0; i < str_len; i++)
        {
            if (ptr[i] != '0')
            {
                break;
            }
        }

        ptr += i;
        str_len -= i;

        if (0 < str_len)
        {
            *str = ptr;
        }
        else 
        {
            (*str)[0] = '0';
            (*str)[1] = '\0';
        }
    }   // else if (ptr[0] == '+')
    else if (ptr[0] == '-')
    {
        for (i = 1; i < str_len; i++)
        {
            if (ptr[i] != '0')
            {
                break;
            }
        }

        if (i == 1)
        {
            *str = ptr;
        }
        else
        {
            ptr += i;
            str_len -= i;
            if (0 < str_len)
            {
                int j = 0;
                (*str)[j++] = '-';
                for (i = 0; i < str_len; i++)
                {
                    (*str)[j++] = ptr[i];
                }
                (*str)[j] = '\0';
            }
            else 
            {
                (*str)[0] = '0';
                (*str)[1] = '\0';
            }
        }
    }   // else if (ptr[0] == '-')
    else 
    {
        (*str)[0] = '0';
        (*str)[1] = '\0';
    }
}


PG_FUNCTION_INFO_V1(rowCount);
Datum
rowCount(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64(affectedRows);
}


PG_FUNCTION_INFO_V1(mysFoundRows);
Datum
mysFoundRows(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64(foundRows);
}

PG_FUNCTION_INFO_V1(mysLastInsertId);
Datum
mysLastInsertId(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64(lastInsertID);
}


PG_FUNCTION_INFO_V1(substringIndex);
Datum
substringIndex(PG_FUNCTION_ARGS)
{
    char *ret = NULL;

    char *str = TextDatumGetCString(PG_GETARG_DATUM(0));
    int strLen = strlen(str);
    char *separator = TextDatumGetCString(PG_GETARG_DATUM(1));
    int separatorLen = strlen(separator);
    int index = DatumGetInt32(PG_GETARG_DATUM(2));

    if (0 < index)
    {
        ret = str;
        for (int i = 0; i < strLen; i++)
        {
            if ((separatorLen <= (strLen - i)) && 
                (strncmp((str + i), separator, separatorLen) == 0))
            {
                index--;
                if (0 == index)
                {
                    str[i] = '\0';
                    ret = str;
                    break;
                }
                else 
                {
                    i += (separatorLen - 1);
                }
            }
        }
    }
    else if (0 == index)
    {
        str[0] = '\0';
        ret = str;
    }
    else 
    {
        ret = str;
        // SELECT SUBSTRING_INDEX('15,151,152,16', ',', -1);
        for (int i = (strLen - separatorLen); 0 <= i; i--)
        {
            if (strncmp((str + i), separator, separatorLen) == 0)
            {
                index++;
                if (0 == index)
                {
                    ret = str + i + 1;
                    break;
                }
                else 
                {
                    i -= (separatorLen - 1);
                }
            }
        }
    }

    PG_RETURN_TEXT_P(cstring_to_text(ret));
}


PG_FUNCTION_INFO_V1(mysVersion);
Datum
mysVersion(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text(halo_mysql_version));
}

PG_FUNCTION_INFO_V1(isValidBinary);
Datum
isValidBinary(PG_FUNCTION_ARGS)
{
    char *str = TextDatumGetCString(PG_GETARG_DATUM(0));
    int strLen = strlen(str);
    for (int i = 0; i < strLen; i++)
    {
        if ((str[i] != '0') && (str[i] != '1'))
        {
            PG_RETURN_BOOL(false);
        }
    }

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(isValidHex);
Datum
isValidHex(PG_FUNCTION_ARGS)
{
    char *str = TextDatumGetCString(PG_GETARG_DATUM(0));
    int strLen = strlen(str);
    for (int i = 0; i < strLen; i++)
    {
        if ((str[i] != '0') && 
            (str[i] != '1') && 
            (str[i] != '2') && 
            (str[i] != '3') && 
            (str[i] != '4') && 
            (str[i] != '5') && 
            (str[i] != '6') && 
            (str[i] != '7') && 
            (str[i] != '8') && 
            (str[i] != '9') && 
            (str[i] != 'a') && 
            (str[i] != 'A') && 
            (str[i] != 'b') && 
            (str[i] != 'B') && 
            (str[i] != 'c') && 
            (str[i] != 'C') && 
            (str[i] != 'd') && 
            (str[i] != 'D') && 
            (str[i] != 'e') && 
            (str[i] != 'E') && 
            (str[i] != 'f') && 
            (str[i] != 'F')) 
        {
            PG_RETURN_BOOL(false);
        }
    }

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(convertTextHexToTextVarbit);
Datum
convertTextHexToTextVarbit(PG_FUNCTION_ARGS)
{
    char *ret;

    char *data = TextDatumGetCString(PG_GETARG_DATUM(0));
    int dataLen = strlen(data);
    int retSize = dataLen * 4 + 1;
    int j = 0;

    if (dataLen <= 16)
    {
        ret = palloc(retSize);
        for (int i = 0; i < dataLen; i++)
        {
            if (data[i] == '0')
            {
                ret[j++] = '0';
                ret[j++] = '0';
                ret[j++] = '0';
                ret[j++] = '0';
            }
            else if (data[i] == '1')
            {
                ret[j++] = '0';
                ret[j++] = '0';
                ret[j++] = '0';
                ret[j++] = '1';
            }
            else if (data[i] == '2')
            {
                ret[j++] = '0';
                ret[j++] = '0';
                ret[j++] = '1';
                ret[j++] = '0';
            }
            else if (data[i] == '3')
            {
                ret[j++] = '0';
                ret[j++] = '0';
                ret[j++] = '1';
                ret[j++] = '1';
            }
            else if (data[i] == '4')
            {
                ret[j++] = '0';
                ret[j++] = '1';
                ret[j++] = '0';
                ret[j++] = '0';
            }
            else if (data[i] == '5')
            {
                ret[j++] = '0';
                ret[j++] = '1';
                ret[j++] = '0';
                ret[j++] = '1';
            }
            else if (data[i] == '6')
            {
                ret[j++] = '0';
                ret[j++] = '1';
                ret[j++] = '1';
                ret[j++] = '0';
            }
            else if (data[i] == '7')
            {
                ret[j++] = '0';
                ret[j++] = '1';
                ret[j++] = '1';
                ret[j++] = '1';
            }
            else if (data[i] == '8')
            {
                ret[j++] = '1';
                ret[j++] = '0';
                ret[j++] = '0';
                ret[j++] = '0';
            }
            else if (data[i] == '9')
            {
                ret[j++] = '1';
                ret[j++] = '0';
                ret[j++] = '0';
                ret[j++] = '1';
            }
            else if ((data[i] == 'a') || (data[i] == 'A'))
            {
                ret[j++] = '1';
                ret[j++] = '0';
                ret[j++] = '1';
                ret[j++] = '0';
            }
            else if ((data[i] == 'b') || (data[i] == 'B'))
            {
                ret[j++] = '1';
                ret[j++] = '0';
                ret[j++] = '1';
                ret[j++] = '1';
            }
            else if ((data[i] == 'c') || (data[i] == 'C'))
            {
                ret[j++] = '1';
                ret[j++] = '1';
                ret[j++] = '0';
                ret[j++] = '0';
            }
            else if ((data[i] == 'd') || (data[i] == 'D'))
            {
                ret[j++] = '1';
                ret[j++] = '1';
                ret[j++] = '0';
                ret[j++] = '1';
            }
            else if ((data[i] == 'e') || (data[i] == 'E'))
            {
                ret[j++] = '1';
                ret[j++] = '1';
                ret[j++] = '1';
                ret[j++] = '0';
            }
            else /* if ((data[i] == 'f') || (data[i] == 'F'))*/
            {
                ret[j++] = '1';
                ret[j++] = '1';
                ret[j++] = '1';
                ret[j++] = '1';
            }
        }
        ret[j++] = '\0';
        for (j = 0; j < (retSize - 2); j++)
        {
            if (ret[j] != '0')
            {
                break;
            }
        }
        ret = ret + j;

        PG_RETURN_TEXT_P(cstring_to_text(ret));
    }
    else 
    {
        PG_RETURN_TEXT_P(cstring_to_text("1111111111111111111111111111111111111111111111111111111111111111"));
    }
}


static bool 
lowerStrs(text *t1, text *t2, char **backupBuf1, char **backupBuf2)
{
    bool ret = false;
    char *str1 = VARDATA_ANY(t1);
    char *str2 = VARDATA_ANY(t2);
    int	len1 = VARSIZE_ANY_EXHDR(t1);
    int	len2 = VARSIZE_ANY_EXHDR(t2);
    for (int i = 0; i < len1; i++)
    {
        if (('A' <= str1[i]) && (str1[i] <= 'Z'))
        {
            if (*backupBuf1 == NULL)
            {
                *backupBuf1 = pstrdup(str1);
            }
            str1[i] += 32;
            ret = true;
        }
    }
    for (int i = 0; i < len2; i++)
    {
        if (('A' <= str2[i]) && (str2[i] <= 'Z'))
        {
            if (*backupBuf2 == NULL)
            {
                *backupBuf2 = pstrdup(str2);
            }
            str2[i] += 32;
            ret = true;
        }
    }

    return ret;
}

static void 
restoreStrs(char *backupBuf1, char *backupBuf2, text *t1, text *t2)
{
    if (backupBuf1 != NULL)
    {
        char *str1 = VARDATA_ANY(t1);
        int	len1 = VARSIZE_ANY_EXHDR(t1);
        memcpy(str1, backupBuf1, len1);
    }
    if (backupBuf2 != NULL)
    {
        char *str2 = VARDATA_ANY(t2);
        int	len2 = VARSIZE_ANY_EXHDR(t2);
        memcpy(str2, backupBuf2, len2);
    }
}
