#ifndef __ARCH_H8300_ATOMIC__
#define __ARCH_H8300_ATOMIC__

#include <linux/types.h>
#include <asm/cmpxchg.h>

/*
 * Atomic operations that C can't guarantee us.  Useful for
 * resource counting etc..
 */

#define ATOMIC_INIT(i)	{ (i) }

#define atomic_read(v)		READ_ONCE((v)->counter)
#define atomic_set(v, i)	WRITE_ONCE(((v)->counter), (i))

#include <linux/kernel.h>

static inline int atomic_add_return(int i, atomic_t *v)
{
	h8300flags flags;
	int ret;

	flags = arch_local_irq_save();
	ret = v->counter += i;
	arch_local_irq_restore(flags);
	return ret;
}

#define atomic_add(i, v) atomic_add_return(i, v)
#define atomic_add_negative(a, v)	(atomic_add_return((a), (v)) < 0)

static inline int atomic_sub_return(int i, atomic_t *v)
{
	h8300flags flags;
	int ret;

	flags = arch_local_irq_save();
	ret = v->counter -= i;
	arch_local_irq_restore(flags);
	return ret;
}

#define atomic_sub(i, v) atomic_sub_return(i, v)
#define atomic_sub_and_test(i, v) (atomic_sub_return(i, v) == 0)

ATOMIC_OP(and, &=)
ATOMIC_OP(or,  |=)
ATOMIC_OP(xor, ^=)

/*
 * atomic_inc_and_test - increment and test
 * @v: pointer of type atomic_t
 *
 * Atomically increments @v by 1
 * and returns true if the result is zero, or false for all
 * other cases.
 */
#define atomic_inc_and_test(v) (atomic_inc_return(v) == 0)

static inline int atomic_dec_return(atomic_t *v)
{
	h8300flags flags;
	int ret;

	flags = arch_local_irq_save();
	--v->counter;
	ret = v->counter;
	arch_local_irq_restore(flags);
	return ret;
}

#define atomic_dec(v) atomic_dec_return(v)

static inline int atomic_dec_and_test(atomic_t *v)
{
	h8300flags flags;
	int ret;

	flags = arch_local_irq_save();
	--v->counter;
	ret = v->counter;
	arch_local_irq_restore(flags);
	return ret == 0;
}

static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
	int ret;
	h8300flags flags;

	flags = arch_local_irq_save();
	ret = v->counter;
	if (likely(ret == old))
		v->counter = new;
	arch_local_irq_restore(flags);
	return ret;
}

static inline int __atomic_add_unless(atomic_t *v, int a, int u)
{
	int ret;
	h8300flags flags;

	flags = arch_local_irq_save();
	ret = v->counter;
	if (ret != u)
		v->counter += a;
	arch_local_irq_restore(flags);
	return ret;
}

#endif /* __ARCH_H8300_ATOMIC __ */
