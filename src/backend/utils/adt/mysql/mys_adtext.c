/*-------------------------------------------------------------------------
 *
 * mys_adtext.c
 *		Extension for MySQL Compatible
 *
 *
 * Copyright (c) 2021, Jet C.X. ZHANG
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/mysql/mys_adtext.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "utils/mysql/mys_adtext.h"
#include "utils/mysql/mys_date.h"
#include "utils/mysql/mys_timestamp.h"


static const ADTExtMethod mys_adtext;


static const ADTExtMethod mys_adtext = {
    .pre_numeric_in = NULL,
    .post_numeric_out = NULL,
    .pre_time_in = mys_pre_time_in,
    .post_time_out = mys_post_time_out,
    .pre_timetz_in = NULL,
	.post_timetz_out = NULL,
    .pre_timestamp_in = NULL,
    .post_timestamp_out = NULL,
    .date_in = mys_date_in,
    .timestamp_in = mys_timestamp_in
};

const ADTExtMethod *
GetMysADTExt(void)
{
    return &mys_adtext;
}