# contrib/aux_mysql/Makefile

EXTENSION = aux_mysql
DATA = aux_mysql--1.1.sql aux_mysql--1.1--1.2.sql aux_mysql--1.2--1.3.sql aux_mysql--1.3--1.4.sql \
	aux_mysql--1.4--1.5.sql 



REGRESS = 
REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/aux_mysql/mysql.conf

# NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/aux_mysql
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# But it can nonetheless be very helpful to run tests on preexisting
# installation, allow to do so, but only if requested explicitly.
installcheck-force:
	$(pg_regress_installcheck) $(REGRESS)
	$(pg_isolation_regress_installcheck) $(ISOLATION)
