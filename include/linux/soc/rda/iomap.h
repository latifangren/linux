/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RDA_IOMAP_H
#define __LINUX_SOC_RDA_IOMAP_H

#include <linux/sizes.h>

/*
 * Keep only addresses currently used by in-tree RDA drivers.
 * Values come from the vendor RDA8810 register map.
 */
#define RDA_MD_SYSCTRL_PHYS	0x11A00000
#define RDA_MD_SYSCTRL_SIZE	SZ_4K

#define RDA_CONFIG_REGS_PHYS	0x11A09000
#define RDA_CONFIG_REGS_SIZE	SZ_4K

#define RDA_SYSCTRL_PHYS	0x20900000
#define RDA_SYSCTRL_SIZE	SZ_4K

#define RDA_IFC_PHYS		0x20AF0000
#define RDA_IFC_SIZE		SZ_4K

#endif /* __LINUX_SOC_RDA_IOMAP_H */