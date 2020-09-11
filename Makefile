PREFIX=/usr/local
CFLAGS=-std=c99 -D_XOPEN_SOURCE=500 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
CWARN=-Wall -Wpedantic -Wextra -Wno-unknown-pragmas -Wno-missing-field-initializers\
	  -Wno-padded -Wsign-conversion -Wno-missing-noreturn -Wno-cast-qual -Wtype-limits
LDFLAGS=
G ?=
O ?= -O3

CFILES=compiler.c grammar.c utils.c vm.c
OBJFILES=$(CFILES:.c=.o)

all: bpeg

.c.o:
	cc -c $(CFLAGS) $(CWARN) $(G) $(O) -o $@ $<

bpeg: $(OBJFILES) bpeg.c
	cc $(CFLAGS) $(CWARN) $(G) $(O) -o $@ $^

clean:
	rm -f bpeg $(OBJFILES)

.PHONY: all clean
