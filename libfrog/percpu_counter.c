#include "percpu_counter.h"

int
percpu_counter_init(
	struct percpu_counter *counter,
	s64 amount,
	gfp_t gfp)
{
	counter->count = amount;

	return 0;
}

int
percpu_counter_set(
	struct percpu_counter *counter,
	s64 amount)
{
	counter->count = amount;

	return 0;
}

