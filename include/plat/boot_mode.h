/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PLAT_RDA_BOOT_MODE_H
#define __PLAT_RDA_BOOT_MODE_H

enum {
	BM_NORMAL = 0,
	BM_CHARGER = 1,
};

static inline int rda_get_boot_mode(void)
{
	return BM_NORMAL;
}

#endif /* __PLAT_RDA_BOOT_MODE_H */