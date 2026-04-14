/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RDA_PLAT_REG_IFC_H
#define __RDA_PLAT_REG_IFC_H

#include <linux/soc/rda/hardware.h>
#include <linux/soc/rda/ifc.h>

#if defined(CONFIG_MMC_RDA_IFC_SG)
#define SYS_IFC_SG_CHAN_NUM 3
#define SYS_IFC_SG_MAX 8
#else
#define SYS_IFC_SG_CHAN_NUM 1
#define SYS_IFC_SG_MAX 1
#endif

#define SYS_IFC_CH_TO_USE(n) (((n) & 15) << 0)

#if defined(CONFIG_ARCH_RDA8810)
#define SYS_IFC_SG_NUM(n) 0
#else
#define SYS_IFC_SG_NUM(n) (((n) & 0x7) << 17)
#endif

typedef volatile struct {
	REG32 start_addr;
	REG32 tc;
} HWP_SYS_SG_T;

typedef volatile struct {
	REG32 get_ch;
	REG32 dma_status;
	REG32 debug_status;
	REG32 reserved_0c;
	struct {
		REG32 control;
		REG32 status;
		HWP_SYS_SG_T sg_table[SYS_IFC_SG_MAX];
	} std_ch[SYS_IFC_STD_CHAN_NB];
	REG32 ch_rfspi_control;
	REG32 ch_rfspi_status;
	REG32 ch_rfspi_start_addr;
	REG32 ch_rfspi_end_addr;
	REG32 ch_rfspi_tc;
} HWP_SYS_IFC_T;

#endif /* __RDA_PLAT_REG_IFC_H */