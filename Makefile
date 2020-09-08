PREFIX=
CFLAGS=-Wall -Wextra -pedantic -Wmissing-prototypes -Wstrict-prototypes
OFLAGS=-O3

all: bpeg

clean:
	rm -f bpeg

bpeg: bpeg.c
	cc $(CFLAGS) $(OFLAGS) $< -o $@

.PHONY: all clean
