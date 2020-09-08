PREFIX=
CFLAGS=-Wall -Wextra -pedantic -Wmissing-prototypes -Wstrict-prototypes
OFLAGS=-O3

all: bpeg

clean:
	rm -f bpeg

bpeg: bpeg.c bpeg.h utils.h
	cc $(CFLAGS) $(OFLAGS) $< -o $@

.PHONY: all clean
