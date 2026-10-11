CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Wpedantic
LDLIBS ?= -ljpeg -lm

all: jpegtool

jpegtool: main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

test: jpegtool
	python3 tests.py ./jpegtool

clean:
	rm -f jpegtool

.PHONY: all test clean
