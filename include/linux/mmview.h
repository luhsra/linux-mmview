#ifndef _MMVIEW_H
#define _MMVIEW_H

#include <linux/mm_types.h>
#include <linux/hugetlb.h>

#define MMVIEW_AVAILABLE	0 /* The view may be accessed through the syscalls */

#define mmview_debug(fmt, ...)					       \
	pr_debug("mmview: %s[%d] " fmt, current->comm,		       \
		 task_pid_nr(current), ##__VA_ARGS__)

void mmview_zap_page(struct mm_struct *mm, unsigned long addr);

#endif /* _MMVIEW_H */
