CC ?= cc
AR ?= ar
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
LIBOBJ := $(filter-out src/cli.o,$(OBJ))

# The shell used to run the test harnesses. They are POSIX shell scripts, so
# a POSIX shell is required for `make check`; override SH if the name differs.
SH ?= sh

# How to remove a file. Nothing here is universal, and the reason is worth
# recording, because two plausible-looking solutions are wrong.
#
# `rm` is a program, so make can start it directly. `del` is a cmd.exe
# builtin and no such program exists, so make cannot start `del` directly any
# more than it can start `echo` or `cd`: a recipe naming one of those fails
# with CreateProcess(...) failed, the system cannot find the file specified.
# The only way to run a builtin is through a program that will run it, which
# on Windows is cmd.exe, and cmd.exe is a real executable that CreateProcess
# can start. So the Windows branch names cmd.exe and passes `del` to it as
# an argument.
#
# The obvious refinement -- branch on make's own SHELL, which is right more
# often than OS is -- does not work. SHELL is a compiled-in default that
# describes what make *would* use, not what it can, and a make built with
# sh.exe as its default reports SHELL=sh.exe on a machine where no sh.exe is
# on PATH at all. Keyed on that, the branch picks `rm`, make looks for a
# shell to run the recipe with, finds none, and falls back to starting `rm`
# directly, which then fails because there is no rm either. The variable is
# stale in exactly the environment this has to survive in, so the decision
# is made on OS instead, and COMSPEC is used because it names the shell the
# machine actually uses rather than one make believes in.
#
# Only files that exist are passed to the command. `del` reports every absent
# name on stderr and sets a failing exit status, so naming artifacts that
# were never built would turn `make clean` on a fresh tree into a visible
# failure. The paths are respelled with backslashes because cmd.exe does not
# take `/` as a separator.
#
# Both spellings of every executable are listed, without extension and with
# it, because the compiler decides: a Windows toolchain appends .exe to the
# name it was given, so `cc -o md` writes md.exe and never a file called md,
# while a POSIX one writes md exactly. `$(wildcard)` discards whichever name
# this build did not produce. Note that the obvious way to find out, `ls md`,
# is no use for settling the question on an MSYS or Cygwin shell, where the
# POSIX layer resolves the missing name to the .exe one and reports success
# either way; make's `$(wildcard)` goes to the same Windows API the compiler
# did and tells the truth. `ar` is given the archive name whole and adds no
# suffix, so libmd.a needs only the one spelling.
CLEANFILES := $(wildcard \
                $(OBJ) \
                md md.exe \
                libmd.a \
                example example.exe \
                mdapitest mdapitest.exe mdapitest.log \
                mdlexertest mdlexertest.exe mdlexertest.log)

# Windows: a real program (cmd.exe) that can be started, wrapping a builtin.
# COMSPEC names the shell this machine actually uses; fall back to the bare
# name if the environment somehow does not set it. POSIX: rm, which is itself
# a program and needs no wrapper.
ifeq ($(OS),Windows_NT)
  WINSHELL := $(if $(strip $(COMSPEC)),$(strip $(COMSPEC)),cmd.exe)
  DELETE = $(WINSHELL) /c del /f /q $(subst /,\,$(CLEANFILES))
  NOTHING = $(WINSHELL) /c echo clean: nothing to remove
else
  DELETE = rm -f $(CLEANFILES)
  NOTHING = echo clean: nothing to remove
endif

# The three Python harnesses below are optional. Pick whichever name the
# machine actually has, so `make sweep` does not fail over a `python3` that
# only exists as a Windows Store alias. Override with PYTHON=... if needed.
#
# This is a shell command, and it is assigned recursively rather than
# simply, so that the probe runs only in a target that actually uses it.
# A simply-expanded variable would be evaluated while make reads the
# makefile, which means every invocation on any platform -- including plain
# `make` and `make clean`, which never need an interpreter -- would run a
# POSIX shell command. On a machine with no POSIX shell that attempt is
# handed to cmd.exe, which cannot parse `for ... do ... done` and answers
# with a parse error on stderr before the build has begun. Deferring the
# probe keeps that noise out of the targets that do not need it, and leaves
# the fallback name below for a platform where the probe cannot run.
PYTHON = $(shell for p in python3 python py; do if $$p -c '' >/dev/null 2>&1; then echo $$p; exit 0; fi; done; echo python3)

.PHONY: all lib check sweep diff json-check example clean

all: md

md: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

lib: libmd.a

libmd.a: $(LIBOBJ)
	$(AR) rcs $@ $(LIBOBJ)

# The public API example, built as a separate program against the archive
# alone. It is linked with -Isrc so that <md.h> resolves, and with nothing
# else: no object file, no other source from src/, no private header. If a
# change to the library makes the archive stop satisfying a caller that
# includes only md.h, this stops building.
#
# The output name carries no suffix here for the reason the clean rule
# records: a Windows toolchain appends .exe to a name it is given, so this
# writes example.exe rather than example.
example: examples/toc_example.c libmd.a
	$(CC) $(CFLAGS) -Isrc -o example examples/toc_example.c libmd.a

check: md libmd.a
	$(SH) tests/check_includes.sh
	$(SH) tests/run_tests.sh
	$(SH) tests/run_api_test.sh
	$(SH) tests/run_lexer_test.sh
	$(SH) tests/run_example_test.sh

# Randomized differential check: the streamed and the buffered path must
# produce identical output for the same bytes, on every mode and extension
# mask, with no crash, no unexpected exit status, and no diagnostic on a
# successful run. Needs python3; it is not part of `make check` because it
# takes seconds rather than milliseconds.
sweep: md
	$(PYTHON) tests/sweep_stream.py ./md 500

# Same question against the committed pre-C5 binary: C5 must not move the
# output it already produced. Needs a baseline built from the previous
# checkpoint and python3; see the header of the script.
diff: md
	$(PYTHON) tests/diff_baseline.py ../baseline/md ./md 1500

# Every successful run has to be one canonical, compact, valid JSON
# document, and every limit failure has to be one valid JSON error document
# on stdout. Needs python3.
json-check: md
	$(PYTHON) tests/check_json.py ./md 500

clean:
ifneq ($(strip $(CLEANFILES)),)
	$(DELETE)
else
	@$(NOTHING)
endif
