#ifndef _KTHREAD_H
#define _KTHREAD_H

struct task_struct {
	pthread_t thread;
	void *journal_info;
};

typedef void *(*)(void *) threadfn_t;

struct task_struct *kthread_run(threadfn_t threadfn, void *data, ...);

#endif	/* _KTHREAD_H */
