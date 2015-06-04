/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Numascale NumaConnect-Specific APIC Code
 *
 * Copyright (C) 2011 Numascale AS. All rights reserved.
 *
 * Send feedback to <support@numascale.com>
 *
 */

#include <linux/errno.h>
#include <linux/threads.h>
#include <linux/cpumask.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/hardirq.h>
#include <linux/delay.h>

#include <asm/numachip/numachip_csr.h>
#include <asm/smp.h>
#include <asm/apic.h>
#include <asm/ipi.h>
#include <asm/apic_flat_64.h>

static int numachip_system __read_mostly;

static struct apic apic_numachip __read_mostly;

static unsigned int get_apic_id(unsigned long x)
{
	unsigned long value;
	unsigned int id;

	rdmsrl(MSR_FAM10H_NODE_ID, value);
	id = ((x >> 24) & 0xffU) | ((value << 2) & 0x3f00U);

	return id;
}

static unsigned long set_apic_id(unsigned int id)
{
	unsigned long x;

	x = ((id & 0xffU) << 24);
	return x;
}

static unsigned int read_xapic_id(void)
{
	return get_apic_id(apic_read(APIC_ID));
}

static int numachip_apic_id_registered(void)
{
	return physid_isset(read_xapic_id(), phys_cpu_present_map);
}

static int numachip_phys_pkg_id(int initial_apic_id, int index_msb)
{
	return initial_apic_id >> index_msb;
}

static const struct cpumask *numachip_target_cpus(void)
{
	return cpu_online_mask;
}

static void numachip_vector_allocation_domain(int cpu, struct cpumask *retmask)
{
	cpumask_clear(retmask);
	cpumask_set_cpu(cpu, retmask);
}

static int __cpuinit numachip_wakeup_secondary(int phys_apicid, unsigned long start_rip)
{
	union numachip_csr_g3_ext_irq_gen int_gen;

	int_gen.s._destination_apic_id = phys_apicid;
	int_gen.s._vector = 0;
	int_gen.s._msgtype = APIC_DM_INIT >> 8;
	int_gen.s._index = 0;

	write_lcsr(CSR_G3_EXT_IRQ_GEN, int_gen.v);

	int_gen.s._msgtype = APIC_DM_STARTUP >> 8;
	int_gen.s._vector = start_rip >> 12;

	write_lcsr(CSR_G3_EXT_IRQ_GEN, int_gen.v);

	atomic_set(&init_deasserted, 1);
	return 0;
}

static void numachip_send_IPI_one(int cpu, int vector)
{
	union numachip_csr_g3_ext_irq_gen int_gen;
	int apicid = per_cpu(x86_cpu_to_apicid, cpu);

	int_gen.s._destination_apic_id = apicid;
	int_gen.s._vector = vector;
	int_gen.s._msgtype = (vector == NMI_VECTOR ? APIC_DM_NMI : APIC_DM_FIXED) >> 8;
	int_gen.s._index = 0;

	write_lcsr(CSR_G3_EXT_IRQ_GEN, int_gen.v);
}

static void numachip_send_IPI_mask(const struct cpumask *mask, int vector)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		numachip_send_IPI_one(cpu, vector);
}

static void numachip_send_IPI_mask_allbutself(const struct cpumask *mask,
						int vector)
{
	unsigned int this_cpu = smp_processor_id();
	unsigned int cpu;

	for_each_cpu(cpu, mask) {
		if (cpu != this_cpu)
			numachip_send_IPI_one(cpu, vector);
	}
}

static void numachip_send_IPI_allbutself(int vector)
{
	unsigned int this_cpu = smp_processor_id();
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		if (cpu != this_cpu)
			numachip_send_IPI_one(cpu, vector);
	}
}

static void numachip_send_IPI_all(int vector)
{
	numachip_send_IPI_mask(cpu_online_mask, vector);
}

static void numachip_send_IPI_self(int vector)
{
	__default_send_IPI_shortcut(APIC_DEST_SELF, vector, APIC_DEST_PHYSICAL);
}

static unsigned int numachip_cpu_mask_to_apicid(const struct cpumask *cpumask)
{
	int cpu;

	/*
	 * We're using fixed IRQ delivery, can only return one phys APIC ID.
	 * May as well be the first.
	 */
	cpu = cpumask_first(cpumask);
	if (likely((unsigned)cpu < nr_cpu_ids))
		return per_cpu(x86_cpu_to_apicid, cpu);

	return BAD_APICID;
}

static unsigned int
numachip_cpu_mask_to_apicid_and(const struct cpumask *cpumask,
				const struct cpumask *andmask)
{
	int cpu;

	/*
	 * We're using fixed IRQ delivery, can only return one phys APIC ID.
	 * May as well be the first.
	 */
	for_each_cpu_and(cpu, cpumask, andmask) {
		if (cpumask_test_cpu(cpu, cpu_online_mask))
			break;
	}
	return per_cpu(x86_cpu_to_apicid, cpu);
}

static int __init numachip_probe(void)
{
	return apic == &apic_numachip;
}

static void __init map_csrs(void)
{
	printk(KERN_INFO "NumaChip: Mapping local CSR space (%016llx - %016llx)\n",
		NUMACHIP_LCSR_BASE, NUMACHIP_LCSR_BASE + NUMACHIP_LCSR_SIZE - 1);
	init_extra_mapping_uc(NUMACHIP_LCSR_BASE, NUMACHIP_LCSR_SIZE);

	printk(KERN_INFO "NumaChip: Mapping global CSR space (%016llx - %016llx)\n",
		NUMACHIP_GCSR_BASE, NUMACHIP_GCSR_BASE + NUMACHIP_GCSR_SIZE - 1);
	init_extra_mapping_uc(NUMACHIP_GCSR_BASE, NUMACHIP_GCSR_SIZE);
}

static void fixup_cpu_id(struct cpuinfo_x86 *c, int node)
{

	if (c->phys_proc_id != node) {
		c->phys_proc_id = node;
		per_cpu(cpu_llc_id, smp_processor_id()) = node;
	}
}

static int __init numachip_system_init(void)
{
	unsigned int val;

	if (!numachip_system)
		return 0;

	x86_cpuinit.fixup_cpu_id = fixup_cpu_id;

	map_csrs();

	val = read_lcsr(CSR_G0_NODE_IDS);
	printk(KERN_INFO "NumaChip: Local NodeID = %08x\n", val);

	return 0;
}
early_initcall(numachip_system_init);

static int numachip_acpi_madt_oem_check(char *oem_id, char *oem_table_id)
{
	if (!strncmp(oem_id, "NUMASC", 6)) {
		numachip_system = 1;
		return 1;
	}

	return 0;
}

static struct apic apic_numachip __refconst = {

	.name				= "NumaConnect system",
	.probe				= numachip_probe,
	.acpi_madt_oem_check		= numachip_acpi_madt_oem_check,
	.apic_id_registered		= numachip_apic_id_registered,

	.irq_delivery_mode		= dest_Fixed,
	.irq_dest_mode			= 0, /* physical */

	.target_cpus			= numachip_target_cpus,
	.disable_esr			= 0,
	.dest_logical			= 0,
	.check_apicid_used		= NULL,
	.check_apicid_present		= NULL,

	.vector_allocation_domain	= numachip_vector_allocation_domain,
	.init_apic_ldr			= flat_init_apic_ldr,

	.ioapic_phys_id_map		= NULL,
	.setup_apic_routing		= NULL,
	.multi_timer_check		= NULL,
	.cpu_present_to_apicid		= default_cpu_present_to_apicid,
	.apicid_to_cpu_present		= NULL,
	.setup_portio_remap		= NULL,
	.check_phys_apicid_present	= default_check_phys_apicid_present,
	.enable_apic_mode		= NULL,
	.phys_pkg_id			= numachip_phys_pkg_id,
	.mps_oem_check			= NULL,

	.get_apic_id			= get_apic_id,
	.set_apic_id			= set_apic_id,
	.apic_id_mask			= 0xffU << 24,

	.cpu_mask_to_apicid		= numachip_cpu_mask_to_apicid,
	.cpu_mask_to_apicid_and		= numachip_cpu_mask_to_apicid_and,

	.send_IPI_mask			= numachip_send_IPI_mask,
	.send_IPI_mask_allbutself	= numachip_send_IPI_mask_allbutself,
	.send_IPI_allbutself		= numachip_send_IPI_allbutself,
	.send_IPI_all			= numachip_send_IPI_all,
	.send_IPI_self			= numachip_send_IPI_self,

	.wakeup_secondary_cpu		= numachip_wakeup_secondary,
	.trampoline_phys_low		= DEFAULT_TRAMPOLINE_PHYS_LOW,
	.trampoline_phys_high		= DEFAULT_TRAMPOLINE_PHYS_HIGH,
	.wait_for_init_deassert		= NULL,
	.smp_callin_clear_local_apic	= NULL,
	.inquire_remote_apic		= NULL, /* REMRD not supported */

	.read				= native_apic_mem_read,
	.write				= native_apic_mem_write,
	.icr_read			= native_apic_icr_read,
	.icr_write			= native_apic_icr_write,
	.wait_icr_idle			= native_apic_wait_icr_idle,
	.safe_wait_icr_idle		= native_safe_apic_wait_icr_idle,
};
apic_driver(apic_numachip);

