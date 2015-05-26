/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) 2012 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */
#define pr_fmt(fmt) "CPU PMU: " fmt

#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>

#include <asm/cputype.h>
#include <asm/irq_regs.h>
#include <asm/pmu.h>

/* Set at runtime when we know what CPU type we are. */
static struct arm_pmu *__oprofile_cpu_pmu;

/*
 * Despite the names, these two functions are CPU-specific and are used
 * by the OProfile/perf code.
 */
const char *perf_pmu_name(void)
{
	if (!__oprofile_cpu_pmu)
		return NULL;

	return __oprofile_cpu_pmu->name;
}
EXPORT_SYMBOL_GPL(perf_pmu_name);

int perf_num_counters(void)
{
	int max_events = 0;

	if (__oprofile_cpu_pmu != NULL)
		max_events = __oprofile_cpu_pmu->num_events;

	return max_events;
}
EXPORT_SYMBOL_GPL(perf_num_counters);

/* Include the PMU-specific implementations. */
#include "perf_event_v7.c"

static void cpu_pmu_enable_percpu_irq(void *data)
{
	int irq = *(int *)data;

	enable_percpu_irq(irq, IRQ_TYPE_NONE);
}

static void cpu_pmu_disable_percpu_irq(void *data)
{
	int irq = *(int *)data;

	disable_percpu_irq(irq);
}

static void cpu_pmu_free_irq(struct arm_pmu *cpu_pmu)
{
	int i, irq, irqs;
	struct platform_device *pmu_device = cpu_pmu->plat_device;
	struct pmu_hw_events __percpu *hw_events = cpu_pmu->hw_events;

	irqs = min(pmu_device->num_resources, num_possible_cpus());

	irq = platform_get_irq(pmu_device, 0);
	if (irq >= 0 && irq_is_percpu(irq)) {
		on_each_cpu(cpu_pmu_disable_percpu_irq, &irq, 1);
		free_percpu_irq(irq, &hw_events->percpu_pmu);
	} else {
		for (i = 0; i < irqs; ++i) {
			int cpu = i;

			if (cpu_pmu->irq_affinity)
				cpu = cpu_pmu->irq_affinity[i];

			if (!cpumask_test_and_clear_cpu(cpu, &cpu_pmu->active_irqs))
				continue;
			irq = platform_get_irq(pmu_device, i);
			if (irq >= 0)
				free_irq(irq, per_cpu_ptr(&hw_events->percpu_pmu, cpu));
		}
	}
}

static int cpu_pmu_request_irq(struct arm_pmu *cpu_pmu, irq_handler_t handler)
{
	int i, err, irq, irqs;
	struct platform_device *pmu_device = cpu_pmu->plat_device;
	struct pmu_hw_events __percpu *hw_events = cpu_pmu->hw_events;

	if (!pmu_device)
		return -ENODEV;

	irqs = min(pmu_device->num_resources, num_possible_cpus());
	if (irqs < 1) {
		pr_warn_once("perf/ARM: No irqs for PMU defined, sampling events not supported\n");
		return 0;
	}

	irq = platform_get_irq(pmu_device, 0);
	if (irq >= 0 && irq_is_percpu(irq)) {
		err = request_percpu_irq(irq, handler, "arm-pmu",
					 &hw_events->percpu_pmu);
		if (err) {
			pr_err("unable to request IRQ%d for ARM PMU counters\n",
				irq);
			return err;
		}
		on_each_cpu(cpu_pmu_enable_percpu_irq, &irq, 1);
	} else {
		for (i = 0; i < irqs; ++i) {
			int cpu = i;

			err = 0;
			irq = platform_get_irq(pmu_device, i);
			if (irq < 0)
				continue;

			if (cpu_pmu->irq_affinity)
				cpu = cpu_pmu->irq_affinity[i];

			/*
			 * If we have a single PMU interrupt that we can't shift,
			 * assume that we're running on a uniprocessor machine and
			 * continue. Otherwise, continue without this interrupt.
			 */
			if (irq_set_affinity(irq, cpumask_of(cpu)) && irqs > 1) {
				pr_warn("unable to set irq affinity (irq=%d, cpu=%u)\n",
					irq, cpu);
				continue;
			}

			err = request_irq(irq, handler,
					  IRQF_NOBALANCING | IRQF_NO_THREAD, "arm-pmu",
					  per_cpu_ptr(&hw_events->percpu_pmu, cpu));
			if (err) {
				pr_err("unable to request IRQ%d for ARM PMU counters\n",
					irq);
				return err;
			}

			cpumask_set_cpu(cpu, &cpu_pmu->active_irqs);
		}
	}

	return 0;
}

/*
 * PMU hardware loses all context when a CPU goes offline.
 * When a CPU is hotplugged back in, since some hardware registers are
 * UNKNOWN at reset, the PMU must be explicitly reset to avoid reading
 * junk values out of them.
 */
static int cpu_pmu_notify(struct notifier_block *b, unsigned long action,
			  void *hcpu)
{
	int cpu = (unsigned long)hcpu;
	struct arm_pmu *pmu = container_of(b, struct arm_pmu, hotplug_nb);

	if ((action & ~CPU_TASKS_FROZEN) != CPU_STARTING)
		return NOTIFY_DONE;

	if (!cpumask_test_cpu(cpu, &pmu->supported_cpus))
		return NOTIFY_DONE;

	if (pmu->reset)
		pmu->reset(pmu);
	else
		return NOTIFY_DONE;

	return NOTIFY_OK;
}

static int cpu_pmu_init(struct arm_pmu *cpu_pmu)
{
	int err;
	int cpu;
	struct pmu_hw_events __percpu *cpu_hw_events;

	cpu_hw_events = alloc_percpu(struct pmu_hw_events);
	if (!cpu_hw_events)
		return -ENOMEM;

	cpu_pmu->hotplug_nb.notifier_call = cpu_pmu_notify;
	err = register_cpu_notifier(&cpu_pmu->hotplug_nb);
	if (err)
		goto out_hw_events;

	for_each_possible_cpu(cpu) {
		struct pmu_hw_events *events = per_cpu_ptr(cpu_hw_events, cpu);
		raw_spin_lock_init(&events->pmu_lock);
		events->percpu_pmu = cpu_pmu;
	}

	cpu_pmu->hw_events	= cpu_hw_events;
	cpu_pmu->request_irq	= cpu_pmu_request_irq;
	cpu_pmu->free_irq	= cpu_pmu_free_irq;

	/* Ensure the PMU has sane values out of reset. */
	if (cpu_pmu->reset)
		on_each_cpu_mask(&cpu_pmu->supported_cpus, cpu_pmu->reset,
			 cpu_pmu, 1);

	/* If no interrupts available, set the corresponding capability flag */
	if (!platform_get_irq(cpu_pmu->plat_device, 0))
		cpu_pmu->pmu.capabilities |= PERF_PMU_CAP_NO_INTERRUPT;

	return 0;

out_hw_events:
	free_percpu(cpu_hw_events);
	return err;
}

static void cpu_pmu_destroy(struct arm_pmu *cpu_pmu)
{
	unregister_cpu_notifier(&cpu_pmu->hotplug_nb);
	free_percpu(cpu_pmu->hw_events);
}

/*
 * PMU platform driver and devicetree bindings.
 */
static const struct of_device_id cpu_pmu_of_device_ids[] = {
	{.compatible = "arm,cortex-a17-pmu",	.data = armv7_a17_pmu_init},
	{.compatible = "arm,cortex-a15-pmu",	.data = armv7_a15_pmu_init},
	{.compatible = "arm,cortex-a12-pmu",	.data = armv7_a12_pmu_init},
	{.compatible = "arm,cortex-a9-pmu",	.data = armv7_a9_pmu_init},
	{.compatible = "arm,cortex-a8-pmu",	.data = armv7_a8_pmu_init},
	{.compatible = "arm,cortex-a7-pmu",	.data = armv7_a7_pmu_init},
	{.compatible = "arm,cortex-a5-pmu",	.data = armv7_a5_pmu_init},
	{.compatible = "qcom,krait-pmu",	.data = krait_pmu_init},
	{.compatible = "qcom,scorpion-pmu",	.data = scorpion_pmu_init},
	{.compatible = "qcom,scorpion-mp-pmu",	.data = scorpion_mp_pmu_init},
	{},
};

static struct platform_device_id cpu_pmu_plat_device_ids[] = {
	{.name = "arm-pmu"},
	{.name = "armv7-pmu"},
	{},
};

static const struct pmu_probe_info pmu_probe_table[] = {
	ARM_PMU_PROBE(ARM_CPU_PART_CORTEX_A8, armv7_a8_pmu_init),
	ARM_PMU_PROBE(ARM_CPU_PART_CORTEX_A9, armv7_a9_pmu_init),
	{ /* sentinel value */ }
};

/*
 * CPU PMU identification and probing.
 */
static int probe_current_pmu(struct arm_pmu *pmu,
			     const struct pmu_probe_info *info)
{
	int cpu = get_cpu();
	unsigned int cpuid = read_cpuid_id();
	int ret = -ENODEV;

	pr_info("probing PMU on CPU %d\n", cpu);

	for (; info->init != NULL; info++) {
		if ((cpuid & info->mask) != info->cpuid)
			continue;
		ret = info->init(pmu);
		break;
	}

	put_cpu();
	return ret;
}

static int of_pmu_irq_cfg(struct arm_pmu *pmu)
{
	int i;
	struct platform_device *pdev = pmu->plat_device;
	int *irqs = kcalloc(pdev->num_resources, sizeof(*irqs), GFP_KERNEL);

	irqs = kcalloc(pdev->num_resources, sizeof(*irqs), GFP_KERNEL);
	if (!irqs)
		return -ENOMEM;

	for (i = 0; i < pdev->num_resources; ++i) {
		struct device_node *dn;
		int cpu;

		dn = of_parse_phandle(pdev->dev.of_node, "interrupt-affinity",
				      i);
		if (!dn) {
			pr_warn("Failed to parse %s/interrupt-affinity[%d]\n",
				of_node_full_name(pdev->dev.of_node), i);
			break;
		}

		for_each_possible_cpu(cpu)
			if (arch_find_n_match_cpu_physical_id(dn, cpu, NULL))
				break;

		of_node_put(dn);
		if (cpu >= nr_cpu_ids) {
			pr_warn("Failed to find logical CPU for %s\n",
				dn->name);
			break;
		}

		irqs[i] = cpu;
		cpumask_set_cpu(cpu, &pmu->supported_cpus);
	}

	if (i == pdev->num_resources) {
		pmu->irq_affinity = irqs;
	} else {
		kfree(irqs);
		cpumask_setall(&pmu->supported_cpus);
	}

	return 0;
}

int arm_pmu_device_probe(struct platform_device *pdev,
			 const struct of_device_id *of_table,
			 const struct pmu_probe_info *probe_table)
{
	const struct of_device_id *of_id;
	const int (*init_fn)(struct arm_pmu *);
	struct device_node *node = pdev->dev.of_node;
	struct arm_pmu *pmu;
	int ret = -ENODEV;

	pmu = kzalloc(sizeof(struct arm_pmu), GFP_KERNEL);
	if (!pmu) {
		pr_info("failed to allocate PMU device!\n");
		return -ENOMEM;
	}

	if (!__oprofile_cpu_pmu)
		__oprofile_cpu_pmu = pmu;

	pmu->plat_device = pdev;

	if (node && (of_id = of_match_node(of_table, pdev->dev.of_node))) {
		init_fn = of_id->data;

		ret = of_pmu_irq_cfg(pmu);
		if (!ret)
			ret = init_fn(pmu);
	} else {
		ret = probe_current_pmu(pmu, probe_table);
		cpumask_setall(&pmu->supported_cpus);
	}

	if (ret) {
		pr_info("failed to probe PMU!\n");
		goto out_free;
	}

	ret = cpu_pmu_init(pmu);
	if (ret)
		goto out_free;

	ret = armpmu_register(pmu, -1);
	if (ret)
		goto out_destroy;

	return 0;

out_destroy:
	cpu_pmu_destroy(pmu);
out_free:
	pr_info("failed to register PMU devices!\n");
	kfree(pmu);
	return ret;
}

static int cpu_pmu_device_probe(struct platform_device *pdev)
{
	return arm_pmu_device_probe(pdev, cpu_pmu_of_device_ids,
				    pmu_probe_table);
}

static struct platform_driver cpu_pmu_driver = {
	.driver		= {
		.name	= "arm-pmu",
		.of_match_table = cpu_pmu_of_device_ids,
	},
	.probe		= cpu_pmu_device_probe,
	.id_table	= cpu_pmu_plat_device_ids,
};

static int __init register_pmu_driver(void)
{
	return platform_driver_register(&cpu_pmu_driver);
}
device_initcall(register_pmu_driver);
