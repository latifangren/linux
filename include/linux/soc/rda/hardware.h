/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_RDA_HARDWARE_H
#define __LINUX_SOC_RDA_HARDWARE_H

#include <linux/types.h>

typedef volatile u32 REG32;

#define GET_BITFIELD(dword, bitfield) \
	(((dword) & (bitfield##_MASK)) >> (bitfield##_SHIFT))

#endif /* __LINUX_SOC_RDA_HARDWARE_H */
