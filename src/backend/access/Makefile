#
# Makefile for the access methods module
#
# src/backend/access/Makefile
#

subdir = src/backend/access
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS	    = brin common gin gist hash heap index nbtree rmgrdesc spgist \
			  table tablesample transam

ifeq ($(with_c2),yes)
SUBDIRS += c2
endif

include $(top_srcdir)/src/backend/common.mk
