CC=gcc
CFLAGS=-Wall -pthread
PROGS=ex1_thread_create ex2_race_mutex ex3_producer_consumer ex4_parallel_sum
all: $(PROGS)
%: %.c
	$(CC) $(CFLAGS) -o $@ $< 
run: all
	./ex1_thread_create 5; for i in 1 2 3; do ./ex2_race_mutex; done; ./ex3_producer_consumer; \
	for t in 1 2 4 8; do ./ex4_parallel_sum $$t; done
clean:
	rm -f $(PROGS)
