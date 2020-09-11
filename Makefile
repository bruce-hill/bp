PREFIX=/usr/local
CFLAGS=-Wall -Wextra -pedantic -Wmissing-prototypes -Wstrict-prototypes
LDFLAGS=
G ?=
O ?= -O3

CFILES=compiler.c grammar.c utils.c vm.c
OBJFILES=$(CFILES:.c=.o)

all: bpeg

.c.o:
	cc -c $(CFLAGS) $(G) $(O) -o $@ $<

bpeg: $(OBJFILES) bpeg.c
	cc $(CFLAGS) $(G) $(O) -o $@ $^

clean:
	rm -f bpeg $(OBJFILES)

.PHONY: all clean
