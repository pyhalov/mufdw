EXTENSION = mufdw
EXTVERSION = 0.1
PGFILEDESC = "mufdw - Minimal usable FDW"
MODULE_big = mufdw
OBJS = $(WIN32RES) \
	mufdw.o

DATA = mufdw--0.1.sql

REGRESS = basic_tests

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
