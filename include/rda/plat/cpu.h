/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RDA_PLAT_CPU_H
#define __RDA_PLAT_CPU_H

#define RDA8810_PROD_ID 8810
#define RDA8810_METAL_10_ID 10

/*
 * Keep the compatibility API used by legacy RDA drivers. Without a full SoC
 * revision reader in mainline-style code yet, default to metal 10 semantics.
 */
static inline unsigned short rda_get_soc_metal_id(void)
{
	return RDA8810_METAL_10_ID;
}

static inline unsigned short rda_get_soc_prod_id(void)
{
	return RDA8810_PROD_ID;
}

static inline int rda_soc_is_older_metal10(void)
{
	return rda_get_soc_prod_id() == 0 ||
		(rda_get_soc_prod_id() == RDA8810_PROD_ID &&
		 rda_get_soc_metal_id() < RDA8810_METAL_10_ID);
}

#endif /* __RDA_PLAT_CPU_H */