/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_NOMMU_BANK_H
#define _LINUX_NOMMU_BANK_H
#include <linux/atomic.h>
extern atomic_long_t nommu_bank_shadow_pages;
extern atomic_long_t nommu_bank_recovered_pages;
extern unsigned long nommu_bank_switch_max_cycles;
extern unsigned long nommu_bank_switch_last_cycles;
struct mm_struct;
struct vm_area_struct;
void nommu_bank_switch(struct mm_struct *mm);
unsigned long nommu_bank_pages(struct mm_struct *mm);
int nommu_bank_dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm);
#endif
