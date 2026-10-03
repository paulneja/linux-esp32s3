/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by nommu.c: experimental UP, eager-copy fork backend. */
#include <linux/sched/signal.h>
#include <linux/moduleparam.h>

/* Successful forks are normal operation, not console diagnostics. */
static bool nommu_bank_trace;
module_param_named(fork_bank_trace, nommu_bank_trace, bool, 0600);
MODULE_PARM_DESC(fork_bank_trace, "Log successful NOMMU bank forks (default off)");
/*
 * Ceiling on the private memory one fork may bank. It also bounds the time
 * nommu_bank_switch() takes, because a switch exchanges every banked page:
 * budget roughly this many bytes of PSRAM traffic per context switch between
 * two banked processes.
 */
static unsigned long nommu_bank_max_bytes = 512UL * 1024;
module_param_named(fork_bank_max_bytes, nommu_bank_max_bytes, ulong, 0644);
MODULE_PARM_DESC(fork_bank_max_bytes,
		 "Largest private memory a fork may bank, in bytes (default 524288)");
#define NOMMU_BANK_MAX_BYTES (READ_ONCE(nommu_bank_max_bytes))
#define BANK_GFP (GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN)
#define BANK_CHUNK		SZ_64K
#define BANK_CHUNK_PAGES	(BANK_CHUNK >> PAGE_SHIFT)
#define BANK_CHUNK_ORDER	ilog2(BANK_CHUNK_PAGES)

/*
 * Shadow pages move between descriptors instead of being copied into a
 * private backup per process. For a region shared by N processes, one is
 * resident and the other N-1 hold a page set each: N-1 sets, not N. The
 * resident owner's descriptor carries no pages at all (count == 0), which is
 * what halves the cost of the first fork.
 *
 * capacity is the region's page count, fixed when the descriptor is created.
 * NOMMU sys_brk() grows the heap inside the region without moving vm_top, so
 * every descriptor for one region agrees on it; the swap checks anyway.
 */
struct nommu_bank {
	struct list_head link;
	struct list_head siblings;
	struct vm_region *region;
	unsigned int count;
	unsigned int capacity;
	void *pages[];
};

atomic_long_t nommu_bank_shadow_pages = ATOMIC_LONG_INIT(0);
atomic_long_t nommu_bank_recovered_pages = ATOMIC_LONG_INIT(0);

/* Set from nommu_bank_switch(), which cannot print. Drained by the fork and
 * exit paths, which can. A single latch is enough: the first inconsistency
 * is the one worth seeing, and the model does not recover from it.
 */
static bool nommu_bank_broken;

/* Pages a context switch had to give up but could not free: free_page()
 * under the runqueue lock with interrupts off is not allowed. Pushed by the
 * switch, drained by fork and exit, which may sleep.
 */
static void *nommu_bank_orphans[64];
static unsigned int nommu_bank_orphan_count;

static void bank_orphan_pages(struct nommu_bank *bank)
{
	unsigned int i;

	for (i = 0; i < bank->count; i++) {
		if (nommu_bank_orphan_count < ARRAY_SIZE(nommu_bank_orphans)) {
			nommu_bank_orphans[nommu_bank_orphan_count++] = bank->pages[i];
		} else {
			/* The list is drained on every fork and exit, so it
			 * filling up means something is very wrong. Leak the
			 * page rather than free it from here.
			 */
			nommu_bank_broken = true;
		}
		bank->pages[i] = NULL;
	}
	atomic_long_sub(bank->count, &nommu_bank_shadow_pages);
	atomic_long_add(bank->count, &nommu_bank_recovered_pages);
	bank->count = 0;
}

static void bank_drain_orphans(void)
{
	void *pages[ARRAY_SIZE(nommu_bank_orphans)];
	unsigned long flags;
	unsigned int i, n;

	if (likely(!READ_ONCE(nommu_bank_orphan_count)))
		return;
	local_irq_save(flags);
	n = nommu_bank_orphan_count;
	for (i = 0; i < n; i++)
		pages[i] = nommu_bank_orphans[i];
	nommu_bank_orphan_count = 0;
	local_irq_restore(flags);
	for (i = 0; i < n; i++)
		free_page((unsigned long)pages[i]);
}

static void bank_report_broken(const char *where)
{
	if (likely(!READ_ONCE(nommu_bank_broken)))
		return;
	WRITE_ONCE(nommu_bank_broken, false);
	pr_err("nommu-bank: inconsistent bank state seen during a context switch, noticed in %s\n",
	       where);
	dump_stack();
}

/* How long a context switch spends exchanging banked pages, in CPU cycles.
 * Reported in /proc/meminfo as ForkSwitchMax, in microseconds at the nominal
 * 240 MHz; write anything to /proc/sys/vm/fork_bank_reset_max to clear it
 * before a measurement.
 */
unsigned long nommu_bank_switch_max_cycles;
unsigned long nommu_bank_switch_last_cycles;

/* A single-owner descriptor survives, but owns no backup pages. Keeping
 * that descriptor stable matters when another mm exits while fork sleeps
 * in bank_alloc(). Only the owning mm can replace its descriptor.
 */
static void bank_release_pages(struct nommu_bank *bank)
{
	unsigned int i;
	for (i = 0; i < bank->count; i++) {
		free_page((unsigned long)bank->pages[i]);
		/* An owner sits at count 0 for its whole resident life, so a
		 * stale pointer here would be a freed page reachable from a
		 * live descriptor. Clear the slot with the page.
		 */
		bank->pages[i] = NULL;
	}
	atomic_long_sub(bank->count, &nommu_bank_shadow_pages);
	bank->count = 0;
}

static void bank_free(struct nommu_bank *bank)
{
	bank_release_pages(bank);
	kfree(bank);
}

static unsigned int bank_capacity(struct vm_region *region)
{
	return (region->vm_top - region->vm_start) >> PAGE_SHIFT;
}

/* A descriptor with no pages: what the resident owner holds. */
static struct nommu_bank *bank_alloc_desc(struct vm_region *region)
{
	unsigned int capacity = bank_capacity(region);
	struct nommu_bank *bank;

	bank = kzalloc(struct_size(bank, pages, capacity), BANK_GFP);
	if (!bank)
		return NULL;
	INIT_LIST_HEAD(&bank->link);
	INIT_LIST_HEAD(&bank->siblings);
	bank->region = region;
	bank->capacity = capacity;
	return bank;
}

static int bank_alloc_pages(struct nommu_bank *bank)
{
	unsigned int i;

	for (i = 0; i < bank->capacity; ) {
		unsigned int n = 1, j;
		void *p = NULL;

		if (bank->capacity - i >= BANK_CHUNK_PAGES &&
		    IS_ALIGNED(bank->region->vm_start + i * PAGE_SIZE, BANK_CHUNK)) {
			struct page *page = alloc_pages(BANK_GFP, BANK_CHUNK_ORDER);

			if (page) {
				split_page(page, BANK_CHUNK_ORDER);
				p = page_address(page);
				n = BANK_CHUNK_PAGES;
			}
		}
		if (!p)
			p = (void *)__get_free_page(BANK_GFP);
		if (!p) {
			bank_release_pages(bank);
			return -ENOMEM;
		}
		for (j = 0; j < n; j++)
			bank->pages[i + j] = p + j * PAGE_SIZE;
		bank->count += n;
		atomic_long_add(n, &nommu_bank_shadow_pages);
		i += n;
	}
	return 0;
}

static struct nommu_bank *bank_alloc(struct vm_region *region)
{
	struct nommu_bank *bank = bank_alloc_desc(region);

	if (!bank)
		return NULL;
	if (bank_alloc_pages(bank)) {
		bank_free(bank);
		return NULL;
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

/*
 * Exchange a resident page with a shadow page. Two loads and two stores per
 * word is exactly what the save-then-restore pair of memcpy() cost, so the
 * traffic per context switch does not change -- what changes is that the
 * outgoing process's data lands in the page set the incoming one vacated,
 * instead of needing a second set of its own.
 */
static void bank_swap_page(void *resident, void *shadow)
{
	unsigned long *a = resident, *b = shadow;
	unsigned int words = PAGE_SIZE / sizeof(unsigned long);

	while (words--) {
		unsigned long tmp = *a;

		*a++ = *b;
		*b++ = tmp;
	}
}

atomic_long_t nommu_bank_mmu_pages = ATOMIC_LONG_INIT(0);

#ifdef CONFIG_XTENSA_PLATFORM_ESP32
#define S3_DCACHE_CTRL		0x600c4000UL
#define S3_DCACHE_SYNC_CTRL	0x600c4028UL
#define S3_DCACHE_SYNC_ADDR	0x600c402cUL
#define S3_DCACHE_SYNC_SIZE	0x600c4030UL
#define S3_DCACHE_AUTOLOAD	0x600c404cUL
#define S3_MMU_TABLE		0x600c5000UL
#define S3_SYNC_INVALIDATE	BIT(0)
#define S3_SYNC_WRITEBACK	BIT(1)
#define S3_SYNC_DONE		BIT(3)
#define S3_MMU_KIND		(BIT(15) | BIT(14))
#define S3_MMU_PSRAM		BIT(15)

static bool nommu_bank_mmu = true;
module_param_named(fork_bank_mmu, nommu_bank_mmu, bool, 0644);
MODULE_PARM_DESC(fork_bank_mmu, "Exchange aligned 64 KiB chunks by remapping (default on)");

static inline u32 s3_rd(unsigned long reg)
{
	mb();
	return *(volatile u32 *)reg;
}

static inline void s3_wr(unsigned long reg, u32 val)
{
	mb();
	*(volatile u32 *)reg = val;
	mb();
}

static inline volatile u32 *s3_mmu_entry(unsigned long addr)
{
	return (volatile u32 *)S3_MMU_TABLE + ((addr & 0x1ffffff) >> 16);
}

static bool bank_mmu_psram(unsigned long addr)
{
	return addr >= 0x3c000000 && addr < 0x3e000000 &&
	       (*s3_mmu_entry(addr) & S3_MMU_KIND) == S3_MMU_PSRAM;
}

/* Same register sequence as the ROM's Cache_*_Items. */
static void s3_dcache_sync(u32 op, unsigned long addr, u32 lines)
{
	s3_wr(S3_DCACHE_SYNC_ADDR, addr);
	s3_wr(S3_DCACHE_SYNC_SIZE, (s3_rd(S3_DCACHE_SYNC_SIZE) & 0xff800000) | lines);
	s3_wr(S3_DCACHE_SYNC_CTRL, s3_rd(S3_DCACHE_SYNC_CTRL) | op);
	while (!(s3_rd(S3_DCACHE_SYNC_CTRL) & S3_SYNC_DONE))
		;
}

static u32 s3_autoload_suspend(void)
{
	u32 was = s3_rd(S3_DCACHE_AUTOLOAD) & BIT(2);

	if (was) {
		s3_wr(S3_DCACHE_AUTOLOAD, s3_rd(S3_DCACHE_AUTOLOAD) & ~BIT(2));
		s3_wr(S3_DCACHE_AUTOLOAD, s3_rd(S3_DCACHE_AUTOLOAD) | BIT(9));
	}
	while (!(s3_rd(S3_DCACHE_AUTOLOAD) & BIT(3)))
		;
	s3_wr(S3_DCACHE_AUTOLOAD, s3_rd(S3_DCACHE_AUTOLOAD) & ~BIT(9));
	return was;
}

static void s3_autoload_resume(u32 was)
{
	if (was)
		s3_wr(S3_DCACHE_AUTOLOAD, s3_rd(S3_DCACHE_AUTOLOAD) | was);
}

static bool bank_mmu_chunk(void *resident, void **pages, unsigned int left)
{
	unsigned long r = (unsigned long)resident, s = (unsigned long)pages[0];
	unsigned int j;

	if (!READ_ONCE(nommu_bank_mmu) || left < BANK_CHUNK_PAGES ||
	    !IS_ALIGNED(r, BANK_CHUNK) || !IS_ALIGNED(s, BANK_CHUNK))
		return false;
	for (j = 1; j < BANK_CHUNK_PAGES; j++)
		if (pages[j] != (void *)(s + j * PAGE_SIZE))
			return false;
	return bank_mmu_psram(r) && bank_mmu_psram(s);
}

/*
 * The cache is tagged by virtual address, so both windows are written back
 * under the old mapping and invalidated after the swap. Nothing may touch
 * them in between: interrupts off, and neither core 0 nor DMA reaches a
 * banked page.
 */
static void bank_mmu_swap(void *resident, void *shadow)
{
	unsigned long r = (unsigned long)resident, s = (unsigned long)shadow;
	u32 lines = BANK_CHUNK / (16 << ((s3_rd(S3_DCACHE_CTRL) >> 3) & 3));
	volatile u32 *er = s3_mmu_entry(r), *es = s3_mmu_entry(s);
	unsigned long flags;
	u32 autoload, tmp;

	local_irq_save(flags);
	autoload = s3_autoload_suspend();
	s3_dcache_sync(S3_SYNC_WRITEBACK, r, lines);
	s3_dcache_sync(S3_SYNC_WRITEBACK, s, lines);
	mb();
	tmp = *er;
	*er = *es;
	*es = tmp;
	mb();
	s3_dcache_sync(S3_SYNC_INVALIDATE, r, lines);
	s3_dcache_sync(S3_SYNC_INVALIDATE, s, lines);
	s3_autoload_resume(autoload);
	local_irq_restore(flags);
	atomic_long_add(BANK_CHUNK_PAGES, &nommu_bank_mmu_pages);
}
#else
#define bank_mmu_chunk(resident, pages, left) false
#define bank_mmu_swap(resident, shadow) do { } while (0)
#endif

static void bank_exchange(struct nommu_bank *bank)
{
	unsigned long start = bank->region->vm_start;
	unsigned int i = 0;

	while (i < bank->count) {
		void *resident = (void *)(start + i * PAGE_SIZE);

		if (bank_mmu_chunk(resident, &bank->pages[i], bank->count - i)) {
			bank_mmu_swap(resident, bank->pages[i]);
			i += BANK_CHUNK_PAGES;
		} else {
			bank_swap_page(resident, bank->pages[i]);
			i++;
		}
	}
}

/* Whether the set may be handed from "from" to "to". Asked before anything
 * is touched: this runs inside switch_mm with interrupts off, where there is
 * no way to report a problem and no way to undo half a move. The receiver is
 * the outgoing resident owner, so it must hold nothing -- overwriting a live
 * pages[] would leak every page in it.
 */
static bool bank_can_move(struct nommu_bank *to, struct nommu_bank *from)
{
	if (to->capacity != from->capacity || to->count) {
		/* Cannot printk here: this is called under the runqueue lock
		 * with interrupts off, and printk wakes the console thread,
		 * which re-enters the scheduler. Latch it and let the next
		 * fork or exit, which may sleep, report it.
		 */
		nommu_bank_broken = true;
		return false;
	}
	return true;
}

/* Hand the page set over: "from" gives up its pages, "to" takes them.
 * Only ever called when bank_can_move() has just returned true.
 */
static void bank_move_pages(struct nommu_bank *to, struct nommu_bank *from)
{
	unsigned int i;

	for (i = 0; i < from->count; i++) {
		to->pages[i] = from->pages[i];
		from->pages[i] = NULL;
	}
	to->count = from->count;
	from->count = 0;
}

void nommu_bank_switch(struct mm_struct *mm)
{
	struct nommu_bank *bank;
	unsigned long started;

	/* init_mm has no user banks and a zero-initialized context. */
	if (!mm->context.nommu_banks.next)
		return;
	preempt_disable();
	started = get_ccount();
	list_for_each_entry(bank, &mm->context.nommu_banks, link) {
		struct vm_region *region = bank->region;
		struct nommu_bank *out = region->bank_owner;

		if (out == bank)
			continue;
		if (out) {
			/* Swap in place: afterwards the region holds the incoming
			 * process's data and bank->pages holds the outgoing one's,
			 * so the pages simply change owner. Ask first: a refusal
			 * here must leave the region exactly as it was, because
			 * swapping and then failing to move the pages would give
			 * the incoming process the outgoing one's memory.
			 */
			if (!bank_can_move(out, bank))
				continue;
			bank_exchange(bank);
			bank_move_pages(out, bank);
		} else {
			/* Nobody owns what is resident: bank_detach() promotes an
			 * heir on every departure, so this should be unreachable.
			 * Restore from our own set and leave it alone -- freeing
			 * pages here would be free_page() under the runqueue lock
			 * with interrupts off. Ownership is not claimed either, so
			 * the invariant "the owner holds no pages" still holds.
			 * The pages cannot be freed from here -- that is
			 * free_page() under the runqueue lock with interrupts
			 * off -- so they go on the orphan list for the next
			 * fork or exit to release. Ownership must be taken:
			 * a region left without an owner makes every later
			 * fork of it fail, and init hangs on its first one.
			 */
			bank_copy(bank, false);
			bank_orphan_pages(bank);
		}
		region->bank_owner = bank;
	}
	/* get_ccount() wraps every 18 seconds at 240 MHz; a switch that took
	 * that long is not a measurement worth keeping.
	 */
	nommu_bank_switch_last_cycles = get_ccount() - started;
	if (nommu_bank_switch_last_cycles > nommu_bank_switch_max_cycles)
		nommu_bank_switch_max_cycles = nommu_bank_switch_last_cycles;
	preempt_enable();
}

static void bank_detach(struct vm_area_struct *vma)
{
	struct nommu_bank *bank = vma->nommu_bank;
	unsigned long flags;
	bool was_owner;

	bank_report_broken("exit");
	bank_drain_orphans();
	if (!bank)
		return;
	local_irq_save(flags);
	list_del_init(&bank->link);
	was_owner = bank->region->bank_owner == bank;
	if (was_owner)
		bank->region->bank_owner = NULL;
	if (!list_empty(&bank->siblings)) {
		struct nommu_bank *heir = list_next_entry(bank, siblings);

		list_del_init(&bank->siblings);
		/* The resident copy belonged to the process that is leaving, so
		 * it is now dead. Promote a sibling: restore its saved state and
		 * hand back the pages it no longer needs, which is the same
		 * invariant a switch maintains -- the resident owner holds none.
		 * UP only: free_page and kfree do not sleep here, and IRQ
		 * exclusion also covers the switch list and a concurrent clone.
		 */
		if (was_owner) {
			bank_exchange(heir);
			heir->region->bank_owner = heir;
			atomic_long_add(heir->count, &nommu_bank_recovered_pages);
			bank_release_pages(heir);
		}
	} else if (was_owner) {
		/* Last user of the region and it was resident: nothing to save. */
		atomic_long_add(bank->count, &nommu_bank_recovered_pages);
	}
	vma->nommu_bank = NULL;
	local_irq_restore(flags);
	bank_free(bank);
}

static int bank_clone(struct vm_area_struct *dst, struct vm_area_struct *src)
{
	struct vm_region *region = src->vm_region;
	struct nommu_bank *parent, *child, *existing;
	unsigned long flags;
	bool fresh;

	bank_report_broken("fork");
	bank_drain_orphans();

	/* Allocate the child first: sleeping here can let the last sibling
	 * exit and release the parent's shadows. Recheck AFTER allocation.
	 *
	 * One page set, not two. Parent and child have identical memory at this
	 * instant and the caller is resident, so the child's copy is the only
	 * one that has to exist; the parent stays resident and owns no pages.
	 * That is the difference between 2P and P for the first fork of a
	 * region -- 552 KiB against 276 KiB for a bash of the measured size.
	 */
	child = bank_alloc(region);
	if (!child)
		return -ENOMEM;
	parent = src->nommu_bank;
	/* The process calling fork is running, so its memory is the resident
	 * copy and its descriptor must be the region's owner. If that does not
	 * hold, the resident bytes belong to somebody else and copying them
	 * into the child would hand it the wrong memory: refuse instead.
	 */
	if (parent && WARN_ON_ONCE(region->bank_owner != parent)) {
		bank_free(child);
		return -EINVAL;
	}
	fresh = !parent;
	if (fresh) {
		parent = bank_alloc_desc(region);
		if (!parent) {
			bank_free(child);
			return -ENOMEM;
		}
	}
	local_irq_save(flags);
	/* One bank per region per mm. Two would make nommu_bank_switch() walk
	 * the same region twice in one pass: the second visit swaps it back,
	 * leaving the process on a stale snapshot with its live pages parked
	 * in a descriptor that no longer owns them. dup_mmap's vm_usage check
	 * should already prevent it, but that check exists for other reasons.
	 */
	list_for_each_entry(existing, &dst->vm_mm->context.nommu_banks, link) {
		if (existing->region == region) {
			local_irq_restore(flags);
			bank_free(child);
			if (fresh)
				bank_free(parent);
			return -EOPNOTSUPP;
		}
	}
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
	if (unlikely(READ_ONCE(nommu_bank_trace)))
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

	/* len comes from __access_remote_vm() and is bounded only by the size
	 * of the VMA, so cat /proc/PID/mem could ask for one 512 KiB memcpy
	 * with interrupts off. The loop is already chunked to a page; keep the
	 * exclusion inside it so the window is a page too.
	 */
	while (len) {
		unsigned long offset = addr - bank->region->vm_start;
		unsigned long chunk = min(len, PAGE_SIZE - offset_in_page(offset));
		void *ptr;

		local_irq_save(flags);
		ptr = bank->region->bank_owner == bank ? (void *)addr :
			bank->pages[offset >> PAGE_SHIFT] + offset_in_page(offset);
		if (write)
			memcpy(ptr, buf, chunk);
		else
			memcpy(buf, ptr, chunk);
		local_irq_restore(flags);
		addr += chunk;
		buf += chunk;
		len -= chunk;
	}
}
