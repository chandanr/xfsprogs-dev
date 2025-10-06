#ifndef __KERNEL_MISC_STAGE2_H__
#define __KERNEL_MISC_STAGE2_H__

/*
 * Requires platform_uuid_*() to be defined before inclusion. Hence this file
 * must be included after xfs/linux.h.
 */

#define uuid_equal(s, d) (platform_uuid_compare((s), (d)) == 0)
#define uuid_copy(s,d)		platform_uuid_copy((s),(d))

#endif	/* __KERNEL_MISC_STAGE2_H__ */
