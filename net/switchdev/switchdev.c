/*
 * net/switchdev/switchdev.c - Switch device API
 * Copyright (c) 2014 Jiri Pirko <jiri@resnulli.us>
 * Copyright (c) 2014-2015 Scott Feldman <sfeldma@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/netdevice.h>
#include <net/ip_fib.h>
#include <net/switchdev.h>

/**
 *	switchdev_port_attr_get - Get port attribute
 *
 *	@dev: port device
 *	@attr: attribute to get
 */
int switchdev_port_attr_get(struct net_device *dev, struct switchdev_attr *attr)
{
	const struct switchdev_ops *ops = dev->switchdev_ops;
	struct net_device *lower_dev;
	struct list_head *iter;
	struct switchdev_attr first = {
		.id = SWITCHDEV_ATTR_UNDEFINED
	};
	int err = -EOPNOTSUPP;

	if (ops && ops->switchdev_port_attr_get)
		return ops->switchdev_port_attr_get(dev, attr);

	if (attr->flags & SWITCHDEV_F_NO_RECURSE)
		return err;

	/* Switch device port(s) may be stacked under
	 * bond/team/vlan dev, so recurse down to get attr on
	 * each port.  Return -ENODATA if attr values don't
	 * compare across ports.
	 */

	netdev_for_each_lower_dev(dev, lower_dev, iter) {
		err = switchdev_port_attr_get(lower_dev, attr);
		if (err)
			break;
		if (first.id == SWITCHDEV_ATTR_UNDEFINED)
			first = *attr;
		else if (memcmp(&first, attr, sizeof(*attr)))
			return -ENODATA;
	}

	return err;
}
EXPORT_SYMBOL_GPL(switchdev_port_attr_get);

static int __switchdev_port_attr_set(struct net_device *dev,
				     struct switchdev_attr *attr)
{
	const struct switchdev_ops *ops = dev->switchdev_ops;
	struct net_device *lower_dev;
	struct list_head *iter;
	int err = -EOPNOTSUPP;

	if (ops && ops->switchdev_port_attr_set)
		return ops->switchdev_port_attr_set(dev, attr);

	if (attr->flags & SWITCHDEV_F_NO_RECURSE)
		return err;

	/* Switch device port(s) may be stacked under
	 * bond/team/vlan dev, so recurse down to set attr on
	 * each port.
	 */

	netdev_for_each_lower_dev(dev, lower_dev, iter) {
		err = __switchdev_port_attr_set(lower_dev, attr);
		if (err)
			break;
	}

	return err;
}

struct switchdev_attr_set_work {
	struct work_struct work;
	struct net_device *dev;
	struct switchdev_attr attr;
};

static void switchdev_port_attr_set_work(struct work_struct *work)
{
	struct switchdev_attr_set_work *asw =
		container_of(work, struct switchdev_attr_set_work, work);
	int err;

	rtnl_lock();
	err = switchdev_port_attr_set(asw->dev, &asw->attr);
	BUG_ON(err);
	rtnl_unlock();

	dev_put(asw->dev);
	kfree(work);
}

static int switchdev_port_attr_set_defer(struct net_device *dev,
					 struct switchdev_attr *attr)
{
	struct switchdev_attr_set_work *asw;

	asw = kmalloc(sizeof(*asw), GFP_ATOMIC);
	if (!asw)
		return -ENOMEM;

	INIT_WORK(&asw->work, switchdev_port_attr_set_work);

	dev_hold(dev);
	asw->dev = dev;
	memcpy(&asw->attr, attr, sizeof(asw->attr));

	schedule_work(&asw->work);

	return 0;
}

/**
 *	switchdev_port_attr_set - Set port attribute
 *
 *	@dev: port device
 *	@attr: attribute to set
 *
 *	Use a 2-phase prepare-commit transaction model to ensure
 *	system is not left in a partially updated state due to
 *	failure from driver/device.
 */
int switchdev_port_attr_set(struct net_device *dev, struct switchdev_attr *attr)
{
	int err;

	if (!rtnl_is_locked()) {
		/* Running prepare-commit transaction across stacked
		 * devices requires nothing moves, so if rtnl_lock is
		 * not held, schedule a worker thread to hold rtnl_lock
		 * while setting attr.
		 */

		return switchdev_port_attr_set_defer(dev, attr);
	}

	/* Phase I: prepare for attr set. Driver/device should fail
	 * here if there are going to be issues in the commit phase,
	 * such as lack of resources or support.  The driver/device
	 * should reserve resources needed for the commit phase here,
	 * but should not commit the attr.
	 */

	attr->trans = SWITCHDEV_TRANS_PREPARE;
	err = __switchdev_port_attr_set(dev, attr);
	if (err) {
		/* Prepare phase failed: abort the transaction.  Any
		 * resources reserved in the prepare phase are
		 * released.
		 */

		attr->trans = SWITCHDEV_TRANS_ABORT;
		__switchdev_port_attr_set(dev, attr);

		return err;
	}

	/* Phase II: commit attr set.  This cannot fail as a fault
	 * of driver/device.  If it does, it's a bug in the driver/device
	 * because the driver said everythings was OK in phase I.
	 */

	attr->trans = SWITCHDEV_TRANS_COMMIT;
	err = __switchdev_port_attr_set(dev, attr);
	BUG_ON(err);

	return err;
}
EXPORT_SYMBOL_GPL(switchdev_port_attr_set);

/**
 *	switchdev_port_stp_update - Notify switch device port of STP
 *					state change
 *	@dev: port device
 *	@state: port STP state
 *
 *	Notify switch device port of bridge port STP state change.
 */
int switchdev_port_stp_update(struct net_device *dev, u8 state)
{
	const struct switchdev_ops *ops = dev->switchdev_ops;
	struct net_device *lower_dev;
	struct list_head *iter;
	int err = -EOPNOTSUPP;

	if (ops && ops->switchdev_port_stp_update)
		return ops->switchdev_port_stp_update(dev, state);

	netdev_for_each_lower_dev(dev, lower_dev, iter) {
		err = switchdev_port_stp_update(lower_dev, state);
		if (err && err != -EOPNOTSUPP)
			return err;
	}

	return err;
}
EXPORT_SYMBOL_GPL(switchdev_port_stp_update);

static DEFINE_MUTEX(switchdev_mutex);
static RAW_NOTIFIER_HEAD(switchdev_notif_chain);

/**
 *	register_switchdev_notifier - Register notifier
 *	@nb: notifier_block
 *
 *	Register switch device notifier. This should be used by code
 *	which needs to monitor events happening in particular device.
 *	Return values are same as for atomic_notifier_chain_register().
 */
int register_switchdev_notifier(struct notifier_block *nb)
{
	int err;

	mutex_lock(&switchdev_mutex);
	err = raw_notifier_chain_register(&switchdev_notif_chain, nb);
	mutex_unlock(&switchdev_mutex);
	return err;
}
EXPORT_SYMBOL_GPL(register_switchdev_notifier);

/**
 *	unregister_switchdev_notifier - Unregister notifier
 *	@nb: notifier_block
 *
 *	Unregister switch device notifier.
 *	Return values are same as for atomic_notifier_chain_unregister().
 */
int unregister_switchdev_notifier(struct notifier_block *nb)
{
	int err;

	mutex_lock(&switchdev_mutex);
	err = raw_notifier_chain_unregister(&switchdev_notif_chain, nb);
	mutex_unlock(&switchdev_mutex);
	return err;
}
EXPORT_SYMBOL_GPL(unregister_switchdev_notifier);

/**
 *	call_switchdev_notifiers - Call notifiers
 *	@val: value passed unmodified to notifier function
 *	@dev: port device
 *	@info: notifier information data
 *
 *	Call all network notifier blocks. This should be called by driver
 *	when it needs to propagate hardware event.
 *	Return values are same as for atomic_notifier_call_chain().
 */
int call_switchdev_notifiers(unsigned long val, struct net_device *dev,
			     struct switchdev_notifier_info *info)
{
	int err;

	info->dev = dev;
	mutex_lock(&switchdev_mutex);
	err = raw_notifier_call_chain(&switchdev_notif_chain, val, info);
	mutex_unlock(&switchdev_mutex);
	return err;
}
EXPORT_SYMBOL_GPL(call_switchdev_notifiers);

/**
 *	switchdev_port_bridge_setlink - Notify switch device port of bridge
 *	port attributes
 *
 *	@dev: port device
 *	@nlh: netlink msg with bridge port attributes
 *	@flags: bridge setlink flags
 *
 *	Notify switch device port of bridge port attributes
 */
int switchdev_port_bridge_setlink(struct net_device *dev,
				  struct nlmsghdr *nlh, u16 flags)
{
	const struct net_device_ops *ops = dev->netdev_ops;

	if (!(dev->features & NETIF_F_HW_SWITCH_OFFLOAD))
		return 0;

	if (!ops->ndo_bridge_setlink)
		return -EOPNOTSUPP;

	return ops->ndo_bridge_setlink(dev, nlh, flags);
}
EXPORT_SYMBOL_GPL(switchdev_port_bridge_setlink);

/**
 *	switchdev_port_bridge_dellink - Notify switch device port of bridge
 *	port attribute delete
 *
 *	@dev: port device
 *	@nlh: netlink msg with bridge port attributes
 *	@flags: bridge setlink flags
 *
 *	Notify switch device port of bridge port attribute delete
 */
int switchdev_port_bridge_dellink(struct net_device *dev,
				  struct nlmsghdr *nlh, u16 flags)
{
	const struct net_device_ops *ops = dev->netdev_ops;

	if (!(dev->features & NETIF_F_HW_SWITCH_OFFLOAD))
		return 0;

	if (!ops->ndo_bridge_dellink)
		return -EOPNOTSUPP;

	return ops->ndo_bridge_dellink(dev, nlh, flags);
}
EXPORT_SYMBOL_GPL(switchdev_port_bridge_dellink);

/**
 *	ndo_dflt_switchdev_port_bridge_setlink - default ndo bridge setlink
 *						 op for master devices
 *
 *	@dev: port device
 *	@nlh: netlink msg with bridge port attributes
 *	@flags: bridge setlink flags
 *
 *	Notify master device slaves of bridge port attributes
 */
int ndo_dflt_switchdev_port_bridge_setlink(struct net_device *dev,
					   struct nlmsghdr *nlh, u16 flags)
{
	struct net_device *lower_dev;
	struct list_head *iter;
	int ret = 0, err = 0;

	if (!(dev->features & NETIF_F_HW_SWITCH_OFFLOAD))
		return ret;

	netdev_for_each_lower_dev(dev, lower_dev, iter) {
		err = switchdev_port_bridge_setlink(lower_dev, nlh, flags);
		if (err && err != -EOPNOTSUPP)
			ret = err;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(ndo_dflt_switchdev_port_bridge_setlink);

/**
 *	ndo_dflt_switchdev_port_bridge_dellink - default ndo bridge dellink
 *						 op for master devices
 *
 *	@dev: port device
 *	@nlh: netlink msg with bridge port attributes
 *	@flags: bridge dellink flags
 *
 *	Notify master device slaves of bridge port attribute deletes
 */
int ndo_dflt_switchdev_port_bridge_dellink(struct net_device *dev,
					   struct nlmsghdr *nlh, u16 flags)
{
	struct net_device *lower_dev;
	struct list_head *iter;
	int ret = 0, err = 0;

	if (!(dev->features & NETIF_F_HW_SWITCH_OFFLOAD))
		return ret;

	netdev_for_each_lower_dev(dev, lower_dev, iter) {
		err = switchdev_port_bridge_dellink(lower_dev, nlh, flags);
		if (err && err != -EOPNOTSUPP)
			ret = err;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(ndo_dflt_switchdev_port_bridge_dellink);

static struct net_device *switchdev_get_lowest_dev(struct net_device *dev)
{
	const struct switchdev_ops *ops = dev->switchdev_ops;
	struct net_device *lower_dev;
	struct net_device *port_dev;
	struct list_head *iter;

	/* Recusively search down until we find a sw port dev.
	 * (A sw port dev supports switchdev_port_attr_get).
	 */

	if (ops && ops->switchdev_port_attr_get)
		return dev;

	netdev_for_each_lower_dev(dev, lower_dev, iter) {
		port_dev = switchdev_get_lowest_dev(lower_dev);
		if (port_dev)
			return port_dev;
	}

	return NULL;
}

static struct net_device *switchdev_get_dev_by_nhs(struct fib_info *fi)
{
	struct switchdev_attr attr = {
		.id = SWITCHDEV_ATTR_PORT_PARENT_ID,
	};
	struct switchdev_attr prev_attr;
	struct net_device *dev = NULL;
	int nhsel;

	/* For this route, all nexthop devs must be on the same switch. */

	for (nhsel = 0; nhsel < fi->fib_nhs; nhsel++) {
		const struct fib_nh *nh = &fi->fib_nh[nhsel];

		if (!nh->nh_dev)
			return NULL;

		dev = switchdev_get_lowest_dev(nh->nh_dev);
		if (!dev)
			return NULL;

		if (switchdev_port_attr_get(dev, &attr))
			return NULL;

		if (nhsel > 0) {
			if (prev_attr.ppid.id_len != attr.ppid.id_len)
				return NULL;
			if (memcmp(prev_attr.ppid.id, attr.ppid.id,
				   attr.ppid.id_len))
				return NULL;
		}

		prev_attr = attr;
	}

	return dev;
}

/**
 *	switchdev_fib_ipv4_add - Add IPv4 route entry to switch
 *
 *	@dst: route's IPv4 destination address
 *	@dst_len: destination address length (prefix length)
 *	@fi: route FIB info structure
 *	@tos: route TOS
 *	@type: route type
 *	@nlflags: netlink flags passed in (NLM_F_*)
 *	@tb_id: route table ID
 *
 *	Add IPv4 route entry to switch device.
 */
int switchdev_fib_ipv4_add(u32 dst, int dst_len, struct fib_info *fi,
			   u8 tos, u8 type, u32 nlflags, u32 tb_id)
{
	struct net_device *dev;
	const struct switchdev_ops *ops;
	int err = 0;

	/* Don't offload route if using custom ip rules or if
	 * IPv4 FIB offloading has been disabled completely.
	 */

#ifdef CONFIG_IP_MULTIPLE_TABLES
	if (fi->fib_net->ipv4.fib_has_custom_rules)
		return 0;
#endif

	if (fi->fib_net->ipv4.fib_offload_disabled)
		return 0;

	dev = switchdev_get_dev_by_nhs(fi);
	if (!dev)
		return 0;
	ops = dev->switchdev_ops;

	if (ops->switchdev_fib_ipv4_add) {
		err = ops->switchdev_fib_ipv4_add(dev, htonl(dst), dst_len,
						  fi, tos, type, nlflags,
						  tb_id);
		if (!err)
			fi->fib_flags |= RTNH_F_OFFLOAD;
	}

	return err;
}
EXPORT_SYMBOL_GPL(switchdev_fib_ipv4_add);

/**
 *	switchdev_fib_ipv4_del - Delete IPv4 route entry from switch
 *
 *	@dst: route's IPv4 destination address
 *	@dst_len: destination address length (prefix length)
 *	@fi: route FIB info structure
 *	@tos: route TOS
 *	@type: route type
 *	@tb_id: route table ID
 *
 *	Delete IPv4 route entry from switch device.
 */
int switchdev_fib_ipv4_del(u32 dst, int dst_len, struct fib_info *fi,
			   u8 tos, u8 type, u32 tb_id)
{
	struct net_device *dev;
	const struct switchdev_ops *ops;
	int err = 0;

	if (!(fi->fib_flags & RTNH_F_OFFLOAD))
		return 0;

	dev = switchdev_get_dev_by_nhs(fi);
	if (!dev)
		return 0;
	ops = dev->switchdev_ops;

	if (ops->switchdev_fib_ipv4_del) {
		err = ops->switchdev_fib_ipv4_del(dev, htonl(dst), dst_len,
						  fi, tos, type, tb_id);
		if (!err)
			fi->fib_flags &= ~RTNH_F_OFFLOAD;
	}

	return err;
}
EXPORT_SYMBOL_GPL(switchdev_fib_ipv4_del);

/**
 *	switchdev_fib_ipv4_abort - Abort an IPv4 FIB operation
 *
 *	@fi: route FIB info structure
 */
void switchdev_fib_ipv4_abort(struct fib_info *fi)
{
	/* There was a problem installing this route to the offload
	 * device.  For now, until we come up with more refined
	 * policy handling, abruptly end IPv4 fib offloading for
	 * for entire net by flushing offload device(s) of all
	 * IPv4 routes, and mark IPv4 fib offloading broken from
	 * this point forward.
	 */

	fib_flush_external(fi->fib_net);
	fi->fib_net->ipv4.fib_offload_disabled = true;
}
EXPORT_SYMBOL_GPL(switchdev_fib_ipv4_abort);
