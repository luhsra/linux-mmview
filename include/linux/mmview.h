#ifndef _MMVIEW_H
#define _MMVIEW_H

#include <linux/mm_types.h>

struct mm_struct *mmview_dup_mm(struct task_struct *tsk,
				struct mm_struct *oldmm);

#endif /* _MMVIEW_H */
