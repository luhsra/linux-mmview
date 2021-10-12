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
	long id;

	vmacache_flush(current);

	new_mm = mmview_dup_mm(current->group_leader, current->mm);
	if (!new_mm)
		goto fail_nomem;

	mmview_debug("created mm %lld\n", new_mm->view_id);

	id = new_mm->view_id;
	mmput(new_mm);

	return id;

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
	if (!new_mm || new_mm->view_id != id ||
	    test_bit(MMVIEW_REMOVED, &new_mm->view_flags)) {
		mmap_read_unlock(old_mm);
		return -EINVAL;
	}

	mmget(new_mm);
	mmap_read_unlock(old_mm);

	/* Change the mm pointers */
	task_lock(current);
	vmacache_flush(current);
	sync_mm_rss(old_mm);

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

	mmview_debug("migrated mm %lld -> %lld\n", old_id, id);

	mmput(old_mm);

	return old_id;
}

SYSCALL_DEFINE2(mmview_unshare, unsigned long, addr, unsigned long, len)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma, *prev, *last, *tmp;
	unsigned long page_size = PAGE_SIZE;
	unsigned long end;
	int ret = 0;

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

	vma = find_vma(mm, addr);
	if (!vma) {
		ret = -EINVAL;
		goto out;
	}
	prev = vma->vm_prev;

	if (is_vm_hugetlb_page(vma)) {
		struct hstate *h = hstate_vma(vma);
		page_size = huge_page_size(h);
	}
	len = ALIGN(len, page_size);

	if (len == 0) {
		ret = -EINVAL;
		goto out;
	}

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
		if ((tmp->vm_flags & VM_SHARED) && !tmp->mmview_shared) {
			ret = -EACCES;
			goto out;
		}
		tmp = tmp->vm_next;
	}

	/* Finally set mmview_shared flag */
	tmp = vma;
	while (tmp && tmp->vm_start < end) {
		tmp->mmview_shared = false;
		tmp = tmp->vm_next;
	}

	mmview_debug("unshared [%p-%p]\n", addr, addr+len);
out:
	mmap_write_unlock(mm);
	return ret;
}

SYSCALL_DEFINE1(mmview_delete, int, id)
{
	struct mm_struct *current_mm = current->mm;
	struct mm_struct *requested_mm;

	if (id <= 0)
		return -EINVAL;

	/* Write lock, in order to avoid concurrent migrations */
	mmap_write_lock(current_mm);

	list_for_each_entry(requested_mm, &current_mm->siblings, siblings) {
		if (requested_mm->view_id == id)
			break;
	}

	if (!requested_mm || requested_mm->view_id != id ||
	    test_bit(MMVIEW_REMOVED, &requested_mm->view_flags))
		goto fail;

	mmget(requested_mm);
	set_bit(MMVIEW_REMOVED, &requested_mm->view_flags);
	mmap_write_unlock(current_mm);

	if (atomic_read(&requested_mm->mm_users) > 0) {
		/* The view will no longer be accessible from the system calls,
		 * but it will be kept in the list, until the last task stops
		 * using it and calls mmput */
		mmview_debug("mm %lld still had %d users\n", id,
			    atomic_read(&requested_mm->mm_users));
	}

	mmput(requested_mm);

	mmview_debug("deleted mm %lld\n", id);

	return 0;

fail:
	mmap_write_unlock(current_mm);
	return -EINVAL;
}
