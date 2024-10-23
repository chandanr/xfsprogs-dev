#ifndef _PSEUDO_PERCPU_H
#define _PSEUDO_PERCPU_H

#include "bitmask.h"

#define alloc_percpu(type) ((typeof(type) *)__p_alloc_percpu(sizeof(type)))

#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < 1; (cpu)++)

#define get_cpu() (0)
#define put_cpu()

static inline
void *per_cpu_ptr(void *arr, int cpu)
{
	ASSERT(cpu == 0);

	return arr;
}

#define this_cpu_ptr(arr) per_cpu_ptr((arr), 0)

struct cpumask {
	unsigned long mask;
};

static inline
bool cpumask_test_cpu(int cpu, const struct cpumask *cpumask)
{
	if (test_bit(cpu, &cpumask->mask))
		return true;

	return false;
}

static inline
bool cpumask_test_and_set_cpu(int cpu, struct cpumask *cpumask)
{
	if (test_and_set_bit(cpu, &cpumask->mask))
		return true;

	return false;
}

#endif	/* _PSEUDO_PERCPU_H */
