#include "sched.h"

/*
 * idle-task scheduling class.
 *
 * (NOTE: these are not related to SCHED_IDLE tasks which are
 *  handled in sched/fair.c)
 */

#ifdef CONFIG_SMP
static int
select_task_rq_idle(struct task_struct *p, int cpu, int sd_flag, int flags,
		    int sibling_count_hint)
{
	return task_cpu(p); /* IDLE tasks as never migrated */
}
#endif /* CONFIG_SMP */

/*
 * Idle tasks are unconditionally rescheduled:
 */
static void check_preempt_curr_idle(struct rq *rq, struct task_struct *p, int flags)
{
	resched_curr(rq);
}

static struct task_struct *
pick_next_task_idle(struct rq *rq, struct task_struct *prev)
{
	put_prev_task(rq, prev);

	schedstat_inc(rq, sched_goidle);
	return rq->idle;
}

/*
 * It is not legal to sleep in the idle task - print a warning
 * message if some code attempts to do it:
 */
static void
dequeue_task_idle(struct rq *rq, struct task_struct *p, int flags)
{
	raw_spin_unlock_irq(&rq->lock);
	printk(KERN_ERR "bad: scheduling from the idle thread!\n");
	dump_stack();
	raw_spin_lock_irq(&rq->lock);
}

static void put_prev_task_idle(struct rq *rq, struct task_struct *prev)
{
	rq_last_tick_reset(rq);
}

static void task_tick_idle(struct rq *rq, struct task_struct *curr, int queued)
{
}

static void set_curr_task_idle(struct rq *rq)
{
}

static void switched_to_idle(struct rq *rq, struct task_struct *p)
{
	BUG();
}

static void
prio_changed_idle(struct rq *rq, struct task_struct *p, int oldprio)
{
	BUG();
}

static unsigned int get_rr_interval_idle(struct rq *rq, struct task_struct *task)
{
	return 0;
}

static void update_curr_idle(struct rq *rq)
{
}

#ifdef CONFIG_SCHED_HMP

static void
inc_hmp_sched_stats_idle(struct rq *rq, struct task_struct *p)
{
}

static void
dec_hmp_sched_stats_idle(struct rq *rq, struct task_struct *p)
{
}

static void
fixup_hmp_sched_stats_idle(struct rq *rq, struct task_struct *p,
			   u32 new_task_load, u32 new_pred_demand)
{
}

#endif

/*
 * Simple, special scheduling class for the per-CPU idle tasks:
 */
const struct sched_class idle_sched_class = {
	/* .next is NULL */
	/* no enqueue/yield_task for idle tasks */

	/* dequeue is not valid, we print a debug message there: */
	.dequeue_task		= dequeue_task_idle,

	.check_preempt_curr	= check_preempt_curr_idle,

	.pick_next_task		= pick_next_task_idle,
	.put_prev_task		= put_prev_task_idle,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_idle,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif

	.set_curr_task          = set_curr_task_idle,
	.task_tick		= task_tick_idle,

	.get_rr_interval	= get_rr_interval_idle,

	.prio_changed		= prio_changed_idle,
	.switched_to		= switched_to_idle,
	.update_curr		= update_curr_idle,
#ifdef CONFIG_SCHED_HMP
	.inc_hmp_sched_stats	= inc_hmp_sched_stats_idle,
	.dec_hmp_sched_stats	= dec_hmp_sched_stats_idle,
	.fixup_hmp_sched_stats	= fixup_hmp_sched_stats_idle,
#endif
};
