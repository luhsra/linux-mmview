#ifndef _MMVIEW_H
#define _MMVIEW_H

#include <linux/mm_types.h>

#define MMVIEW_AVAILABLE	0 /* The view may be accessed through the syscalls */

#define mmview_debug(fmt, ...)					       \
	pr_debug("mmview: %s[%d] " fmt, current->comm,		       \
		 task_pid_nr(current), ##__VA_ARGS__)

#endif /* _MMVIEW_H */
