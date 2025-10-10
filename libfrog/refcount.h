#ifndef _REFCOUNT_H
#define _REFCOUNT_H

typedef struct refcount_struct {
	atomic_t refs;
} refcount_t;

#define refcount_inc_not_zero(refcount) ASSERT(0)
#define refcount_dec_and_test(refcount) ASSERT(0)
#define refcount_set(refcount, nr)	ASSERT(0)

#endif	/* _REFCOUNT_H */
