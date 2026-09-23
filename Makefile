CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g
CPPFLAGS += -Iinclude -Isrc  # separate so CFLAGS=... on the cmd line doesnt wipe it

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)
LIB = build/librdt.a

TESTS = $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))
TOOLS = $(patsubst tools/%.c,build/%,$(wildcard tools/*.c))

all: $(LIB) $(TESTS) $(TOOLS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	ar rcs $@ $^

build/test_%: tests/test_%.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

build/%: tools/%.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

build:
	mkdir -p build

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# rebuild everything with address + undefined sanitizers and run tests
asan:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"

clean:
	rm -rf build

.PHONY: all test clean asan
