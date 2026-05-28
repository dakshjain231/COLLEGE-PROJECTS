## Makefile --- IDB Assignment
##  Daksh Jain  (Roll No. B24bs1114)
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Iinclude
LDFLAGS ?=

SRCS    := src/pf.c src/slotted.c src/static_rec.c src/am.c
OBJS    := $(SRCS:.c=.o)

BENCHES := bench/obj1_bench bench/obj2_bench bench/obj3_bench

.PHONY: all clean run plot

all: $(BENCHES)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

bench/obj1_bench: bench/obj1_bench.c $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

bench/obj2_bench: bench/obj2_bench.c $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

bench/obj3_bench: bench/obj3_bench.c $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJS) $(BENCHES) results/*.csv results/*.png \
	      /tmp/idb_*.dat

run: all
	./bench/obj1_bench
	./bench/obj2_bench
	./bench/obj3_bench

plot:
	python3 bench/plot_obj1.py
