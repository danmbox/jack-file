### User configurable section

prefix      = /usr/local

exec_prefix = $(prefix)
bindir      = $(exec_prefix)/bin
sbindir     = $(exec_prefix)/sbin
datarootdir = $(prefix)/share
datadir     = $(datarootdir)
mandir      = $(datarootdir)/man
man1dir     = $(mandir)/man1

INSTALL         = install
INSTALL_PROGRAM = $(INSTALL)
INSTALL_DATA    = $(INSTALL) -m 644

CFLAGS +=

DESTDIR =

### End user configurable section

EXTPKG   := jack sndfile  # extern package dependencies

CFLAGS   += -Os -g -Wall -Wextra -pipe $(shell pkg-config --cflags $(EXTPKG))
LDFLAGS  += $(shell pkg-config --libs $(EXTPKG))

TMP_WILD := $(TMP_WILD) *~ *.bak cscope.*
TMP_PAT  := $(subst *,%,$(TMP_WILD))

RELEASE  := $(shell tr -d '"' < csrc/release.h)
MYNAME   := jack-file
DISTNAME := $(MYNAME)-$(RELEASE)

PROGS := file2jack jacktransportloop
MANS := $(addprefix man/, $(PROGS:=.1))
DESKTOPS := $(wildcard data/*.desktop)

CLEAN_FILES := $(MANS) $(PROGS)

.PHONY: clean all srcdist

all: $(PROGS) $(MANS)

%: csrc/%.c $(wildcard csrc/*.h) Makefile
	$(CC) $(CFLAGS) -std=c99 -D_REENTRANT $(LDFLAGS) -lm -lpthread -lrt $< -o $@

man/%.1: % $(filter-out $(wildcard man), man) Makefile
	help2man -N -o $@ $(abspath $<) || { $< --help || :; $< --version || :; false; }

install: all installdirs
	$(INSTALL_PROGRAM) $(PROGS) $(DESTDIR)$(bindir)
	$(INSTALL_DATA) $(MANS) $(DESTDIR)$(man1dir)
	$(INSTALL_DATA) $(DESKTOPS) $(DESTDIR)$(datadir)/applications

clean:
	set -f; for pat in $(TMP_WILD); do find . -iname $$pat -exec rm {} \; ; done; \
	rm -rf $(CLEAN_FILES)

srcdist: clean
	git archive --format=tar --prefix=$(DISTNAME)/ HEAD | \
	  gzip -c >/tmp/$(DISTNAME).tar.gz

showvars:
	@echo "RELEASE := " $(RELEASE)
	@echo "TMP_PAT := " $(TMP_PAT)
	@echo "CFLAGS  := " $(CFLAGS)
	@echo "LDFLAGS := " $(LDFLAGS)

man:
	mkdir man

installdirs: mkinstalldirs
	./mkinstalldirs $(DESTDIR)$(bindir) $(DESTDIR)$(datadir) \
	$(DESTDIR)$(mandir) $(DESTDIR)$(man1dir) $(DESTDIR)$(datadir)/applications
