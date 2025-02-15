/*
 * drivers/staging/android/fiq_debugger.c
 *
 * Serial Debugger Interface accessed through an FIQ interrupt.
 *
 * Copyright (C) 2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdarg.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/console.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/kernel_stat.h>
#include <linux/kmsg_dump.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/sysrq.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/wakelock.h>

#ifdef CONFIG_FIQ_GLUE
#include <asm/fiq_glue.h>
#endif

#include <linux/uaccess.h>

#include "fiq_debugger.h"
#include "fiq_debugger_priv.h"
#include "fiq_debugger_ringbuf.h"

#define DEBUG_MAX 64
#define MAX_UNHANDLED_FIQ_COUNT 1000000

#define MAX_FIQ_DEBUGGER_PORTS 4

struct fiq_debugger_state {
#ifdef CONFIG_FIQ_GLUE
	struct fiq_glue_handler handler;
#endif
	struct fiq_debugger_output output;

	int fiq;
	int uart_irq;
	int signal_irq;
	int wakeup_irq;
	bool wakeup_irq_no_set_wake;
	struct clk *clk;
	struct fiq_debugger_pdata *pdata;
	struct platform_device *pdev;

	char debug_cmd[DEBUG_MAX];
	int debug_busy;
	int debug_abort;

	char debug_buf[DEBUG_MAX];
	int debug_count;

	bool no_sleep;
	bool debug_enable;
	bool ignore_next_wakeup_irq;
	struct timer_list sleep_timer;
	spinlock_t sleep_timer_lock;
	bool uart_enabled;
	struct wake_lock debugger_wake_lock;
	bool console_enable;
	int current_cpu;
	atomic_t unhandled_fiq_count;
	bool in_fiq;

	struct work_struct work;
	spinlock_t work_lock;
	char work_cmd[DEBUG_MAX];

#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE
	spinlock_t console_lock;
	struct console console;
	struct tty_port tty_port;
	struct fiq_debugger_ringbuf *tty_rbuf;
	bool syslog_dumping;
#endif

	unsigned int last_irqs[NR_IRQS];
	unsigned int last_local_timer_irqs[NR_CPUS];
};

#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE
struct tty_driver *fiq_tty_driver;
#endif

#ifdef CONFIG_FIQ_DEBUGGER_NO_SLEEP
static bool initial_no_sleep = true;
#else
static bool initial_no_sleep;
#endif

#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE_DEFAULT_ENABLE
static bool initial_debug_enable = true;
static bool initial_console_enable = true;
#else
static bool initial_debug_enable;
static bool initial_console_enable;
#endif

static bool fiq_kgdb_enable;

module_param_named(no_sleep, initial_no_sleep, bool, 0644);
module_param_named(debug_enable, initial_debug_enable, bool, 0644);
module_param_named(console_enable, initial_console_enable, bool, 0644);
module_param_named(kgdb_enable, fiq_kgdb_enable, bool, 0644);

#ifdef CONFIG_FIQ_DEBUGGER_WAKEUP_IRQ_ALWAYS_ON
static inline
void fiq_debugger_enable_wakeup_irq(struct fiq_debugger_state *state) {}
static inline
void fiq_debugger_disable_wakeup_irq(struct fiq_debugger_state *state) {}
#else
static inline
void fiq_debugger_enable_wakeup_irq(struct fiq_debugger_state *state)
{
	if (state->wakeup_irq < 0)
		return;
	enable_irq(state->wakeup_irq);
	if (!state->wakeup_irq_no_set_wake)
		enable_irq_wake(state->wakeup_irq);
}
static inline
void fiq_debugger_disable_wakeup_irq(struct fiq_debugger_state *state)
{
	if (state->wakeup_irq < 0)
		return;
	disable_irq_nosync(state->wakeup_irq);
	if (!state->wakeup_irq_no_set_wake)
		disable_irq_wake(state->wakeup_irq);
}
#endif

static inline bool fiq_debugger_have_fiq(struct fiq_debugger_state *state)
{
	return (state->fiq >= 0);
}

#ifdef CONFIG_FIQ_GLUE
static void fiq_debugger_force_irq(struct fiq_debugger_state *state)
{
	unsigned int irq = state->signal_irq;

	if (WARN_ON(!fiq_debugger_have_fiq(state)))
		return;
	if (state->pdata->force_irq) {
		state->pdata->force_irq(state->pdev, irq);
	} else {
		struct irq_chip *chip = irq_get_chip(irq);
		if (chip && chip->irq_retrigger)
			chip->irq_retrigger(irq_get_irq_data(irq));
	}
}
#endif

static void fiq_debugger_uart_enable(struct fiq_debugger_state *state)
{
	if (state->clk)
		clk_enable(state->clk);
	if (state->pdata->uart_enable)
		state->pdata->uart_enable(state->pdev);
}

static void fiq_debugger_uart_disable(struct fiq_debugger_state *state)
{
	if (state->pdata->uart_disable)
		state->pdata->uart_disable(state->pdev);
	if (state->clk)
		clk_disable(state->clk);
}

static void fiq_debugger_uart_flush(struct fiq_debugger_state *state)
{
	if (state->pdata->uart_flush)
		state->pdata->uart_flush(state->pdev);
}

static void fiq_debugger_putc(struct fiq_debugger_state *state, char c)
{
	state->pdata->uart_putc(state->pdev, c);
}

static void fiq_debugger_puts(struct fiq_debugger_state *state, char *s)
{
	unsigned c;
	while ((c = *s++)) {
		if (c == '\n')
			fiq_debugger_putc(state, '\r');
		fiq_debugger_putc(state, c);
	}
}

static void fiq_debugger_prompt(struct fiq_debugger_state *state)
{
	fiq_debugger_puts(state, "debug> ");
}

static void fiq_debugger_dump_kernel_log(struct fiq_debugger_state *state)
{
	char buf[512];
	size_t len;
	struct kmsg_dumper dumper = { .active = true };


	kmsg_dump_rewind_nolock(&dumper);
	while (kmsg_dump_get_line_nolock(&dumper, true, buf,
					 sizeof(buf) - 1, &len)) {
		buf[len] = 0;
		fiq_debugger_puts(state, buf);
	}
}

static void fiq_debugger_printf(struct fiq_debugger_output *output,
			       const char *fmt, ...)
{
	struct fiq_debugger_state *state;
	char buf[256];
	va_list ap;

	state = container_of(output, struct fiq_debugger_state, output);
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fiq_debugger_puts(state, buf);
}

/* Safe outside fiq context */
static int fiq_debugger_printf_nfiq(void *cookie, const char *fmt, ...)
{
	struct fiq_debugger_state *state = cookie;
	char buf[256];
	va_list ap;
	unsigned long irq_flags;

	va_start(ap, fmt);
	vsnprintf(buf, 128, fmt, ap);
	va_end(ap);

	local_irq_save(irq_flags);
	fiq_debugger_puts(state, buf);
	fiq_debugger_uart_flush(state);
	local_irq_restore(irq_flags);
	return state->debug_abort;
}

static void fiq_debugger_dump_irqs(struct fiq_debugger_state *state)
{
	int n;
	struct irq_desc *desc;

	fiq_debugger_printf(&state->output,
			"irqnr       total  since-last   status  name\n");
	for_each_irq_desc(n, desc) {
		struct irqaction *act = desc->action;
		if (!act && !kstat_irqs(n))
			continue;
		fiq_debugger_printf(&state->output, "%5d: %10u %11u %8x  %s\n", n,
			kstat_irqs(n),
			kstat_irqs(n) - state->last_irqs[n],
			desc->status_use_accessors,
			(act && act->name) ? act->name : "???");
		state->last_irqs[n] = kstat_irqs(n);
	}
}

static void fiq_debugger_do_ps(struct fiq_debugger_state *state)
{
	struct task_struct *g;
	struct task_struct *p;
	unsigned task_state;
	static const char stat_nam[] = "RSDTtZX";

	fiq_debugger_printf(&state->output, "pid   ppid  prio task            pc\n");
	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		task_state = p->state ? __ffs(p->state) + 1 : 0;
		fiq_debugger_printf(&state->output,
			     "%5d %5d %4d ", p->pid, p->parent->pid, p->prio);
		fiq_debugger_printf(&state->output, "%-13.13s %c", p->comm,
			     task_state >= sizeof(stat_nam) ? '?' : stat_nam[task_state]);
		if (task_state == TASK_RUNNING)
			fiq_debugger_printf(&state->output, " running\n");
		else
			fiq_debugger_printf(&state->output, " %08lx\n",
					thread_saved_pc(p));
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);
}

#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE
static void fiq_debugger_begin_syslog_dump(struct fiq_debugger_state *state)
{
	state->syslog_dumping = true;
}

static void fiq_debugger_end_syslog_dump(struct fiq_debugger_state *state)
{
	state->syslog_dumping = false;
}
#else
extern int do_syslog(int type, char __user *bug, int count);
static void fiq_debugger_begin_syslog_dump(struct fiq_debugger_state *state)
{
	do_syslog(5 /* clear */, NULL, 0);
}

static void fiq_debugger_end_syslog_dump(struct fiq_debugger_state *state)
{
	fiq_debugger_dump_kernel_log(state);
}
#endif

static void fiq_debugger_do_sysrq(struct fiq_debugger_state *state, char rq)
{
	if ((rq == 'g' || rq == 'G') && !fiq_kgdb_enable) {
		fiq_debugger_printf(&state->output, "sysrq-g blocked\n");
		return;
	}
	fiq_debugger_begin_syslog_dump(state);
	handle_sysrq(rq);
	fiq_debugger_end_syslog_dump(state);
}

#ifdef CONFIG_KGDB
static void fiq_debugger_do_kgdb(struct fiq_debugger_state *state)
{
	if (!fiq_kgdb_enable) {
		fiq_debugger_printf(&state->output, "kgdb through fiq debugger not enabled\n");
		return;
	}

	fiq_debugger_printf(&state->output, "enabling console and triggering kgdb\n");
	state->console_enable = true;
	handle_sysrq('g');
}
#endif

static void fiq_debugger_schedule_work(struct fiq_debugger_state *state,
		char *cmd)
{
	unsigned long flags;

	spin_lock_irqsave(&state->work_lock, flags);
	if (state->work_cmd[0] != '\0') {
		fiq_debugger_printf(&state->output, "work command processor busy\n");
		spin_unlock_irqrestore(&state->work_lock, flags);
		return;
	}

	strlcpy(state->work_cmd, cmd, sizeof(state->work_cmd));
	spin_unlock_irqrestore(&state->work_lock, flags);

	schedule_work(&state->work);
}

static void fiq_debugger_work(struct work_struct *work)
{
	struct fiq_debugger_state *state;
	char work_cmd[DEBUG_MAX];
	char *cmd;
	unsigned long flags;

	state = container_of(work, struct fiq_debugger_state, work);

	spin_lock_irqsave(&state->work_lock, flags);

	strlcpy(work_cmd, state->work_cmd, sizeof(work_cmd));
	state->work_cmd[0] = '\0';

	spin_unlock_irqrestore(&state->work_lock, flags);

	cmd = work_cmd;
	if (!strncmp(cmd, "reboot", 6)) {
		cmd += 6;
		while (*cmd == ' ')
			cmd++;
		if ((*cmd != '\0') && sysrq_on())
			kernel_restart(cmd);
		else
			kernel_restart(NULL);
	} else {
		fiq_debugger_printf(&state->output, "unknown work command '%s'\n",
				work_cmd);
	}
}

/* This function CANNOT be called in FIQ context */
static void fiq_debugger_irq_exec(struct fiq_debugger_state *state, char *cmd)
{
	if (!strcmp(cmd, "ps"))
		fiq_debugger_do_ps(state);
	if (!strcmp(cmd, "sysrq"))
		fiq_debugger_do_sysrq(state, 'h');
	if (!strncmp(cmd, "sysrq ", 6))
		fiq_debugger_do_sysrq(state, cmd[6]);
#ifdef CONFIG_KGDB
	if (!strcmp(cmd, "kgdb"))
		fiq_debugger_do_kgdb(state);
#endif
	if (!strncmp(cmd, "reboot", 6))
		fiq_debugger_schedule_work(state, cmd);
}

static void fiq_debugger_help(struct fiq_debugger_state *state)
{
	fiq_debugger_printf(&state->output,
		"FIQ Debugger commands:\n");
	if (sysrq_on()) {
		fiq_debugger_printf(&state->output,
			" pc            PC status\n"
			" regs          Register dump\n"
			" allregs       Extended Register dump\n"
			" bt            Stack trace\n");
		fiq_debugger_printf(&state->output,
			" reboot [<c>]  Reboot with command <c>\n"
			" reset [<c>]   Hard reset with command <c>\n"
			" irqs          Interrupt status\n"
			" kmsg          Kernel log\n"
			" version       Kernel version\n");
		fiq_debugger_printf(&state->output,
			" cpu           Current CPU\n"
			" cpu <number>  Switch to CPU<number>\n"
			" sysrq         sysrq options\n"
			" sysrq <param> Execute sysrq with <param>\n");
	} else {
		fiq_debugger_printf(&state->output,
			" reboot        Reboot\n"
			" reset         Hard reset\n"
			" irqs          Interrupt status\n");
	}
	fiq_debugger_printf(&state->output,
			" sleep         Allow sleep while in FIQ\n"
			" nosleep       Disable sleep while in FIQ\n"
			" console       Switch terminal to console\n"
			" ps            Process list\n");
#ifdef CONFIG_KGDB
	if (fiq_kgdb_enable)
		fiq_debugger_printf(&state->output,
			" kgdb          Enter kernel debugger\n");
#endif
}

static void fiq_debugger_take_affinity(void *info)
{
	struct fiq_debugger_state *state = info;
	struct cpumask cpumask;

	cpumask_clear(&cpumask);
	cpumask_set_cpu(get_cpu(), &cpumask);

	irq_set_affinity(state->uart_irq, &cpumask);
}

static void fiq_debugger_switch_cpu(struct fiq_debugger_state *state, int cpu)
{
	if (!fiq_debugger_have_fiq(state))
		smp_call_function_single(cpu, fiq_debugger_take_affinity, state,
				false);
	state->current_cpu = cpu;
}

static bool fiq_debugger_fiq_exec(struct fiq_debugger_state *state,
			const char *cmd, const struct pt_regs *regs,
			void *svc_sp)
{
	bool signal_helper = false;

	if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
		fiq_debugger_help(state);
	} else if (!strcmp(cmd, "pc")) {
		if (sysrq_on())
			fiq_debugger_dump_pc(&state->output, regs);
	} else if (!strcmp(cmd, "regs")) {
		if (sysrq_on())
			fiq_debugger_dump_regs(&state->output, regs);
	} else if (!strcmp(cmd, "allregs")) {
		if (sysrq_on())
			fiq_debugger_dump_allregs(&state->output, regs);
	} else if (!strcmp(cmd, "bt")) {
		if (sysrq_on())
			fiq_debugger_dump_stacktrace(&state->output, regs,
							100, svc_sp);
	} else if (!strncmp(cmd, "reset", 5)) {
		cmd += 5;
		while (*cmd == ' ')
			cmd++;
		if (*cmd && sysrq_on()) {
			char tmp_cmd[32];
			strlcpy(tmp_cmd, cmd, sizeof(tmp_cmd));
			machine_restart(tmp_cmd);
		} else {
			machine_restart(NULL);
		}
	} else if (!strcmp(cmd, "irqs")) {
		fiq_debugger_dump_irqs(state);
	} else if (!strcmp(cmd, "kmsg")) {
		if (sysrq_on())
			fiq_debugger_dump_kernel_log(state);
	} else if (!strcmp(cmd, "version")) {
		if (sysrq_on())
			fiq_debugger_printf(&state->output, "%s\n",
						linux_banner);
	} else if (!strcmp(cmd, "sleep")) {
		state->no_sleep = false;
		fiq_debugger_printf(&state->output, "enabling sleep\n");
	} else if (!strcmp(cmd, "nosleep")) {
		state->no_sleep = true;
		fiq_debugger_printf(&state->output, "disabling sleep\n");
	} else if (!strcmp(cmd, "console")) {
		fiq_debugger_printf(&state->output, "console mode\n");
		fiq_debugger_uart_flush(state);
		state->console_enable = true;
	} else if (!strcmp(cmd, "cpu")) {
		if (sysrq_on())
			fiq_debugger_printf(&state->output, "cpu %d\n",state->current_cpu);
	} else if (!strncmp(cmd, "cpu ", 4) && sysrq_on()) {
		unsigned long cpu = 0;
		if (kstrtoul(cmd + 4, 10, &cpu) == 0)
			fiq_debugger_switch_cpu(state, cpu);
		else
			fiq_debugger_printf(&state->output, "invalid cpu\n");
		fiq_debugger_printf(&state->output, "cpu %d\n",
					state->current_cpu);
	} else {
		if (state->debug_busy) {
			fiq_debugger_printf(&state->output,
				"command processor busy. trying to abort.\n");
			state->debug_abort = -1;
		} else {
			strcpy(state->debug_cmd, cmd);
			state->debug_busy = 1;
		}

		return true;
	}
	if (!state->console_enable)
		fiq_debugger_prompt(state);

	return signal_helper;
}

static void fiq_debugger_sleep_timer_expired(unsigned long data)
{
	struct fiq_debugger_state *state = (struct fiq_debugger_state *)data;
	unsigned long flags;

	spin_lock_irqsave(&state->sleep_timer_lock, flags);
	if (state->uart_enabled && !state->no_sleep) {
		if (state->debug_enable && !state->console_enable) {
			state->debug_enable = false;
			fiq_debugger_printf_nfiq(state,
					"suspending fiq debugger\n");
		}
		state->ignore_next_wakeup_irq = true;
		fiq_debugger_uart_disable(state);
		state->uart_enabled = false;
		fiq_debugger_enable_wakeup_irq(state);
	}
	wake_unlock(&state->debugger_wake_lock);
	spin_unlock_irqrestore(&state->sleep_timer_lock, flags);
}

static void fiq_debugger_handle_wakeup(struct fiq_debugger_state *state)
{
	unsigned long flags;

	spin_lock_irqsave(&state->sleep_timer_lock, flags);
	if (state->wakeup_irq >= 0 && state->ignore_next_wakeup_irq) {
		state->ignore_next_wakeup_irq = false;
	} else if (!state->uart_enabled) {
		wake_lock(&state->debugger_wake_lock);
		fiq_debugger_uart_enable(state);
		state->uart_enabled = true;
		fiq_debugger_disable_wakeup_irq(state);
		mod_timer(&state->sleep_timer, jiffies + HZ / 2);
	}
	spin_unlock_irqrestore(&state->sleep_timer_lock, flags);
}

static irqreturn_t fiq_debugger_wakeup_irq_handler(int irq, void *dev)
{
	struct fiq_debugger_state *state = dev;

	if (!state->no_sleep)
		fiq_debugger_puts(state, "WAKEUP\n");
	fiq_debugger_handle_wakeup(state);

	return IRQ_HANDLED;
}

static
void fiq_debugger_handle_console_irq_context(struct fiq_debugger_state *state)
{
#if defined(CONFIG_FIQ_DEBUGGER_CONSOLE)
	if (state->tty_port.ops) {
		int i;
		int count = fiq_debugger_ringbuf_level(state->tty_rbuf);
		for (i = 0; i < count; i++) {
			int c = fiq_debugger_ringbuf_peek(state->tty_rbuf, 0);
			tty_insert_flip_char(&state->tty_port, c, TTY_NORMAL);
			if (!fiq_debugger_ringbuf_consume(state->tty_rbuf, 1))
				pr_warn("fiq tty failed to consume byte\n");
		}
		tty_flip_buffer_push(&state->tty_port);
	}
#endif
}

static void fiq_debugger_handle_irq_context(struct fiq_debugger_state *state)
{
	if (!state->no_sleep) {
		unsigned long flags;

		spin_lock_irqsave(&state->sleep_timer_lock, flags);
		wake_lock(&state->debugger_wake_lock);
		mod_timer(&state->sleep_timer, jiffies + HZ * 5);
		spin_unlock_irqrestore(&state->sleep_timer_lock, flags);
	}
	fiq_debugger_handle_console_irq_context(state);
	if (state->debug_busy) {
		fiq_debugger_irq_exec(state, state->debug_cmd);
		if (!state->console_enable)
			fiq_debugger_prompt(state);
		state->debug_busy = 0;
	}
}

static int fiq_debugger_getc(struct fiq_debugger_state *state)
{
	return state->pdata->uart_getc(state->pdev);
}

static bool fiq_debugger_handle_uart_interrupt(struct fiq_debugger_state *state,
			int this_cpu, const struct pt_regs *regs, void *svc_sp)
{
	int c;
	static int last_c;
	int count = 0;
	bool signal_helper = false;

	if (this_cpu != state->current_cpu) {
		if (state->in_fiq)
			return false;

		if (atomic_inc_return(&state->unhandled_fiq_count) !=
					MAX_UNHANDLED_FIQ_COUNT)
			return false;

		fiq_debugger_printf(&state->output,
			"fiq_debugger: cpu %d not responding, "
			"reverting to cpu %d\n", state->current_cpu,
			this_cpu);

		atomic_set(&state->unhandled_fiq_count, 0);
		fiq_debugger_switch_cpu(state, this_cpu);
		return false;
	}

	state->in_fiq = true;

	while ((c = fiq_debugger_getc(state)) != FIQ_DEBUGGER_NO_CHAR) {
		count++;
		if (!state->debug_enable) {
			if ((c == 13) || (c == 10)) {
				state->debug_enable = true;
				state->debug_count = 0;
				fiq_debugger_prompt(state);
			}
		} else if (c == FIQ_DEBUGGER_BREAK) {
			state->console_enable = false;
			fiq_debugger_puts(state, "fiq debugger mode\n");
			state->debug_count = 0;
			fiq_debugger_prompt(state);
#ifdef CONFIG_FIQ_DEBUGGER_CONSOLE
		} else if (state->console_enable && state->tty_rbuf) {
			fiq_debugger_ringbuf_push(state->tty_rbuf, c);
			signal_helper = true;
#endif
		} else if ((c >= ' ') && (c < 127)) {
			if (state->debug_count < (DEBUG_MAX - 1)) {
				state->debug_buf[state->debug_count++] = c;
				fiq_debugger_putc(state, c);
			}
		} else if ((c == 8) || (c == 127)) {
			if (state->debug_count > 0) {
				state->debug_count--;
				fiq_debugger_putc(state, 8);
				fiq_debugger_putc(state, ' ');
				fiq_debugger_putc(state, 8);
			}
		} else if ((c == 13) || (c == 10)) {
			if (c == '\r' || (c == '\n' && last_c != '\r')) {
				fiq_debugger_putc(state, '\r');
				fiq_debugger_putc(state, '\n');
			}
			if (state->debug_count) {
				state->debug_buf[state->debug_count] = 0;
				state->debug_count = 0;
				signal_helper |=
					fiq_debugger_fiq_exec(state,
							state->debug_buf,
							regs, svc_sp);
			} else {
				fiq_debugger_prompt(state);
			}
		}
		last_c = c;
	}
	if (!state->console_enable)
		fiq_debugger_uart_flush(state);
	if (state->pdata->fiq_ack)
		state->pdata->fiq_ack(state->pdev, state->fiq);

	/* poke sleep timer if necessary */
	if (state->debug_enable && !state->no_sleep)
		signal_helper = true;

	atomic_set(&state->unhandled_fiq_count, 0);
	state->in_fiq = false;

	return signal_helper;
}

#ifdef CONFIG_FIQ_GLUE
static void fiq_debugger_fiq(struct fiq_glue_handler *h,
		const struct pt_regs *regs, void *svc_sp)
{
	struct fiq_debugger_state *state =
		container_of(h, struct fiq_debugger_state, handler);
	unsigned int this_cpu = THREAD_INFO(svc_sp)->cpu;
	bool need_irq;

	need_irq = fiq_debugger_handle_uart_interrupt(state, this_cpu, regs,
			svc_sp);
	if (need_irq)
		fiq_debugger_force_irq(state);
}
#endif

/*
 * When not using FIQs, we only use this single interrupt as an entry point.
 * This just effectively takes over the UART interrupt and does all the work
 * in this context.
 */
static irqreturn_t fiq_debugger_uart_irq(int irq, void *dev)
{
	struct fiq_debugger_state *state = dev;
	bool not_done;

	fiq_debugger_handle_wakeup(state);

	/* handle the debugger irq in regular context */
	not_done = fiq_debugger_handle_uart_interrupt(state, smp_processor_id(),
					      get_irq_regs(),
					      current_thread_info());
	if (not_done)
		fiq_debugger_handle_irq_context(state);

	return IRQ_HANDLED;
}

/*
 * If FIQs are used, not everything can happen in fiq context.
 * FIQ handler does what it can and then signals this interrupt to finish the
 * job in irq context.
 */
static irqreturn_t fiq_debugger_signal_irq(int irq, void *dev)
{
	struct fiq_debugger_state *state = dev;

	if (state->pdata->force_irq_ack)
		state->pdata->force_irq_ack(state->pdev, state->signal_irq);

	fiq_debugger_handle_irq_context(state);

	return IRQ_HANDLED;
}

#ifdef CONFIG_FIQ_GLUE
static void fiq_debugger_resume(struct fiq_glue_handler *h)
{
	struct fiq_debugger_state *state =
		container_of(h, struct fiq_debugger_state, handler);
	if (state->pdata->uart_resume)
		state->pdata->uart_resume(state->pdev);
}
#endif

#if defined(CONFIG_FIQ_DEBUGGER_CONSOLE)
struct tty_driver *fiq_debugger_console_device(struct console *co, int *index)
{
	*index = co->index;
	return fiq_tty_driver;
}

static void fiq_debugger_console_write(struct console *co,
				const char *s, unsigned int count)
{
	struct fiq_debugger_state *state;
	unsigned long flags;

	state = container_of(co, struct fiq_debugger_state, console);

	if (!state->console_enable && !state->syslog_dumping)
		return;

	fiq_debugger_uart_enable(state);
	spin_lock_irqsave(&state->console_lock, flags);
	while (count--) {
		if (*s == '\n')
			fiq_debugger_putc(state, '\r');
		fiq_debugger_putc(state, *s++);
	}
	fiq_debugger_uart_flush(state);
	spin_unlock_irqrestore(&state->console_lock, flags);
	fiq_debugger_uart_disable(state);
}

static struct console fiq_debugger_console = {
	.name = "ttyFIQ",
	.device = fiq_debugger_console_device,
	.write = fiq_debugger_console_write,
	.flags = CON_PRINTBUFFER | CON_ANYTIME | CON_ENABLED,
};

int fiq_tty_open(struct tty_struct *tty, struct file *filp)
{
	int line = tty->index;
	struct fiq_debugger_state **states = tty->driver->driver_state;
	struct fiq_debugger_state *state = states[line];

	return tty_port_open(&state->tty_port, tty, filp);
}

void fiq_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

int  fiq_tty_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	int i;
	int line = tty->index;
	struct fiq_debugger_state **states = tty->driver->driver_state;
	struct fiq_debugger_state *state = states[line];

	if (!state->console_enable)
		return count;

	fiq_debugger_uart_enable(state);
	spin_lock_irq(&state->console_lock);
	for (i = 0; i < count; i++)
		fiq_debugger_putc(state, *buf++);
	spin_unlock_irq(&state->console_lock);
	fiq_debugger_uart_disable(state);

	return count;
}

int  fiq_tty_write_room(struct tty_struct *tty)
{
	return 16;
}

#ifdef CONFIG_CONSOLE_POLL
static int fiq_tty_poll_init(struct tty_driver *driver, int line, char *options)
{
	return 0;
}

static int fiq_tty_poll_get_char(struct tty_driver *driver, int line)
{
	struct fiq_debugger_state **states = driver->driver_state;
	struct fiq_debugger_state *state = states[line];
	int c = NO_POLL_CHAR;

	fiq_debugger_uart_enable(state);
	if (fiq_debugger_have_fiq(state)) {
		int count = fiq_debugger_ringbuf_level(state->tty_rbuf);
		if (count > 0) {
			c = fiq_debugger_ringbuf_peek(state->tty_rbuf, 0);
			fiq_debugger_ringbuf_consume(state->tty_rbuf, 1);
		}
	} else {
		c = fiq_debugger_getc(state);
		if (c == FIQ_DEBUGGER_NO_CHAR)
			c = NO_POLL_CHAR;
	}
	fiq_debugger_uart_disable(state);

	return c;
}

static void fiq_tty_poll_put_char(struct tty_driver *driver, int line, char ch)
{
	struct fiq_debugger_state **states = driver->driver_state;
	struct fiq_debugger_state *state = states[line];
	fiq_debugger_uart_enable(state);
	fiq_debugger_putc(state, ch);
	fiq_debugger_uart_disable(state);
}
#endif

static const struct tty_port_operations fiq_tty_port_ops;

static const struct tty_operations fiq_tty_driver_ops = {
	.write = fiq_tty_write,
	.write_room = fiq_tty_write_room,
	.open = fiq_tty_open,
	.close = fiq_tty_close,
#ifdef CONFIG_CONSOLE_POLL
	.poll_init = fiq_tty_poll_init,
	.poll_get_char = fiq_tty_poll_get_char,
	.poll_put_char = fiq_tty_poll_put_char,
#endif
};

static int fiq_debugger_tty_init(void)
{
	int ret;
	struct fiq_debugger_state **states = NULL;

	states = kzalloc(sizeof(*states) * MAX_FIQ_DEBUGGER_PORTS, GFP_KERNEL);
	if (!states) {
		pr_err("Failed to allocate fiq debugger state structres\n");
		return -ENOMEM;
	}

	fiq_tty_driver = alloc_tty_driver(MAX_FIQ_DEBUGGER_PORTS);
	if (!fiq_tty_driver) {
		pr_err("Failed to allocate fiq debugger tty\n");
		ret = -ENOMEM;
		goto err_free_state;
	}

	fiq_tty_driver->owner		= THIS_MODULE;
	fiq_tty_driver->driver_name	= "fiq-debugger";
	fiq_tty_driver->name		= "ttyFIQ";
	fiq_tty_driver->type		= TTY_DRIVER_TYPE_SERIAL;
	fiq_tty_driver->subtype		= SERIAL_TYPE_NORMAL;
	fiq_tty_driver->init_termios	= tty_std_termios;
	fiq_tty_driver->flags		= TTY_DRIVER_REAL_RAW |
					  TTY_DRIVER_DYNAMIC_DEV;
	fiq_tty_driver->driver_state	= states;

	fiq_tty_driver->init_termios.c_cflag =
					B115200 | CS8 | CREAD | HUPCL | CLOCAL;
	fiq_tty_driver->init_termios.c_ispeed = 115200;
	fiq_tty_driver->init_termios.c_ospeed = 115200;

	tty_set_operations(fiq_tty_driver, &fiq_tty_driver_ops);

	ret = tty_register_driver(fiq_tty_driver);
	if (ret) {
		pr_err("Failed to register fiq tty: %d\n", ret);
		goto err_free_tty;
	}

	pr_info("Registered FIQ tty driver\n");
	return 0;

err_free_tty:
	put_tty_driver(fiq_tty_driver);
	fiq_tty_driver = NULL;
err_free_state:
	kfree(states);
	return ret;
}

static int fiq_debugger_tty_init_one(struct fiq_debugger_state *state)
{
	int ret;
	struct device *tty_dev;
	struct fiq_debugger_state **states = fiq_tty_driver->driver_state;

	states[state->pdev->id] = state;

	state->tty_rbuf = fiq_debugger_ringbuf_alloc(1024);
	if (!state->tty_rbuf) {
		pr_err("Failed to allocate fiq debugger ringbuf\n");
		ret = -ENOMEM;
		goto err;
	}

	tty_port_init(&state->tty_port);
	state->tty_port.ops = &fiq_tty_port_ops;

	tty_dev = tty_port_register_device(&state->tty_port, fiq_tty_driver,
					   state->pdev->id, &state->pdev->dev);
	if (IS_ERR(tty_dev)) {
		pr_err("Failed to register fiq debugger tty device\n");
		ret = PTR_ERR(tty_dev);
		goto err;
	}

	device_set_wakeup_capable(tty_dev, 1);

	pr_info("Registered fiq debugger ttyFIQ%d\n", state->pdev->id);

	return 0;

err:
	fiq_debugger_ringbuf_free(state->tty_rbuf);
	state->tty_rbuf = NULL;
	return ret;
}
#endif

static int fiq_debugger_dev_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct fiq_debugger_state *state = platform_get_drvdata(pdev);

	if (state->pdata->uart_dev_suspend)
		return state->pdata->uart_dev_suspend(pdev);
	return 0;
}

static int fiq_debugger_dev_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct fiq_debugger_state *state = platform_get_drvdata(pdev);

	if (state->pdata->uart_dev_resume)
		return state->pdata->uart_dev_resume(pdev);
	return 0;
}

static int fiq_debugger_probe(struct platform_device *pdev)
{
	int ret;
	struct fiq_debugger_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct fiq_debugger_state *state;
	int fiq;
	int uart_irq;

	if (pdev->id >= MAX_FIQ_DEBUGGER_PORTS)
		return -EINVAL;

	if (!pdata->uart_getc || !pdata->uart_putc)
		return -EINVAL;
	if ((pdata->uart_enable && !pdata->uart_disable) ||
	    (!pdata->uart_enable && pdata->uart_disable))
		return -EINVAL;

	fiq = platform_get_irq_byname(pdev, "fiq");
	uart_irq = platform_get_irq_byname(pdev, "uart_irq");

	/* uart_irq mode and fiq mode are mutually exclusive, but one of them
	 * is required */
	if ((uart_irq < 0 && fiq < 0) || (uart_irq >= 0 && fiq >= 0))
		return -EINVAL;
	if (fiq >= 0 && !pdata->fiq_enable)
		return -EINVAL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	state->output.printf = fiq_debugger_printf;
	setup_timer(&state->sleep_timer, fiq_debugger_sleep_timer_expired,
		    (unsigned long)state);
	state->pdata = pdata;
	state->pdev = pdev;
	state->no_sleep = initial_no_sleep;
	state->debug_enable = initial_debug_enable;
	state->console_enable = initial_console_enable;

	state->fiq = fiq;
	state->uart_irq = uart_irq;
	state->signal_irq = platform_get_irq_byname(pdev, "signal");
	state->wakeup_irq = platform_get_irq_byname(pdev, "wakeup");

	INIT_WORK(&state->work, fiq_debugger_work);
	spin_lock_init(&state->work_lock);

	platform_set_drvdata(pdev, state);

	spin_lock_init(&state->sleep_timer_lock);

	if (state->wakeup_irq < 0 && fiq_debugger_have_fiq(state))
		state->no_sleep = true;
	state->ignore_next_wakeup_irq = !state->no_sleep;

	wake_lock_init(&state->debugger_wake_lock,
			WAKE_LOCK_SUSPEND, "serial-debug");

	state->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(state->clk))
		state->clk = NULL;

	/* do not call pdata->uart_enable here since uart_init may still
	 * need to do some initialization before uart_enable can work.
	 * So, only try to manage the clock during init.
	 */
	if (state->clk)
		clk_enable(state->clk);

	if (pdata->uart_init) {
		ret = pdata->uart_init(pdev);
		if (ret)
			goto err_uart_init;
	}

	fiq_debugger_printf_nfiq(state,
				"<hit enter %sto activate fiq debugger>\n",
				state->no_sleep ? "" : "twice ");

#ifdef CONFIG_FIQ_GLUE
	if (fiq_debugger_have_fiq(state)) {
		state->handler.fiq = fiq_debugger_fiq;
		state->handler.resume = fiq_debugger_resume;
		ret = fiq_glue_register_handler(&state->handler);
		if (ret) {
			pr_err("%s: could not install fiq handler\n", __func__);
			goto err_register_irq;
		}

		pdata->fiq_enable(pdev, state->fiq, 1);
	} else
#endif
	{
		ret = request_irq(state->uart_irq, fiq_debugger_uart_irq,
				  IRQF_NO_SUSPEND, "debug", state);
		if (ret) {
			pr_err("%s: could not install irq handler\n", __func__);
			goto err_register_irq;
		}

		/* for irq-only mode, we want this irq to wake us up, if it
		 * can.
		 */
		enable_irq_wake(state->uart_irq);
	}

	if (state->clk)
		clk_disable(state->clk);

	if (state->signal_irq >= 0) {
		ret = request_irq(state->signal_irq, fiq_debugger_signal_irq,
			  IRQF_TRIGGER_RISING, "debug-signal", state);
		if (ret)
			pr_err("serial_debugger: could not install signal_irq");
	}

	if (state->wakeup_irq >= 0) {
		ret = request_irq(state->wakeup_irq,
				  fiq_debugger_wakeup_irq_handler,
				  IRQF_TRIGGER_FALLING,
				  "debug-wakeup", state);
		if (ret) {
			pr_err("serial_debugger: "
				"could not install wakeup irq\n");
			state->wakeup_irq = -1;
		} else {
			ret = enable_irq_wake(state->wakeup_irq);
			if (ret) {
				pr_err("serial_debugger: "
					"could not enable wakeup\n");
				state->wakeup_irq_no_set_wake = true;
			}
		}
	}
	if (state->no_sleep)
		fiq_debugger_handle_wakeup(state);

#if defined(CONFIG_FIQ_DEBUGGER_CONSOLE)
	spin_lock_init(&state->console_lock);
	state->console = fiq_debugger_console;
	state->console.index = pdev->id;
	if (!console_set_on_cmdline)
		add_preferred_console(state->console.name,
			state->console.index, NULL);
	register_console(&state->console);
	fiq_debugger_tty_init_one(state);
#endif
	return 0;

err_register_irq:
	if (pdata->uart_free)
		pdata->uart_free(pdev);
err_uart_init:
	if (state->clk)
		clk_disable(state->clk);
	if (state->clk)
		clk_put(state->clk);
	wake_lock_destroy(&state->debugger_wake_lock);
	platform_set_drvdata(pdev, NULL);
	kfree(state);
	return ret;
}

static const struct dev_pm_ops fiq_debugger_dev_pm_ops = {
	.suspend	= fiq_debugger_dev_suspend,
	.resume		= fiq_debugger_dev_resume,
};

static struct platform_driver fiq_debugger_driver = {
	.probe	= fiq_debugger_probe,
	.driver	= {
		.name	= "fiq_debugger",
		.pm	= &fiq_debugger_dev_pm_ops,
	},
};

static int __init fiq_debugger_init(void)
{
#if defined(CONFIG_FIQ_DEBUGGER_CONSOLE)
	fiq_debugger_tty_init();
#endif
	return platform_driver_register(&fiq_debugger_driver);
}

postcore_initcall(fiq_debugger_init);
