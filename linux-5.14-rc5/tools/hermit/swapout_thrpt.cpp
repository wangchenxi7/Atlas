#include <linux/kernel.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/errno.h>
#include <pthread.h>

#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>

// c++ stl multithread random number generator
#include <random>

#include "my_timer.h"
uint64_t stt, end;

/**
 * Warning : the tradition srand() is a sequential generator.
 * The multiple threads will be bound at the random number generation.
 */

#define ONE_MB 1048576UL // 1024 x 1024 bytes
#define ONE_GB 1073741824UL // 1024 x 1024 x 1024 bytes

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

#define start_ADDR (0x400000000000UL - 3 * ONE_GB - ONE_MB)
#define ARRAY_BYTE_SIZE (12 * ONE_GB)

int prepare_cores = 48;
int test_cores = 1;

struct thread_args {
	int tid; // thread index, start from 0
	void *buf;
};

/**
 * Reserve memory at fixed address
 */
static void *reserve_anon_memory(void *start, size_t len, bool fixed)
{
	void *ret;
	int flags;

	flags = MAP_PRIVATE | MAP_NORESERVE | MAP_ANONYMOUS;
	if (fixed == true) {
		printf("Request fixed addr 0x%lx ",
		       (unsigned long)start);

		flags |= MAP_FIXED;
	}

	// Map reserved/uncommitted pages PROT_NONE so we fail early if we
	// touch an uncommitted page. Otherwise, the read/write might
	// succeed if we have enough swap space to back the physical page.
	ret = mmap(start, len, PROT_NONE, flags, -1, 0);

	return ret == MAP_FAILED ? NULL : ret;
}

/**
 * Commit memory at reserved memory range.
 *
 */
static void *commit_anon_memory(void *start, size_t len, bool exec)
{
	int prot = (exec == true) ? PROT_READ | PROT_WRITE | PROT_EXEC :
					  PROT_READ | PROT_WRITE;
	int flags =  MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS;
	void *ret = mmap(start, len, prot, flags, -1, 0);

	// commit memory successfully.
	if (ret == MAP_FAILED) {
		// print errno here.
		return NULL;
	}

	return start;
}

// pthread function #1
static void *sequ_write(void *_args)
{
	struct thread_args *args = (struct thread_args *)_args;
	size_t tid = (size_t)args->tid; // [0, prepare_cores)
	char *buf = (char *)args->buf;
	size_t chunk = ARRAY_BYTE_SIZE / sizeof(unsigned long) / prepare_cores;
	size_t start = chunk * tid;
	unsigned long *buf_ptr = (unsigned long *)buf;

	if (tid == 0)
		printf("Thread[%3lu] Phase #1, trigger swap out. \n", tid);
	for (size_t i = start; i < start + chunk; i++)
		buf_ptr[i] = 1;

	return NULL;
}

static void *sequ_read(void *_args)
{
	struct thread_args *args = (struct thread_args *)_args;
	size_t tid = (size_t)args->tid; // [0, prepare_cores)
	char *buf = (char *)args->buf;
	size_t chunk = ARRAY_BYTE_SIZE / sizeof(unsigned long) / prepare_cores;
	size_t start = chunk * tid;
	size_t sum;

	unsigned long *buf_ptr = (unsigned long *)buf;

	sum = 0;
	if (tid == 0)
		printf("Thread[%3lu] Phase #2, trigger swap in.\n", tid);
	for (size_t i = start; i < start + chunk; i++) {
		sum += buf_ptr[i]; // the sum should be 0x7,FFF,FFE,000,000.
	}

	printf("Thread[%3lu] sum : 0x%lx \n", tid, sum);

	return NULL;
}

// pthread function #1
// scan the entire array from start to the end.
// make the memory access random
static void *rand_read(void *_args)
{
	struct thread_args *args = (struct thread_args *)_args;
	size_t tid = (size_t)args->tid; // [0, prepare_cores)
	char *buf = (char *)args->buf;
	size_t chunk = ARRAY_BYTE_SIZE / sizeof(unsigned long) / prepare_cores;
	size_t start = chunk * tid;
	size_t sum;

	thread_local std::mt19937 engine(std::random_device{}());
	std::uniform_int_distribution<unsigned long> dist(1, ARRAY_BYTE_SIZE);

	unsigned long *buf_ptr = (unsigned long *)buf;

	sum = 0;
	if (tid == 0)
		printf("Thread[%3lu] Phase #2, trigger swap in.\n", tid);
	// for (i = start; i < start + chunk; i += PAGE_SIZE / sizeof(unsigned long))
	for (size_t i = 0; i < 19200000 / prepare_cores; i++) { // step is PAGE
		unsigned long index_to_access =
			dist(engine) * (i + 1) % chunk;

		//debug - check the random level
		//printf("Thread[%3lu] access buf_ptr[%lu], page[%lu] \n", tid, index_to_access, (index_to_access >> (PAGE_SHIFT -3 ) ) );

		sum += buf_ptr
			[index_to_access]; // the sum should be 0x7,FFF,FFE,000,000.
	}

	if (tid == 0)
		printf("Thread[%3lu] sum : 0x%lx of chunk[0x%lx, 0x%lx ]  \n",
		       tid, sum, (unsigned long)(buf_ptr + start),
		       (unsigned long)(buf_ptr + chunk));

	pthread_exit(NULL);
}

static void evict_syscall(void *start, size_t len)
{
	const int SYS_hermit_dbg = 500;
	int type = 0;
	unsigned long args[2] = { (unsigned long)start, len };
	syscall(SYS_hermit_dbg, type, &args, sizeof(args));
}

static void *evict_range(void *_args)
{
	struct thread_args *args = (struct thread_args *)_args;
	size_t tid = (size_t)args->tid; // [0, prepare_cores)
	char *buf = (char *)args->buf;
	size_t chunk = ARRAY_BYTE_SIZE / test_cores;
	size_t start = chunk * tid;

	if (tid == 0)
		printf("Thread[%3lu] starting evict range [%p, %p)\n",
		       tid, buf + start, buf + start + chunk);

	evict_syscall(buf + start, chunk);

	if (tid == 0)
		printf("Thread[%3lu] finish evict range [%p, %p)\n",
		       tid, buf + start, buf + start + chunk);

	return NULL;
}

int main()
{
	void *request_addr = (void *)start_ADDR;
	unsigned long size = ARRAY_BYTE_SIZE;
	void *buf;
	unsigned long i;
	pthread_t threads[prepare_cores];
	struct thread_args args[prepare_cores];
	int ret = 0;

	srand(time(NULL)); // generate a random interger

	// 1) reserve space by mmap
	buf = reserve_anon_memory(request_addr, size, true);
	if (buf == NULL) {
		printf("Reserve buffer, 0x%lx failed. \n",
		       (unsigned long)request_addr);
	} else {
		printf("Reserve buffer: 0x%lx, bytes_len: 0x%lx \n",
		       (unsigned long)buf, size);
	}

	// 2) commit the space
	buf = commit_anon_memory(request_addr, size, false);
	if (buf == NULL) {
		printf("Commit buffer, 0x%lx failed. \n",
		       (unsigned long)request_addr);
	} else {
		printf("Commit buffer: 0x%lx, bytes_len: 0x%lx \n",
		       (unsigned long)buf, size);
	}

	// 3) swap out
	for (i = 0; i < prepare_cores; i++) {
		args[i].buf = buf;
		args[i].tid = i;

		ret = pthread_create(&threads[i], NULL, sequ_write,
				     (void *)&args[i]);
		if (ret) {
			printf("ERROR; return code from pthread_create() is %d\n",
			       ret);
			return 0;
		}
	}
	for (i = 0; i < prepare_cores; i++)
		ret = pthread_join(threads[i], NULL);
	printf("Prepare done. Start testing...\n");

	stt = start_timer();
	// evict_syscall(request_addr, size);
	printf("request addr %p, len %lu\n", request_addr, size);
	for (i = 0; i < test_cores; i++) {
		args[i].buf = buf;
		args[i].tid = i;

		ret = pthread_create(&threads[i], NULL, evict_range,
				     (void *)&args[i]);
		if (ret) {
			printf("ERROR; return code from pthread_create() is %d\n",
			       ret);
			return 0;
		}
	}
	for (i = 0; i < test_cores; i++)
		ret = pthread_join(threads[i], NULL);
	end = end_timer();
	duration(stt, end);

	// // 4) swap in
	// stt = start_timer();
	// for (i = 0; i < prepare_cores; i++) {
	// 	args[i].buf = buf;
	// 	args[i].tid = i;

	// 	ret = pthread_create(&threads[i], NULL, rand_read,
	// 			     (void *)&args[i]);
	// 	//ret = pthread_create(&threads[i], NULL, scan_array_sequential_overleap, (void*)&args[i]);
	// 	if (ret) {
	// 		printf("ERROR; return code from pthread_create() is %d\n",
	// 		       ret);
	// 		return 0;
	// 	}
	// }
	// for (i = 0; i < prepare_cores; i++)
	// 	ret = pthread_join(threads[i], NULL);
	// end = end_timer();
	// duration(stt, end);

	return 0;
}
