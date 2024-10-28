#ifndef _PERCPU_COUNTER_H
#define _PERCPU_COUNTER_H

struct percpu_counter {
	__s64 count;
};

static inline __s64
percpu_counter_read(
	struct percpu_counter *counter)
{
	return counter->count;
}

static inline __s64
percpu_counter_read_positive(
	struct percpu_counter *counter)
{
	if (counter->count > 0)
		return counter->count;

	return 0;
}

static inline __s64
percpu_counter_sum(
	struct percpu_counter *counter)
{
	if (counter->count > 0)
		return counter->count;

	return 0;
}

static inline __s64
percpu_counter_sum_positive(
	struct percpu_counter *counter)
{
	if (counter->count > 0)
		return counter->count;

	return 0;
}

static inline void
percpu_counter_add_batch(
	struct percpu_counter *counter,
	__s64 amount,
	__s32 batch)
{
	counter->count += amount;
}

void
percpu_counter_add(
	struct percpu_counter *counter,
	__s64 amount)
{
	counter->count += amount;
}

int percpu_counter_init(struct percpu_counter *counter, __s64 amount,
		gfp_t gfp);
int percpu_counter_set(struct percpu_counter *counter, __s64 amount);

#endif	/* _PERCPU_COUNTER_H */
