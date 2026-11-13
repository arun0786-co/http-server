CC ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Werror
SRC := src/parser.c src/path.c

all: build/tinyhttp build/tinyhttp-poll build/test_parser build/fuzz

build/tinyhttp: src/server.c $(SRC) src/*.h | build
	$(CC) $(CFLAGS) src/server.c $(SRC) -o $@

build/tinyhttp-poll: src/server.c $(SRC) src/*.h | build
	$(CC) $(CFLAGS) -DUSE_POLL src/server.c $(SRC) -o $@

build/test_parser: tests/test_parser.c $(SRC) src/*.h | build
	$(CC) $(CFLAGS) -Isrc tests/test_parser.c $(SRC) -o $@

build/fuzz: tests/fuzz.c $(SRC) src/*.h | build
	$(CC) $(CFLAGS) -Isrc tests/fuzz.c $(SRC) -o $@

build:
	mkdir -p build

# unit tests, a fuzz run, then the servers end to end (both event loops)
test: all
	./build/test_parser
	./build/fuzz 200000 1
	python3 tests/test_server.py build/tinyhttp
	python3 tests/test_server.py build/tinyhttp-poll

sanitize: | build
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc tests/test_parser.c $(SRC) -o build/test_parser_san
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc tests/fuzz.c $(SRC) -o build/fuzz_san
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all src/server.c $(SRC) -o build/tinyhttp_san
	./build/test_parser_san
	./build/fuzz_san 100000 2
	python3 tests/test_server.py build/tinyhttp_san

clean:
	rm -rf build

.PHONY: all test sanitize clean
