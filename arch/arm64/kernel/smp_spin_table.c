// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spin Table SMP initialisation
 *
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/init.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/mm.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/io.h>
#include <asm/smp_plat.h>

extern void secondary_holding_pen(void);
volatile unsigned long __section(".mmuoff.data.read")
secondary_holding_pen_release = INVALID_HWID;
/*
 * RTK vendor firmware variants may still poll this symbol name.
 * Keep both release variables in sync.
 */
volatile unsigned long __section(".mmuoff.data.read")
rtk_secondary_holding_pen_release = INVALID_HWID;

static phys_addr_t cpu_release_addr[NR_CPUS];

#define RTK_AARCH_REGISTER	0x9800707c
#define RTK_CPU_RELEASE_ADDR	0x98007f30
#define RTK_CPU_MARK_ENTER	0x98007f34
#define RTK_CPU_MARK_PARK	0x98007f38
#define RTK_CPU_MARK_JUMP	0x98007f3c
#define RTK_CHAIN_UBOOT_BASE	0x05000000
#define RTK_SCPU_PWR_STAT	0x9801d538
#define RTK_SCPU_RESET_CTRL	0x9801d100
#define RTK_SCPU_BOOT_CTRL	0x9801d900
#define RTK_SCPU_WRAP_BASE	0x9801d000

static bool rtk_secondaries_released;

static void rtk_writel(u32 val, phys_addr_t addr)
{
	void __iomem *reg = ioremap(addr, sizeof(u32));

	if (!reg)
		return;

	writel_relaxed(val, reg);
	readl_relaxed(reg);
	iounmap(reg);
}

static u32 rtk_readl(phys_addr_t addr)
{
	void __iomem *reg = ioremap(addr, sizeof(u32));
	u32 val = 0;

	if (!reg)
		return 0;

	val = readl_relaxed(reg);
	iounmap(reg);

	return val;
}

static void rtk_cpu_power_up(unsigned int cpu)
{
	void __iomem *base = ioremap(RTK_SCPU_WRAP_BASE, 0x1000);
	u32 tmp;
	u32 pwr;
	u32 stat;
	u32 reset;
	u32 boot;
	u32 cpu_offset = cpu * 4;
	unsigned int timeout;

	if (!base)
		return;

	tmp = readl_relaxed(base + 0x538);
	tmp |= BIT(cpu) | BIT(8 + cpu);
	writel_relaxed(tmp, base + 0x538);

	for (timeout = 1000; timeout; timeout--) {
		tmp = readl_relaxed(base + 0x53c);
		if ((tmp & (BIT(cpu) | BIT(8 + cpu))) ==
		    (BIT(cpu) | BIT(8 + cpu)))
			break;
		udelay(1);
	}

	tmp = readl_relaxed(base + 0x538);
	tmp &= ~BIT(16 + cpu);
	writel_relaxed(tmp, base + 0x538);

	tmp = readl_relaxed(base + 0x540 + cpu_offset);
	tmp &= 0x0000003f;
	writel_relaxed(tmp, base + 0x540 + cpu_offset);
	tmp |= 0x01000008;
	writel_relaxed(tmp, base + 0x540 + cpu_offset);

	if (cpu < 4) {
		tmp = readl_relaxed(base + 0x100);
		tmp |= BIT(cpu) | BIT(4 + cpu);
		writel_relaxed(tmp, base + 0x100);
	} else {
		tmp = readl_relaxed(base + 0x900);
		tmp |= BIT(cpu - 4) | BIT(4 + cpu - 4);
		writel_relaxed(tmp, base + 0x900);
	}

	readl_relaxed(base + 0x538);
	pwr = readl_relaxed(base + 0x538);
	stat = readl_relaxed(base + 0x53c);
	reset = readl_relaxed(base + 0x100);
	boot = readl_relaxed(base + 0x900);
	iounmap(base);

	pr_info("rtk-spin-table: vendor power CPU%u pwr 0x%08x stat 0x%08x reset 0x%08x boot 0x%08x timeout %u\n",
		cpu, pwr, stat, reset, boot, timeout);
}

static void rtk_release_secondaries(u32 entry)
{
	unsigned int cpu;
	u64 smc_res;

	if (rtk_secondaries_released)
		return;

	for (cpu = 1; cpu < nr_cpu_ids; cpu++)
		if (cpu_possible(cpu))
			rtk_cpu_power_up(cpu);

	asm volatile("mov x0, #0x84000000\n"
		     "movk x0, #0xff04\n"
		     "mov x1, %1\n"
		     "smc #0\n"
		     "mov %0, x0"
		     : "=r" (smc_res)
		     : "r" ((u64)entry)
		     : "x0", "x1", "memory");
	pr_info("rtk-spin-table: set BL31 pm param 0x%08x returned 0x%llx\n",
		entry, smc_res);

	/*
	 * Match the RTD161x firmware sequence.  The ROM/FSBL path expects to
	 * enter U-Boot first; the chain U-Boot parks secondaries until the kernel
	 * writes the real holding-pen address below.
	 */
	rtk_writel(0x0, RTK_AARCH_REGISTER);
	rtk_writel(RTK_CHAIN_UBOOT_BASE, RTK_CPU_RELEASE_ADDR);
	rtk_writel(0x00003f3f, RTK_SCPU_PWR_STAT);
	rtk_writel(rtk_readl(RTK_SCPU_RESET_CTRL) | 0xff, RTK_SCPU_RESET_CTRL);
	rtk_writel(0x00003233, RTK_SCPU_BOOT_CTRL);
	dsb(sy);
	sev();
	mdelay(10);

	rtk_writel(entry, RTK_CPU_RELEASE_ADDR);
	dsb(sy);
	sev();
	udelay(100);

	rtk_secondaries_released = true;
	pr_info("rtk-spin-table: released secondary CPUs via 0x%08x to 0x%08x, mailbox 0x%08x pwr 0x%08x reset 0x%08x boot 0x%08x mark enter 0x%08x park 0x%08x jump 0x%08x\n",
		RTK_CHAIN_UBOOT_BASE, entry, rtk_readl(RTK_CPU_RELEASE_ADDR),
		rtk_readl(RTK_SCPU_PWR_STAT), rtk_readl(RTK_SCPU_RESET_CTRL),
		rtk_readl(RTK_SCPU_BOOT_CTRL), rtk_readl(RTK_CPU_MARK_ENTER),
		rtk_readl(RTK_CPU_MARK_PARK), rtk_readl(RTK_CPU_MARK_JUMP));
}

/*
 * Write secondary_holding_pen_release in a way that is guaranteed to be
 * visible to all observers, irrespective of whether they're taking part
 * in coherency or not.  This is necessary for the hotplug code to work
 * reliably.
 */
static void write_pen_release(u64 val)
{
	void *start = (void *)&secondary_holding_pen_release;
	void *rtk_start = (void *)&rtk_secondary_holding_pen_release;
	unsigned long size = sizeof(secondary_holding_pen_release);

	secondary_holding_pen_release = val;
	rtk_secondary_holding_pen_release = val;
	dcache_clean_inval_poc((unsigned long)start, (unsigned long)start + size);
	dcache_clean_inval_poc((unsigned long)rtk_start,
			      (unsigned long)rtk_start + size);
}


static int smp_spin_table_cpu_init(unsigned int cpu)
{
	struct device_node *dn;
	int ret;

	dn = of_get_cpu_node(cpu, NULL);
	if (!dn)
		return -ENODEV;

	/*
	 * Determine the address from which the CPU is polling.
	 */
	ret = of_property_read_u64(dn, "cpu-release-addr",
				   &cpu_release_addr[cpu]);
	if (ret)
		pr_err("CPU %d: missing or invalid cpu-release-addr property\n",
		       cpu);

	of_node_put(dn);
	pr_info("rtk-spin-table: init CPU%u release 0x%llx ret %d\n",
		cpu, cpu_release_addr[cpu], ret);

	return ret;
}

static int smp_spin_table_cpu_prepare(unsigned int cpu)
{
	__le64 __iomem *release_addr;
	phys_addr_t pa_holding_pen = __pa_symbol(secondary_holding_pen);

	if (!cpu_release_addr[cpu])
		return -ENODEV;

	/*
	 * The cpu-release-addr may or may not be inside the linear mapping.
	 * As ioremap_cache will either give us a new mapping or reuse the
	 * existing linear mapping, we can use it to cover both cases. In
	 * either case the memory will be MT_NORMAL.
	 */
	release_addr = ioremap_cache(cpu_release_addr[cpu],
				     sizeof(*release_addr));
	if (!release_addr)
		return -ENOMEM;

	/*
	 * We write the release address as LE regardless of the native
	 * endianness of the kernel. Therefore, any boot-loaders that
	 * read this address need to convert this address to the
	 * boot-loader's endianness before jumping. This is mandated by
	 * the boot protocol.
	 */
	writeq_relaxed(pa_holding_pen, release_addr);
	dcache_clean_inval_poc((__force unsigned long)release_addr,
			    (__force unsigned long)release_addr +
				    sizeof(*release_addr));

	/*
	 * Send an event to wake up the secondary CPU.
	 */
	sev();

	iounmap(release_addr);

	return 0;
}

/*
 * Realtek firmware expects 32-bit release-addr writes for rtk-spin-table.
 * Standard spin-table uses 64-bit writeq().
 */
static int rtk_smp_spin_table_cpu_prepare(unsigned int cpu)
{
	u32 __iomem *release_addr;
	u32 pa_holding_pen = (u32)__pa_symbol(secondary_holding_pen);

	pr_info("rtk-spin-table: prepare CPU%u release 0x%llx pen 0x%08x\n",
		cpu, cpu_release_addr[cpu], pa_holding_pen);

	if (!cpu_release_addr[cpu])
		return -ENODEV;

	release_addr = ioremap(cpu_release_addr[cpu], sizeof(*release_addr));
	if (!release_addr)
		return -ENOMEM;

	writel_relaxed(pa_holding_pen, release_addr);
	readl_relaxed(release_addr);
	dsb(sy);

	rtk_release_secondaries(pa_holding_pen);

	sev();
	iounmap(release_addr);

	return 0;
}

static int smp_spin_table_cpu_boot(unsigned int cpu)
{
	pr_info("rtk-spin-table: boot CPU%u mpidr 0x%llx\n",
		cpu, cpu_logical_map(cpu));

	if (cpu > 0)
		rtk_smp_spin_table_cpu_prepare(cpu);

	/*
	 * Update the pen release flag.
	 */
	write_pen_release(cpu_logical_map(cpu));

	/*
	 * Send an event, causing the secondaries to read pen_release.
	 */
	sev();

	return 0;
}

const struct cpu_operations smp_spin_table_ops = {
	.name		= "spin-table",
	.cpu_init	= smp_spin_table_cpu_init,
	.cpu_prepare	= smp_spin_table_cpu_prepare,
	.cpu_boot	= smp_spin_table_cpu_boot,
};

const struct cpu_operations rtk_smp_spin_table_ops = {
	.name		= "rtk-spin-table",
	.cpu_init	= smp_spin_table_cpu_init,
	.cpu_prepare	= rtk_smp_spin_table_cpu_prepare,
	.cpu_boot	= smp_spin_table_cpu_boot,
};
