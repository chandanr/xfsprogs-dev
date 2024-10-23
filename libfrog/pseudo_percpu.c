#include "platform_defs.h"
#include "pseudo_percpu.h"

void *__p_alloc_percpu(size_t size)
{
	return malloc(size);
}
