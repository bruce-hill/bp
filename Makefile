NAME=bp
CC=cc
PREFIX=/usr/local
SYSCONFDIR=/etc
CFLAGS=-std=c99 -Werror -D_XOPEN_SOURCE=700 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CWARN=-Wall -Wpedantic -Wextra -Wsign-conversion -Wtype-limits -Wunused-result
EXTRA=
G=
O=-O3
ALL_FLAGS=$(CFLAGS) -DBP_NAME="\"$(NAME)\"" $(EXTRA) $(CWARN) $(G) $(O)

CFILES=pattern.c definitions.c utils.c match.c files.c print.c json.c
OBJFILES=$(CFILES:.c=.o)

all: $(NAME)

%.o: %.c %.h types.h
	$(CC) -c $(ALL_FLAGS) -o $@ $<

$(NAME): $(OBJFILES) bp.c
	$(CC) $(ALL_FLAGS) -o $@ $(OBJFILES) bp.c

clean:
	rm -f $(NAME) $(OBJFILES)

leaktest:
	make G=-ggdb O=-O0 EXTRA=-DDEBUG_HEAP clean bp
	valgrind --leak-check=full ./bp -l -g grammars/bp.bp -p Grammar grammars/bp.bp

splint:
	splint -posix-lib -standard -mustfreefresh -mustfreeonly -temptrans -immediatetrans -branchstate \
		-compmempass -nullret -nullpass -nullderef -kepttrans -boolops -initallelements -fullinitblock \
		-compdef -usereleased -unrecog -dependenttrans -predboolothers -ownedtrans -unqualifiedtrans \
		-onlytrans -usedef -nullassign -compdestroy -globstate -nullstate -statictrans -predboolint \
		$(CFILES) bp.c

install: $(NAME)
	mkdir -p -m 755 "$(PREFIX)/share/man/man1" "$(PREFIX)/bin" "$(SYSCONFDIR)/xdg/$(NAME)"
	cp -r grammars/* "$(SYSCONFDIR)/xdg/$(NAME)/"
	cp bp.1 "$(PREFIX)/share/man/man1/$(NAME).1"
	rm -f "$(PREFIX)/bin/$(NAME)"
	cp $(NAME) "$(PREFIX)/bin/"

uninstall:
	rm -rf "$(PREFIX)/bin/$(NAME)" "$(PREFIX)/share/man/man1/$(NAME).1" "$(SYSCONFDIR)/xdg/$(NAME)"
	@if [ -d ~/.config/$(NAME) ]; then \
	  printf 'Config files exist in ~/.config/$(NAME) Do you want to delete them? [Y/n] '; \
	  read confirm; \
	  [ "$$confirm" != n ] && rm -rf ~/.config/$(NAME); \
	fi

.PHONY: all clean install uninstall leaktest splint
