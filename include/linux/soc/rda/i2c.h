/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RDA_I2C_H
#define __LINUX_SOC_RDA_I2C_H

#include <linux/soc/rda/hardware.h>

#define RDA_I2C_REG_CTRL 0x0000
#define RDA_I2C_REG_STATUS 0x0004
#define RDA_I2C_REG_TXRX_BUFFER 0x0008
#define RDA_I2C_REG_CMD 0x000c
#define RDA_I2C_REG_IRQ_CLR 0x0010

typedef volatile struct {
	REG32 CTRL;
	REG32 STATUS;
	REG32 TXRX_BUFFER;
	REG32 CMD;
	REG32 IRQ_CLR;
} HWP_I2C_MASTER_T;

/* CTRL */
#define I2C_MASTER_EN (1 << 0)
#define I2C_MASTER_IRQ_MASK (1 << 8)
#define I2C_MASTER_CLOCK_PRESCALE(n) (((n) & 0xFFFF) << 16)
#define I2C_MASTER_CLOCK_PRESCALE_MASK (0xFFFF << 16)

/* STATUS */
#define I2C_MASTER_IRQ_CAUSE (1 << 0)
#define I2C_MASTER_IRQ_STATUS (1 << 4)
#define I2C_MASTER_TIP (1 << 8)
#define I2C_MASTER_AL (1 << 12)
#define I2C_MASTER_BUSY (1 << 16)
#define I2C_MASTER_RXACK (1 << 20)

/* CMD */
#define I2C_MASTER_ACK (1 << 0)
#define I2C_MASTER_RD (1 << 4)
#define I2C_MASTER_STO (1 << 8)
#define I2C_MASTER_WR (1 << 12)
#define I2C_MASTER_STA (1 << 16)

/* IRQ_CLR */
#define I2C_MASTER_IRQ_CLR (1 << 0)

#endif /* __LINUX_SOC_RDA_I2C_H */