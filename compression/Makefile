# PRISM - context-mixing compressor
CC      ?= cc
CFLAGS  ?= -O3 -funroll-loops -Wall -Wextra
LDFLAGS ?=

all: prism

prism: src/prism.c
	$(CC) $(CFLAGS) -o $@ src/prism.c $(LDFLAGS)

# Same thing but allowed to use this machine's SIMD width. Streams stay
# portable either way -- the model is integer-exact and does not depend on
# the instruction set.
native: src/prism.c
	$(CC) $(CFLAGS) -march=native -o prism src/prism.c $(LDFLAGS)

check: prism
	./bench/roundtrip.sh ./prism
	./bench/portability.sh

clean:
	rm -f prism

.PHONY: all native check clean
