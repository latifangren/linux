/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RDA_REG_CFG_REGS_H
#define __LINUX_SOC_RDA_REG_CFG_REGS_H

#include <linux/soc/rda/hardware.h>

/* Minimal AP GPIO mapping used by the combo clock mux path. */
enum {
	AP_PIN_CLK_OUT = 8,
};

typedef volatile struct {
	REG32 CHIP_ID;
	REG32 Build_Version;
	REG32 BB_GPIO_Mode;
	REG32 AP_GPIO_A_Mode;
	REG32 AP_GPIO_B_Mode;
	REG32 AP_GPIO_D_Mode;
	REG32 Alt_mux_select;
	REG32 IO_Drive1_Select;
	REG32 IO_Drive2_Select;
	REG32 RAM_DRIVE;
	REG32 H2X_AP_Offset;
	REG32 H2X_DDR_Offset;
	REG32 audio_pd_set;
	REG32 audio_pd_clr;
	REG32 audio_sel_cfg;
	REG32 audio_mic_cfg;
	REG32 audio_spk_cfg;
	REG32 audio_rcv_gain;
	REG32 audio_head_gain;
	REG32 TSC_DATA;
	REG32 GPADC_DATA_CH[8];
} HWP_CFG_REGS_T;

#define CFG_REGS_CLK_OUT_MASK		(1U << 28)
#define CFG_REGS_CLK_OUT_CLK_OUT	(1U << 28)

/* Alt_mux_select bits used by USBID on i96 board (GPO_1 over KEYOUT_5/LPG). */
#define CFG_REGS_GPO_1_MASK		(3U << 22)
#define CFG_REGS_GPO_1_GPO_1		(0U << 22)
#define CFG_REGS_GPO_1_LPG		(1U << 22)
#define CFG_REGS_GPO_1_KEYOUT_5	(2U << 22)

#endif /* __LINUX_SOC_RDA_REG_CFG_REGS_H */
