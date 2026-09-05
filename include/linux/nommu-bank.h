/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_NOMMU_BANK_H
#define _LINUX_NOMMU_BANK_H
struct mm_struct;
struct vm_area_struct;
void nommu_bank_switch(struct mm_struct *mm);
int nommu_bank_dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm);
#endif
