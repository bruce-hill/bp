NAME=bpeg
PREFIX=/usr/local
CFLAGS=-std=c99 -D_XOPEN_SOURCE=500 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CWARN=-Wall -Wpedantic -Wextra -Wno-unknown-pragmas -Wno-missing-field-initializers\
	  -Wno-padded -Wsign-conversion -Wno-missing-noreturn -Wno-cast-qual -Wtype-limits
G ?=
O ?= -O3

CFILES=compiler.c grammar.c utils.c vm.c
OBJFILES=$(CFILES:.c=.o)

all: $(NAME)

.c.o:
	cc -c $(CFLAGS) $(CWARN) $(G) $(O) -o $@ $<

$(NAME): $(OBJFILES) $(NAME).c
	cc $(CFLAGS) $(CWARN) $(G) $(O) -o $@ $^

clean:
	rm -f $(NAME) $(OBJFILES)

install: $(NAME)
	@prefix="$(PREFIX)"; \
	if [ ! "$$prefix" ]; then \
		printf '\033[1mWhere do you want to install? (default: /usr/local) \033[0m'; \
		read prefix; \
	fi; \
	[ ! "$$prefix" ] && prefix="/usr/local"; \
	[ ! "$$sysconfdir" ] && sysconfdir=/etc; \
	mkdir -pv -m 755 "$$prefix/share/man/man1" "$$prefix/bin" \
	&& cp -rv grammars/* "$$sysconfdir/xdg/bpeg/" \
	&& cp -v $(NAME).1 "$$prefix/share/man/man1/" \
	&& rm -f "$$prefix/bin/$(NAME)" \
	&& cp -v $(NAME) "$$prefix/bin/"

uninstall:
	@prefix="$(PREFIX)"; \
	if [ ! "$$prefix" ]; then \
		printf '\033[1mWhere do you want to uninstall from? (default: /usr/local) \033[0m'; \
		read prefix; \
	fi; \
	[ ! "$$prefix" ] && prefix="/usr/local"; \
	[ ! "$$sysconfdir" ] && sysconfdir=/etc; \
	echo "Deleting..."; \
	rm -rvf "$$prefix/bin/$(NAME)" "$$prefix/share/man/man1/$(NAME).1" "$$sysconfdir/xdg/bpeg"; \
	printf "\033[1mIf you created any config files in ~/.config/$(NAME), you may want to delete them manually.\033[0m\n"

.PHONY: all, clean, install, uninstall
