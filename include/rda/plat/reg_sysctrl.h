/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RDA_PLAT_REG_SYSCTRL_H
#define __RDA_PLAT_REG_SYSCTRL_H

#include <linux/soc/rda/hardware.h>

/* Only fields currently used by the in-tree RDA MMC driver are modelled. */
typedef volatile struct {
	REG32 reserved_00_48[0x4c / sizeof(REG32)];
	REG32 APB2_Rst_Set;
	REG32 APB2_Rst_Clr;
} HWP_SYS_CTRL_AP_T;

#if defined(CONFIG_ARCH_RDA8850E)
#define APB2_RST_SDMMC1 9
#else
#define APB2_RST_SDMMC1 8
#endif

#endif /* __RDA_PLAT_REG_SYSCTRL_H */