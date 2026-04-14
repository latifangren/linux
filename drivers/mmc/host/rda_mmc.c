#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdesc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <rda/mach/ifc.h>
#include <rda/mach/iomap.h>
#include <rda/mach/regulator.h>
#include <rda/plat/devices.h>
#include <rda/plat/reg_sysctrl.h>
#include <rda/plat/reg_mmc.h>
#include <rda/plat/reg_cfg_regs.h>
#include <rda/plat/cpu.h>
#include <linux/irq.h>
#include <linux/irqnr.h>
#include <linux/kernel.h>

#define RDA_MMC_USE_INT

#define RDA_MMC0_REQ_NUM_PAGE	(16)

#define MCD_CMD_TIMEOUT_MS	( 100 )
#define MCD_DATA_TIMEOUT_MS 	( 5000 )
#define RDA_MMC_HOST_COUNT 	( 4 )

typedef struct
{
	u8* sysMemAddr;
	u32 blockNum;
	u32 blockSize;
	HAL_SDMMC_DIRECTION_T direction;
	HAL_IFC_REQUEST_ID_T ifcReq;
	u32 channel;
} HAL_SDMMC_TRANSFER_T;

struct rda_mmc_host {
	struct mmc_host *mmc;
	struct mmc_request	*mrq;
	struct mmc_command	*cmd;
	struct mmc_data *data;
	
	spinlock_t		lock;
	void __iomem		*base;		/* virtual */
	int 		irq;

	HAL_SDMMC_TRANSFER_T data_transfer;
	int transfer_done;
	struct completion req_done;

	unsigned int dma_len;
	unsigned int dma_dir;
	int data_err;

	u8 suspend;
	u8 sys_suspend;
	u8 sdio_irq_enable;
	u8 eirq_enable;
	u8 detpin_enable;
	bool main_irq_registered;
	bool eirq_registered;
	bool detirq_registered;
	bool tasklet_inited;
	int eirq;
	int det_irq;
	int id;
	struct tasklet_struct	finish_tasklet;
	u8 mmc_pm;

	struct regulator *host_reg;
	struct clk *master_clk;
	unsigned long clock;
	unsigned int mclk_adj;
	unsigned long bus_width;

	int clk_inv;
	struct gpio_desc* eirq_pin;
	struct gpio_desc* det_pin;
	int present;
	
	struct delayed_work timeout_work;
	/*
	 * Note:
	 * Following definition is for fixing a bug of IFC dma before U06.
	 * If HW has fixed it, we will remove these definitions.
	 */
	void *tmp_buf;
	dma_addr_t phys_tmp;

	unsigned int unaligned_count;
	unsigned int reset_count;
};

static void rda_mmc_reset(u32 host_id);
static int hal_mmc_init(struct rda_mmc_host *host);
static void hal_set_bus_width(struct rda_mmc_host *host, unsigned char bus_width);

/* Exported RDA MMC helper APIs used by combo/WiFi drivers. */
void rda_mmc_set_sdio_irq(u32 host_id, u8 enable);
void rda_mmc_set_present(u32 host_id, u8 present);
bool rda_mmc_card_attached(u32 host_id);
void rda_mmc_bus_scan(u32 host_id);

static HWP_SYS_CTRL_AP_T *hwp_apSysCtrl;
static struct rda_mmc_host *rda_mmc_hosts[RDA_MMC_HOST_COUNT];

static void rda_mmc_force_bb_pins_alt(struct platform_device *pdev, int host_id)
{
	void __iomem *cfg_base;
	void __iomem *bb_gpio_mode_reg;
	u32 mode;
	u32 mask;

	switch (host_id) {
	case 0:
		/* SDMMC1 uses BB pins C9..C14. */
		mask = GENMASK(14, 9);
		break;
	case 1:
		/* SDMMC2 uses BB pins C15..C20. */
		mask = GENMASK(20, 15);
		break;
	case 2:
		/* SDMMC3 does not use BB pin range in this register map. */
		return;
	default:
		return;
	}

	cfg_base = ioremap(RDA_CONFIG_REGS_PHYS, RDA_CONFIG_REGS_SIZE);
	if (!cfg_base) {
		dev_warn(&pdev->dev, "failed to map CFG_REGS for pinmux\n");
		return;
	}

	bb_gpio_mode_reg = (void __iomem *)&((HWP_CFG_REGS_T __iomem *)cfg_base)->BB_GPIO_Mode;
	mode = readl(bb_gpio_mode_reg);

	/* In BB_GPIO_Mode, 0 means ALT function, 1 means GPIO. */
	if (mode & mask) {
		u32 new_mode = mode & ~mask;

		writel(new_mode, bb_gpio_mode_reg);
		dev_dbg(&pdev->dev,
			 "host%d pinmux: force BB_GPIO_Mode 0x%08x -> 0x%08x (mask=0x%08x)\n",
			 host_id, mode, new_mode, mask);
	} else {
		dev_dbg(&pdev->dev,
			 "host%d pinmux: BB_GPIO_Mode already ALT (0x%08x mask=0x%08x)\n",
			 host_id, mode, mask);
	}

	iounmap(cfg_base);
}


static void hal_send_cmd(struct rda_mmc_host *host, struct mmc_command *cmd, struct mmc_data *data)
{
	u32 configReg = SDMMC_SDMMC_SENDCMD;

	((HWP_SDMMC_T*)host->base)->SDMMC_CONFIG = 0x00000000;
	
	if (cmd->flags & MMC_RSP_PRESENT)
	{
		configReg |= SDMMC_RSP_EN;
		if (cmd->flags & MMC_RSP_136)
			configReg |= SDMMC_RSP_SEL_R2;
		else if (cmd->flags & MMC_RSP_CRC)
			configReg |= SDMMC_RSP_SEL_OTHER;
		else
			configReg |= SDMMC_RSP_SEL_R3;
	}

	/* cases for data transfer */
	if (cmd->opcode == MMC_READ_SINGLE_BLOCK) {
		configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_READ);
	} else if (cmd->opcode == MMC_READ_MULTIPLE_BLOCK) {
		configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_READ |
				SDMMC_S_M_SEL_MULTIPLE);

		if (host->id == 0 || host->id == 2) {
			/*
			 * Multiple block transfers for SD require CMD12 to stop the transactions.
			 * The Host Controller automatically issues CMD12 when the last block transfer
			 * is completed.
			 * For more detals, please refer to SD Host Controller Specification.
			 * */
			configReg |= SDMMC_AUTO_FLAG_EN;
		}
	} 
	else if (cmd->opcode == MMC_WRITE_BLOCK)
		{ configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_WRITE); } 
	
	else if (cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK) {
		configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_WRITE |
				SDMMC_S_M_SEL_MULTIPLE);

		if (host->id == 0 || host->id == 2) {
			configReg |= SDMMC_AUTO_FLAG_EN;
		}
	} else if (cmd->opcode == SD_APP_SEND_SCR) {
		configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_READ);
	} else if (cmd->opcode == MMC_SEND_STATUS && data) {
		configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_READ);
	} else if (cmd->opcode == MMC_SWITCH && data) {
		configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_READ);
	} 
	else if(cmd->opcode == MMC_SEND_EXT_CSD && data)
	{
		configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_READ);
	} 
	else if(cmd->opcode == SD_IO_RW_EXTENDED)
	{
		if (cmd->data->flags & MMC_DATA_WRITE) configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_WRITE);
		
		else configReg |= (SDMMC_RD_WT_EN | SDMMC_RD_WT_SEL_READ);

		if(cmd->data->blocks > 1) configReg |= SDMMC_S_M_SEL_MULTIPLE;

		if((&host->data_transfer)->blockSize != data->blksz)
		{
			cmd->arg &= 0xfffffe00;
			cmd->arg |= (&host->data_transfer)->blockSize;
		}
	}

	((HWP_SDMMC_T*)host->base)->SDMMC_CMD_INDEX = SDMMC_COMMAND(cmd->opcode);
	((HWP_SDMMC_T*)host->base)->SDMMC_CMD_ARG   = SDMMC_ARGUMENT(cmd->arg);
	((HWP_SDMMC_T*)host->base)->SDMMC_CONFIG    = configReg ;
}

static void rda_mmc_set_clock(struct rda_mmc_host *host, unsigned long clock)
{
	unsigned long mclk;
	unsigned long clk_div;

	host->clock = clock;
	if (!host->clock) {
		((HWP_SDMMC_T*)host->base)->SDMMC_MCLK_ADJUST = SDMMC_CLK_DISA;
		return;
	}

	mclk = clk_get_rate(host->master_clk);
	if (!mclk)
		return;

	clk_div = mclk / (2 * host->clock);
	if (mclk % (2 * host->clock))
		clk_div++;

	if (clk_div >= 1)
		clk_div -= 1;

	if (clk_div > 255)
		clk_div = 255;

	dev_dbg(mmc_dev(host->mmc),
		"set clk = %d, bus_clk = %d, divider = %d\n",
		(int)host->clock, (int)mclk, (int)clk_div);

	((HWP_SDMMC_T*)host->base)->SDMMC_TRANS_SPEED =
		SDMMC_TRANS_SPEED(clk_div);
	((HWP_SDMMC_T*)host->base)->SDMMC_MCLK_ADJUST =
		SDMMC_MCLK_ADJUST(host->mclk_adj);
	if (host->clk_inv)
		((HWP_SDMMC_T*)host->base)->SDMMC_MCLK_ADJUST |= SDMMC_CLK_INV;
}

static void rda_mmc_restore_ios(struct rda_mmc_host *host)
{
	/* Host reset clears controller registers; re-apply active ios settings. */
	rda_mmc_set_clock(host, host->clock);
	hal_set_bus_width(host, host->bus_width);
}

static int hal_cmd_done(struct rda_mmc_host *host)
{
	return (!(((HWP_SDMMC_T*)host->base)->SDMMC_STATUS & SDMMC_NOT_SDMMC_OVER));
}

static int hal_wait_cmd_done(struct rda_mmc_host *host, struct mmc_command *cmd)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(MCD_CMD_TIMEOUT_MS);

	/* command done need be polled, no interrupt indicating cmd done */
	while (time_before(jiffies, timeout) && !hal_cmd_done(host));

	if (!hal_cmd_done(host))
	{
		dev_dbg(mmc_dev(host->mmc), "cmd %d timeout\n", cmd->opcode);
		return -ETIMEDOUT;
	}

	return 0;
}

static HAL_SDMMC_OP_STATUS_T hal_get_op_status(struct rda_mmc_host *host)
{
	return ((HAL_SDMMC_OP_STATUS_T)(u32)((HWP_SDMMC_T*)host->base)->SDMMC_STATUS);
}

static int hal_wait_cmd_resp(struct rda_mmc_host *host)
{
	HAL_SDMMC_OP_STATUS_T status = hal_get_op_status(host);

	if (status.fields.noResponseReceived) {
		dev_dbg(mmc_dev(host->mmc), "rsp noResponseReceived\n");
		return -EIO;
	}

	if (status.fields.responseCrcError) {
		dev_dbg(mmc_dev(host->mmc), "rsp responseCrcError\n");
		return -EIO;
	}

	return 0;
}

static void hal_get_resp(struct rda_mmc_host *host, struct mmc_command *cmd)
{
	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136) {
			cmd->resp[0] = ((HWP_SDMMC_T*)host->base)->SDMMC_RESP_ARG3;
			cmd->resp[1] = ((HWP_SDMMC_T*)host->base)->SDMMC_RESP_ARG2;
			cmd->resp[2] = ((HWP_SDMMC_T*)host->base)->SDMMC_RESP_ARG1;
			cmd->resp[3] = ((HWP_SDMMC_T*)host->base)->SDMMC_RESP_ARG0 << 1;
		}
		else {
			cmd->resp[0] = ((HWP_SDMMC_T*)host->base)->SDMMC_RESP_ARG3;
			cmd->resp[1] = 0;
			cmd->resp[2] = 0;
			cmd->resp[3] = 0;
		}
	}
}

static int hal_data_transfer_start(struct rda_mmc_host *host, HAL_SDMMC_TRANSFER_T* transfer)
{
	u32 length = transfer->blockSize;
	u32 lengthExp = 2;
	u32 blockSize = 4;


	/* The block size of register must be 2 by order. */
	if (length > 4) {
		while (blockSize < length) {
			blockSize = blockSize << 1;
			lengthExp++;
		}
	}
	/* resize blockSize */
	transfer->blockSize = blockSize;

	// Configure amount of data
	((HWP_SDMMC_T*)host->base)->SDMMC_BLOCK_CNT = SDMMC_SDMMC_BLOCK_CNT(transfer->blockNum);
	((HWP_SDMMC_T*)host->base)->SDMMC_BLOCK_SIZE = SDMMC_SDMMC_BLOCK_SIZE(lengthExp);

	// Configure Bytes reordering
	((HWP_SDMMC_T*)host->base)->SDMMC_CTRL = SDMMC_SOFT_RST_L | SDMMC_L_ENDIAN(1);

	switch (transfer->direction){
		case HAL_SDMMC_DIRECTION_READ:
			transfer->ifcReq = HAL_IFC_SDMMC_RX + host->id * 2;
			break;

		case HAL_SDMMC_DIRECTION_WRITE:
			transfer->ifcReq = HAL_IFC_SDMMC_TX + host->id *2;
			break;

		default:
			dev_err(mmc_dev(host->mmc),
				"hal_data_transfer_start, invalide direction %d\n",
				transfer->direction);
			return -EILSEQ;
	}

	transfer->channel = ifc_transfer_start(
			transfer->ifcReq, transfer->sysMemAddr,
			transfer->blockNum*transfer->blockSize,
			HAL_IFC_SIZE_32_MODE_MANUAL);
	if (transfer->channel == HAL_UNKNOWN_CHANNEL){
		dev_err(mmc_dev(host->mmc), "transfer start with invalide channel\n");
		return -EILSEQ;
	}
	else return 0;
		
		
	
}

static void hal_data_transfer_stop(struct rda_mmc_host *host, HAL_SDMMC_TRANSFER_T* transfer)
{
	// Configure amount of data
	((HWP_SDMMC_T*)host->base)->SDMMC_BLOCK_CNT	= SDMMC_SDMMC_BLOCK_CNT(0);
	((HWP_SDMMC_T*)host->base)->SDMMC_BLOCK_SIZE = SDMMC_SDMMC_BLOCK_SIZE(0);

	//dev_info(mmc_dev(host->mmc),"stop channel %d\n", (int)transfer->channel);

	if (transfer->channel == HAL_UNKNOWN_CHANNEL || transfer->channel >= SYS_IFC_STD_CHAN_NB)
	{
		dev_dbg(mmc_dev(host->mmc), "hal_data_transfer_stop invalide channel %d \n", transfer->channel);
		return;
	}

	/* Check if there is sd-card as doing hot-plug. */
	if (host->present && host->data && (host->data->flags & MMC_DATA_READ)) {
		/* IFC supports only flush operation with read. */
		ifc_transfer_flush(transfer->ifcReq, transfer->channel);
	}

	if (host->data)
	{
		ifc_transfer_stop(transfer->ifcReq, transfer->channel);
	}
	transfer->channel = HAL_UNKNOWN_CHANNEL;
	transfer->ifcReq = HAL_IFC_NO_REQWEST;
}



#ifdef RDA_MMC_USE_INT
static void hal_irq_clear(struct rda_mmc_host *host, u32 int_status)
{
	((HWP_SDMMC_T*)host->base)->SDMMC_INT_CLEAR = (int_status & 0xFF);
}
#endif

#ifndef RDA_MMC_USE_INT
static int hal_data_transfer_done(struct rda_mmc_host *host, HAL_SDMMC_TRANSFER_T* transfer)
{
	u32 int_status = ((HWP_SDMMC_T*)host->base)->SDMMC_INT_STATUS;

	BUG_ON(transfer->channel == HAL_UNKNOWN_CHANNEL);

	if ((transfer->direction == HAL_SDMMC_DIRECTION_READ && int_status & SDMMC_RXDMA_DONE_INT) ||
	(transfer->direction == HAL_SDMMC_DIRECTION_WRITE && int_status & SDMMC_DAT_OVER_INT))
	{
		// Transfer is over
		((HWP_SDMMC_T*)host->base)->SDMMC_INT_CLEAR = SDMMC_DAT_OVER_CL;
		ifc_transfer_stop(transfer->ifcReq, transfer->channel);

		dev_info(mmc_dev(host->mmc), "release channel %d\n", (int)transfer->channel);

		// We finished a read
		transfer->channel = HAL_UNKNOWN_CHANNEL;

		//	Put the FIFO in reset state.
		//hwp_sdmmc->SDMMC_CTRL = 0 | SDMMC_L_ENDIAN(1);
		((HWP_SDMMC_T*)host->base)->SDMMC_CTRL = SDMMC_SOFT_RST_L | SDMMC_L_ENDIAN(1);

		return 1;
	}

	return 0;
		
	
}

static int hal_wait_data_transfer_done(struct rda_mmc_host *host, HAL_SDMMC_TRANSFER_T* transfer)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(MCD_DATA_TIMEOUT_MS* transfer->blockNum);

	// Wait (This could be done in interrupt */
	while(!hal_data_transfer_done(host, transfer))
	{
		if (time_after(jiffies, timeout))
		{
			hal_data_transfer_stop(host, transfer);
			dev_err(mmc_dev(host->mmc),"wait transfer done timeout\n");
			return -ETIMEDOUT;
		}
	}
	return 0;
}
#endif

static inline int hal_data_write_check_crc(struct rda_mmc_host *host)
{
	HAL_SDMMC_OP_STATUS_T operationStatus = hal_get_op_status(host);
	
	if (operationStatus.fields.crcStatus != 2)
	{
		dev_err(mmc_dev(host->mmc),"data_write_check_crc fail, status:%08x\n",operationStatus.reg);
		return -EILSEQ;
	}
	
	return 0;
}

static void hal_set_bus_width(struct rda_mmc_host *host, unsigned char bus_width)
{
	((HWP_SDMMC_T*)host->base)->SDMMC_DATA_WIDTH = 1<<bus_width;
}


static int hal_mmc_init(struct rda_mmc_host *host)
{
	/* we only care DATA_OVER and DATA ERR Interrupt */
	((HWP_SDMMC_T*)host->base)->SDMMC_INT_MASK = 0x5F;

	return 0;
}

static int hal_mmc_disable(struct rda_mmc_host *host)
{

	((HWP_SDMMC_T*)host->base)->SDMMC_INT_MASK = 0;
	((HWP_SDMMC_T*)host->base)->SDMMC_MCLK_ADJUST = SDMMC_CLK_DISA;

	return 0;
}

#ifdef RDA_MMC_USE_INT
static int do_data_abort(struct rda_mmc_host *host)
{
	int result = 0;
	HAL_SDMMC_TRANSFER_T *transfer = &host->data_transfer;
	struct mmc_data *data = host->data;

	dev_dbg(mmc_dev(host->mmc), "do_data_abort\n");

	hal_data_transfer_stop(host, transfer);

	if (host->mrq) {
		host->mrq->cmd->error = -EILSEQ;
	}

	if (data) {
		data->error = -EILSEQ;
	}

	return result;
}

static int do_data_complete(struct rda_mmc_host *host)
{
	int result = 0;
	HAL_SDMMC_TRANSFER_T *transfer = &host->data_transfer;


	//dev_info(mmc_dev(host->mmc), "host id:%d do_data_complete \n", host->id);

	if (!host->data || transfer->channel == HAL_UNKNOWN_CHANNEL || transfer->channel >= SYS_IFC_STD_CHAN_NB) {
		//dev_err(mmc_dev(host->mmc), "do_data_complete invalid channel(%d), flags = 0x%x\n", transfer->channel, (data ? data->flags : 0));
		return -EREMOTEIO;
	}

	if (ifc_transfer_get_tc(transfer->ifcReq, transfer->channel) == 0) {
		ifc_transfer_stop(transfer->ifcReq, transfer->channel);
		//dev_info(mmc_dev(host->mmc), "release channel %d\n", (int)transfer->channel);
		transfer->channel = HAL_UNKNOWN_CHANNEL;
	}
	else {
		hal_data_transfer_stop(host, transfer);
		//dev_err(mmc_dev(host->mmc), "data complete but DMA not done\n");
		host->data->error = -ETIMEDOUT;
	}

	/* 
	 * Just check if there is a crc error of writing.
	 * Do not care reading, because our HW does not clear crc error automatically.
	 * In fact, there is always RD_ERR_INT before crc error. It will be processed firstly.
	 */
	if (host->data->flags & MMC_DATA_WRITE) {
		result = hal_data_write_check_crc(host);
		if (result) {
			//dev_err(mmc_dev(host->mmc), "hal_data_write_check_crc fail, ret = %d\n", result);
			host->data->error = result;
		}
	}

	return result;
}

static void tasklet_worker(unsigned long param)
{
	struct rda_mmc_host *host = (struct rda_mmc_host*)param;

	if (host->data_err) {
		do_data_abort(host);
		host->data_err = 0;
	} else {
		do_data_complete(host);
	}

	complete(&host->req_done);

	return;
}

static irqreturn_t rda_mmc_irq(int irq, void *dev_id)
{
	struct rda_mmc_host *host = dev_id;
	u32 int_status = ((HWP_SDMMC_T*)host->base)->SDMMC_INT_STATUS;
	
	hal_irq_clear(host, int_status);

	//dev_info(mmc_dev(host->mmc), "rda_mmc_irq, int_status = 0x%08x\n", int_status);

	/* If there isn't card, we return immediately. */
	if (!host->present) {
		return IRQ_HANDLED;
	}


	/* we only care DATA_OVER and DATA ERR Interrupt */
	if (int_status & (SDMMC_RD_ERR_INT | SDMMC_WR_ERR_INT))
	{
		host->data_err = 1;
		tasklet_schedule(&host->finish_tasklet);
	}

	/*
	 * RXDMA_DONE might arrive before DAT_OVER setting as reading,
	 * or RXDMA_DONE has arrived, but there is DAT_OVER flag.
	 * So, we check only RXDMA_DONE flag to indicate that reading is over.
	 *
	 */
	else if (((host->data && (host->data->flags & MMC_DATA_READ)) && (int_status & SDMMC_RXDMA_DONE_INT)) ||
	((int_status & SDMMC_DAT_OVER_INT) && (host->data && (host->data->flags & MMC_DATA_WRITE))))
	{
		tasklet_schedule(&host->finish_tasklet);
	}


	if ((int_status & SDMMC_SDIO_INT) && host->sdio_irq_enable && !host->eirq_enable)
	{
		mmc_signal_sdio_irq(host->mmc);
	}

	return IRQ_HANDLED;
}
#endif

static void finish_request(struct rda_mmc_host *host, struct mmc_request *mrq)
{
	mmc_request_done(host->mmc, mrq);
}

/* send command to the mmc card and wait for results */

static int do_command(struct mmc_host *mmc, struct mmc_command *cmd, struct mmc_data *data)
{
	int result;
	struct rda_mmc_host *host = mmc_priv(mmc);
	bool sdio_probe_cmd;

	sdio_probe_cmd = (cmd->opcode == SD_IO_SEND_OP_COND ||
			 cmd->opcode == SD_IO_RW_DIRECT ||
			 cmd->opcode == SD_IO_RW_EXTENDED);

	if (host->id == 1 && sdio_probe_cmd) {
		pr_debug("rda_mmc host1: CMD%u arg=0x%08x flags=0x%x data=%p\n",
			cmd->opcode, cmd->arg, cmd->flags, data);
	}

	//dev_info(mmc_dev(host->mmc), "do_command, cmdidx = %d, cmdarg = 0x%08x, flags = %x\n", cmd->opcode, cmd->arg, cmd->flags);

	host->cmd = cmd;
	hal_send_cmd(host, cmd, data);
	result = hal_wait_cmd_done(host, cmd);
	
	if (result)
	{
		if (host->id == 1 && sdio_probe_cmd) {
			dev_warn(mmc_dev(host->mmc),
				"host1 CMD%u hard-recover after wait_cmd_done ret=%d\n",
				cmd->opcode, result);
			rda_mmc_reset(host->id);
			hal_mmc_init(host);
			rda_mmc_restore_ios(host);
			udelay(50);

			hal_send_cmd(host, cmd, data);
			result = hal_wait_cmd_done(host, cmd);
			if (!result && (cmd->flags & MMC_RSP_PRESENT))
				result = hal_wait_cmd_resp(host);

			if (!result)
				goto cmd_done_ok;
		}

		result = (host->present == 0) ? -ENOMEDIUM : result;
		cmd->error = result;

		dev_dbg(mmc_dev(host->mmc), "cmd %d, wait cmd fail, ret = %d\n", cmd->opcode, result);
		if (host->id == 1 && sdio_probe_cmd)
			pr_debug("rda_mmc host1: CMD%u wait_cmd_done failed ret=%d present=%d pm=%u suspend=%u\n",
				cmd->opcode, result, host->present, host->mmc_pm,
				host->suspend);

		return result;
	}

	if (cmd->flags & MMC_RSP_PRESENT)
	{
		result= hal_wait_cmd_resp(host);

		if (result)
		{
			if (host->id == 1 && sdio_probe_cmd) {
				dev_dbg(mmc_dev(host->mmc),
					"host1 CMD%u hard-recover after wait_resp ret=%d\n",
					cmd->opcode, result);
				rda_mmc_reset(host->id);
				hal_mmc_init(host);
				rda_mmc_restore_ios(host);
				udelay(50);

				hal_send_cmd(host, cmd, data);
				result = hal_wait_cmd_done(host, cmd);
				if (!result)
					result = hal_wait_cmd_resp(host);

				if (!result)
					goto cmd_done_ok;
			}

			result = (host->present == 0) ? -ENOMEDIUM : result;
			cmd->error = result;

			dev_dbg(mmc_dev(host->mmc),
				"cmd %d, wait resp fail, ifc = %d, ret = %d\n",
				cmd->opcode, (int)host->data_transfer.channel, result);
			if (host->id == 1 && sdio_probe_cmd)
				pr_debug("rda_mmc host1: CMD%u wait_resp failed ret=%d op_status=0x%08x clock=%lu present=%d pm=%u suspend=%u\n",
					cmd->opcode, result, hal_get_op_status(host).reg,
					host->clock, host->present,
					host->mmc_pm, host->suspend);

			return result;
		}
	}

cmd_done_ok:
	hal_get_resp(host, cmd);

	if (host->id == 1 && sdio_probe_cmd)
		pr_debug("rda_mmc host1: CMD%u resp0=0x%08x\n", cmd->opcode, cmd->resp[0]);

	//if (cmd->flags & MMC_RSP_PRESENT)
	//{
		//dev_info(mmc_dev(host->mmc), "  response: %08x %08x %08x %08x\n", cmd->resp[0], cmd->resp[1], cmd->resp[2], cmd->resp[3]);
	//}

	return result;
}

static int __do_data_transfer(struct mmc_host *mmc, struct mmc_command *cmd, struct mmc_data *data, dma_addr_t dma_addr)
{
	int result = 0;
	struct rda_mmc_host *host = mmc_priv(mmc);
	HAL_SDMMC_TRANSFER_T *transfer = &host->data_transfer;
	unsigned int tsize = data->blocks * data->blksz;
	bool use_tmp_buf = false;


	//dev_info(mmc_dev(host->mmc),  "%s, id:%d cmdidx = %d, blks = %d, addr = 0x%08x\n", __func__, host->id,cmd->opcode, data->blocks, dma_addr);

	/* Check if address of dma is aligened with 4bytes. */
	if (!IS_ALIGNED(dma_addr, 4)) {
		return -EFAULT;
	}

	transfer->sysMemAddr = (u8 *)dma_addr;
	transfer->blockNum = data->blocks;
	transfer->blockSize = data->blksz;

	if (data->flags & MMC_DATA_READ) transfer->direction  = HAL_SDMMC_DIRECTION_READ;
	
	else if (data->flags & MMC_DATA_WRITE) transfer->direction  = HAL_SDMMC_DIRECTION_WRITE;
	

	if (rda_soc_is_older_metal10()) {
		if (!IS_ALIGNED(dma_addr, 16) &&
			(dma_addr >> PAGE_SHIFT != ((dma_addr + tsize -1) >> PAGE_SHIFT))) {
			transfer->sysMemAddr = (u8 *)host->phys_tmp;
			use_tmp_buf = true;
			++host->unaligned_count;

			if (data->blocks * data->blksz > PAGE_SIZE) {
				dev_err(mmc_dev(host->mmc),
					"data is too large! size = %d\n",
					data->blocks * data->blksz);
				BUG_ON(1);
			}

			if (data->flags & MMC_DATA_WRITE)
				sg_copy_to_buffer(data->sg, data->sg_len,
						host->tmp_buf, tsize);
		}
	}

	// Initiate data migration through Ifc.
	result = hal_data_transfer_start(host, transfer);
	if (result) {
		cmd->error = result;
		dev_err(mmc_dev(host->mmc),
			"data transfer start fail, ret = %d\n", result);
		goto exit;
	}

	/* Config completion of request */
	init_completion(&host->req_done);

	result = do_command(mmc, cmd, data);
	if (result) {
		dev_err(mmc_dev(host->mmc),
			"do_command fail, ret = %d\n", result);
		hal_data_transfer_stop(host, transfer);
		goto exit;
	}

#ifndef RDA_MMC_USE_INT
	result = hal_wait_data_transfer_done(host, transfer);
	if (result) {
		dev_err(mmc_dev(host->mmc), "wait transfer done fail, ret = %d\n", result);
		hal_data_transfer_stop(host, transfer);
		goto exit;
	}

	if (data->flags & MMC_DATA_READ) {
		result = hal_data_read_check_crc(host);
		if (result) {
			dev_err(mmc_dev(host->mmc), "read check crc fail, ret = %d\n", result);
			goto exit;
		}
	} else if (data->flags & MMC_DATA_WRITE) {
		result = hal_data_write_check_crc(host);
		if (result) {
			dev_err(mmc_dev(host->mmc), "write check crc fail, ret = %d\n", result);
			goto exit;
		}
	}

	if (!data->error)
		data->bytes_xfered += data->blocks * (data->blksz);
	else
		data->bytes_xfered = 0;

	if (data->stop) {
		result = do_command(mmc, data->stop, NULL);
	}
	if (result) {
		dev_err(mmc_dev(host->mmc), "stop command fail, ret = %d\n", result);
	}

	host->transfer_done = 1;
#else
	unsigned long timeout = wait_for_completion_timeout(&host->req_done, msecs_to_jiffies(MCD_DATA_TIMEOUT_MS));
	if (timeout == 0)
	{		
		u32 ifc_tc = ifc_transfer_get_tc(transfer->ifcReq,
				transfer->channel);

		if (data->flags & MMC_DATA_READ) {
			dev_err(mmc_dev(host->mmc),
				"transfer (read) cmd timeout(%d ms): "
				"cmd(%d), blocks(%d), irq = 0x%x, "
				"op_sta = 0x%x, tc = 0x%x\n",
				MCD_DATA_TIMEOUT_MS, cmd->opcode,
				data->blocks, ((HWP_SDMMC_T*)host->base)->SDMMC_INT_STATUS, (hal_get_op_status(host)).reg, ifc_tc);
		} else if (data->flags & MMC_DATA_WRITE) {
			dev_err(mmc_dev(host->mmc),
				"transfer (write) cmd timeout(%d ms): "
				"cmd(%d), blocks(%d), irq = 0x%x, "
				"op_sta = 0x%x, tc = 0x%x\n",
				MCD_DATA_TIMEOUT_MS, cmd->opcode,
				data->blocks, ((HWP_SDMMC_T*)host->base)->SDMMC_INT_STATUS, (hal_get_op_status(host)).reg, ifc_tc);
		}

		/* Stop dma's action */
		hal_data_transfer_stop(host, transfer);

		/* Not for SDIO */
		if (host->id == 0 || host->id == 2) {
			struct mmc_command stop;

			/*
		 	 * As timeout in sending-data/receive-data state, HW doesn't automatically return to
		 	 * transfer state. So we have to send a CMD12 to HW to return to transfer state.
		 	 * Otherwise, HW will not process any data.
		 	 * For more details, please refer to SD' specification.
		 	 */
			stop.opcode = MMC_STOP_TRANSMISSION;
			stop.arg = 0;
			stop.flags = MMC_RSP_R1B | MMC_CMD_AC;

			do_command(host->mmc, &stop, NULL);
		}

		result = -ETIMEDOUT;
		/* Report error to upper layer */
		cmd->error = result;
		data->error = result;
	} else {
		host->transfer_done = 1;
	}

	/* Check if there is a card. */
	if (host->present == 0) {
		cmd->error = -ENOMEDIUM;
		data->error = -ENOMEDIUM;
	}

	if (!data->error) {
		data->bytes_xfered += data->blocks * (data->blksz);
		if (data->flags & MMC_DATA_READ) {
			if (use_tmp_buf)
				sg_copy_from_buffer(data->sg, data->sg_len,
						    host->tmp_buf, tsize);
		}

	} else {
		hal_data_transfer_stop(host, transfer);
		dev_dbg(mmc_dev(host->mmc), "transfer data error %d\n",
			data->error);

		if ((host->id == 0 || host->id == 2) && data->blocks > 1 &&
		    cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK) {
			struct mmc_command stop = {
				.opcode = MMC_STOP_TRANSMISSION,
				.arg = 0,
				.flags = MMC_RSP_R1B | MMC_CMD_AC,
			};

			result = do_command(host->mmc, &stop, NULL);
			if (result)
				dev_dbg(mmc_dev(host->mmc), "cmd12 recovery failed, ret = %d\n", result);
		}
	}

#endif /* RDA_MMC_USE_INT */

exit:

	return result;
}

static int do_data_transfer(struct mmc_host *mmc, struct mmc_command *cmd, struct mmc_data *data)
{
	struct rda_mmc_host *host = mmc_priv(mmc);
	unsigned int i;
	int ret = 0;

	if (data->flags & MMC_DATA_READ) {
		host->dma_dir = DMA_FROM_DEVICE;
		host->dma_len = dma_map_sg(mmc_dev(mmc), data->sg,
			data->sg_len, host->dma_dir);

	} else if (data->flags & MMC_DATA_WRITE) {
		host->dma_dir = DMA_TO_DEVICE;
		host->dma_len = dma_map_sg(mmc_dev(mmc), data->sg,
			data->sg_len, host->dma_dir);

	} else {
		/* Invalid flag */
		return -EINVAL;
	}

	if (!host->dma_len) {
		return -ENOMEM;
	}

	if (host->dma_len != 1) {
		dev_err(mmc_dev(host->mmc),
			"unsupported mapped sg segments: %u\n",
			host->dma_len);
		ret = -EINVAL;
		goto out_unmap;
	}

	host->data = data;

	for (i = 0; i < host->dma_len; i++) {
		//dev_info(mmc_dev(host->mmc), "do_data_transfer, sg %d, size = %d\n", i, data->sg[i].length);



		/*
		 * For some card of emmc, when exec command 18 & 25,
		 * the start offset must be aligned to 2.
		 */
		if (host->id == 2 && (cmd->arg & 1) && data->blocks > 1 && (cmd->opcode == MMC_READ_MULTIPLE_BLOCK || cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK))
		{
			u32 blksz = data->blksz;
			u32 blknum = data->blocks;
			u32 opcode = cmd->opcode;
			u32 cmd_arg = cmd->arg;
			dma_addr_t dma_addr = sg_dma_address(&data->sg[i]);

			//dev_err(mmc_dev(host->mmc), "WARNING: unaligned block offset 0x%08x.\n", cmd->arg);

			/* read/write one block first by single r/w command */
			if(opcode == MMC_WRITE_MULTIPLE_BLOCK) {
				cmd->opcode = MMC_WRITE_BLOCK;
			}
			else {
				cmd->opcode = MMC_READ_SINGLE_BLOCK;
			}
			data->blocks = 1;
			ret = __do_data_transfer(mmc, cmd, data, dma_addr);
			if (ret) {
				cmd->opcode = opcode;
				cmd->arg = cmd_arg;
				data->blocks = blknum;
				break;
			}

			/* read/write other block(s) by aligned offset */
			if((blknum - 1) > 1) {
				cmd->opcode = opcode;
			}
			data->blocks = blknum - 1;
			cmd->arg += 1; /* aligned offset */
			dma_addr += blksz;
			ret = __do_data_transfer(mmc, cmd, data, dma_addr);
			cmd->opcode = opcode;
			cmd->arg = cmd_arg;
			data->blocks = blknum;
			if (ret) {
				break;
			}
		} else {
			dma_addr_t dma_addr = sg_dma_address(&data->sg[i]);
			ret = __do_data_transfer(mmc, cmd, data, dma_addr);
			if (ret) {
				break;
			}
		}

	}

	host->data = NULL;

out_unmap:
	dma_unmap_sg(mmc_dev(host->mmc), data->sg, data->sg_len, host->dma_dir);

	return ret;
}

static void rda_mmc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct rda_mmc_host *host = mmc_priv(mmc);
	int ret;

	/*
	 * HW supports four controllers of sdmmc.
	 * Only do SDIO command for sdmmc1
	 */
	if ((host->id != 1 && mrq->cmd->opcode == SD_IO_SEND_OP_COND) ||
		(host->id != 1 && mrq->cmd->opcode == SD_IO_RW_DIRECT)) {
		mrq->cmd->error = -EINVAL;
		mmc_request_done(mmc, mrq);
		return;
	}

	host->mrq = mrq;

	host->transfer_done = 0;
	if (mrq->data) {
		ret = do_data_transfer(mmc, mrq->cmd, mrq->data);
	} else {
		ret = do_command(mmc, mrq->cmd, mrq->data);
		mrq->cmd->error = ret;
	}

	host->mrq = NULL;

	if (ret) {
		dev_dbg(mmc_dev(host->mmc),
			"rda_mmc_request fail, ret = %d\n", ret);
		mmc_request_done(mmc, mrq);
		return;
	}

	if (!mrq->data || host->transfer_done) {
		finish_request(host, mrq);
	}
}

static int rda_mmc_get_ro(struct mmc_host *mmc)
{
	/*
	 * Board doesn't support read only detection; let the mmc core
	 * decide what to do.
	 */
	//return -ENOSYS;

	/* return not read-only for now */
	return 0;
}

static void rda_mmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct rda_mmc_host *host = mmc_priv(mmc);
	u8 old_pm;
	

	if (!host) return;
	old_pm = host->mmc_pm;

	/* Power control */
	switch (ios->power_mode) {
		case MMC_POWER_OFF:
			if (host->mmc_pm != MMC_POWER_OFF && host->id == 0 && host->host_reg) {
				regulator_disable(host->host_reg);
				/* Waiting 40ms until power is completely power off. */
				mdelay(40);
			}
			host->mmc_pm = MMC_POWER_OFF;
			break;

		case MMC_POWER_UP:
			if (host->mmc_pm == MMC_POWER_OFF && host->id == 0 && host->host_reg) {
				int ret = regulator_enable(host->host_reg);
				if (ret) {
					dev_err(mmc_dev(host->mmc), "Failed to enable host_reg: %d\n", ret);
				}
			}
			host->mmc_pm = MMC_POWER_UP;
			break;

		case MMC_POWER_ON:
			host->mmc_pm = MMC_POWER_ON;
			break;

		default:
			break;
	}

	if (host->clock != ios->clock) {
		rda_mmc_set_clock(host, ios->clock);
	}

	if (host->bus_width != ios->bus_width) {
		host->bus_width = ios->bus_width;
		dev_dbg(mmc_dev(host->mmc), "set bus_width = %d\n",
			1<<host->bus_width);
		hal_set_bus_width(host, host->bus_width);
	}
	dev_dbg(mmc_dev(host->mmc), "host:(%d)set bus_width to %d pm_mode %d \n", host->id, ios->bus_width, ios->power_mode);

	if (host->id == 1 && (old_pm != host->mmc_pm || ios->clock || ios->power_mode != MMC_POWER_OFF)) {
		pr_debug("rda_mmc_set_ios host1: power_mode=%u mmc_pm=%u clock=%u vdd=%u width=%u present=%d suspend=%u\n",
			ios->power_mode, host->mmc_pm, ios->clock, ios->vdd,
			1 << ios->bus_width, host->present, host->suspend);
	}
}

static void rda_mmc_mask_eirq(struct rda_mmc_host *host)
{
	struct irq_desc * desc = irq_to_desc(host->eirq);

	if (!desc)
		return;
	
	if (!desc->depth) disable_irq_nosync(host->eirq);
		
	
}

static void rda_mmc_unmask_eirq(struct rda_mmc_host *host)
{
	struct irq_desc * desc  = irq_to_desc(host->eirq);

	if (!desc)
		return;


	while (desc->depth) enable_irq(host->eirq);
		
	
}
static void rda_mmc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	unsigned long flags;
	struct rda_mmc_host *host = (struct rda_mmc_host *)mmc_priv(mmc);
	
	spin_lock_irqsave(&host->lock, flags);

	if (enable) {
		if(!host->eirq_enable)
			((HWP_SDMMC_T*)host->base)->SDMMC_INT_MASK |= SDMMC_SDIO_INT_MK;
		else
			rda_mmc_unmask_eirq(host);
	} else {
		if(!host->eirq_enable) {
			((HWP_SDMMC_T*)host->base)->SDMMC_INT_MASK &= ~SDMMC_SDIO_INT_MK;
		}
		else
			rda_mmc_mask_eirq(host);
	}
	host->sdio_irq_enable = !!enable;

	spin_unlock_irqrestore(&host->lock, flags);
}

void rda_mmc_set_sdio_irq(u32 host_id, u8 enable)
{
	struct rda_mmc_host *host;

	if (host_id >= RDA_MMC_HOST_COUNT)
	{
		pr_debug("rda_mmc_set_sdio_irq: invalid host_id=%u\n", host_id);
		return;
	}

	host = rda_mmc_hosts[host_id];
	if (!host || !host->mmc) {
		pr_debug("rda_mmc_set_sdio_irq: host%u not ready\n", host_id);
		return;
	}

	pr_debug("rda_mmc_set_sdio_irq: host%u enable=%u\n", host_id, !!enable);

	rda_mmc_enable_sdio_irq(host->mmc, !!enable);
}
EXPORT_SYMBOL(rda_mmc_set_sdio_irq);

void rda_mmc_set_present(u32 host_id, u8 present)
{
	struct rda_mmc_host *host;

	if (host_id >= RDA_MMC_HOST_COUNT)
		return;

	host = rda_mmc_hosts[host_id];
	if (!host || !host->mmc)
		return;

	host->present = !!present;
	if (host_id == 1)
		pr_debug("rda_mmc_set_present host1: present=%u\n", !!present);
}
EXPORT_SYMBOL(rda_mmc_set_present);

bool rda_mmc_card_attached(u32 host_id)
{
	struct rda_mmc_host *host;

	if (host_id >= RDA_MMC_HOST_COUNT)
		return false;

	host = rda_mmc_hosts[host_id];
	if (!host || !host->mmc)
		return false;

	return READ_ONCE(host->mmc->card) != NULL;
}
EXPORT_SYMBOL(rda_mmc_card_attached);

void rda_mmc_bus_scan(u32 host_id)
{
	struct rda_mmc_host *host;

	if (host_id >= RDA_MMC_HOST_COUNT)
	{
		pr_debug("rda_mmc_bus_scan: invalid host_id=%u\n", host_id);
		return;
	}

	host = rda_mmc_hosts[host_id];
	if (!host || !host->mmc) {
		pr_debug("rda_mmc_bus_scan: host%u not ready\n", host_id);
		return;
	}

	/* Match old vendor sequence: reset/init host before forcing a rescan. */
	rda_mmc_reset(host_id);
	hal_mmc_init(host);
	rda_mmc_restore_ios(host);

	pr_debug("rda_mmc_bus_scan: host%u detect_change (rescan_disable=%u detect_change=%u caps=0x%x pm=%u present=%d suspend=%u)\n",
		host_id, host->mmc->rescan_disable, host->mmc->detect_change,
		host->mmc->caps, host->mmc_pm, host->present, host->suspend);

	if (host->suspend) {
		host->suspend = 0;
		return;
	}

	mmc_detect_change(host->mmc, msecs_to_jiffies(50));
}
EXPORT_SYMBOL(rda_mmc_bus_scan);

static int rda_mmc_get_cd(struct mmc_host *mmc)
{
	int present = -ENOSYS;
	struct rda_mmc_host *host = mmc_priv(mmc);

	/*
	 * Host1 WiFi SDIO slot is combo-managed on this platform. When there is
	 * no detect pin, trust combo-controlled presence regardless of mmc caps.
	 */
	if (host->id == 1 && !host->detpin_enable) {
		present = !!host->present;
		pr_debug("rda_mmc_get_cd host1: detpin_enable=%u caps=0x%x present=%d\n",
			host->detpin_enable, mmc->caps, present);
		return present;
	}

	if (mmc->caps & MMC_CAP_NONREMOVABLE) {
		host->present = 1;
		return 1;
	}

	if (host->detpin_enable)
	{
		present = !gpiod_get_value(host->det_pin);
		
		host->present = present;
		dev_dbg(mmc_dev(host->mmc), "card is %s present\n", present ? "" : "not");

		if (!present) {
			hal_mmc_disable(host);
			/* Clear all pending irqs. */
			hal_irq_clear(host, 0xFF);
			/* Reset ifc to clear fifo. */
			rda_mmc_reset(host->id);
			hal_set_bus_width(host, 0);
			host->bus_width = 0;
		}
	} else {
		/* If no detection, we assume there is a card. */
		present = 1;
		host->present = 1;
	}

	if (host->id == 1)
		pr_debug("rda_mmc_get_cd host1: detpin_enable=%u caps=0x%x present=%d\n",
			host->detpin_enable, mmc->caps, present);

	return present;
}

static irqreturn_t rda_mmc_det_irq(int irq, void *data)
{
	struct rda_mmc_host *host = data;

	if (host->mmc->caps & MMC_CAP_NONREMOVABLE)
		return IRQ_HANDLED;

	int present = !gpiod_get_value(host->det_pin);

	/* entering this ISR means that we have configured det_pin:
	 * we can use its value in board structure */
	
	/*
	 * we expect this irq on both insert and remove,
	 * and use a short delay to debounce.
	 */
	if (present != host->present) {
		host->present = present;
		pr_info("%s: card %s\n", mmc_hostname(host->mmc),
			present ? "insert" : "remove");

		if (!present) {
			if (host->data) {
				/* Wake up requeset's waiting queue */
				complete(&host->req_done);
			}
		} else {
			hal_mmc_init(host);
			rda_mmc_restore_ios(host);
		}

		mmc_detect_change(host->mmc, 0);
	}

	return IRQ_HANDLED;
}

static const struct mmc_host_ops rda_mmc_ops = {
	.request	= rda_mmc_request,
	.get_ro 	= rda_mmc_get_ro,
	.get_cd		= rda_mmc_get_cd,
	.set_ios	= rda_mmc_set_ios,
	.enable_sdio_irq = rda_mmc_enable_sdio_irq,
};

static void rda_sdio_timeout_work(struct work_struct *work)
{
	struct rda_mmc_host *host = container_of(work, struct rda_mmc_host, timeout_work.work);

	if (host )mmc_signal_sdio_irq(host->mmc);
}



static irqreturn_t rda_sdio_eirq_handler(int irq, void *dev_id)
{
	unsigned long flags;
	struct rda_mmc_host *host = dev_id;
	if(!host) return IRQ_HANDLED;
			
	u32 int_status = ((HWP_SDMMC_T*)host->base)->SDMMC_INT_STATUS;

	if (!(int_status & SDMMC_SDIO_INT))
	{
		//dev_info(mmc_dev(host->mmc), "host id : %d,int_status:%d\n", host->id, int_status);
		return IRQ_NONE;
	}

	if(host->mmc->sdio_irqs) {
		if (host->suspend) {
			/*
			 * The handler is invoked as soon as AP is waked up via WiFi,
			 * but resume function is not called by AP at this time.
			 * So,we disable interrupt at first, because the interrupt is triggered
			 * via low-level, then schedule a delayed work to wait for resume callback.
			 * */
			rda_mmc_enable_sdio_irq(host->mmc, 0);
			schedule_delayed_work(&host->timeout_work, msecs_to_jiffies(10));
		} else {
			mmc_signal_sdio_irq(host->mmc);
		}
	} else {
		spin_lock_irqsave(&host->lock, flags);
		rda_mmc_mask_eirq(host);
		spin_unlock_irqrestore(&host->lock, flags);
	}

	return IRQ_HANDLED;
}

static ssize_t rda_mmc_unaligned_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct rda_mmc_host *host = (struct rda_mmc_host *)mmc_priv(mmc);

	return sprintf(buf, "%d\n", host->unaligned_count);
}

static ssize_t rda_mmc_reset_num_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mmc_host *mmc = dev_get_drvdata(dev);
	struct rda_mmc_host *host = (struct rda_mmc_host *)mmc_priv(mmc);

	return sprintf(buf, "%d\n", host->reset_count);
}

static struct device_attribute rda_mmc_attributes[] = {
	__ATTR(mmc_unalign, 0444, rda_mmc_unaligned_show, NULL),
	__ATTR(reset_num, 0444, rda_mmc_reset_num_show, NULL),
};

static u32 rda_mmc_get_dt_u32(struct platform_device *pdev,
			      const char *property_name, u32 default_value)
{
	u32 value;

	if (of_property_read_u32(pdev->dev.of_node, property_name, &value))
		return default_value;

	return value;
}



static int rda_mmc_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc = NULL;
	struct rda_mmc_host *host = NULL;
	struct resource *res;
	int ret = 0, irq = 0;
	unsigned long flags;
	int index;
	u32 host_id;
	
	hwp_apSysCtrl = (HWP_SYS_CTRL_AP_T *)ioremap(RDA_SYSCTRL_PHYS, RDA_SYSCTRL_SIZE);
	
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	if (res == NULL || irq < 0) {
		return -ENXIO;
	}
	if (of_property_read_u32(pdev->dev.of_node, "mmc_id", &host_id)) {
		int alias_id = of_alias_get_id(pdev->dev.of_node, "mmc");

		if (alias_id < 0)
			return -EINVAL;

		host_id = alias_id;
	}

	pdev->id = host_id;
	dev_dbg(&pdev->dev, "rda_mmc_probe, %d, base = %08x, irq = %d\n", pdev->id, res->start, irq);

	mmc = mmc_alloc_host(sizeof(struct rda_mmc_host), &pdev->dev);
	if (!mmc) {
		return -ENOMEM;
	}

	mmc->ops = &rda_mmc_ops;

	/*
	 * We do not have SG-DMA, use 1.
	 */
	mmc->max_segs = 1;
	/*
	 * Our hardware DMA can handle a maximum of one page per SG entry.
	 */
	if (pdev->id == 0) {
		mmc->max_req_size = RDA_MMC0_REQ_NUM_PAGE * PAGE_SIZE;
		mmc->max_seg_size = RDA_MMC0_REQ_NUM_PAGE * PAGE_SIZE;
	} else {
		mmc->max_req_size = 16 * PAGE_SIZE;
		mmc->max_seg_size = 16 * PAGE_SIZE;
	}

	/*
	 * Block length register is only 10 bits before PXA27x.
	 */
	mmc->max_blk_size = 4096;

	/*
	 * Block count register is 16 bits.
	 */
	mmc->max_blk_count = 65535;
	

	host = mmc_priv(mmc);
	host->id = pdev->id;
	host->mmc = mmc;
	host->irq = irq;
	host->sdio_irq_enable = 0;
	host->mmc_pm = MMC_POWER_OFF;
	host->main_irq_registered = false;
	host->eirq_registered = false;
	host->detirq_registered = false;
	host->tasklet_inited = false;
	host->eirq = -1;
	host->det_irq = -1;

	init_completion(&host->req_done);

	host->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(host->base)) {
		ret = PTR_ERR(host->base);
		goto err_free_host;
	}

	rda_mmc_force_bb_pins_alt(pdev, host->id);
	

	if (host->id == 0)
	{
	host->host_reg = devm_regulator_get_optional(&pdev->dev, LDO_SDMMC);
		if (IS_ERR(host->host_reg)) {
			if (PTR_ERR(host->host_reg) == -ENODEV ||
			    PTR_ERR(host->host_reg) == -ENOENT) {
				host->host_reg = NULL;
			} else {
				dev_err(&pdev->dev, "could not find regulator devices\n");
				ret = PTR_ERR(host->host_reg);
				goto err_free_reg;
			}
		}
	}
	

	if (rda_soc_is_older_metal10()) {
		dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
		/*
	 	* A 4KBytes memory is enough to save data
	 	* whose address is not aligned with page.
	 	*/
		host->tmp_buf = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &host->phys_tmp, GFP_KERNEL);
		if (!host->tmp_buf) {
			dev_err(&pdev->dev, "could not allocate a reserved memory of dma\n");
			ret = -ENOMEM;
			goto err_dma_alloc;
		}
	}
	
	/*
	 * Keep vendor default identification clock unless DT overrides it.
	 * RDA combo SDIO bring-up is known to be timing-sensitive here.
	 */
	mmc->f_min = rda_mmc_get_dt_u32(pdev, "min-frequency", 10000000);
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->f_max = rda_mmc_get_dt_u32(pdev, "max-frequency", 30000000);
	mmc->caps = rda_mmc_get_dt_u32(pdev, "caps", MMC_CAP_4_BIT_DATA);
	mmc->pm_caps = rda_mmc_get_dt_u32(pdev, "pm_caps", 0);
	
	host->det_pin = devm_gpiod_get_optional(&pdev->dev, "detpin", GPIOD_IN);
	
	host->detpin_enable = false;
	host->present = 1;
	
	if (IS_ERR(host->det_pin))
	{
		ret = PTR_ERR(host->det_pin);
		if (ret == -EPROBE_DEFER)
			goto out;
		host->det_pin = NULL;
	}
	else if (host->det_pin)
	{
		host->detpin_enable = true;
	}

	if (mmc->caps & MMC_CAP_NONREMOVABLE)
		host->detpin_enable = false;

	/*
	 * Keep combo-managed SDIO host absent until WiFi power-on sequence runs,
	 * preventing premature SDIO probes while RF/PMU are still off.
	 */
	if (host->id == 1 && !host->detpin_enable)
		host->present = 0;
	
	host->eirq_pin = devm_gpiod_get_optional(&pdev->dev, "eirqpin", GPIOD_IN);
	
	host->eirq_enable = false;
	
	if (IS_ERR(host->eirq_pin))
	{
		ret = PTR_ERR(host->eirq_pin);
		if (ret == -EPROBE_DEFER)
			goto out;
		host->eirq_pin = NULL;
	}
	else if (host->eirq_pin)
	{
		host->eirq_enable = true;
	}
	
	host->sys_suspend = rda_mmc_get_dt_u32(pdev, "sys_suspend", 1);
	host->clk_inv = rda_mmc_get_dt_u32(pdev, "clk_inv", 0);
	host->mclk_adj = rda_mmc_get_dt_u32(pdev, "mclk_adj", 0);
	if (host->id == 1)
		dev_dbg(&pdev->dev,
			 "host1 cfg: f_min=%u f_max=%u caps=0x%x pm_caps=0x%x detpin=%u eirq=%u\n",
			 mmc->f_min, mmc->f_max, mmc->caps, mmc->pm_caps,
			 host->detpin_enable, host->eirq_enable);
	/*
	 * Calculate minimum clock rate, rounding up.
	 */
	hal_mmc_init(host);
	host->id = pdev->id;
	hal_set_bus_width(host, 0);
	host->bus_width = 0;
	spin_lock_init(&host->lock);
	
	host->master_clk = devm_clk_get_optional(&pdev->dev, NULL);
	
	if (IS_ERR(host->master_clk)) {
		dev_err(&pdev->dev, "no handler of clock\n");
		ret = -EINVAL;
		goto err_free_reg;
	}
	host->clock = 0;
	
	if (host->master_clk) {
		ret = clk_prepare_enable(host->master_clk);
		if (ret < 0) {
			dev_err(&pdev->dev, "do not enable the specified clock\n");
			goto err_clk;
		}
	}


	tasklet_init(&host->finish_tasklet, tasklet_worker, (unsigned long)host);
	host->tasklet_inited = true;

	/* Request IRQ for MMC operations */
	ret = request_irq(host->irq, rda_mmc_irq, 0x0, mmc_hostname(mmc), host);
	if (ret) {
		dev_err(&pdev->dev, "unable to request IRQ ERR = %d\n", ret);
		goto err_clk;
	}
	host->main_irq_registered = true;
	
	if (host->eirq_enable)
	{
		ret = gpiod_direction_input(host->eirq_pin);
		
		if (ret < 0)
		{
			dev_warn(&pdev->dev, "unable to configure eirq pin as input, fallback to SDIO interrupt\n");
			host->eirq_enable = false;
		}

		if (host->eirq_enable) {
			host->eirq = gpiod_to_irq(host->eirq_pin);
			if (host->eirq < 0) {
				dev_warn(&pdev->dev, "invalid sd eirq (%d), fallback to SDIO interrupt\n", host->eirq);
				host->eirq_enable = false;
			}
		}

		if (host->eirq_enable) {
			ret = request_irq(host->eirq, rda_sdio_eirq_handler,
					  IRQF_TRIGGER_LOW | IRQF_NO_SUSPEND,
					  "rda_wlan_irq", host);

			if (ret) {
				dev_warn(&pdev->dev, "request_irq sd eirq failed, fallback to SDIO interrupt\n");
				host->eirq_enable = false;
			} else {
				host->eirq_registered = true;
				spin_lock_irqsave(&host->lock, flags);
				rda_mmc_mask_eirq(host);
				spin_unlock_irqrestore(&host->lock, flags);
			}
		}
	}
	

	if (host->detpin_enable)
	{
		ret = gpiod_direction_input(host->det_pin);
		if (ret >= 0)
		{
			int det_irq = gpiod_to_irq(host->det_pin);
			host->det_irq = det_irq;

			if (det_irq < 0) {
				dev_warn(&pdev->dev, "invalid MMC detect irq (%d), fallback to polling\n", det_irq);
				mmc->caps |= MMC_CAP_NEEDS_POLL;
			} else {
				ret = request_irq(det_irq, rda_mmc_det_irq,
						  IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND,
						  "SD_det_pin", host);
			
				if (ret)
				{
					dev_warn(&pdev->dev, "request MMC detect irq failed, fallback to polling\n");
					mmc->caps |= MMC_CAP_NEEDS_POLL;
					host->det_irq = -1;
				} else {
					host->detirq_registered = true;
				}
			}
		}
		else {
			dev_warn(&pdev->dev,"det pin fail, fallback to polling\n");
			mmc->caps |= MMC_CAP_NEEDS_POLL;
			host->det_irq = -1;
		}
		
	}
	
	

	/* Just for sdio host. */
	if (host->id == 1) {
		INIT_DELAYED_WORK(&host->timeout_work, rda_sdio_timeout_work);
		
	}

	ret = mmc_add_host(mmc);
	if (ret)
		goto out;

	platform_set_drvdata(pdev, (void *)mmc);
	if (host->id < RDA_MMC_HOST_COUNT)
		rda_mmc_hosts[host->id] = host;

	if (host->eirq_enable) {
		device_init_wakeup(&pdev->dev, 1);
	}

	host->unaligned_count = 0;
	for (index = 0; index < ARRAY_SIZE(rda_mmc_attributes); index++)
	{
		device_create_file(&pdev->dev, &rda_mmc_attributes[index]);
	}

	dev_dbg(&pdev->dev, "rda_sdmmc %d initialized.\n", pdev->id);
	
	
	return 0;

out:

#ifdef RDA_MMC_USE_INT
	if (host->main_irq_registered)
		free_irq(host->irq, host);
	if (host->tasklet_inited)
		tasklet_kill(&host->finish_tasklet);
#endif

	if (host->eirq_registered && host->eirq >= 0)
		free_irq(host->eirq, host);

	if (host->detirq_registered && host->det_irq >= 0)
		free_irq(host->det_irq, host);
	

	if (host->master_clk) {
		clk_disable_unprepare(host->master_clk);
	}

err_clk:

err_dma_alloc:
	if (host->tmp_buf) {
		dma_free_coherent(&pdev->dev, PAGE_SIZE, host->tmp_buf, host->phys_tmp);
	}

err_free_reg:

	if (host->id == 0) {
		if (host->host_reg) {
			regulator_disable(host->host_reg);
		}
	}

err_free_host:

	if (mmc) {
		mmc_free_host(mmc);
	}

	return ret;
}

static void rda_mmc_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	

	if (mmc) {
		struct rda_mmc_host *host = mmc_priv(mmc);

		if (host->id < RDA_MMC_HOST_COUNT &&
			rda_mmc_hosts[host->id] == host)
			rda_mmc_hosts[host->id] = NULL;

		if (host->master_clk) {
			clk_disable_unprepare(host->master_clk);
		}

		if (host->id == 0) {
			if (host->host_reg) {
				regulator_disable(host->host_reg);
			}
		}

		host->mmc_pm = MMC_POWER_OFF;

		mmc_remove_host(mmc);

#ifdef RDA_MMC_USE_INT
		if (host->main_irq_registered)
			free_irq(host->irq, host);
		if (host->tasklet_inited)
			tasklet_kill(&host->finish_tasklet);
#endif
		
		
		if (host->eirq_registered && host->eirq >= 0)
		{
			free_irq(host->eirq, host);
		}

		if (host->detirq_registered && host->det_irq >= 0)
		{
			free_irq(host->det_irq, host);
		}

		if (host->tmp_buf) {
			dma_free_coherent(&pdev->dev, PAGE_SIZE, host->tmp_buf, host->phys_tmp);
		}

		mmc_free_host(mmc);
	}
}

static void rda_mmc_reset(u32 host_id)
{
	struct rda_mmc_host *host;

	if (host_id > 2) {
		return;
	}

	host = rda_mmc_hosts[host_id];
	if (host)
		host->reset_count++;

	hwp_apSysCtrl->APB2_Rst_Set=1<<(APB2_RST_SDMMC1+host_id);
	mdelay(1);
	hwp_apSysCtrl->APB2_Rst_Clr=1<<(APB2_RST_SDMMC1+host_id);
	mdelay(1);
}

#ifdef CONFIG_PM
static int rda_mmc_suspend(struct platform_device *dev, pm_message_t state)
{
	struct mmc_host *mmc = platform_get_drvdata(dev);
	struct rda_mmc_host *host;
	int ret = 0;

	if (mmc) {
			host = mmc_priv(mmc);
		if (host) {

			dev_info(mmc_dev(host->mmc), "host:(%d) rda_mmc_suspend \n",host->id);

			if (!host->sys_suspend) {
				return ret;
			}

			if (!host->suspend) {
				
				if (host->master_clk)
					clk_disable_unprepare(host->master_clk);
			}

			host->suspend = 1;
			if (!ret) {
				if (host->id == 0 && host->mmc_pm != MMC_POWER_OFF) {
					if (host->host_reg) {
						ret = regulator_disable(host->host_reg);
					}
				}
				host->mmc_pm = MMC_POWER_OFF;
			}
		}
	}

	return ret;
}

static int rda_mmc_resume(struct platform_device *dev)
{
	struct mmc_host *mmc = platform_get_drvdata(dev);
	struct rda_mmc_host *host;
	int ret = 0;

	if (mmc) {
			host = mmc_priv(mmc);
		if (host) {
			dev_info(mmc_dev(host->mmc), "host:(%d) rda_mmc_resume \n",host->id);

			if (!host->sys_suspend) {
				return ret;
			}

			if (host->id == 0 && host->mmc_pm == MMC_POWER_OFF) {
				if (host->host_reg) {
					ret = regulator_enable(host->host_reg);
				}
				host->mmc_pm = MMC_POWER_UP;
			}

			if (host->suspend) {
				if (host->master_clk)
					clk_prepare_enable(host->master_clk);
				
			} else {
				mmc_detect_change(host->mmc, msecs_to_jiffies(50));
			}
			host->suspend = 0;
		}
	}

	return ret;
}
#else
#define rda_mmc_suspend NULL
#define rda_mmc_resume	NULL
#endif

static const struct of_device_id rda_mmc_dt_matches[] = {
	{ .compatible = "rda,8810pl-mmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rda_mmc_dt_matches);

static struct platform_driver rda_mmc_driver = {
	.probe		= rda_mmc_probe,
	.remove 	= rda_mmc_remove,
	.suspend	= rda_mmc_suspend,
	.resume 	= rda_mmc_resume,
	.driver 	= {
		.name	= RDA_MMC_DRV_NAME,
		.of_match_table = rda_mmc_dt_matches,
	},
};

static int __init rda_mmc_init(void)
{
	return platform_driver_register(&rda_mmc_driver);

}

static void __exit rda_mmc_exit(void)
{
	platform_driver_unregister(&rda_mmc_driver);
}

module_init(rda_mmc_init);
module_exit(rda_mmc_exit);

MODULE_DESCRIPTION("RDA Multimedia Card Interface Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:rda-mmc");
