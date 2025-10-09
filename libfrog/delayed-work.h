#ifndef _DELAYED_WORK_H
#define _DELAYED_WORK_H

#include "workqueue.h"

struct delayed_work {
	struct workqueue *wq;
	workqueue_func_t *func;
	uint32_t index;
	void *arg;
	struct timespec ts;
	struct list_head list;
};

int delayed_work_module_init(void);
void init_delayed_work(struct delayed_work *dwork, workqueue_func_t *func,
		       uint32_t index, void *arg);
int queue_delayed_work(struct workqueue *wq, struct delayed_work *dwork,
		unsigned long delay);

#define INIT_DELAYED_WORK(dwork, func)	\
	init_delayed_work((dwork), (func), 0, NULL)

#define cancel_delayed_work_sync(work) do { } while(0)

#endif	/* _DELAYED_WORK_H */
