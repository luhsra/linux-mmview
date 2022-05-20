#include <linux/syscalls.h>
#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/vmacache.h>
#include <linux/hugetlb.h>
#include <linux/mmu_context.h>
#include <linux/mmview.h>
#include <linux/sched/task.h>
#include <linux/rmap.h>

SYSCALL_DEFINE0(mmview_create)
{
	struct mm_struct *new_mm;
	long id;

	vmacache_flush(current);

	new_mm = dup_mm(current->group_leader, current->mm, true);
	if (!new_mm)
		goto fail_nomem;

	mmview_debug("created mm %lu\n", new_mm->view_id);

	id = new_mm->view_id;
	mmput(new_mm);

	return id;

fail_nomem:
	return -ENOMEM;
}

SYSCALL_DEFINE1(mmview_migrate, long, id)
{
	struct mm_struct *old_mm = current->mm;
	struct mm_struct *new_mm;
	unsigned long old_id;
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
	    !test_bit(MMVIEW_AVAILABLE, &new_mm->view_flags)) {
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

	mmview_debug("migrated mm %lu -> %lu\n", old_id, id);

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

	mmview_debug("unshared [%lx-%lx]\n", addr, addr+len);
out:
	mmap_write_unlock(mm);
	return ret;
}

static int
migrate_pte_range(struct vm_area_struct *dst_vma, struct vm_area_struct *src_vma,
		  pmd_t *dst_pmd, pmd_t *src_pmd, unsigned long addr,
		  unsigned long end)
{
	struct mm_struct *dst_mm = dst_vma->vm_mm;
	struct mm_struct *src_mm = src_vma->vm_mm;
	pte_t *orig_src_pte, *orig_dst_pte;
	pte_t *src_pte, *dst_pte;
	spinlock_t *src_ptl, *dst_ptl;

	dst_pte = pte_alloc_map_lock(dst_mm, dst_pmd, addr, &dst_ptl);
	if (!dst_pte)
		return -ENOMEM;
	src_pte = pte_offset_map(src_pmd, addr);
	src_ptl = pte_lockptr(src_mm, src_pmd);
	spin_lock_nested(src_ptl, SINGLE_DEPTH_NESTING);
	orig_src_pte = src_pte;
	orig_dst_pte = dst_pte;
	arch_enter_lazy_mmu_mode();

	do {
		struct page *page;
		if (pte_none(*src_pte))
			continue;
		if (pte_none(*dst_pte)) {
			page = vm_normal_page(src_vma, addr, *src_pte);
			if (page) {
				get_page(page);
				page_dup_rmap(page, false, true);
				add_mm_counter(dst_vma->vm_mm, mm_counter(page), 1);
				// FIXME: see add_mm_counter_fast
			}
			set_pte_at(dst_mm, addr, dst_pte, *src_pte);
		}
	} while (dst_pte++, src_pte++, addr += PAGE_SIZE, addr != end);

	arch_leave_lazy_mmu_mode();
	spin_unlock(src_ptl);
	pte_unmap(orig_src_pte);
	pte_unmap_unlock(orig_dst_pte, dst_ptl);
	cond_resched();
	return 0;
}

static inline int
migrate_pmd_range(struct vm_area_struct *dst_vma, struct vm_area_struct *src_vma,
		  pud_t *dst_pud, pud_t *src_pud, unsigned long addr,
		  unsigned long end)
{
	struct mm_struct *dst_mm = dst_vma->vm_mm;
	pmd_t *src_pmd, *dst_pmd;
	unsigned long next;

	dst_pmd = pmd_alloc(dst_mm, dst_pud, addr);
	if (!dst_pmd)
		return -ENOMEM;
	src_pmd = pmd_offset(src_pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (is_swap_pmd(*src_pmd) || pmd_trans_huge(*src_pmd)
			|| pmd_devmap(*src_pmd)) {
			BUG(); // not supported
		}
		if (pmd_none_or_clear_bad(src_pmd))
			continue;
		if (migrate_pte_range(dst_vma, src_vma, dst_pmd, src_pmd,
				      addr, next))
			return -ENOMEM;
	} while (dst_pmd++, src_pmd++, addr = next, addr != end);
	return 0;
}

static inline int
migrate_pud_range(struct vm_area_struct *dst_vma, struct vm_area_struct *src_vma,
		  p4d_t *dst_p4d, p4d_t *src_p4d, unsigned long addr,
		  unsigned long end)
{
	struct mm_struct *dst_mm = dst_vma->vm_mm;
	pud_t *src_pud, *dst_pud;
	unsigned long next;

	dst_pud = pud_alloc(dst_mm, dst_p4d, addr);
	if (!dst_pud)
		return -ENOMEM;
	src_pud = pud_offset(src_p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_trans_huge(*src_pud) || pud_devmap(*src_pud))
			BUG(); // no support
		if (pud_none_or_clear_bad(src_pud))
			continue;
		if (migrate_pmd_range(dst_vma, src_vma, dst_pud, src_pud,
				      addr, next))
			return -ENOMEM;
	} while (dst_pud++, src_pud++, addr = next, addr != end);
	return 0;
}

static inline int
migrate_p4d_range(struct vm_area_struct *dst_vma, struct vm_area_struct *src_vma,
		  pgd_t *dst_pgd, pgd_t *src_pgd, unsigned long addr,
		  unsigned long end)
{
	struct mm_struct *dst_mm = dst_vma->vm_mm;
	p4d_t *src_p4d, *dst_p4d;
	unsigned long next;

	dst_p4d = p4d_alloc(dst_mm, dst_pgd, addr);
	if (!dst_p4d)
		return -ENOMEM;
	src_p4d = p4d_offset(src_pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(src_p4d))
			continue;
		if (migrate_pud_range(dst_vma, src_vma, dst_p4d, src_p4d,
				      addr, next))
			return -ENOMEM;
	} while (dst_p4d++, src_p4d++, addr = next, addr != end);
	return 0;
}

static int
migrate_page_range(struct vm_area_struct *dst_vma, struct vm_area_struct *src_vma)
{
	pgd_t *src_pgd, *dst_pgd;
	unsigned long next;
	unsigned long addr = src_vma->vm_start;
	unsigned long end = src_vma->vm_end;
	struct mm_struct *dst_mm = dst_vma->vm_mm;
	struct mm_struct *src_mm = src_vma->vm_mm;
	int ret;

	ret = 0;
	dst_pgd = pgd_offset(dst_mm, addr);
	src_pgd = pgd_offset(src_mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(src_pgd))
			continue;
		if (unlikely(migrate_p4d_range(dst_vma, src_vma, dst_pgd, src_pgd,
					       addr, next))) {
			ret = -ENOMEM;
			goto out;
		}
	} while (dst_pgd++, src_pgd++, addr = next, addr != end);

out:
	return ret;
}

static int migrate_base_mm(void) {
	struct vm_area_struct *vma;
	struct mm_struct *old_base, *new_base = current->mm;

	// TODO ensure that base_mm is only fetched with mmap lock

	mmap_write_lock(new_base);
	old_base = new_base->common->base;

	for (vma = old_base->mmap; vma; vma = vma->vm_next) {
		struct vm_area_struct *new_vma;
		struct anon_vma *anon_vma = NULL;

		if (!vma->mmview_shared || (vma->vm_flags & VM_SHARED))
			continue;

		new_vma = find_extend_vma(new_base, vma->vm_start);

		anon_vma = smp_load_acquire(&vma->anon_vma);
		if (anon_vma && !cmpxchg(&vma->anon_vma, NULL, anon_vma))
			if (anon_vma_clone(new_vma, vma))
				return VM_FAULT_OOM;

		migrate_page_range(new_vma, vma);
	}

	new_base->common->base = new_base;
	mmap_write_unlock(new_base);

	return 0;
}

SYSCALL_DEFINE1(mmview_delete, long, id)
{
	struct mm_struct *current_mm = current->mm;
	struct mm_struct *requested_mm;

	if (id < 0)
		return migrate_base_mm();

	/* Write lock, in order to avoid concurrent migrations */
	mmap_write_lock(current_mm);

	list_for_each_entry(requested_mm, &current_mm->siblings, siblings) {
		if (requested_mm->view_id == id)
			break;
	}

	if (!requested_mm || requested_mm->view_id != id ||
	    !test_bit(MMVIEW_AVAILABLE, &requested_mm->view_flags))
		goto fail;

	if (mm_is_base(requested_mm))
		goto fail;

	clear_bit(MMVIEW_AVAILABLE, &requested_mm->view_flags);
	mmap_write_unlock(current_mm);

	if (atomic_read(&requested_mm->mm_users) > 1) {
		/* The view will no longer be accessible from the system calls,
		 * but it will be kept in the list, until the last task stops
		 * using it and calls mmput */
		mmview_debug("mm %lu still had %d users\n", id,
			     atomic_read(&requested_mm->mm_users));
	}

	mmput_view(requested_mm);

	mmview_debug("deleted mm %lu\n", id);

	return 0;

fail:
	mmap_write_unlock(current_mm);
	return -EINVAL;
}
