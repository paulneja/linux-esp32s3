/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_MMU_H
#define __ASM_GENERIC_MMU_H

/*
 * This is the mmu.h header for nommu implementations.
 * Architectures with an MMU need something more complex.
 */
#ifndef __ASSEMBLY__
#ifdef CONFIG_XTENSA_NOMMU_FORK
#include <linux/list.h>
#endif
typedef struct {
	unsigned long		end_brk;
#ifdef CONFIG_XTENSA_NOMMU_FORK
	struct list_head nommu_banks;
#endif

#ifdef CONFIG_BINFMT_ELF_FDPIC
	unsigned long		exec_fdpic_loadmap;
	unsigned long		interp_fdpic_loadmap;
#endif
} mm_context_t;
#endif

#endif /* __ASM_GENERIC_MMU_H */
