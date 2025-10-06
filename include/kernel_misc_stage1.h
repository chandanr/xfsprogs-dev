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

#define memalloc_nofs_save() (0);
#define memalloc_nofs_restore(a)
#define memalloc_noreclaim_save() (0)
#define memalloc_noreclaim_restore(a) \
	((a) = (a));
#define lockdep_assert_held(a)

#endif	/* __KERNEL_MISC_STAGE1_H__ */
