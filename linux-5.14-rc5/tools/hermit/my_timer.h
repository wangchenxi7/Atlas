#include <stdio.h>
#include <stdint.h>

static inline uint64_t start_timer(void)
{
	uint64_t tsc;
	__asm__ __volatile__("mfence\n\t"
			"lfence\n\t"
			"RDTSC\n\t"
			"shl $32,%%rdx\n\t"
			"or %%rdx,%%rax\n\t"
			: "=a"(tsc)
			:
			: "%rdx");
			/* : "%rcx", "%rdx"); */
	return tsc;
}

static inline uint64_t end_timer(void)
{
	uint64_t tsc;
	__asm__ __volatile__("RDTSCP\n\t"
			"lfence\n\t" // better than CPUID, espcially in VM
			"shl $32,%%rdx\n\t"
			"or %%rdx,%%rax\n\t"
			: "=a"(tsc)
			:
			: "%rcx", "%rdx");
	return tsc;
}

#define RMG_CPU_FREQ 2100 // in MHz
static inline uint64_t duration(uint64_t stt, uint64_t end)
{
	uint64_t dur_in_ms = (end - stt) / 1000 / RMG_CPU_FREQ;
	printf("Duration: %lums\n", dur_in_ms);
	return dur_in_ms;
}