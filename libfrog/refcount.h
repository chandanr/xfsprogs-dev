#ifndef _REFCOUNT_H
#define _REFCOUNT_H

typedef struct refcount {
	/* chandan: should this be atomic_t as in the kernel? */
	u32 refs;
} refcount_t;

#define refcount_inc_not_zero(refcount) (1)
#define refcount_dec_and_test(refcount) (1)
#define refcount_set(refcount, nr)

#endif	/* _REFCOUNT_H */
