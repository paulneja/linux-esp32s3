/* SPDX-License-Identifier: GPL-2.0 */
static inline void init_mmu(void)
{
}

static inline void init_kio(void)
{
}

#ifdef CONFIG_XTENSA_NOMMU_FORK
#include <linux/mm_types.h>
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
	nommu_bank_switch(next);
}
#include <asm-generic/mmu_context.h>
#else
#include <asm-generic/nommu_context.h>
#endif
