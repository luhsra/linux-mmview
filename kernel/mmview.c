#include <linux/printk.h>
#include <linux/syscalls.h>
#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/vmacache.h>
#include <linux/hugetlb.h>
#include <asm/mmu_context.h>
#include <linux/mmview.h>

SYSCALL_DEFINE0(mmview_create)
{
	struct mm_struct *new_mm;

	vmacache_flush(current);

	new_mm = mmview_dup_mm(current->group_leader, current->mm);
	if (!new_mm)
		goto fail_nomem;

	/* FIXME mmview_dup_mm returns with mm_users == 1.
		 mmview_delete decreases the mm_users */

	printk(KERN_INFO "mm_view: created: %lld\n", new_mm->view_id);

	printk(KERN_INFO "mm_view: mm_users: %d, mm_count: %d\n",
	       atomic_read(&new_mm->mm_users), atomic_read(&new_mm->mm_count));

	return new_mm->view_id;

fail_nomem:
	return -ENOMEM;
}

SYSCALL_DEFINE1(mmview_migrate, int, id)
{
	struct mm_struct *old_mm = current->mm;
	struct mm_struct *new_mm;
	u64 old_id;
	unsigned long flags;

	/* Passing an invalid id (< 0) returns the current view id */
	if (id < 0)
		return old_mm->view_id;

	/* Find view with id */
	mmap_read_lock(old_mm);

	list_for_each_entry(new_mm, &old_mm->siblings, siblings) {
		if (new_mm->view_id == id)
			break;
	}
	if (!new_mm || new_mm->view_id != id) {
		mmap_read_unlock(old_mm);
		return -EINVAL;
	}

	mmget(new_mm);
	mmap_read_unlock(old_mm);

	/* Change the mm pointers */
	task_lock(current);
	vmacache_flush(current);
	sync_mm_rss(old_mm);

	printk(KERN_INFO "DEBUG: migrate %p -> %p\n", old_mm, new_mm);

	/* Switch mm */
	local_irq_save(flags);
	lockdep_assert_irqs_disabled();
	current->mm = new_mm;
	current->active_mm = new_mm;
	membarrier_update_current_mm(new_mm);
	switch_mm_irqs_off(old_mm, new_mm, current);
	local_irq_restore(flags);
	old_id = old_mm->view_id;
	task_unlock(current);
	mmput(old_mm);

	printk(KERN_INFO "DEBUG: old users: %d\n", atomic_read(&old_mm->mm_users));
	printk(KERN_INFO "DEBUG: new users: %d\n", atomic_read(&new_mm->mm_users));
	printk(KERN_INFO "DEBUG: common users: %d\n", atomic_read(&new_mm->common->users));

	printk(KERN_INFO "old_id: %llu\n", old_id);
	return old_id;
}

SYSCALL_DEFINE2(mmview_unshare, unsigned long, addr, unsigned long, len)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma, *prev, *last, *tmp;
	unsigned long end;
	int ret = 0;

	printk(KERN_INFO "DEBUG: mmview_migrate\n");

	mmap_write_lock(mm);

	/* Unshare is only allowed before any views have been created. */
	if (mm_has_views(mm)) {
		ret = -EPERM;
		goto out;
	}

	if ((offset_in_page(addr)) || addr > TASK_SIZE || len > TASK_SIZE-addr) {
		ret = -EINVAL;
		goto out;
	}

	len = PAGE_ALIGN(len);
	if (len == 0) {
		ret = -EINVAL;
		goto out;
	}

	vma = find_vma(mm, addr);
	if (!vma) {
		ret = -EINVAL;
		goto out;
	}
	prev = vma->vm_prev;

	end = addr + len;
	if (vma->vm_start >= end) {
		ret = -EINVAL;
		goto out;
	}

	/* Split beginning VMA */
	if (addr > vma->vm_start) {
		if (mm->map_count >= sysctl_max_map_count) {
			ret = -ENOMEM;
			goto out;
		}

		ret = __split_vma(mm, vma, addr, 0);
		if (ret)
			goto out;
		prev = vma;
	}

	/* Split ending VMA */
	last = find_vma(mm, end);
	if (last && end > last->vm_start) {
		ret = __split_vma(mm, last, end, 1);
		if (ret)
			goto out;
	}
	vma = prev ? prev->vm_next : mm->mmap;

	/* Make some checks */
	tmp = vma;
	while (tmp && tmp->vm_start < end) {
		if (is_vm_hugetlb_page(tmp))
			BUG(); /* FIXME */
		if ((tmp->vm_flags & VM_SHARED) && !tmp->mm_view_shared) {
			ret = -EACCES;
			goto out;
		}
		tmp = tmp->vm_next;
	}

	/* Finally set as_generation_shared flag */
	tmp = vma;
	while (tmp && tmp->vm_start < end) {
		tmp->mm_view_shared = false;
		tmp = tmp->vm_next;
	}
out:
	mmap_write_unlock(mm);
	return ret;
}

SYSCALL_DEFINE1(mmview_delete, int, id)
{
	printk(KERN_INFO "DEBUG: mmview_delete\n");
	return -EPERM;
}
