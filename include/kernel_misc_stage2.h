#ifndef __KERNEL_MISC_STAGE2_H__
#define __KERNEL_MISC_STAGE2_H__

/*
 * Requires platform_uuid_*() to be defined before inclusion. Hence this file
 * must be included after xfs/linux.h.
 */

#define uuid_equal(s, d) (platform_uuid_compare((s), (d)) == 0)
#define uuid_copy(s,d)		platform_uuid_copy((s),(d))

#include "linux-err.h"
#include "xfs_cksum.h"
#include "xfs_buf.h"

/*
 * We have no need for the "linux" dev_t in userspace, so these
 * are no-ops, and an xfs_dev_t is stored in VFS_I(ip)->i_rdev
 */
#define xfs_to_linux_dev_t(dev)	dev
#define linux_to_xfs_dev_t(dev) dev

/* fake up iomap, (not) used in xfs_bmap.[ch] */
#define IOMAP_F_SHARED				0x04
#define xfs_bmbt_to_iomap(a, b, c, d, e, f)	((void) 0)

/* fake up kernel's iomap, (not) used in xfs_bmap.[ch] */
struct iomap;

#ifndef EWRONGFS
#define EWRONGFS	EINVAL
#endif

#define xfs_error_level			0

#define STATIC				static

#define xfs_buf_ioerror_alert(bp,f)	((void) 0);

#define xfs_hex_dump(d,n)		((void) 0)
#define xfs_stack_trace()		((void) 0)

#define xfs_mod_delalloc(a,b) 		((void) 0)

#define __section(section)	__attribute__((__section__(section)))

#define xfs_printk_once(func, dev, fmt, ...)			\
({								\
	static bool __section(".data.once") __print_once;	\
	bool __ret_print_once = !__print_once;			\
								\
	if (!__print_once) {					\
		__print_once = true;				\
		func(dev, fmt, ##__VA_ARGS__);			\
	}							\
	unlikely(__ret_print_once);				\
})

#define xfs_info_once(dev, fmt, ...)				\
	xfs_printk_once(xfs_info, dev, fmt, ##__VA_ARGS__)

/* miscellaneous kernel routines not in user space */
#define likely(x)		(x)
#define unlikely(x)		(x)

/*
 * get_random_u32 is used for di_gen inode allocation, it must be zero for
 * libxfs or all sorts of badness can occur!
 */
#define get_random_u32()	(0)

#define PAGE_SIZE		getpagesize()

#define inode_peek_iversion(inode)	(inode)->i_version
#define inode_set_iversion_queried(inode, version) do { \
	(inode)->i_version = (version);	\
} while (0)

/**
 * swap - swap values of @a and @b
 * @a: first value
 * @b: second value
 */
#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#endif	/* __KERNEL_MISC_STAGE2_H__ */
