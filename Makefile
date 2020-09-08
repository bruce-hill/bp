PREFIX=
CFLAGS=-Wall -Wextra -pedantic -Wmissing-prototypes -Wstrict-prototypes
G ?=
O ?= -O3

all: bpeg

clean:
	rm -f bpeg

bpeg: bpeg.c bpeg.h utils.h
	cc $(CFLAGS) $(G) $(O) $< -o $@

.PHONY: all clean
