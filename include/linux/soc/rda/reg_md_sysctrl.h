/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RDA_REG_MD_SYSCTRL_H
#define __LINUX_SOC_RDA_REG_MD_SYSCTRL_H

#include <linux/soc/rda/hardware.h>

typedef volatile struct {
	REG32 REG_DBG;
	REG32 Sys_Rst_Set;
	REG32 Sys_Rst_Clr;
	REG32 BB_Rst_Set;
	REG32 BB_Rst_Clr;
	REG32 Clk_Sys_Mode;
	REG32 Clk_Sys_Enable;
	REG32 Clk_Sys_Disable;
	REG32 Clk_Per_Mode;
	REG32 Clk_Per_Enable;
	REG32 Clk_Per_Disable;
	REG32 Clk_BB_Mode;
	REG32 Clk_BB_Enable;
	REG32 Clk_BB_Disable;
	REG32 Clk_Other_Mode;
	REG32 Clk_Other_Enable;
	REG32 Clk_Other_Disable;
	REG32 Pll_Ctrl;
	REG32 Sel_Clock;
	REG32 Cfg_Clk_Sys;
	REG32 Cfg_Clk_Mem_Bridge;
	REG32 Cfg_Clk_Out;
	REG32 Cfg_Clk_Host_Uart;
	REG32 Cfg_Clk_Auxclk;
} HWP_SYS_CTRL_T;

#define SYS_CTRL_PROTECT_LOCK		0x00A50000
#define SYS_CTRL_PROTECT_UNLOCK	0x00A50001

#define SYS_CTRL_ENABLE_OC_CLK_OUT	(1U << 6)
#define SYS_CTRL_DISABLE_OC_CLK_OUT	(1U << 6)

#define SYS_CTRL_CLKOUT_DIVIDER(n)	(((n) & 31U) << 0)
#define SYS_CTRL_CLKOUT_SEL_OSC	(0U << 8)

#define SYS_CTRL_AUXCLK_EN_DISABLE	(0U << 0)
#define SYS_CTRL_AUXCLK_EN_ENABLE	(1U << 0)

#endif /* __LINUX_SOC_RDA_REG_MD_SYSCTRL_H */