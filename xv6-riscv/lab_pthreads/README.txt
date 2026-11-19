Lab: POSIX Threads (Pthreads)
Build/run on Linux (gcc, glibc). No warnings with -Wall -pthread.

Ex1:  gcc -Wall -o ex1_thread_create ex1_thread_create.c -pthread
      ./ex1_thread_create        (default N=5)
      ./ex1_thread_create 1 ; ./ex1_thread_create 5 ; ./ex1_thread_create 20

Ex2:  gcc -Wall -o ex2_race_mutex ex2_race_mutex.c -pthread
      ./ex2_race_mutex           (run several times; prints unsafe and mutex results with timings)

Ex3:  gcc -Wall -o ex3_producer_consumer ex3_producer_consumer.c -pthread
      ./ex3_producer_consumer

Ex4:  gcc -Wall -o ex4_parallel_sum ex4_parallel_sum.c -pthread
      ./ex4_parallel_sum 1 ; ./ex4_parallel_sum 2 ; ./ex4_parallel_sum 4 ; ./ex4_parallel_sum 8

Or: make all / make run   (see Makefile; binaries are not to be submitted)
