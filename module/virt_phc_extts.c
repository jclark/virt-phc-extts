// SPDX-License-Identifier: GPL-2.0
/* A raw-monotonic clock with externally scheduled, nominal-time timestamps. */
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/net_tstamp.h>
#include <linux/platform_device.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/timekeeping.h>
#include <linux/version.h>

#define EDGE_COUNT 16
#define MAX_AHEAD_NS (3ULL * NSEC_PER_SEC)
#define MAX_ADJ 62499999
#define SCALED_PPM_DIV 65536000000ULL

static long freq_error_ppb = 10000;
module_param(freq_error_ppb, long, 0444);
MODULE_PARM_DESC(freq_error_ppb, "Intrinsic frequency error, parts per billion");
static long initial_offset_ns = 10000000000L;
module_param(initial_offset_ns, long, 0444);
MODULE_PARM_DESC(initial_offset_ns, "Initial offset from guest realtime, ns");
static bool no_crosststamp;
module_param(no_crosststamp, bool, 0444);
static char *driver_name = "virt_phc_extts";
module_param(driver_name, charp, 0444);

static struct {
	struct platform_device *pdev;
	struct net_device *netdev;
	struct ptp_clock *clock;
	struct ptp_clock_info info;
	struct ptp_pin_desc pin;
	spinlock_t lock;
	struct hrtimer timer;
	u64 base_raw;
	u64 base_phc;
	s64 rate;
	u64 edges[EDGE_COUNT];
	unsigned int count;
	bool enabled;
	bool assigned;
} vphc;

/* The anchor only moves on adjustments; full-width arithmetic avoids a worker
 * and the short overflow horizon of a cyclecounter multiplier.
 */
static u64 phc_at(u64 raw)
{
	u64 delta = raw - vphc.base_raw;
	u64 correction = mul_u64_u64_div_u64(delta, abs(vphc.rate),
					    SCALED_PPM_DIV);

	return vphc.base_phc + delta +
		(vphc.rate < 0 ? -correction : correction);
}

/* Retire elapsed edges before changing the clock or input state. A delayed
 * timer must not apply a later clock adjustment to an earlier physical edge.
 * All callers hold lock, including ptp_clock_event publication.
 */
static void emit_due(u64 now)
{
	while (vphc.count && vphc.edges[0] <= now) {
		struct ptp_clock_event event = {
			.type = PTP_CLOCK_EXTTS,
			.index = 0,
			.timestamp = phc_at(vphc.edges[0]),
		};

		vphc.count--;
		memmove(vphc.edges, vphc.edges + 1,
			vphc.count * sizeof(vphc.edges[0]));
		if (vphc.enabled && vphc.assigned)
			ptp_clock_event(vphc.clock, &event);
	}
}

static ktime_t next_expiry(void)
{
	s64 remaining = vphc.edges[0] - ktime_get_raw_ns();

	/* RAW cannot drive an hrtimer. Recheck RAW on wakeup: system frequency
	 * steering can make this approximate monotonic deadline slightly early.
	 */
	return ktime_add_ns(ktime_get(), max_t(s64, remaining, 1));
}

static enum hrtimer_restart edge_timer(struct hrtimer *timer)
{
	unsigned long flags;
	enum hrtimer_restart ret = HRTIMER_NORESTART;

	spin_lock_irqsave(&vphc.lock, flags);
	emit_due(ktime_get_raw_ns());
	if (vphc.count) {
		hrtimer_set_expires(timer, next_expiry());
		ret = HRTIMER_RESTART;
	}
	spin_unlock_irqrestore(&vphc.lock, flags);
	return ret;
}

static ssize_t edge_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	unsigned long flags;
	u64 edge, now;
	unsigned int i;
	int ret = kstrtou64(buf, 10, &edge);

	if (ret)
		return ret;
	spin_lock_irqsave(&vphc.lock, flags);
	now = ktime_get_raw_ns();
	emit_due(now);
	if (edge <= now || edge - now > MAX_AHEAD_NS) {
		ret = -EINVAL;
		goto out;
	}
	if (vphc.count == EDGE_COUNT) {
		ret = -ENOSPC;
		goto out;
	}
	for (i = 0; i < vphc.count; i++) {
		if (vphc.edges[i] == edge) {
			ret = -EEXIST;
			goto out;
		}
		if (vphc.edges[i] > edge)
			break;
	}
	memmove(vphc.edges + i + 1, vphc.edges + i,
		(vphc.count - i) * sizeof(edge));
	vphc.edges[i] = edge;
	vphc.count++;
	if (!i)
		hrtimer_start(&vphc.timer, next_expiry(), HRTIMER_MODE_ABS);
out:
	spin_unlock_irqrestore(&vphc.lock, flags);
	return ret ? ret : len;
}
static DEVICE_ATTR_WO(edge);

static int vphc_gettimex(struct ptp_clock_info *info, struct timespec64 *ts,
			struct ptp_system_timestamp *sts)
{
	unsigned long flags;
	u64 now;

	spin_lock_irqsave(&vphc.lock, flags);
	ptp_read_system_prets(sts);
	now = ktime_get_raw_ns();
	ptp_read_system_postts(sts);
	*ts = ns_to_timespec64(phc_at(now));
	spin_unlock_irqrestore(&vphc.lock, flags);
	return 0;
}

static int vphc_cross(struct ptp_clock_info *info,
		      struct system_device_crosststamp *xt)
{
	struct system_time_snapshot snap;
	unsigned long flags;

	spin_lock_irqsave(&vphc.lock, flags);
#ifdef VPHC_SNAPSHOT_ID
	ktime_get_snapshot_id(xt->clock_id, &snap);
	if (!snap.valid) {
		spin_unlock_irqrestore(&vphc.lock, flags);
		return -EINVAL;
	}
	xt->sys_systime = snap.systime;
	xt->sys_monoraw = snap.monoraw;
	xt->sys_counter = (struct system_counterval_t) {
		.cycles = snap.cycles,
		.cs_id = snap.cs_id,
	};
#else
	ktime_get_snapshot(&snap);
	xt->sys_realtime = snap.real;
	xt->sys_monoraw = snap.raw;
#endif
	xt->device = ns_to_ktime(phc_at(ktime_to_ns(xt->sys_monoraw)));
	spin_unlock_irqrestore(&vphc.lock, flags);
	return 0;
}

static void reanchor(u64 now)
{
	emit_due(now);
	vphc.base_phc = phc_at(now);
	vphc.base_raw = now;
}

static int vphc_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	unsigned long flags;

	spin_lock_irqsave(&vphc.lock, flags);
	reanchor(ktime_get_raw_ns());
	vphc.rate = scaled_ppm + div_s64((s64)freq_error_ppb * 65536, 1000);
	spin_unlock_irqrestore(&vphc.lock, flags);
	return 0;
}

static int vphc_adjtime(struct ptp_clock_info *info, s64 delta)
{
	unsigned long flags;

	spin_lock_irqsave(&vphc.lock, flags);
	reanchor(ktime_get_raw_ns());
	vphc.base_phc += delta;
	spin_unlock_irqrestore(&vphc.lock, flags);
	return 0;
}

static int vphc_settime(struct ptp_clock_info *info, const struct timespec64 *ts)
{
	unsigned long flags;

	spin_lock_irqsave(&vphc.lock, flags);
	reanchor(ktime_get_raw_ns());
	vphc.base_phc = timespec64_to_ns(ts);
	spin_unlock_irqrestore(&vphc.lock, flags);
	return 0;
}

static int vphc_verify(struct ptp_clock_info *info, unsigned int pin,
		       enum ptp_pin_function func, unsigned int chan)
{
	unsigned long flags;

	if (pin || chan || (func != PTP_PF_NONE && func != PTP_PF_EXTTS))
		return -EOPNOTSUPP;
	spin_lock_irqsave(&vphc.lock, flags);
	emit_due(ktime_get_raw_ns());
	vphc.assigned = func == PTP_PF_EXTTS;
	spin_unlock_irqrestore(&vphc.lock, flags);
	return 0;
}

static int vphc_enable(struct ptp_clock_info *info,
		       struct ptp_clock_request *req, int on)
{
	unsigned long flags;

	if (req->type != PTP_CLK_REQ_EXTTS || req->extts.index)
		return -EOPNOTSUPP;
	if (on && (req->extts.flags & PTP_FALLING_EDGE))
		return -EOPNOTSUPP;
	spin_lock_irqsave(&vphc.lock, flags);
	emit_due(ktime_get_raw_ns());
	vphc.enabled = on;
	spin_unlock_irqrestore(&vphc.lock, flags);
	return 0;
}

static netdev_tx_t vphc_xmit(struct sk_buff *skb, struct net_device *dev)
{
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int vphc_open(struct net_device *dev)
{
	netif_carrier_on(dev);
	netif_start_queue(dev);
	return 0;
}

static int vphc_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	netif_carrier_off(dev);
	return 0;
}

static const struct net_device_ops vphc_net_ops = {
	.ndo_open = vphc_open,
	.ndo_stop = vphc_stop,
	.ndo_start_xmit = vphc_xmit,
};

static int vphc_ts_info(struct net_device *dev,
			struct kernel_ethtool_ts_info *info)
{
	info->phc_index = ptp_clock_index(vphc.clock);
	return 0;
}

static void vphc_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, driver_name, sizeof(info->driver));
}

static const struct ethtool_ops vphc_ethtool_ops = {
	.get_ts_info = vphc_ts_info,
	.get_drvinfo = vphc_drvinfo,
	.get_link = ethtool_op_get_link,
};

static int __init vphc_init(void)
{
	struct system_time_snapshot snap;
	int ret;

	if (freq_error_ppb < -MAX_ADJ || freq_error_ppb > MAX_ADJ)
		return -EINVAL;
	spin_lock_init(&vphc.lock);
#ifdef VPHC_HRTIMER_SETUP
	hrtimer_setup(&vphc.timer, edge_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
#else
	hrtimer_init(&vphc.timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	vphc.timer.function = edge_timer;
#endif
#ifdef VPHC_SNAPSHOT_ID
	ktime_get_snapshot_id(CLOCK_REALTIME, &snap);
	if (!snap.valid)
		return -EINVAL;
	vphc.base_raw = ktime_to_ns(snap.monoraw);
	vphc.base_phc = ktime_to_ns(snap.systime) + initial_offset_ns;
#else
	ktime_get_snapshot(&snap);
	vphc.base_raw = ktime_to_ns(snap.raw);
	vphc.base_phc = ktime_to_ns(snap.real) + initial_offset_ns;
#endif
	vphc.rate = div_s64((s64)freq_error_ppb * 65536, 1000);
	strscpy(vphc.pin.name, "SDP0", sizeof(vphc.pin.name));
	vphc.info = (struct ptp_clock_info) {
		.owner = THIS_MODULE,
		.name = "virt-phc-extts",
		.max_adj = MAX_ADJ,
		.n_ext_ts = 1,
		.n_pins = 1,
		.pin_config = &vphc.pin,
		.adjfine = vphc_adjfine,
		.adjtime = vphc_adjtime,
		.gettimex64 = vphc_gettimex,
		.settime64 = vphc_settime,
		.getcrosststamp = no_crosststamp ? NULL : vphc_cross,
		.enable = vphc_enable,
		.verify = vphc_verify,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
		.supported_extts_flags = PTP_STRICT_FLAGS | PTP_RISING_EDGE,
#endif
	};
	vphc.pdev = platform_device_register_simple("virt-phc-extts", -1, NULL, 0);
	if (IS_ERR(vphc.pdev))
		return PTR_ERR(vphc.pdev);
	vphc.clock = ptp_clock_register(&vphc.info, &vphc.pdev->dev);
	if (IS_ERR(vphc.clock)) {
		ret = PTR_ERR(vphc.clock);
		goto unregister_device;
	}
	vphc.netdev = alloc_netdev(0, "vphc%d", NET_NAME_UNKNOWN, ether_setup);
	if (!vphc.netdev) {
		ret = -ENOMEM;
		goto unregister_clock;
	}
	vphc.netdev->netdev_ops = &vphc_net_ops;
	vphc.netdev->ethtool_ops = &vphc_ethtool_ops;
	SET_NETDEV_DEV(vphc.netdev, &vphc.pdev->dev);
	eth_hw_addr_random(vphc.netdev);
	netif_carrier_off(vphc.netdev);
	ret = register_netdev(vphc.netdev);
	if (ret)
		goto free_netdev;
	ret = device_create_file(&vphc.pdev->dev, &dev_attr_edge);
	if (ret)
		goto unregister_netdev;
	pr_info("virt-phc-extts: %s ptp%d, drift %ld ppb\n",
		vphc.netdev->name, ptp_clock_index(vphc.clock), freq_error_ppb);
	return 0;

unregister_netdev:
	unregister_netdev(vphc.netdev);
free_netdev:
	free_netdev(vphc.netdev);
unregister_clock:
	ptp_clock_unregister(vphc.clock);
unregister_device:
	platform_device_unregister(vphc.pdev);
	return ret;
}

static void __exit vphc_exit(void)
{
	device_remove_file(&vphc.pdev->dev, &dev_attr_edge);
	hrtimer_cancel(&vphc.timer);
	unregister_netdev(vphc.netdev);
	free_netdev(vphc.netdev);
	ptp_clock_unregister(vphc.clock);
	platform_device_unregister(vphc.pdev);
}

module_init(vphc_init);
module_exit(vphc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtual PHC with nominal-time external timestamps");
