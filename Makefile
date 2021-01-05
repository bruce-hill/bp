NAME=bp
CC=cc
PREFIX=/usr/local
SYSCONFDIR=/etc
CFLAGS=-std=c99 -Werror -D_XOPEN_SOURCE=700 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CWARN=-Wall -Wpedantic -Wextra -Wsign-conversion -Wtype-limits -Wunused-result
G=
O=-O3

CFILES=compiler.c grammar.c utils.c vm.c file_loader.c viz.c json.c
OBJFILES=$(CFILES:.c=.o)

all: $(NAME)

.c.o:
	$(CC) -c $(CFLAGS) $(CWARN) $(G) $(O) -o $@ $<

$(NAME): $(OBJFILES) $(NAME).c
	$(CC) $(CFLAGS) $(CWARN) $(G) $(O) -o $@ $(OBJFILES) $(NAME).c

clean:
	rm -f $(NAME) $(OBJFILES)

install: $(NAME)
	mkdir -p -m 755 "$(PREFIX)/share/man/man1" "$(PREFIX)/bin" "$(SYSCONFDIR)/xdg/bp"
	cp -rv grammars/* "$(SYSCONFDIR)/xdg/bp/"
	cp -v $(NAME).1 "$(PREFIX)/share/man/man1/"
	rm -f "$(PREFIX)/bin/$(NAME)"
	cp -v $(NAME) "$(PREFIX)/bin/"

uninstall:
	rm -rvf "$(PREFIX)/bin/$(NAME)" "$(PREFIX)/share/man/man1/$(NAME).1" "$(SYSCONFDIR)/xdg/bp";
	@if [ -d ~/.config/$(NAME) ]; then \
	  printf 'Config files exist in ~/.config/$(NAME) Do you want to delete them? [Y/n] '; \
	  read confirm; \
	  [ "$$confirm" != n ] && rm -rf ~/.config/$(NAME); \
	fi

.PHONY: all clean install uninstall
