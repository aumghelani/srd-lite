CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g
CFLAGS  += -Iinclude -Isrc

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)
LIB = build/librdt.a

TESTS = $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

all: $(LIB) $(TESTS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	ar rcs $@ $^

build/test_%: tests/test_%.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) -o $@

build:
	mkdir -p build

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

clean:
	rm -rf build

.PHONY: all test clean
