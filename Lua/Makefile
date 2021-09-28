NAME=bp
CC=cc
PREFIX=/usr/local
LUA_DIR=/usr/local
LUA_INC= $(LUA_DIR)/include
LUA=lua
INCS=-I$(LUA_INC)
CFLAGS=-std=c99 -Werror -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L -flto
CWARN=-Wall -Wextra
  # -Wpedantic -Wsign-conversion -Wtype-limits -Wunused-result -Wnull-dereference \
	# -Waggregate-return -Walloc-zero -Walloca -Warith-conversion -Wcast-align -Wcast-align=strict \
	# -Wdangling-else -Wdate-time -Wdisabled-optimization -Wdouble-promotion -Wduplicated-branches \
	# -Wduplicated-cond -Wexpansion-to-defined -Wfloat-conversion -Wfloat-equal -Wformat-nonliteral \
	# -Wformat-security -Wformat-signedness -Wframe-address -Winline -Winvalid-pch -Wjump-misses-init \
	# -Wlogical-op -Wlong-long -Wmissing-format-attribute -Wmissing-include-dirs -Wmissing-noreturn \
	# -Wnull-dereference -Woverlength-strings -Wpacked -Wpacked-not-aligned -Wpointer-arith \
	# -Wredundant-decls -Wshadow -Wshadow=compatible-local -Wshadow=global -Wshadow=local \
	# -Wsign-conversion -Wstack-protector -Wsuggest-attribute=const -Wswitch-default -Wswitch-enum \
	# -Wsync-nand -Wtrampolines -Wundef -Wunsuffixed-float-constants -Wunused -Wunused-but-set-variable \
	# -Wunused-const-variable -Wunused-local-typedefs -Wunused-macros -Wvariadic-macros -Wvector-operation-performance \
	# -Wvla -Wwrite-strings
OSFLAGS != case $$(uname -s) in *BSD|Darwin) echo '-D_BSD_SOURCE';; Linux) echo '-D_GNU_SOURCE';; *) echo '-D_DEFAULT_SOURCE';; esac
EXTRA=
G=
O=-O3
ALL_FLAGS=$(CFLAGS) $(OSFLAGS) $(INCS) $(EXTRA) $(CWARN) $(G) $(O)

ifeq ($(shell uname -s),Darwin)
	MAKESO= $(CC) -bundle -undefined dynamic_lookup
else
	MAKESO= $(CC) -shared
endif

all: bp.so

clean:
	rm -f lbp.o bp.so

lbp.o: lbp.c builtins.h
	$(CC) -c $(ALL_FLAGS) -o $@ $<

bp.so: lbp.o ../pattern.o ../utils.o ../utf8.o ../match.o
	$(MAKESO) -o $@ $^

builtins.h: ../grammars/builtins.bp
	sed 's/\\/\\\\/g;s/"/\\"/g;s/  /\\t/g;s/^/"/;s/$$/\\n"/' $< >$@

test: bp.so
	$(LUA) test.lua

.PHONY: all clean test
