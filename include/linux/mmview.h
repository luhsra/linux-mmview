#ifndef _MMVIEW_H
#define _MMVIEW_H

#include <linux/mm_types.h>

struct mm_struct *mmview_dup_mm(struct task_struct *tsk,
				struct mm_struct *oldmm);

#define MMVIEW_REMOVED	0 /* The view may not be accessed through the syscalls */

#define mmview_debug(fmt, ...)					       \
	pr_debug("mmview: %s[%d] " fmt, current->comm,		       \
		 task_pid_nr(current), ##__VA_ARGS__)

#endif /* _MMVIEW_H */
