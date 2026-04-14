/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RDA_IFC_H
#define __LINUX_SOC_RDA_IFC_H

#include <linux/scatterlist.h>
#include <linux/types.h>

#define HAL_UNKNOWN_CHANNEL 0xff

#define SYS_IFC_STD_CHAN_NB 11

#define SYS_IFC_ENABLE (1 << 0)
#define SYS_IFC_DISABLE (1 << 1)
#define SYS_IFC_CH_RD_HW_EXCH (1 << 2)
#define SYS_IFC_AUTODISABLE (1 << 4)
#define SYS_IFC_SIZE_BYTE (0 << 5)
#define SYS_IFC_SIZE_HALF_WORD (1 << 5)
#define SYS_IFC_SIZE_WORD (2 << 5)
#define SYS_IFC_REQ_SRC(n) (((n) & 31) << 8)
#define SYS_IFC_FLUSH (1 << 16)
#define SYS_IFC_FIFO_EMPTY (1 << 4)

typedef enum {
	HAL_IFC_UART_TX,
	HAL_IFC_UART_RX,
	HAL_IFC_UART2_TX,
	HAL_IFC_UART2_RX,
	HAL_IFC_SPI_TX,
	HAL_IFC_SPI_RX,
	HAL_IFC_SPI2_TX,
	HAL_IFC_SPI2_RX,
	HAL_IFC_SPI3_TX,
	HAL_IFC_SPI3_RX,
	HAL_IFC_SDMMC_TX,
	HAL_IFC_SDMMC_RX,
	HAL_IFC_SDMMC2_TX,
	HAL_IFC_SDMMC2_RX,
	HAL_IFC_SDMMC3_TX,
	HAL_IFC_SDMMC3_RX,
	HAL_IFC_NFSC_TX,
	HAL_IFC_NFSC_RX,
	HAL_IFC_UART3_TX,
	HAL_IFC_UART3_RX,
	HAL_IFC_NO_REQWEST,
} HAL_IFC_REQUEST_ID_T;

typedef enum {
	HAL_IFC_SIZE_8_MODE_MANUAL = 0,
	HAL_IFC_SIZE_8_MODE_AUTO = SYS_IFC_AUTODISABLE,
	HAL_IFC_SIZE_32_MODE_MANUAL = SYS_IFC_SIZE_WORD,
	HAL_IFC_SIZE_32_MODE_AUTO = SYS_IFC_SIZE_WORD | SYS_IFC_AUTODISABLE,
} HAL_IFC_MODE_T;

typedef struct {
	struct scatterlist *sg;
	u32 sg_len;
	HAL_IFC_REQUEST_ID_T request;
	u32 channel;
} HAL_IFC_CFG_T;

u8 ifc_transfer_sg_start(HAL_IFC_CFG_T *ifc, u32 xfer_size,
			 HAL_IFC_MODE_T ifc_mode);
u32 ifc_transfer_sg_get_tc(HAL_IFC_CFG_T *ifc);
void ifc_transfer_sg_stop(HAL_IFC_CFG_T *ifc);

u8 ifc_transfer_start(HAL_IFC_REQUEST_ID_T request_id, u8 *mem_addr,
		      u32 xfer_size, HAL_IFC_MODE_T ifc_mode);
u32 ifc_transfer_get_tc(HAL_IFC_REQUEST_ID_T request_id, u8 channel);
void ifc_transfer_flush(HAL_IFC_REQUEST_ID_T request_id, u8 channel);
void ifc_transfer_stop(HAL_IFC_REQUEST_ID_T request_id, u8 channel);

#endif /* __LINUX_SOC_RDA_IFC_H */
