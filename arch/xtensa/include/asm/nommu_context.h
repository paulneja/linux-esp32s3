/* SPDX-License-Identifier: GPL-2.0 */
static inline void init_mmu(void)
{
}

static inline void init_kio(void)
{
}

#ifdef CONFIG_XTENSA_NOMMU_FORK
#include <linux/irqflags.h>
#include <linux/mm_types.h>
#include <asm/current.h>
#include <linux/nommu-bank.h>
#include <asm-generic/mm_hooks.h>
#define init_new_context init_new_context
static inline int init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	INIT_LIST_HEAD(&mm->context.nommu_banks);
	return 0;
}
static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
			     struct task_struct *tsk)
{
	if (irqs_disabled())
		next->context.bank_switch_pending = 1;
	else
		nommu_bank_switch(next);
}

#define finish_arch_post_lock_switch finish_arch_post_lock_switch
static inline void finish_arch_post_lock_switch(void)
{
	struct mm_struct *mm = current->mm;

	if (mm && mm->context.bank_switch_pending) {
		mm->context.bank_switch_pending = 0;
		nommu_bank_switch(mm);
	}
}
#include <asm-generic/mmu_context.h>
#else
#include <asm-generic/nommu_context.h>
#endif
