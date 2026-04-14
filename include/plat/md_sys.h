/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PLAT_RDA_MD_SYS_H
#define __PLAT_RDA_MD_SYS_H

#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/slab.h>

#define SYS_PM_MOD 0
#define SYS_PM_CMD_SET_CHARGER_TYPE 0
#define SYS_PM_MESG_BATT_STATUS 0

struct msys_device {
	unsigned int module;
	const char *name;
	struct notifier_block notifier;
	void *private;
};

struct client_cmd {
	struct msys_device *pmsys_dev;
	unsigned int mod_id;
	unsigned int mesg_id;
	void *pdata;
	unsigned int data_size;
};

struct client_mesg {
	unsigned int mod_id;
	unsigned int mesg_id;
	unsigned int param;
};

static inline struct msys_device *rda_msys_alloc_device(void)
{
	return kzalloc(sizeof(struct msys_device), GFP_KERNEL);
}

static inline void rda_msys_free_device(struct msys_device *dev)
{
	kfree(dev);
}

static inline int rda_msys_register_device(struct msys_device *dev)
{
	return 0;
}

static inline void rda_msys_unregister_device(struct msys_device *dev)
{
}

static inline unsigned int rda_msys_send_cmd(struct client_cmd *cmd)
{
	return 0;
}

#endif /* __PLAT_RDA_MD_SYS_H */