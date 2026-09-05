/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by nommu.c: experimental UP, eager-copy fork backend. */
#include <linux/sched/signal.h>
#define NOMMU_BANK_MAX_BYTES (512UL * 1024)
#define BANK_GFP (GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN)

struct nommu_bank {
	struct list_head link;
	struct list_head siblings;
	struct vm_region *region;
	unsigned int count;
	void *pages[];
};

atomic_long_t nommu_bank_shadow_pages = ATOMIC_LONG_INIT(0);
atomic_long_t nommu_bank_recovered_pages = ATOMIC_LONG_INIT(0);

/* A single-owner descriptor survives, but owns no backup pages. Keeping
 * that descriptor stable matters when another mm exits while fork sleeps
 * in bank_alloc(). Only the owning mm can replace its descriptor.
 */
static void bank_release_pages(struct nommu_bank *bank)
{
	unsigned int i;
	for (i = 0; i < bank->count; i++)
		free_page((unsigned long)bank->pages[i]);
	atomic_long_sub(bank->count, &nommu_bank_shadow_pages);
	bank->count = 0;
}

static void bank_free(struct nommu_bank *bank)
{
	bank_release_pages(bank);
	kfree(bank);
}

static struct nommu_bank *bank_alloc(struct vm_region *region)
{
	unsigned int i, count = (region->vm_top - region->vm_start) >> PAGE_SHIFT;
	struct nommu_bank *bank;

	bank = kzalloc(struct_size(bank, pages, count), BANK_GFP);
	if (!bank)
		return NULL;
	INIT_LIST_HEAD(&bank->link);
	INIT_LIST_HEAD(&bank->siblings);
	bank->region = region;
	for (i = 0; i < count; i++) {
		bank->pages[i] = (void *)__get_free_page(BANK_GFP);
		if (!bank->pages[i]) {
			bank_free(bank);
			return NULL;
		}
		bank->count++;
		atomic_long_inc(&nommu_bank_shadow_pages);
	}
	return bank;
}

/* IRQs off for ownership changes; no allocations or sleeping here. */
static void bank_copy(struct nommu_bank *bank, bool save)
{
	unsigned int i;
	for (i = 0; i < bank->count; i++) {
		void *resident = (void *)(bank->region->vm_start + i * PAGE_SIZE);
		if (save)
			memcpy(bank->pages[i], resident, PAGE_SIZE);
		else
			memcpy(resident, bank->pages[i], PAGE_SIZE);
	}
}

void nommu_bank_switch(struct mm_struct *mm)
{
	struct nommu_bank *bank;
	unsigned long flags;

	/* init_mm has no user banks and a zero-initialized context. */
	if (!mm->context.nommu_banks.next)
		return;
	local_irq_save(flags);
	list_for_each_entry(bank, &mm->context.nommu_banks, link) {
		struct vm_region *region = bank->region;
		if (region->bank_owner == bank)
			continue;
		if (region->bank_owner)
			bank_copy(region->bank_owner, true);
		bank_copy(bank, false);
		region->bank_owner = bank;
	}
	local_irq_restore(flags);
}

static void bank_detach(struct vm_area_struct *vma)
{
	struct nommu_bank *bank = vma->nommu_bank;
	unsigned long flags;
	if (!bank)
		return;
	local_irq_save(flags);
	list_del_init(&bank->link);
	if (bank->region->bank_owner == bank)
		bank->region->bank_owner = NULL;
	if (!list_empty(&bank->siblings)) {
		struct nommu_bank *last = list_next_entry(bank, siblings);

		list_del_init(&bank->siblings);
		if (list_empty(&last->siblings)) {
			/* The departing process may still be resident. Restore the
			 * survivor before discarding its only valid saved state.
			 * UP only: free_page/kfree do not sleep; IRQ exclusion also
			 * protects the switch list and a concurrent clone's checks.
			 */
			if (last->region->bank_owner != last)
				bank_copy(last, false);
			last->region->bank_owner = last;
			list_del_init(&last->link);
			atomic_long_add(last->count, &nommu_bank_recovered_pages);
			bank_release_pages(last);
		}
	}
	vma->nommu_bank = NULL;
	local_irq_restore(flags);
	bank_free(bank);
}

static int bank_clone(struct vm_area_struct *dst, struct vm_area_struct *src)
{
	struct vm_region *region = src->vm_region;
	struct nommu_bank *parent, *child, *old = NULL;
	unsigned long flags;
	bool fresh;

	/* Allocate the child first: sleeping here can let the last sibling
	 * exit and release the parent's shadows. Recheck AFTER allocation.
	 */
	child = bank_alloc(region);
	if (!child)
		return -ENOMEM;
	parent = src->nommu_bank;
	fresh = !parent || !parent->count;
	if (fresh) {
		old = parent;
		parent = bank_alloc(region);
		if (!parent) {
			bank_free(child);
			return -ENOMEM;
		}
	}
	local_irq_save(flags);
	if (fresh) {
		src->nommu_bank = parent;
		list_add_tail(&parent->link, &src->vm_mm->context.nommu_banks);
		region->bank_owner = parent;
	}
	/* The calling process is resident, even if allocation scheduled. */
	bank_copy(child, true);
	dst->nommu_bank = child;
	list_add_tail(&child->siblings, &parent->siblings);
	list_add_tail(&child->link, &dst->vm_mm->context.nommu_banks);
	local_irq_restore(flags);
	if (old)
		bank_free(old);
	return 0;
}

unsigned long nommu_bank_pages(struct mm_struct *mm)
{
	struct nommu_bank *bank;
	unsigned long flags, pages = 0;
	local_irq_save(flags);
	list_for_each_entry(bank, &mm->context.nommu_banks, link)
		pages += bank->count;
	local_irq_restore(flags);
	return pages;
}

int nommu_bank_dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	VMA_ITERATOR(src_iter, oldmm, 0);
	VMA_ITERATOR(dst_iter, mm, 0);
	struct vm_area_struct *src, *dst;
	unsigned long bytes = 0, addr;
	int ret;

	/* mm_users also includes temporary /proc and ptrace references. They
	 * are not extra executing threads. Reject actual CLONE_VM users only.
	 * Holding mmap_lock already serializes readers of the VMA layout.
	 */
	if (atomic_read(&oldmm->mm_users) > 1) {
		struct task_struct *group, *task;
		bool shared = false;

		rcu_read_lock();
		for_each_process_thread(group, task) {
			if (task != current && READ_ONCE(task->mm) == oldmm) {
				shared = true;
				goto checked_users;
			}
		}
checked_users:
		rcu_read_unlock();
		if (shared)
			return -EOPNOTSUPP;
	}
	for_each_vma(src_iter, src) {
		if (src->vm_flags & VM_SHARED)
			continue;
		if (!(src->vm_flags & VM_MAPPED_COPY)) {
			if (src->vm_flags & VM_WRITE)
				return -EOPNOTSUPP;
			continue; /* Read-only XIP mappings stay shared. */
		}
		/* RAM executable mappings need a separate I/D cache protocol. */
		if (src->vm_file && (src->vm_flags & VM_EXEC))
			return -EOPNOTSUPP;
		if (src->vm_region->vm_usage != 1 && !src->nommu_bank)
			return -EOPNOTSUPP;
		bytes += src->vm_region->vm_top - src->vm_region->vm_start;
		if (bytes > NOMMU_BANK_MAX_BYTES)
			return -ENOMEM;
		for (addr = src->vm_region->vm_start;
		     addr < src->vm_region->vm_top; addr += PAGE_SIZE)
			if (page_count(virt_to_page((void *)addr)) != 1)
				return -EBUSY;
	}
	vma_iter_set(&src_iter, 0);
	for_each_vma(src_iter, src) {
		if (src->vm_flags & (VM_DONTCOPY | VM_WIPEONFORK))
			return -EOPNOTSUPP;
		dst = vm_area_dup(src);
		if (!dst)
			return -ENOMEM;
		dst->vm_mm = mm;
		dst->nommu_bank = NULL;
		vma_iter_set(&dst_iter, dst->vm_start);
		vma_iter_config(&dst_iter, dst->vm_start, dst->vm_end);
		if (vma_iter_prealloc(&dst_iter, dst)) {
			vm_area_free(dst);
			return -ENOMEM;
		}
		if (!(src->vm_flags & VM_SHARED) && (src->vm_flags & VM_MAPPED_COPY)) {
			ret = bank_clone(dst, src);
			if (ret) {
				vma_iter_free(&dst_iter);
				vm_area_free(dst);
				return ret;
			}
		}
		down_write(&nommu_region_sem);
		dst->vm_region->vm_usage++;
		up_write(&nommu_region_sem);
		if (dst->vm_file)
			get_file(dst->vm_file);
		if (dst->vm_ops && dst->vm_ops->open)
			dst->vm_ops->open(dst);
		setup_vma_to_mm(dst, mm);
		mm->map_count++;
		vma_iter_store_new(&dst_iter, dst);
	}
	pr_info("fork-bank: parent=%d private=%lu pages=%lu\n",
		current->pid, bytes, bytes >> PAGE_SHIFT);
	return 0;
}

/* /proc and ptrace must access the target's bank, not another resident. */
static void bank_access(struct vm_area_struct *vma, unsigned long addr,
			void *buf, unsigned long len, bool write)
{
	struct nommu_bank *bank = vma->nommu_bank;
	unsigned long flags;
	local_irq_save(flags);
	while (len) {
		unsigned long offset = addr - bank->region->vm_start;
		unsigned long chunk = min(len, PAGE_SIZE - offset_in_page(offset));
		void *ptr = bank->region->bank_owner == bank ? (void *)addr :
			bank->pages[offset >> PAGE_SHIFT] + offset_in_page(offset);
		if (write)
			memcpy(ptr, buf, chunk);
		else
			memcpy(buf, ptr, chunk);
		addr += chunk;
		buf += chunk;
		len -= chunk;
	}
	local_irq_restore(flags);
}
