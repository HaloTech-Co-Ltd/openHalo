#-------------------------------------------------------------------------
#
# Makefile for MySQL Compatible Parser
#
# src/backend/parser/mysql/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/parser/mysql
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	mys_analyze.o \
	mys_parse_agg.o \
	mys_parse_clause.o \
	mys_parse_expr.o \
	mys_parse_func.o \
	mys_parse_oper.o \
	mys_parse_utilcmd.o \
	mys_keywords.o \
	mys_gram.o \
	mys_scan.o \
	mys_parser.o

include $(top_srcdir)/src/backend/common.mk


# There is no correct way to write a rule that generates two files.
# Rules with two targets don't have that meaning, they are merely
# shorthand for two otherwise separate rules.  If we have an action
# that in fact generates two or more files, we must choose one of them
# as primary and show it as the action's output, then make all of the
# other output files dependent on the primary, like this.  Furthermore,
# the "touch" action is essential, because it ensures that gram.h is
# marked as newer than (or at least no older than) gram.c.  Without that,
# make is likely to try to rebuild gram.h in subsequent runs, which causes
# failures in VPATH builds from tarballs.
# where to find gen_keywordlist.pl and subsidiary files
TOOLSDIR = $(top_srcdir)/src/tools
GEN_KEYWORDLIST = $(PERL) -I $(TOOLSDIR) $(TOOLSDIR)/gen_keywordlist.pl
GEN_KEYWORDLIST_DEPS = $(TOOLSDIR)/gen_keywordlist.pl $(TOOLSDIR)/PerfectHash.pm

mys_kwlist_d.h: $(top_srcdir)/src/include/parser/mysql/mys_kwlist.h $(GEN_KEYWORDLIST_DEPS)
	$(GEN_KEYWORDLIST) --extern --varname=MysScanKeywords $<

mys_gram.h: mys_gram.c
	touch $@

mys_gram.c: BISONFLAGS += -r solved -d -v
mys_gram.c: BISON_CHECK_CMD = $(PERL) $(srcdir)/check_myskeywords.pl $< $(top_srcdir)/src/include/parser/mysql/mys_kwlist.h


mys_scan.c: FLEXFLAGS = -CF -p -p
mys_scan.c: FLEX_NO_BACKUP=yes
mys_scan.c: FLEX_FIX_WARNING=yes


# Force these dependencies to be known even without dependency info built:
mys_keywords.o mys_gram.o mys_scan.o mys_parser.o: mys_gram.h mys_kwlist_d.h

# mys_gram.c, mys_gram.h are in the distribution tarball, so they
# are not cleaned here.
clean distclean maintainer-clean:
	rm -f lex.backup mys_kwlist_d.h
