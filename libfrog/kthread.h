#ifndef _KTHREAD_H
#define _KTHREAD_H

struct task_struct {
	pthread_t thread;
	void *journal_info;
};

typedef void *(*)(void *) threadfn_t;

struct task_struct *kthread_run(threadfn_t threadfn, void *data, ...);

#define kthread_stop(...)
#define kthread_should_stop() (0)

#endif	/* _KTHREAD_H */
