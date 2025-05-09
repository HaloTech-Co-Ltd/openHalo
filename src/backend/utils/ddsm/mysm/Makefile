#-------------------------------------------------------------------------
#
# src/backend/utils/ddsm/mysm/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/ddsm/mysm/
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

NAME		= mysm
PGFILEDESC	= "MySQL Supplementary Modules - mysm"

OBJS = \
	aggregates.o \
	bpchar.o \
    common_funcs.o \
	date_func.o \
	time_func.o \
	timestamp_func.o \
	operators.o \
	partListColumns.o \
	strfuncs.o \
	systemVar.o \
	user_var.o \
	user.o \
    uuid_short.o

#include $(srcdir)/../proc.mk
rpath =

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
