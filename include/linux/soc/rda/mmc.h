/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RDA_MMC_H
#define __LINUX_SOC_RDA_MMC_H

#include <linux/soc/rda/hardware.h>

typedef volatile struct {
	REG32 SDMMC_CTRL;
	REG32 Reserved_00000004;
	REG32 SDMMC_FIFO_TXRX;
	REG32 Reserved_0000000C[509];
	REG32 SDMMC_CONFIG;
	REG32 SDMMC_STATUS;
	REG32 SDMMC_CMD_INDEX;
	REG32 SDMMC_CMD_ARG;
	REG32 SDMMC_RESP_INDEX;
	REG32 SDMMC_RESP_ARG3;
	REG32 SDMMC_RESP_ARG2;
	REG32 SDMMC_RESP_ARG1;
	REG32 SDMMC_RESP_ARG0;
	REG32 SDMMC_DATA_WIDTH;
	REG32 SDMMC_BLOCK_SIZE;
	REG32 SDMMC_BLOCK_CNT;
	REG32 SDMMC_INT_STATUS;
	REG32 SDMMC_INT_MASK;
	REG32 SDMMC_INT_CLEAR;
	REG32 SDMMC_TRANS_SPEED;
	REG32 SDMMC_MCLK_ADJUST;
} HWP_SDMMC_T;

#define SDMMC_L_ENDIAN(n) (((n) & 7) << 0)
#define SDMMC_SOFT_RST_L (1 << 3)

#define SDMMC_SDMMC_SENDCMD (1 << 0)
#define SDMMC_RSP_EN (1 << 4)
#define SDMMC_RSP_SEL_R2 (2 << 5)
#define SDMMC_RSP_SEL_R3 (1 << 5)
#define SDMMC_RSP_SEL_OTHER (0 << 5)
#define SDMMC_RD_WT_EN (1 << 8)
#define SDMMC_RD_WT_SEL_READ (0 << 9)
#define SDMMC_RD_WT_SEL_WRITE (1 << 9)
#define SDMMC_S_M_SEL_MULTIPLE (1 << 10)
#define SDMMC_AUTO_FLAG_EN (1 << 16)
#define SDMMC_SAMPLE_EDGE_SEL_FALL_EN (1 << 17)

#define SDMMC_NOT_SDMMC_OVER (1 << 0)
#define SDMMC_COMMAND(n) (((n) & 0x3F) << 0)
#define SDMMC_ARGUMENT(n) (((n) & 0xFFFFFFFF) << 0)
#define SDMMC_SDMMC_BLOCK_SIZE(n) (((n) & 15) << 0)
#define SDMMC_SDMMC_BLOCK_CNT(n) (((n) & 0xFFFF) << 0)

#define SDMMC_RD_ERR_INT (1 << 2)
#define SDMMC_WR_ERR_INT (1 << 3)
#define SDMMC_DAT_OVER_INT (1 << 4)
#define SDMMC_RXDMA_DONE_INT (1 << 6)
#define SDMMC_SDIO_INT (1 << 7)

#define SDMMC_SDIO_INT_MK (1 << 7)
#define SDMMC_DAT_OVER_CL (1 << 4)
#define SDMMC_TRANS_SPEED(n) (((n) & 0xFF) << 0)
#define SDMMC_MCLK_ADJUST(n) (((n) & 15) << 0)
#define SDMMC_CLK_INV (1 << 4)
#define SDMMC_CLK_DISA (1 << 5)

typedef union {
	u32 reg;
	struct {
		u32 operationNotOver : 1;
		u32 busy : 1;
		u32 dataLineBusy : 1;
		u32 suspend : 1;
		u32 : 4;
		u32 responseCrcError : 1;
		u32 noResponseReceived : 1;
		u32 : 2;
		u32 crcStatus : 3;
		u32 : 1;
		u32 dataError : 8;
	} fields;
} HAL_SDMMC_OP_STATUS_T;

typedef enum {
	HAL_SDMMC_DIRECTION_READ,
	HAL_SDMMC_DIRECTION_WRITE,
	HAL_SDMMC_DIRECTION_QTY,
} HAL_SDMMC_DIRECTION_T;

#endif /* __LINUX_SOC_RDA_MMC_H */
