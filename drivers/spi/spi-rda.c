/*******************************************************************************
 * FileName      :   spi-rda.c
 * Author          :   xiankuiwei
 * Description   :   The file is to achieve the rda's spi controller driver.
 * Version         :    rdadroid-4.0.4_r1.2  Date: 2012-12-12
 * Copyright      :   RDA Microelectronics Company Limited
 * Function List :
 *                 1.
 * History:
 *     <author>   <time>    <version >   <desc>
*******************************************************************************/
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/spi/spi.h>
#include <linux/slab.h>
#include <linux/soc/rda/hardware.h>
#include <linux/soc/rda/ifc.h>
#include <linux/soc/rda/spi.h>

#define spi_alloc_master spi_alloc_host
#define spi_master spi_controller
#define spi_master_get_devdata spi_controller_get_devdata
#define spi_register_master spi_register_controller
#define spi_unregister_master spi_unregister_controller
#define spi_master_put spi_controller_put

#define RDA_SPI_DRV_NAME "rda-spi"

struct rda_spi_device_data {
	unsigned char ifc_rxchannel;
	unsigned char ifc_txchannel;
	unsigned char csnum;
	unsigned char dmaMode;
};


/*
 * The core SPI transfer engine just talks to a register bank to set up
 * DMA transfers; transfer queue progress is driven by IRQs.  The clock
 * framework provides the base clock, subdivided for each spi_device.
 */
struct rda_spi {
	spinlock_t		lock;

	HWP_SPI_T		*regs;
	int			irq;
	struct clk		*master_clk;
	struct platform_device	*pdev;
	struct spi_device	*stay;

	u8			stopping;
	struct list_head	queue;
	struct spi_transfer	*current_transfer;
	unsigned long		current_remaining_bytes;
	struct spi_transfer	*next_transfer;
	unsigned long		next_remaining_bytes;
	u8			dma_support;
	HAL_IFC_REQUEST_ID_T	rxdmarequestid;
	HAL_IFC_REQUEST_ID_T	txdmarequestid;
	u8			dmarxchannel;
	u8			dmatxchannel;
	void			*buffer;
	dma_addr_t		buffer_dma;
};

/* Controller-specific per-slave state */
struct rda_spi_device {
	u32			ctrl;
	u32			cfg;
	u32			irqmask;
};

#define BUFFER_SIZE		PAGE_SIZE
#define INVALID_DMA_ADDRESS	0xffffffff
u32 rda_spi_get_cs(u32 csNum);

static u32 rda_spi_set_config(struct rda_spi *rdaspi, struct spi_device *spi)
{
	u32 ctrl = 0;
	u32 chip_select = spi_get_chipselect(spi, 0);
	struct rda_spi_device *rdarg = (struct rda_spi_device *)spi->controller_state;
	RDA_SPI_PARAMETERS * rdaSpiPara = (RDA_SPI_PARAMETERS *)spi->controller_data;

	/* we don't support clock phase position */
	if (rdaSpiPara){
		ctrl =  SPI_ENABLE
			| rda_spi_get_cs(chip_select)
			| (rdaSpiPara->inputEn?SPI_INPUT_MODE:0)
			| ((spi->mode & SPI_CPOL)?SPI_CLOCK_POLARITY:0)
			| SPI_CLOCK_DELAY(rdaSpiPara->clkDelay )
			| SPI_DO_DELAY(rdaSpiPara->doDelay )
			| SPI_DI_DELAY(rdaSpiPara->diDelay )
			| SPI_CS_DELAY(rdaSpiPara->csDelay )
			| SPI_CS_PULSE(rdaSpiPara->csPulse )
			| SPI_FRAME_SIZE(rdaSpiPara->frameSize + rdaSpiPara->spi_read_bits - 1)
			| SPI_OE_DELAY(rdaSpiPara->oeRatio)
			| ((rdaSpiPara->oeRatio == 0)?SPI_INPUT_SEL(1):0);
	}

	rdarg->ctrl = ctrl;
	return ctrl;
}

static void rda_cs_activate(struct rda_spi *rdaspi, struct spi_device *spi)
{
	struct rda_spi_device *rdacs = spi->controller_state;

	/*re-config spi registers*/
	/* Set the register */
	rdaspi->regs->ctrl = rda_spi_set_config(rdaspi, spi);
	rdaspi->regs->cfg = rdacs->cfg;
	rdaspi->regs->irq = rdacs->irqmask;
	rdaspi->dmarxchannel = HAL_UNKNOWN_CHANNEL;
	rdaspi->dmatxchannel = HAL_UNKNOWN_CHANNEL;

	return;
}

static bool rda_spi_tx_finish(struct rda_spi *rdaspi)
{
	u32 spiStatus;

	spiStatus = rdaspi->regs->status;

	if ((!(spiStatus & SPI_ACTIVE_STATUS))
	    && (SPI_TX_FIFO_SIZE == GET_BITFIELD(spiStatus, SPI_TX_SPACE)))
	{
		return true;
	}
	else
	{
		return false;
	}
}

static void rda_cs_deactivate(struct rda_spi *rdaspi, struct spi_device *spi)
{
	/* clear control register */
	rdaspi->regs->ctrl = 0;
	rdaspi->regs->cfg  = 0;

	/* release dma tx and rx ,now using auto realease*/

	/* clear the irq cause  */
	rdaspi->regs->status = 0xffffffff;

	return;
}

static inline int rda_spi_xfer_is_last(struct spi_message *msg,
					struct spi_transfer *xfer)
{
	return msg->transfers.prev == &xfer->transfer_list;
}

static inline int rda_spi_xfer_can_be_chained(struct spi_transfer *xfer)
{
	return spi_delay_to_ns(&xfer->delay, xfer) <= 0 && !xfer->cs_change;
}

static void rda_spi_next_xfer_data(struct spi_master *master,
				struct spi_transfer *xfer,
				dma_addr_t *tx_dma,
				dma_addr_t *rx_dma,
				u32 *plen)
{
	struct rda_spi	*rdaspi = spi_master_get_devdata(master);
	u32			len = *plen;

	/* use scratch buffer only when rx or tx data is unspecified */
	if (xfer->rx_buf)
		*rx_dma = xfer->rx_dma + xfer->len - *plen;
	else {
		*rx_dma = rdaspi->buffer_dma;
		if (len > BUFFER_SIZE)
			len = BUFFER_SIZE;
	}
	if (xfer->tx_buf)
		*tx_dma = xfer->tx_dma + xfer->len - *plen;
	else {
		*tx_dma = rdaspi->buffer_dma;
		if (len > BUFFER_SIZE)
			len = BUFFER_SIZE;
		memset(rdaspi->buffer, 0, len);
		dma_sync_single_for_device(&rdaspi->pdev->dev,
				rdaspi->buffer_dma, len, DMA_TO_DEVICE);
	}

	*plen = len;
}

/*
 * Submit next transfer for DMA.
 * lock is held, spi irq is blocked
 */
static void rda_spi_next_xfer(struct spi_master *master,
				struct spi_message *msg)
{
	struct rda_spi	*rdaspi = spi_master_get_devdata(master);
	struct spi_transfer	*xfer;
	u32			len, remaining;
	u32			freeRoom;
	dma_addr_t		tx_dma, rx_dma;
	HAL_IFC_MODE_T ifc_mode = HAL_IFC_SIZE_8_MODE_AUTO;

	if (!rdaspi->current_transfer)
		xfer = list_entry(msg->transfers.next,
				struct spi_transfer, transfer_list);
	else if (!rdaspi->next_transfer)
		xfer = list_entry(rdaspi->current_transfer->transfer_list.next,
				struct spi_transfer, transfer_list);
	else
		xfer = NULL;

	if (xfer) {
		len = xfer->len;

		if (rdaspi->dma_support){
			rda_spi_next_xfer_data(master, xfer, &tx_dma, &rx_dma, &len);
			remaining = xfer->len - len;

			if ((msg->spi->bits_per_word > 8) && (0 == master->bus_num)){
				ifc_mode = SYS_IFC_SIZE_HALF_WORD | SYS_IFC_AUTODISABLE;
			}else{
				ifc_mode = HAL_IFC_SIZE_8_MODE_AUTO;
			}

			/* Call dma interface to startf transfer*/
			rdaspi->dmarxchannel = ifc_transfer_start(rdaspi->rxdmarequestid,
								(u8*)rx_dma, len, ifc_mode);

			rdaspi->dmatxchannel = ifc_transfer_start(rdaspi->txdmarequestid,
								(u8*)tx_dma, len, ifc_mode);
		}else {
			freeRoom = GET_BITFIELD(rdaspi->regs->status, SPI_TX_SPACE);
			if (freeRoom > len)
			{
				freeRoom = len;
			}
			remaining = xfer->len - freeRoom;

			if (xfer->tx_buf){
				if (msg->spi->bits_per_word <= 8){
					iowrite8_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->tx_buf, freeRoom);
				}else if ((msg->spi->bits_per_word > 8) && (msg->spi->bits_per_word <= 16)){
					iowrite16_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->tx_buf, freeRoom);
				}else{
					iowrite32_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->tx_buf, freeRoom);
				}
			}

			while(!rda_spi_tx_finish(rdaspi));

			if (xfer->rx_buf){
				while(GET_BITFIELD(rdaspi->regs->status, SPI_RX_LEVEL) != freeRoom){
					 printk(KERN_WARNING"wait read ready,%d\n",GET_BITFIELD(rdaspi->regs->status, SPI_RX_LEVEL));
				}

				if (msg->spi->bits_per_word <= 8){
					ioread8_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->rx_buf, freeRoom);
				}else if((msg->spi->bits_per_word > 8) && (msg->spi->bits_per_word <= 16)){
					ioread16_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->rx_buf, freeRoom);
				}else{
					ioread32_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->rx_buf, freeRoom);
				}
			}
		}
	} else {
		xfer = rdaspi->next_transfer;
		remaining = rdaspi->next_remaining_bytes;
	}

	rdaspi->current_transfer = xfer;
	rdaspi->current_remaining_bytes = remaining;

	if (remaining > 0)
		len = remaining;
	else if (!rda_spi_xfer_is_last(msg, xfer)
			&& rda_spi_xfer_can_be_chained(xfer)) {
		xfer = list_entry(xfer->transfer_list.next,
				struct spi_transfer, transfer_list);
		len = xfer->len;
	} else
		xfer = NULL;

	rdaspi->next_transfer = xfer;

	if (xfer) {
		u32	total = len;

		if (rdaspi->dma_support){
			if ((msg->spi->bits_per_word > 8) && (0 == master->bus_num)){
					ifc_mode = SYS_IFC_SIZE_HALF_WORD | SYS_IFC_AUTODISABLE;
				}else{
				ifc_mode = HAL_IFC_SIZE_8_MODE_AUTO;
			}

			rda_spi_next_xfer_data(master, xfer, &tx_dma, &rx_dma, &len);
			rdaspi->next_remaining_bytes = total - len;

			/* Call dma interface to startf transfer*/
			rdaspi->dmarxchannel = ifc_transfer_start(rdaspi->rxdmarequestid,
								(u8*)rx_dma, len, ifc_mode);


			rdaspi->dmatxchannel = ifc_transfer_start(rdaspi->txdmarequestid,
								(u8*)tx_dma, len, ifc_mode);
		}else{
			freeRoom = GET_BITFIELD(rdaspi->regs->status, SPI_TX_SPACE);
			if (freeRoom > len){
				freeRoom = len;
			}
			rdaspi->next_remaining_bytes = total - freeRoom;

			if (xfer->tx_buf){
				if (msg->spi->bits_per_word <= 8){
					iowrite8_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->tx_buf, freeRoom);
				}else if((msg->spi->bits_per_word > 8) && (msg->spi->bits_per_word <= 16)){
					iowrite16_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->tx_buf, freeRoom);
				}else{
					iowrite32_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->tx_buf, freeRoom);
				}
			}

			while(!rda_spi_tx_finish(rdaspi));

			if (xfer->rx_buf){
				if (msg->spi->bits_per_word <= 8){
					ioread8_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->rx_buf, freeRoom);
				}else if((msg->spi->bits_per_word > 8) && (msg->spi->bits_per_word <= 16)){
					ioread16_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->rx_buf, freeRoom);
				}else{
					ioread32_rep((void __iomem *)(&rdaspi->regs->rxtx_buffer),
							xfer->rx_buf, freeRoom);
				}
			}
		}

	}

	/* enable the interrupt */
	if (rdaspi->dma_support ){
		rdaspi->regs->irq |= (SPI_MASK_RX_OVF_IRQ | SPI_MASK_RX_DMA_IRQ | SPI_MASK_TX_DMA_IRQ);
	}else{
		rdaspi->regs->irq |= (SPI_MASK_RX_OVF_IRQ | SPI_MASK_TX_TH_IRQ | SPI_MASK_RX_TH_IRQ);
	}

	return;
}

static void rda_spi_next_message(struct spi_master *master)
{
	struct rda_spi	*rdaspi = spi_master_get_devdata(master);
	struct spi_message	*msg;
	struct spi_device	*spi;

	msg = list_entry(rdaspi->queue.next, struct spi_message, queue);
	spi = msg->spi;

	/* select chip if it's not still active */
	if (rdaspi->stay) {
		if (rdaspi->stay != spi) {
			rda_cs_deactivate(rdaspi, rdaspi->stay);
			rda_cs_activate(rdaspi, spi);
		}
		rdaspi->stay = NULL;
	} else
		rda_cs_activate(rdaspi, spi);

	rda_spi_next_xfer(master, msg);
}

/*
 * For DMA, tx_buf/tx_dma have the same relationship as rx_buf/rx_dma:
 *  - The buffer is either valid for CPU access, else NULL
 *  - If the buffer is valid, so is its DMA address
 *
 * This driver always maps/unmaps transfer buffers when DMA mode is used.
 */
static int
rda_spi_dma_map_xfer(struct rda_spi *rdaspi, struct spi_transfer *xfer)
{
	struct device	*dev = &rdaspi->pdev->dev;

	xfer->tx_dma = xfer->rx_dma = INVALID_DMA_ADDRESS;
	if (xfer->tx_buf) {
		/* tx_buf is a const void* where we need a void * for the dma
		 * mapping */
		void *nonconst_tx = (void *)xfer->tx_buf;

		xfer->tx_dma = dma_map_single(dev,nonconst_tx, xfer->len,
						DMA_TO_DEVICE);
		if (dma_mapping_error(dev, xfer->tx_dma))
			return -ENOMEM;
	}
	if (xfer->rx_buf) {
		xfer->rx_dma = dma_map_single(dev,xfer->rx_buf, xfer->len,
						DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, xfer->rx_dma)) {
			if (xfer->tx_buf)
				dma_unmap_single(dev,xfer->tx_dma,
						 xfer->len, DMA_TO_DEVICE);
			return -ENOMEM;
		}
	}

	return 0;
}

static void rda_spi_dma_unmap_xfer(struct spi_master *master,
				     struct spi_transfer *xfer)
{
	if (xfer->tx_dma != INVALID_DMA_ADDRESS)
		dma_unmap_single(master->dev.parent, xfer->tx_dma,
				 xfer->len, DMA_TO_DEVICE);
	if (xfer->rx_dma != INVALID_DMA_ADDRESS)
		dma_unmap_single(master->dev.parent, xfer->rx_dma,
				 xfer->len, DMA_FROM_DEVICE);
}

static void rda_spi_msg_done(struct spi_master *master, struct rda_spi *rdaspi,
									struct spi_message *msg, int status, int stay)
{
	if (!stay || status < 0)
		rda_cs_deactivate(rdaspi, msg->spi);
	else
		rdaspi->stay = msg->spi;

	list_del(&msg->queue);
	msg->status = status;

	spin_unlock(&rdaspi->lock);
	msg->complete(msg->context);
	spin_lock(&rdaspi->lock);

	rdaspi->current_transfer = NULL;
	rdaspi->next_transfer = NULL;

	/* continue if needed */
	if (list_empty(&rdaspi->queue) || rdaspi->stopping)
		return;
	else
		rda_spi_next_message(master);
}

static irqreturn_t rda_spi_interrupt(int irq, void *dev_id)
{
	struct spi_master	*master = dev_id;
	struct rda_spi	*rdaspi = spi_master_get_devdata(master);
	struct spi_message	*msg;
	struct spi_transfer	*xfer;
	u32			status;
	int			ret = IRQ_NONE;

	spin_lock(&rdaspi->lock);

	xfer = rdaspi->current_transfer;
	msg = list_entry(rdaspi->queue.next, struct spi_message, queue);

	status = rdaspi->regs->status;

	if (status & SPI_CAUSE_RX_OVF_IRQ ) {
		int timeout;

		ret = IRQ_HANDLED;
		rdaspi->regs->status |= SPI_CAUSE_RX_OVF_IRQ;

		/* disable rx irq  */
		if(!rdaspi->dma_support){
			rdaspi->regs->irq &= ~(SPI_MASK_RX_OVF_IRQ | SPI_MASK_RX_TH_IRQ | SPI_MASK_TX_TH_IRQ);
		}else{
			rdaspi->regs->irq &= ~(SPI_MASK_RX_OVF_IRQ | SPI_MASK_RX_DMA_IRQ | SPI_MASK_TX_DMA_IRQ);
		}

		/*
		 * When we get an overrun, we disregard the current
		 * transfer. Data will not be copied back from any
		 * bounce buffer and msg->actual_len will not be
		 * updated with the last xfer.
		 *
		 * We will also not process any remaning transfers in
		 * the message.
		 *
		 * First, stop the transfer and unmap the DMA buffers.
		 */
		if (rdaspi->dma_support)
			rda_spi_dma_unmap_xfer(master, xfer);

		spi_delay_exec(&xfer->delay, xfer);

		for (timeout = 1000; timeout; timeout--)
			if (!(rdaspi->regs->status & SPI_ACTIVE_STATUS))
				break;

		if (!timeout)
			dev_dbg(master->dev.parent,"timeout waiting for TXEMPTY");

		rda_spi_msg_done(master, rdaspi, msg, -EIO, 0);
	} else if (status & (SPI_CAUSE_RX_DMA_IRQ | SPI_CAUSE_TX_DMA_IRQ
			| SPI_CAUSE_TX_TH_IRQ | SPI_CAUSE_RX_TH_IRQ)) {
		ret = IRQ_HANDLED;

		/* disable the  corresponding interrupt*/
		if(rdaspi->dma_support){
			rdaspi->regs->status |= (SPI_CAUSE_RX_DMA_IRQ | SPI_CAUSE_TX_DMA_IRQ);
			rdaspi->regs->irq &= ~(SPI_MASK_RX_OVF_IRQ | SPI_MASK_RX_DMA_IRQ | SPI_MASK_TX_DMA_IRQ);
		} else {
			rdaspi->regs->irq &= ~(SPI_MASK_RX_OVF_IRQ | SPI_MASK_RX_TH_IRQ | SPI_MASK_TX_TH_IRQ);
		}

		if (rdaspi->current_remaining_bytes == 0) {
			msg->actual_length += xfer->len;

			/* flush the fifo */
			rdaspi->regs->status |= SPI_FIFO_FLUSH;

			if (rdaspi->dma_support)
				rda_spi_dma_unmap_xfer(master, xfer);

			spi_delay_exec(&xfer->delay, xfer);

			if (rda_spi_xfer_is_last(msg, xfer)) {
				/* report completed message */
				rda_spi_msg_done(master, rdaspi, msg, 0,
						xfer->cs_change);
			} else {
				if (xfer->cs_change) {
					rda_cs_deactivate(rdaspi, msg->spi);
					udelay(1);
					rda_cs_activate(rdaspi, msg->spi);
				}

				/*
				 * Not done yet. Submit the next transfer.
				 */
				rda_spi_next_xfer(master, msg);
			}
		} else {
			/*
			 * Keep going, we still have data to send in
			 * the current transfer.
			 */
			 rda_spi_next_xfer(master, msg);
		}
	}

	spin_unlock(&rdaspi->lock);

	return ret;
}

u32 rda_spi_get_cs(u32 csNum)
{
	u32 result=0;

	switch (csNum){
		case 0:
			result = SPI_CS_SEL_CS0;
			break;
		case 1:
			result = SPI_CS_SEL_CS1;
			break;
		case 2:
			result = SPI_CS_SEL_CS2;
			break;
		case 3:
			result = SPI_CS_SEL_CS3;
			break;
		default:
			break;;
	}
	return result;
}

static int rda_spi_setup(struct spi_device *spi)
{
	struct rda_spi	*rdaspi;
	struct rda_spi_device	*rdarg;
	u32		chip_select = spi_get_chipselect(spi, 0);
	u32		mclk, clk_div;
	u32		bits = spi->bits_per_word;
	u32		cfg = 0;
	u32		ctrl= 0;
	u32		irqMask = 0;
	unsigned active = spi->mode & SPI_CS_HIGH;
	RDA_SPI_PARAMETERS * rdaSpiPara = (RDA_SPI_PARAMETERS *)spi->controller_data;

	rdaspi = spi_master_get_devdata(spi->controller);

	if (rdaspi->stopping)
		return -ESHUTDOWN;

	if (chip_select >= spi->controller->num_chipselect) {
		dev_dbg(&spi->dev,
				"setup: invalid chipselect %u (%u defined)\n",
				chip_select, spi->controller->num_chipselect);
		return -EINVAL;
	}

	if (bits < 4 || bits > 32) {
		dev_dbg(&spi->dev,
				"setup: invalid bits_per_word %u (8 to 16)\n",
				bits);
		return -EINVAL;
	}

	/* we don't support clock phase position */
	if (rdaSpiPara){
		ctrl =  SPI_ENABLE
			| rda_spi_get_cs(chip_select)
			| (rdaSpiPara->inputEn?SPI_INPUT_MODE:0)
			| ((spi->mode & SPI_CPOL)?SPI_CLOCK_POLARITY:0)
			| SPI_CLOCK_DELAY(rdaSpiPara->clkDelay )
			| SPI_DO_DELAY(rdaSpiPara->doDelay )
			| SPI_DI_DELAY(rdaSpiPara->diDelay )
			| SPI_CS_DELAY(rdaSpiPara->csDelay )
			| SPI_CS_PULSE(rdaSpiPara->csPulse )
			| SPI_FRAME_SIZE(bits - 1)
			| SPI_OE_DELAY(rdaSpiPara->oeRatio)
			| ((rdaSpiPara->oeRatio == 0)?SPI_INPUT_SEL(1):0);
	}

	mclk = clk_get_rate(rdaspi->master_clk);
	if (spi->max_speed_hz) {
		clk_div = mclk / ( 2 * spi->max_speed_hz);
		if (mclk % (2 * spi->max_speed_hz))
			clk_div += 1;

		clk_div = (clk_div) ? (clk_div - 1) : 0;

		if (clk_div > 1023){
			dev_warn(&spi->dev,
				"%d Hz too slow, use slowest %d\n",
				spi->max_speed_hz, (int)mclk/2048);
			clk_div = 1023;
		}
		dev_info(&spi->dev,
			"max_speed_hz = %d, bus_speed_hz = %d, divider = %d\n",
			spi->max_speed_hz, (int)mclk, clk_div);
	} else{
		dev_warn(&spi->dev, "no max_speed_hz specified, use slowest %u\n",
			(int)mclk/2048);
		clk_div = 1023;
	}

	cfg |= SPI_CLOCK_DIVIDER(clk_div);

	switch (chip_select) {
		case 0:
		if (!active){
			cfg |= SPI_CS_POLARITY_0_ACTIVE_LOW;
		}
		break;

		case 1:
		if (!active){
			cfg |= SPI_CS_POLARITY_1_ACTIVE_LOW;
		}
		break;

		case 2:
		if (!active){
			cfg |= SPI_CS_POLARITY_2_ACTIVE_LOW;
		}
		break;

		case 3:
		if (!active){
			cfg |= SPI_CS_POLARITY_3_ACTIVE_LOW;
		}
		break;

		default:
		break;
	}
	rdarg = spi->controller_state;
	if (!rdarg) {
		rdarg = kzalloc(sizeof(struct rda_spi_device), GFP_KERNEL);
		if (!rdarg)
			return -ENOMEM;

		spi->controller_state = rdarg;
	} else {
		unsigned long       flags;

		spin_lock_irqsave(&rdaspi->lock, flags);
		if (rdaspi->stay == spi)
			rdaspi->stay = NULL;
		rda_cs_deactivate(rdaspi, spi);
		spin_unlock_irqrestore(&rdaspi->lock, flags);
	}
	
	if (!rdaspi->dma_support)
		irqMask |= SPI_TX_THRESHOLD_12_EMPTY_SLOTS;

	rdarg->ctrl = ctrl;
	rdarg->cfg = cfg;
	rdarg->irqmask = irqMask;

	return 0;
}

static int rda_spi_transfer(struct spi_device *spi, struct spi_message *msg)
{
	struct rda_spi	*rdaspi;
	struct spi_transfer	*xfer;
	unsigned long		flags;
	//struct device		*controller = spi->master->dev.parent;
	//u8			bits;
	//struct rda_spi_device	*asd;

	rdaspi = spi_master_get_devdata(spi->controller);

	if (unlikely(list_empty(&msg->transfers)))
		return -EINVAL;

	if (rdaspi->stopping)
		return -ESHUTDOWN;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (!(xfer->tx_buf || xfer->rx_buf) && xfer->len) {
			dev_dbg(&spi->dev,"missing rx or tx buf\n");
			return -EINVAL;
		}

		/* FIXME implement these protocol options!! */
		if (xfer->speed_hz) {
			dev_dbg(&spi->dev,"no protocol options yet\n");
			//return -ENOPROTOOPT;
		}

		/*
		 * DMA map early, for performance (empties dcache ASAP) and
		 * better fault reporting.  This is a DMA-only driver.
		 *
		 * NOTE that if dma_unmap_single() ever starts to do work on
		 * platforms supported by this driver, we would need to clean
		 * up mappings for previously-mapped transfers.
		 */
		if (rdaspi->dma_support) {
			if (rda_spi_dma_map_xfer(rdaspi, xfer) < 0){
				dev_dbg(&spi->dev,"rda_spi_dma_map_xfer error\n");
				return -ENOMEM;
			}
		}
	}

	msg->status = -EINPROGRESS;
	msg->actual_length = 0;

	spin_lock_irqsave(&rdaspi->lock, flags);
	list_add_tail(&msg->queue, &rdaspi->queue);
	if (!rdaspi->current_transfer)
		rda_spi_next_message(spi->controller);
	spin_unlock_irqrestore(&rdaspi->lock, flags);

	return 0;
}

static void rda_spi_cleanup(struct spi_device *spi)
{
	struct rda_spi	*rdaspi = spi_master_get_devdata(spi->controller);
	struct rda_spi_device	*rdasd = spi->controller_state;
	unsigned long		flags;

	if (!rdasd)
		return;

	spin_lock_irqsave(&rdaspi->lock, flags);
	if (rdaspi->stay == spi) {
		rdaspi->stay = NULL;
		rda_cs_deactivate(rdaspi, spi);
	}
	spin_unlock_irqrestore(&rdaspi->lock, flags);
	/*realease the state*/
	spi->controller_state = NULL;
	kfree(rdasd);
}

static int rda_spi_bus_num(struct platform_device *pdev, struct resource *res_mem)
{
	int bus_num = pdev->id;

	if (bus_num < 0 && pdev->dev.of_node)
		bus_num = of_alias_get_id(pdev->dev.of_node, "spi");

	if (bus_num >= 0)
		return bus_num;

	if (res_mem->start == 0x20A20000)
		return 0;
	if (res_mem->start == 0x20A30000)
		return 1;
	if (res_mem->start == 0x20A40000)
		return 2;

	return 0;
}

static void rda_spi_default_data(int bus_num, struct rda_spi_device_data *pdata)
{
	memset(pdata, 0, sizeof(*pdata));

	switch (bus_num) {
	case 0:
		pdata->ifc_rxchannel = HAL_IFC_SPI_RX;
		pdata->ifc_txchannel = HAL_IFC_SPI_TX;
		pdata->csnum = 3;
		break;
	case 1:
		pdata->ifc_rxchannel = HAL_IFC_SPI2_RX;
		pdata->ifc_txchannel = HAL_IFC_SPI2_TX;
		pdata->csnum = 3;
		break;
	default:
		pdata->ifc_rxchannel = HAL_IFC_SPI3_RX;
		pdata->ifc_txchannel = HAL_IFC_SPI3_TX;
		pdata->csnum = 2;
		break;
	}
}

static int rda_spi_probe(struct platform_device *pdev)
{
	struct resource		*res_mem;
	struct resource		*res_irq;
	void __iomem		*regs;
	int			ret = -ENOMEM;
	struct spi_master	*master;
	struct rda_spi	*rdaspi;
	struct rda_spi_device_data pdata_data;
	struct rda_spi_device_data *pdata = &pdata_data;
	int bus_num;
	u32 cs_num;

	res_mem  = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res_mem )
		return -ENXIO;

	res_irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res_irq)
		return -ENXIO;

	bus_num = rda_spi_bus_num(pdev, res_mem);
	rda_spi_default_data(bus_num, pdata);
	if (pdev->dev.platform_data)
		memcpy(pdata, pdev->dev.platform_data, sizeof(*pdata));
	if (pdev->dev.of_node && !of_property_read_u32(pdev->dev.of_node, "num-cs", &cs_num))
		pdata->csnum = cs_num;

	/* setup spi core then rda-specific driver state */
	master = spi_alloc_master(&pdev->dev, sizeof *rdaspi);
	if (!master)
		goto out_free;

	/* the spi->mode bits understood by this driver: */
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;

	master->bus_num = bus_num;
	master->num_chipselect = pdata->csnum;
	master->setup = rda_spi_setup;
	master->transfer = rda_spi_transfer;
	master->cleanup = rda_spi_cleanup;
	platform_set_drvdata(pdev, master);

	rdaspi = spi_master_get_devdata(master);

	/*
	 * Scratch buffer is used for throwaway rx and tx data.
	 * It's coherent to minimize dcache pollution.
	 */
	rdaspi->buffer = dma_alloc_coherent(&pdev->dev, BUFFER_SIZE,
					&rdaspi->buffer_dma, GFP_KERNEL);
	if (!rdaspi->buffer){
		goto out_free;
	}

	spin_lock_init(&rdaspi->lock);
	INIT_LIST_HEAD(&rdaspi->queue);
	rdaspi->pdev = pdev;
	regs = devm_ioremap_resource(&pdev->dev, res_mem);
	if (IS_ERR(regs)) {
		ret = PTR_ERR(regs);
		goto out_free_buffer;
	}
	rdaspi->regs = (HWP_SPI_T *)regs;

	rdaspi->irq = res_irq->start;
	rdaspi->master_clk = devm_clk_get_optional(&pdev->dev, NULL);
	if (IS_ERR(rdaspi->master_clk)) {
		ret = PTR_ERR(rdaspi->master_clk);
		goto out_free_buffer;
	}
	if (rdaspi->master_clk) {
		ret = clk_prepare_enable(rdaspi->master_clk);
		if (ret)
			goto out_free_buffer;
	}
	rdaspi->rxdmarequestid = pdata->ifc_rxchannel;
	rdaspi->txdmarequestid = pdata->ifc_txchannel;
	rdaspi->dma_support = 0;
	#if defined(CONFIG_SPI_RDA_DMA)
	{
		rdaspi->dma_support = 1;
	}
	#endif

	ret = request_irq(rdaspi->irq, rda_spi_interrupt, 0,
			dev_name(&pdev->dev), master);
	if (ret)
		goto out_put_clk;

	ret = spi_register_master(master);
	if (ret)
		goto out_reset_hw;

	rdaspi->regs->ctrl = 0;
	rdaspi->regs->cfg = 0;
	rdaspi->regs->irq = 0;
	rdaspi->dmarxchannel = HAL_UNKNOWN_CHANNEL;
	rdaspi->dmatxchannel = HAL_UNKNOWN_CHANNEL;

	return 0;

out_reset_hw:
	free_irq(rdaspi->irq, master);
out_put_clk:
	if (rdaspi->master_clk)
		clk_disable_unprepare(rdaspi->master_clk);
out_free_buffer:
	dma_free_coherent(&pdev->dev, BUFFER_SIZE, rdaspi->buffer,
			rdaspi->buffer_dma);
out_free:
	spi_master_put(master);
	return ret;
}

static void rda_spi_remove(struct platform_device *pdev)
{
	struct spi_master	*master = platform_get_drvdata(pdev);
	struct rda_spi	*rdaspi = spi_master_get_devdata(master);
	struct spi_message	*msg;

	/* reset the hardware and block queue progress */
	spin_lock_irq(&rdaspi->lock);
	rdaspi->regs->ctrl = 0;
	rdaspi->regs->cfg = 0;
	rdaspi->regs->irq = 0;

	/* release dma tx and rx */

	spin_unlock_irq(&rdaspi->lock);

	/* Terminate remaining queued transfers */
	list_for_each_entry(msg, &rdaspi->queue, queue) {
		/* REVISIT unmapping the dma is a NOP on ARM and AVR32
		 * but we shouldn't depend on that...
		 */
		msg->status = -ESHUTDOWN;
		msg->complete(msg->context);
	}

	dma_free_coherent(&pdev->dev, BUFFER_SIZE, rdaspi->buffer,
			rdaspi->buffer_dma);

	if (rdaspi->master_clk)
		clk_disable_unprepare(rdaspi->master_clk);

	free_irq(rdaspi->irq, master);

	spi_unregister_master(master);
}

static const struct of_device_id rda_spi_of_match[] = {
	{ .compatible = "rda,8810pl-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, rda_spi_of_match);

static struct platform_driver rda_spi_driver = {
	.driver		= {
		.name	= RDA_SPI_DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = rda_spi_of_match,
	},
	.probe		= rda_spi_probe,
	.remove		= rda_spi_remove,
};
module_platform_driver(rda_spi_driver);

MODULE_DESCRIPTION("RDA SPI Controller driver");
MODULE_AUTHOR("Kuiwei Xian");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:RDA");
