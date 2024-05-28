NAME=bp
CC=cc
PREFIX=/usr/local
SYSCONFDIR=/etc
CFLAGS=-std=c99 -Werror -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L -flto=auto
CWARN=-Wall -Wextra -Wno-format
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
ALL_FLAGS=$(CFLAGS) $(OSFLAGS) -DBP_NAME="\"$(NAME)\"" $(EXTRA) $(CWARN) $(G) $(O)

LIBFILE=lib$(NAME).so
CFILES=pattern.c utils.c match.c files.c printmatch.c json.c utf8.c
HFILES=files.h json.h match.h pattern.h printmatch.h utf8.h utils.h
OBJFILES=$(CFILES:.c=.o)

$(NAME): $(OBJFILES) bp.c
	$(CC) $(ALL_FLAGS) -o $@ $(OBJFILES) bp.c

$(LIBFILE): $(OBJFILES)
	$(CC) $^ $(ALL_FLAGS) -Wl,-soname,$(LIBFILE) -shared -o $@

all: $(NAME) $(LIBFILE) bp.1 lua

%.o: %.c %.h utf8.h
	$(CC) -c $(ALL_FLAGS) -o $@ $<

bp.1: bp.1.md
	pandoc --lua-filter=.pandoc/bold-code.lua -s $< -t man -o $@

tags: $(CFILES) bp.c
	ctags *.c *.h

clean:
	rm -f $(NAME) $(OBJFILES)

lua:
	@cd Lua && make

test: $(NAME)
	./$(NAME) Comment -r '[@0]' >/dev/null
	./$(NAME) -g ./grammars/bp.bp '{Grammar}' ./grammars/bp.bp >/dev/null
	for test in tests/*.sh; do \
		PATH=".:$$PATH" sh "$$test" <"$${test/.sh/.in}" | diff -q - "$${test/.sh/.out}" ||\
			PATH=".:$$PATH" sh "$$test" <"$${test/.sh/.in}" | diff -y --color=always - "$${test/.sh/.out}"; \
	done

tutorial:
	./tutorial.sh

leaktest: bp
	valgrind --leak-check=full ./bp -l -g ./grammars/bp.bp '{Grammar}' ./grammars/bp.bp

splint:
	splint -posix-lib -weak -unrecog -initallelements -fullinitblock $(CFILES) bp.c

#splint:
#	splint -posix-lib -checks -mustfreefresh -mustfreeonly -temptrans -immediatetrans -branchstate \
#		-compmempass -nullret -nullpass -nullderef -kepttrans -boolops -initallelements -fullinitblock \
#		-compdef -usereleased -unrecog -dependenttrans -predboolothers -ownedtrans -unqualifiedtrans \
#		-onlytrans -usedef -nullassign -compdestroy -globstate -nullstate -statictrans -predboolint \
#		$(CFILES) bp.c

install: $(NAME) bp.1
	mkdir -p -m 755 "$(PREFIX)/man/man1" "$(PREFIX)/bin" "$(SYSCONFDIR)/$(NAME)"
	cp -r grammars/* "$(SYSCONFDIR)/$(NAME)/"
	cp bp.1 "$(PREFIX)/man/man1/$(NAME).1"
	rm -f "$(PREFIX)/bin/$(NAME)"
	cp $(NAME) "$(PREFIX)/bin/"

install-lib: $(LIBFILE) bp.1
	mkdir -p -m 755 "$(PREFIX)/lib" "$(PREFIX)/include/$(NAME)"
	cp $(LIBFILE) "$(PREFIX)/lib"
	cp $(HFILES) "$(PREFIX)/include/$(NAME)"

uninstall:
	rm -rf "$(PREFIX)/bin/$(NAME)" "$(PREFIX)/man/man1/$(NAME).1" "$(SYSCONFDIR)/$(NAME)"
	@if [ -d ~/.config/$(NAME) ]; then \
	  printf 'Config files exist in ~/.config/$(NAME) Do you want to delete them? [Y/n] '; \
	  read confirm; \
	  [ "$$confirm" != n ] && rm -rf ~/.config/$(NAME); \
	fi

profile_grammar: bp
	perf stat -r 100 -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores -e cycles ./bp -f plain -g bp -p Grammar grammars/bp.bp >/dev/null

profile_pattern: bp
	perf stat -r 1 -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores -e cycles ./bp -f plain -p 'id parens' /usr/include/*.h >/dev/null

.PHONY: all clean install install-lib uninstall leaktest splint test tutorial lua profile
