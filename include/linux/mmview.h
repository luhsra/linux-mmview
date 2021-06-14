#ifndef _MMVIEW_H
#define _MMVIEW_H

#include <linux/mm_types.h>

struct mm_struct *mmview_dup_mm(struct task_struct *tsk,
				struct mm_struct *oldmm);

#define MMVIEW_REMOVED	0 /* The view may not be accessed through the syscalls */

#endif /* _MMVIEW_H */
