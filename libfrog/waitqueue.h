#ifndef _WAITQUEUE_H
#define _WAITQUEUE_H

#define TASK_KILLABLE 0

typedef struct wait_queue_head {
	;
} wait_queue_head_t;

struct wait_queue_entry {
	;
};


#define wake_up(...) ((void)0)
#define wake_up_all(...) ((void)0)
#define DEFINE_WAIT(name) \
	do {\
		struct wait_queue_entry name = {}; \
		name = name; \
	} while (0)

#define prepare_to_wait(...) ((void)0)
#define waitqueue_active(a) true
#define finish_wait(...) ((void)0)

#endif	/* _WAITQUEUE_H */
