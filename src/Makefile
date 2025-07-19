CFLAGS=-std=c17 -O2

.PHONY: all
all: mill

.PHONY: debug
debug: CFLAGS += -O0 -g # -fsanitize=address
debug: all

.PHONY: clean
clean:
	rm -f mill
	rm -rf mill.dSYM
