/*-------------------------------------------------------------------------
 *
 * printsimple.h
 *	  print simple tuples without catalog access
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/printsimple.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PRINTSIMPLE_H
#define PRINTSIMPLE_H

#include "tcop/dest.h"

extern bool printsimple(TupleTableSlot *slot, DestReceiver *self, CommandTag commandTag);
extern void printsimple_startup(DestReceiver *self, int operation,
								TupleDesc tupdesc, CommandTag commandTag);

#endif							/* PRINTSIMPLE_H */
