#ifndef __KERNEL_MISC_STAGE1_H__
#define __KERNEL_MISC_STAGE1_H__

#define min_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x < __y ? __x: __y; })
#define max_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x > __y ? __x: __y; })

#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

static inline __attribute__((const))
int is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

/*
 * xfs_iroundup: round up argument to next power of two
 */
static inline uint
roundup_pow_of_two(uint v)
{
	int	i;
	uint	m;

	if ((v & (v - 1)) == 0)
		return v;
	ASSERT((v & 0x80000000) == 0);
	if ((v & (v + 1)) == 0)
		return v + 1;
	for (i = 0, m = 1; i < 31; i++, m <<= 1) {
		if (v & m)
			continue;
		v |= m;
		if ((v & (v + 1)) == 0)
			return v + 1;
	}
	ASSERT(0);
	return 0;
}

#define memalloc_nofs_save() (0);
#define memalloc_nofs_restore(a)
#define memalloc_noreclaim_save() (0)
#define memalloc_noreclaim_restore(a) \
	((a) = (a));
#define lockdep_assert_held(a)

#endif	/* __KERNEL_MISC_STAGE1_H__ */
