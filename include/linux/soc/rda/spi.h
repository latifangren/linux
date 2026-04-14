/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RDA_SPI_H
#define __LINUX_SOC_RDA_SPI_H

#include <linux/types.h>
#include <linux/soc/rda/hardware.h>

#define SPI_TX_FIFO_SIZE 16
#define SPI_RX_FIFO_SIZE 16

typedef volatile struct {
	REG32 ctrl;
	REG32 status;
	REG32 rxtx_buffer;
	REG32 cfg;
	REG32 pattern;
	REG32 stream;
	REG32 pin_control;
	REG32 irq;
} HWP_SPI_T;

#define SPI_ENABLE (1 << 0)
#define SPI_CS_SEL_CS0 (0 << 1)
#define SPI_CS_SEL_CS1 (1 << 1)
#define SPI_CS_SEL_CS2 (2 << 1)
#define SPI_CS_SEL_CS3 (3 << 1)
#define SPI_INPUT_MODE (1 << 4)
#define SPI_CLOCK_POLARITY (1 << 5)
#define SPI_CLOCK_DELAY(n) (((n) & 3) << 6)
#define SPI_DO_DELAY(n) (((n) & 3) << 8)
#define SPI_DI_DELAY(n) (((n) & 3) << 10)
#define SPI_CS_DELAY(n) (((n) & 3) << 12)
#define SPI_CS_PULSE(n) (((n) & 3) << 14)
#define SPI_FRAME_SIZE(n) (((n) & 31) << 16)
#define SPI_OE_DELAY(n) (((n) & 31) << 24)
#define SPI_INPUT_SEL(n) (((n) & 3) << 30)

#define SPI_ACTIVE_STATUS (1 << 0)
#define SPI_CAUSE_RX_OVF_IRQ (1 << 3)
#define SPI_CAUSE_TX_TH_IRQ (1 << 4)
#define SPI_CAUSE_TX_DMA_IRQ (1 << 5)
#define SPI_CAUSE_RX_TH_IRQ (1 << 6)
#define SPI_CAUSE_RX_DMA_IRQ (1 << 7)
#define SPI_TX_SPACE(n) (((n) & 31) << 16)
#define SPI_TX_SPACE_MASK (31 << 16)
#define SPI_TX_SPACE_SHIFT 16
#define SPI_RX_LEVEL(n) (((n) & 31) << 24)
#define SPI_RX_LEVEL_MASK (31 << 24)
#define SPI_RX_LEVEL_SHIFT 24
#define SPI_FIFO_FLUSH (1 << 30)

#define SPI_CS_POLARITY_0_ACTIVE_LOW (1 << 0)
#define SPI_CS_POLARITY_1_ACTIVE_LOW (1 << 1)
#define SPI_CS_POLARITY_2_ACTIVE_LOW (1 << 2)
#define SPI_CS_POLARITY_3_ACTIVE_LOW (1 << 3)
#define SPI_CLOCK_DIVIDER(n) (((n) & 0x3FF) << 16)

#define SPI_MASK_RX_OVF_IRQ (1 << 0)
#define SPI_MASK_TX_TH_IRQ (1 << 1)
#define SPI_MASK_TX_DMA_IRQ (1 << 2)
#define SPI_MASK_RX_TH_IRQ (1 << 3)
#define SPI_MASK_RX_DMA_IRQ (1 << 4)
#define SPI_TX_THRESHOLD_12_EMPTY_SLOTS (3 << 5)

typedef enum {
	RDA_SPI_HALF_CLK_PERIOD_0,
	RDA_SPI_HALF_CLK_PERIOD_1,
	RDA_SPI_HALF_CLK_PERIOD_2,
	RDA_SPI_HALF_CLK_PERIOD_3,
} RDA_SPI_DELAY_T;

typedef enum {
	RDA_SPI_TX_TRIGGER_1_EMPTY,
	RDA_SPI_TX_TRIGGER_4_EMPTY,
	RDA_SPI_TX_TRIGGER_8_EMPTY,
	RDA_SPI_TX_TRIGGER_12_EMPTY,
} RDA_SPI_TX_TRIGGER_CFG_T;

typedef enum {
	RDA_SPI_RX_TRIGGER_1_BYTE,
	RDA_SPI_RX_TRIGGER_4_BYTE,
	RDA_SPI_RX_TRIGGER_8_BYTE,
	RDA_SPI_RX_TRIGGER_12_BYTE,
} RDA_SPI_RX_TRIGGER_CFG_T;

typedef enum {
	RDA_SPI_DIRECT_POLLING = 0,
	RDA_SPI_DIRECT_IRQ,
	RDA_SPI_DMA_POLLING,
	RDA_SPI_DMA_IRQ,
	RDA_SPI_OFF,
} RDA_SPI_TRANSFERT_MODE_T;

typedef struct {
	unsigned int rxOvf : 1;
	unsigned int txTh : 1;
	unsigned int txDmaDone : 1;
	unsigned int rxTh : 1;
	unsigned int rxDmaDone : 1;
} RDA_SPI_IRQ_STATUS_T;

typedef void (*RDA_SPI_IRQ_HANDLER_T)(RDA_SPI_IRQ_STATUS_T);

typedef struct {
	unsigned char inputEn;
	RDA_SPI_DELAY_T clkDelay;
	RDA_SPI_DELAY_T doDelay;
	RDA_SPI_DELAY_T diDelay;
	RDA_SPI_DELAY_T csDelay;
	RDA_SPI_DELAY_T csPulse;
	u32 frameSize;
	u8 oeRatio;
	RDA_SPI_RX_TRIGGER_CFG_T rxTrigger;
	RDA_SPI_TX_TRIGGER_CFG_T txTrigger;
	RDA_SPI_TRANSFERT_MODE_T rxMode;
	RDA_SPI_TRANSFERT_MODE_T txMode;
	RDA_SPI_IRQ_STATUS_T mask;
	RDA_SPI_IRQ_HANDLER_T handler;
	u8 spi_read_bits;
} RDA_SPI_PARAMETERS;

#endif /* __LINUX_SOC_RDA_SPI_H */
