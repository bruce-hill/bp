ifeq ($(wildcard config.mk),)
all: config.mk
	$(MAKE) all
install: config.mk
	$(MAKE) install
install-files: config.mk
	$(MAKE) install-files
install-lib: config.mk
	$(MAKE) install-lib
test: config.mk
	$(MAKE) test
config.mk: configure.sh
	bash ./configure.sh
else

include config.mk
NAME=bp
CC=cc
CFLAGS=-std=c2x -Werror -D_GNU_SOURCE -fPIC -flto=auto -fvisibility=hidden \
			 -fsanitize=signed-integer-overflow -fno-sanitize-recover -DBP_PREFIX='"$(PREFIX)"'
CWARN=-Wall -Wextra -Wno-format -Wshadow
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
CFILES=pattern.c utils.c match.c files.c printmatch.c utf8.c
HFILES=files.h match.h pattern.h printmatch.h utf8.h utils.h
OBJFILES=$(CFILES:.c=.o)

$(NAME): $(OBJFILES) bp.c
	$(CC) $(ALL_FLAGS) -o $@ $(OBJFILES) bp.c

$(LIBFILE): pattern.o utils.o match.o utf8.o
	$(CC) $^ $(ALL_FLAGS) -Wl,-soname,$(LIBFILE) -shared -o $@

all: $(NAME) bp.1

%.o: %.c %.h utf8.h
	$(CC) -c $(ALL_FLAGS) -o $@ $<

bp.1: bp.1.md
	pandoc --lua-filter=.pandoc/bold-code.lua -s $< -t man -o $@

tags: $(CFILES) bp.c
	ctags *.c *.h

clean:
	rm -f $(NAME) $(OBJFILES)
	@cd Lua && make clean

lua:
	@cd Lua && make

luatest:
	@cd Lua && make test

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
	mkdir -p -m 755 "$(PREFIX)/man/man1" "$(PREFIX)/bin" "$(PREFIX)/share/$(NAME)"
	cp -r grammars "$(PREFIX)/share/$(NAME)/"
	cp bp.1 "$(PREFIX)/man/man1/$(NAME).1"
	rm -f "$(PREFIX)/bin/$(NAME)"
	cp $(NAME) "$(PREFIX)/bin/"

install-lib: $(LIBFILE) bp.1
	mkdir -p -m 755 "$(PREFIX)/lib" "$(PREFIX)/include/$(NAME)"
	cp $(LIBFILE) "$(PREFIX)/lib"
	cp pattern.h match.h "$(PREFIX)/include/$(NAME)"

uninstall:
	rm -rf "$(PREFIX)/bin/$(NAME)" "$(PREFIX)/man/man1/$(NAME).1" "$(PREFIX)/share/$(NAME)"

profile_grammar: bp
	perf stat -r 100 -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores -e cycles ./bp -f plain -g bp -p Grammar grammars/bp.bp >/dev/null

profile_pattern: bp
	perf stat -r 1 -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores -e cycles ./bp -f plain -p 'id parens' /usr/include/*.h >/dev/null

endif

.PHONY: all clean install install-lib uninstall leaktest splint test tutorial lua profile luatest
