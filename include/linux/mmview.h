#ifndef _MMVIEW_H
#define _MMVIEW_H

#include <linux/mm_types.h>
#include <linux/hugetlb.h>

/*
 * mm->view_flags:
 */
#define MMVIEW_AVAILABLE	0 /* The view may be accessed through the syscalls */

/*
 * mmview() operations:
 */
#define MMVIEW_CREATE		0
#define MMVIEW_DELETE		1
#define MMVIEW_CURRENT		2
#define MMVIEW_MIGRATE		3
#define MMVIEW_UNSHARE		4
#define MMVIEW_SHARE		5
#define MMVIEW_SWITCH_BASE	6

#define mmview_debug(fmt, ...)					       \
	pr_debug("mmview: %s[%d] " fmt, current->comm,		       \
		 task_pid_nr(current), ##__VA_ARGS__)

void mmview_zap_page(struct mm_struct *mm, unsigned long addr);

#endif /* _MMVIEW_H */
