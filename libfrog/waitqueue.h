#ifndef _WAITQUEUE_H
#define _WAITQUEUE_H

typedef struct wait_queue_head {
	pthread_mutex_t		cond_mutex;
	pthread_cond_t		cond;
	pthread_mutex_t		list_mutex;
	struct list_head	head;
} wait_queue_head_t;

struct wait_queue_entry {
	struct task_struct	*task;
	struct list_head	entry;
};

#define DECLARE_WAITQUEUE(name, task)					\
	struct wait_queue_entry name = {				\
		.task		     = task;				\
		.entry		     = LIST_HEAD_INIT((name).entry);	\
	}

#define DEFINE_WAIT(name, task)						\
	struct wait_queue_entry name = {				\
		.task		     = task;				\
		.entry		     = LIST_HEAD_INIT((name).entry);	\
	}

#define add_wait_queue(wq_head, wq_entry) \
	prepare_to_wait((wq_head), (wq_entry), 0)

#define remove_wait_queue(wq_head, wq_entry) \
	finish_wait((wq_head), (wq_entry))

#define wake_up_all(wq_head) wake_up(wq_head)

static inline int waitqueue_active(struct wait_queue_head *wq_head)
{
	return !list_empty(&wq_head->head);
}

void init_waitqueue_head(struct wait_queue_head	*wq_head);
void prepare_to_wait(struct wait_queue_head *wq_head,
		struct wait_queue_entry *wq_entry, int state);
void finish_wait(struct wait_queue_head *wq_head,
		struct wait_queue_entry *wq_entry);
int wake_up_process(struct task_struct *ts,pthread_cond_t *cond);
int wake_up(struct wait_queue_head *wq);

#endif	/* _WAITQUEUE_H */
