#ifndef _LIBXLOG_PRIV_H
#define _LIBXLOG_PRIV_H

/* TODO: chandan: include libxlog_api_defs.h  */
#include "platform_defs.h"
#include "xfs.h"

#define memalloc_nofs_save() (0);
#define memalloc_nofs_restore(a)

/*
 * TODO: chandan: Can we get cpp to eliminate references to current
 * entirely?
 */
struct task_struct {
	void *journal_info;
};

static struct task_struct ts;
static struct task_struct *current = &ts;

#endif	/* _LIBXLOG_PRIV_H */
