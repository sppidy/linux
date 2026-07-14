// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS Zenbook A14 (UX3407QA / UX3407RA) Embedded Controller driver
 *
 * QA fork additions vs the RA upstream:
 *   - Bound as an i2c_client via DT (compatible "asus,zenbook-a14-ec"),
 *     not as a synthetic platform device + bus_find_device_by_name().
 *   - asus_ec_pp_set() tries the QA DSDT WEBC(0x11, 1, [byte]) block-write
 *     at 0x76 first; on NACK it falls back to the RA fan-mode dressup so
 *     the same source works on both variants.
 *
 * Original RA notes (PoC, step 5: platform_profile):
 *
 *   fan1_input   eccr(0x01, 0x09) × 88        RPM (calibrated)
 *   fan1_label   "fan"
 *   pwm1         eccr(0x01, 0x0a)             0-255       (RW)
 *   pwm1_enable  eccr(0x01, 0x02) → mapped    1=manual, 2=auto (RW)
 *   temp1_input  eccr(0x05, 0x02) × 1000      m°C
 *   temp1_label  "ec"
 *
 * Manual mode is gated by the watchdog kthread: when userspace selects
 * pwm1_enable=1, the driver starts a 2s loop that reads the max SoC
 * thermal-zone temperature and pushes it to the fan controller via
 * Request(0x76).write(0x20,0x01,0x02,lo,hi). If this stream stops for
 * more than ~2 minutes while the EC is in manual mode, the EC will
 * hard-reset the system. The driver therefore:
 *   - kicks one temp send synchronously BEFORE switching to manual
 *   - tears manual mode down (back to auto) BEFORE stopping the kthread
 *   - forces auto mode in remove() and on suspend
 *
 * Hardware details (see system-snapshot.md):
 *  - SoC: Qualcomm X1E80100
 *  - EC sits at I²C address 0x5b on platform device "b94000.i2c"
 *  - Companion fan controller sits at 0x76 on the same bus
 *
 * EC protocol (mirrors x1e-ec-tool/tool.py):
 *  - ecrb(maj, min)        : raw register read (1 byte)
 *  - ecwb(maj, min, val)   : raw register write (1 byte)
 *  - ec_settle()           : wait for register (0xc4, 0x30) == 0
 *  - eccr(a1, a2)          : compound read with atomic-exchange semantics
 *  - eccw(a1, a2, val)     : compound write
 *
 * Copyright (C) 2026 Sombre-Osmoze <sombre@osmoze.xyz>
 */

#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_profile.h>
#include <linux/pm.h>
#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <linux/thermal.h>

#define DRV_NAME		"asus_zenbook_a14_ec"

#define EC_I2C_ADDR		0x5b
#define FAN_I2C_ADDR		0x76

/* EC protocol opcodes */
#define EC_OP_ADDR		0x10
#define EC_OP_DATA		0x11

/* Compound-op mailboxes at major 0xc4 */
#define EC_CC_BUSY		0x30
#define EC_CC_REGSEL		0x31
#define EC_CC_DATA		0x32

#define EC_SETTLE_INTERVAL_US	50000
#define EC_SETTLE_TIMEOUT_MS	2000

/* QA-only: WEBC block-write window at major 0xc9 (DSDT WEBC method).
 * Payload bytes go to 0x40..0x6e, then BMCR(0x6f) is set to 0x80 to kick.
 */
#define EC_BLK_MAJ		0xc9
#define EC_BLK_DATA0		0x40
#define EC_BLK_CMD		0x6e
#define EC_BLK_BMCR		0x6f
#define EC_BLK_BMCR_KICK	0x80
#define EC_BLK_BMCR_BUSY	0x40
#define EC_BLK_PRE_KICK_TRIES	200
#define EC_BLK_PRE_KICK_INTERVAL_US 100
#define WEBC_CMD_PROFILE	0x11

/* QA-only: WEBC profile bytes (matches the DSDT _WEBC payload). */
#define EC_PROFILE_BYTE_BALANCED	0x01
#define EC_PROFILE_BYTE_QUIET		0x02
#define EC_PROFILE_BYTE_PERFORMANCE	0x04
#define EC_PROFILE_BYTE_MAX_POWER	0x10

/* hwmon-exposed registers (validated during EC investigation) */
#define EC_REG_FAN_MODE_MAJ	0x01
#define EC_REG_FAN_MODE_RMIN	0x02	/* read:  0=auto, 2=manual */
#define EC_REG_FAN_MODE_WMIN	0x82	/* write: 0=auto, 2=manual */
#define EC_REG_FAN_TACH_MAJ	0x01
#define EC_REG_FAN_TACH_MIN	0x09	/* RPM = value × 88 */
#define EC_REG_PWM_MAJ		0x01
#define EC_REG_PWM_RMIN		0x0a	/* read:  0-255 */
#define EC_REG_PWM_WMIN		0x8a	/* write: 0-255 */
#define EC_REG_FAN_SEL_WMIN	0x8c	/* fan-id selector for PWM/tach */
#define EC_REG_TEMP_MAJ		0x05
#define EC_REG_TEMP_MIN		0x02	/* °C */

/* fan 0 is left, fan 1 is right */
#define EC_NUM_FANS		2

/* Tach-to-RPM conversion: empirically RPM ≈ tach × 88
 * (audio FFT calibration, system-snapshot.md).
 */
#define EC_TACH_RPM_MULT	88

/* PWM floor below which the fan does not spin (calibration). */
#define EC_PWM_SPIN_FLOOR	0x4b	/* 75 */

/* Fan mode encoding observed in (0x01, 0x02). */
#define EC_FAN_MODE_AUTO	0
#define EC_FAN_MODE_MANUAL	2

/* Fan-controller (0x76) opcodes (mirrors x1e-ec-tool/tool.py) */
#define FAN_OP_PUSH_TEMP	0x20	/* [0x20, 0x01, 0x02, lo, hi] */
#define FAN_OP_SUSPEND		0x23	/* [0x23, mode] */
#define FAN_OP_PROFILE		0x24	/* [0x24, profile_idx]; Vivobook proto */

/* Profile readback hypothesis: eccr(0x01, 0x0b) returns current idx
 * (0..3). Validated empirically on Zenbook A14 by writing via 0x24
 * and observing the readback. See system-snapshot.md.
 */
#define EC_REG_PROFILE_MAJ	0x01
#define EC_REG_PROFILE_RMIN	0x0b

/* EC profile indices (matches MODELS["ASUS Vivobook S 15"].profiles) */
#define EC_PROFILE_WHISPER	0
#define EC_PROFILE_STANDARD	1
#define EC_PROFILE_PERFORMANCE	2
#define EC_PROFILE_FULL_SPEED	3
#define EC_PROFILE_MAX		3

/* Watchdog: must be well below the EC's ~2 min timeout. */
#define WATCHDOG_PERIOD_MS	2000

/* CPU frequency cap used while quiet mode relies on passive cooling. */
#define PP_QUIET_FREQ_KHZ	1440000
#define PP_MAX_POLICIES		4

/* Thermal zones to feed to the EC (max of). */
#define ASUS_EC_MAX_ZONES	4
static const char * const asus_ec_thermal_zones[] = {
	"cpu0-0-top-thermal",
	"cpu1-0-top-thermal",
	"cpu2-0-top-thermal",
	"gpuss-0-thermal",
};

struct asus_ec {
	struct device		*dev;
	struct i2c_adapter	*adapter;
	struct i2c_client	*ec_client;
	struct i2c_client	*fan_client;
	struct device		*hwmon_dev;
	struct mutex		lock;	/* serialises EC compound access */

	/* Watchdog state */
	struct task_struct	*watchdog_task;
	struct mutex		mode_lock;	/* gates pwm_enable transitions */
	bool			manual_active;
	struct thermal_zone_device *zones[ASUS_EC_MAX_ZONES];
	int			n_zones;

	/* Profile state. ACPI's platform_profile framework requires
	 * acpi_kobj which doesn't exist on DT-only ARM64 systems, so we
	 * expose a compatible interface as our own sysfs group on the
	 * platform device. Userspace tooling can bridge to PPD later.
	 */
	u8			profile_cached;	/* last value we wrote */
	struct device		*ppdev;		/* platform_profile class device */
	enum platform_profile_option pp_active;	/* currently active profile */
	struct freq_qos_request freq_req[PP_MAX_POLICIES];
	int			n_freq_req;

	/* QA-only: WEBC profile path. proven_bad sticks after first NACK so
	 * we don't keep retrying on RA where the EC will never accept it.
	 */
	bool			webc_profile_works;
	bool			webc_profile_proven_bad;

	/* DMA-safe scratch (kmalloc-backed via devm_kzalloc) */
	u8			tx[3];
	u8			rx;
};

/* ------------------------------------------------------------------ */
/* EC primitives — caller MUST hold ec->lock                          */
/* ------------------------------------------------------------------ */

static int __ec_rb(struct asus_ec *ec, u8 maj, u8 min, u8 *out)
{
	struct i2c_msg msgs[3];
	static const u8 op_data = EC_OP_DATA;
	int ret;

	ec->tx[0] = EC_OP_ADDR;
	ec->tx[1] = maj;
	ec->tx[2] = min;

	msgs[0].addr  = EC_I2C_ADDR;
	msgs[0].flags = 0;
	msgs[0].len   = 3;
	msgs[0].buf   = ec->tx;

	msgs[1].addr  = EC_I2C_ADDR;
	msgs[1].flags = 0;
	msgs[1].len   = 1;
	msgs[1].buf   = (u8 *)&op_data;

	msgs[2].addr  = EC_I2C_ADDR;
	msgs[2].flags = I2C_M_RD;
	msgs[2].len   = 1;
	msgs[2].buf   = &ec->rx;

	ret = i2c_transfer(ec->adapter, msgs, 3);
	if (ret < 0) {
		dev_dbg(ec->dev, "ecrb(0x%02x,0x%02x) failed: %d\n",
			maj, min, ret);
		return ret;
	}
	if (ret != 3)
		return -EIO;

	*out = ec->rx;
	return 0;
}

static int __ec_wb(struct asus_ec *ec, u8 maj, u8 min, u8 val)
{
	struct i2c_msg msgs[2];
	u8 data[2];
	int ret;

	ec->tx[0] = EC_OP_ADDR;
	ec->tx[1] = maj;
	ec->tx[2] = min;

	data[0] = EC_OP_DATA;
	data[1] = val;

	msgs[0].addr  = EC_I2C_ADDR;
	msgs[0].flags = 0;
	msgs[0].len   = 3;
	msgs[0].buf   = ec->tx;

	msgs[1].addr  = EC_I2C_ADDR;
	msgs[1].flags = 0;
	msgs[1].len   = 2;
	msgs[1].buf   = data;

	ret = i2c_transfer(ec->adapter, msgs, 2);
	if (ret < 0) {
		dev_dbg(ec->dev, "ecwb(0x%02x,0x%02x,0x%02x) failed: %d\n",
			maj, min, val, ret);
		return ret;
	}
	if (ret != 2)
		return -EIO;
	return 0;
}

static int __ec_settle(struct asus_ec *ec)
{
	unsigned long deadline;
	u8 v;
	int ret;

	deadline = jiffies + msecs_to_jiffies(EC_SETTLE_TIMEOUT_MS);

	for (;;) {
		ret = __ec_rb(ec, 0xc4, EC_CC_BUSY, &v);
		if (ret)
			return ret;
		if (v == 0)
			return 0;
		if (time_after(jiffies, deadline)) {
			dev_warn(ec->dev,
				 "ec_settle timeout (last busy=0x%02x)\n", v);
			return -ETIMEDOUT;
		}
		usleep_range(EC_SETTLE_INTERVAL_US,
			     EC_SETTLE_INTERVAL_US + 10000);
	}
}

static int __ec_cr(struct asus_ec *ec, u8 a1, u8 a2, u8 *out)
{
	int ret;
	u8 v;

	if (a2 >= 0x80) {
		dev_err(ec->dev,
			"eccr refused: a2=0x%02x >= 0x80 destructive\n", a2);
		return -EINVAL;
	}

	ret = __ec_settle(ec);
	if (ret)
		return ret;

	ret = __ec_wb(ec, 0xc4, EC_CC_REGSEL, a2);
	if (ret)
		return ret;
	ret = __ec_wb(ec, 0xc4, EC_CC_BUSY, a1);
	if (ret)
		return ret;

	ret = __ec_settle(ec);
	if (ret)
		return ret;

	ret = __ec_rb(ec, 0xc4, EC_CC_DATA, &v);
	if (ret)
		return ret;

	ret = __ec_wb(ec, 0xc4, EC_CC_DATA, 0x00);
	if (ret)
		return ret;

	*out = v;
	return 0;
}

static int __ec_cw(struct asus_ec *ec, u8 a1, u8 a2, u8 val)
{
	int ret;

	ret = __ec_settle(ec);
	if (ret)
		return ret;

	ret = __ec_wb(ec, 0xc4, EC_CC_REGSEL, a2);
	if (ret)
		return ret;
	ret = __ec_wb(ec, 0xc4, EC_CC_DATA, val);
	if (ret)
		return ret;
	ret = __ec_wb(ec, 0xc4, EC_CC_BUSY, a1);
	if (ret)
		return ret;

	return __ec_settle(ec);
}

static int asus_ec_read_reg(struct asus_ec *ec, u8 maj, u8 min, u8 *out)
{
	int ret;

	mutex_lock(&ec->lock);
	ret = __ec_cr(ec, maj, min, out);
	mutex_unlock(&ec->lock);
	return ret;
}

static int asus_ec_write_reg(struct asus_ec *ec, u8 maj, u8 min, u8 val)
{
	int ret;

	mutex_lock(&ec->lock);
	ret = __ec_cw(ec, maj, min, val);
	mutex_unlock(&ec->lock);
	return ret;
}

/*
 * QA DSDT WEBC block-write. Payload goes to 0xc9:0x40..; BMCR(0x6f) is set
 * to 0x80 to kick; the command byte goes to 0x6e last. Fire-and-forget —
 * we drop the lock immediately after the kick rather than polling BMCR
 * (the post-kick wait deadlocked an earlier attempt). Returns 0 on
 * success, -EBUSY if the pre-kick busy poll never cleared, or any errno
 * from the underlying register writes.
 */
static int asus_ec_webc(struct asus_ec *ec, u8 cmd, const u8 *buf, size_t len)
{
	u8 bmcr = 0;
	int ret, i;

	if (len > (EC_BLK_BMCR - EC_BLK_DATA0))
		return -EINVAL;

	mutex_lock(&ec->lock);

	for (i = 0; i < EC_BLK_PRE_KICK_TRIES; i++) {
		ret = __ec_rb(ec, EC_BLK_MAJ, EC_BLK_BMCR, &bmcr);
		if (ret)
			goto out;
		if (bmcr == 0)
			break;
		usleep_range(EC_BLK_PRE_KICK_INTERVAL_US,
			     EC_BLK_PRE_KICK_INTERVAL_US + 50);
	}
	if (bmcr != 0) {
		(void)__ec_wb(ec, EC_BLK_MAJ, EC_BLK_BMCR,
			      bmcr | EC_BLK_BMCR_BUSY);
		ret = -EBUSY;
		goto out;
	}

	for (i = 0; i < (int)len; i++) {
		ret = __ec_wb(ec, EC_BLK_MAJ, EC_BLK_DATA0 + i, buf[i]);
		if (ret)
			goto out;
	}

	ret = __ec_wb(ec, EC_BLK_MAJ, EC_BLK_BMCR, bmcr | EC_BLK_BMCR_KICK);
	if (ret)
		goto out;

	ret = __ec_wb(ec, EC_BLK_MAJ, EC_BLK_CMD, cmd);

out:
	mutex_unlock(&ec->lock);
	return ret;
}

/* High-level EC operations */

static int asus_ec_set_fan_mode(struct asus_ec *ec, u8 mode)
{
	return asus_ec_write_reg(ec, EC_REG_FAN_MODE_MAJ,
				 EC_REG_FAN_MODE_WMIN, mode);
}

static int asus_ec_set_pwm(struct asus_ec *ec, int fan_id, u8 speed)
{
	int ret;

	if (fan_id < 0 || fan_id >= EC_NUM_FANS)
		return -EINVAL;

	mutex_lock(&ec->lock);
	ret = __ec_cw(ec, EC_REG_PWM_MAJ, EC_REG_FAN_SEL_WMIN, fan_id);
	if (ret)
		goto out;
	ret = __ec_cw(ec, EC_REG_PWM_MAJ, EC_REG_PWM_WMIN, speed);
out:
	mutex_unlock(&ec->lock);
	return ret;
}

static int asus_ec_set_pwm_both(struct asus_ec *ec, u8 speed)
{
	int ret;

	ret = asus_ec_set_pwm(ec, 0, speed);
	if (ret)
		return ret;

	return asus_ec_set_pwm(ec, 1, speed);
}

static int asus_ec_read_fan_reg(struct asus_ec *ec, int fan_id,
				u8 reg_min, u8 *out)
{
	int ret;

	if (fan_id < 0 || fan_id >= EC_NUM_FANS)
		return -EINVAL;

	mutex_lock(&ec->lock);
	ret = __ec_cw(ec, EC_REG_PWM_MAJ, EC_REG_FAN_SEL_WMIN, fan_id);
	if (!ret)
		ret = __ec_cr(ec, EC_REG_FAN_TACH_MAJ, reg_min, out);
	mutex_unlock(&ec->lock);

	return ret;
}

/* ------------------------------------------------------------------ */
/* Fan-controller (0x76) direct writes                                */
/* ------------------------------------------------------------------ */

static int fan_send_temp_dc(struct asus_ec *ec, u16 deci_celsius)
{
	u8 buf[5];
	int ret;

	if (deci_celsius > 2000)
		deci_celsius = 2000;

	buf[0] = FAN_OP_PUSH_TEMP;
	buf[1] = 0x01;
	buf[2] = 0x02;
	buf[3] = deci_celsius & 0xff;
	buf[4] = (deci_celsius >> 8) & 0xff;

	ret = i2c_master_send(ec->fan_client, buf, sizeof(buf));
	return ret < 0 ? ret : 0;
}

static int __maybe_unused fan_set_suspend(struct asus_ec *ec, u8 mode)
{
	u8 buf[2] = { FAN_OP_SUSPEND, mode };
	int ret;

	ret = i2c_master_send(ec->fan_client, buf, sizeof(buf));
	return ret < 0 ? ret : 0;
}

static int fan_set_profile(struct asus_ec *ec, u8 profile)
{
	u8 buf[2] = { FAN_OP_PROFILE, profile };
	int ret;

	if (profile > EC_PROFILE_MAX)
		return -EINVAL;

	ret = i2c_master_send(ec->fan_client, buf, sizeof(buf));
	return ret < 0 ? ret : 0;
}

/* ------------------------------------------------------------------ */
/* Profile sysfs (custom; mirrors platform_profile string semantics)  */
/* ------------------------------------------------------------------ */

/*
 * Two protocol hypotheses for setting the profile on the X1E80100 EC:
 *   (a) tool.py / Vivobook: Request(0x76).write(0x24, idx)   ← used here
 *   (b) +0x80 convention:    eccw(0x01, 0x8b, idx)            ← unverified
 *
 * (a) is the documented vendor approach and is known to work on the
 * sibling Vivobook S 15. (b) is hypothesised because eccr(0x01, 0x0b)
 * returns 0x02 by default on the A14, matching "Performance" idx 2.
 *
 * We pick (a) and *verify* it by reading back 0x0b after every set:
 * if the readback tracks the written value, both hypotheses are
 * consistent with each other (probably (b) is just the underlying
 * register write that 0x24 performs).
 *
 * The kernel's platform_profile framework would normally own the
 * /sys/firmware/acpi/platform_profile sysfs node, but it requires
 * ACPI (acpi_disabled check + acpi_kobj as sysfs root). This box
 * is DT-only, so we expose the same string vocabulary as a custom
 * sysfs group on our platform device:
 *
 *   /sys/devices/platform/asus_zenbook_a14_ec/profile
 *   /sys/devices/platform/asus_zenbook_a14_ec/profile_choices
 *
 * Once a non-ACPI platform_profile path lands upstream (or once we
 * patch it ourselves), this whole block becomes a thin wrapper
 * around devm_platform_profile_register().
 */

static const char * const profile_names[] = {
	[EC_PROFILE_WHISPER]	 = "quiet",
	[EC_PROFILE_STANDARD]	 = "balanced",
	[EC_PROFILE_PERFORMANCE] = "balanced-performance",
	[EC_PROFILE_FULL_SPEED]	 = "performance",
};

static int profile_name_to_idx(const char *name, size_t len)
{
	int i;

	/* Strip trailing whitespace/newline. */
	while (len && (name[len - 1] == '\n' || name[len - 1] == ' ' ||
		       name[len - 1] == '\t'))
		len--;

	/* Allow numeric "0".."3" too, for scripts. */
	if (len == 1 && name[0] >= '0' && name[0] <= '3')
		return name[0] - '0';

	for (i = 0; i < ARRAY_SIZE(profile_names); i++) {
		if (strlen(profile_names[i]) == len &&
		    !strncmp(profile_names[i], name, len))
			return i;
	}
	return -EINVAL;
}

static int asus_ec_profile_apply(struct asus_ec *ec, u8 idx)
{
	u8 readback;
	int ret;

	if (idx > EC_PROFILE_MAX)
		return -EINVAL;

	ret = fan_set_profile(ec, idx);
	if (ret) {
		dev_err(ec->dev, "profile_set(%u) failed: %d\n", idx, ret);
		return ret;
	}
	ec->profile_cached = idx;

	/* Verify the (0x01, 0x0b) readback hypothesis. Best-effort. */
	if (!asus_ec_read_reg(ec, EC_REG_PROFILE_MAJ,
			      EC_REG_PROFILE_RMIN, &readback)) {
		if (readback != idx)
			dev_dbg(ec->dev,
				"profile readback (0x%02x) != written (0x%02x)\n",
				readback, idx);
	}

	dev_dbg(ec->dev, "profile set to %u (%s)\n",
		idx, profile_names[idx]);
	return 0;
}

static ssize_t profile_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct asus_ec *ec = dev_get_drvdata(dev);
	u8 v;

	if (asus_ec_read_reg(ec, EC_REG_PROFILE_MAJ,
			     EC_REG_PROFILE_RMIN, &v) || v > EC_PROFILE_MAX)
		v = ec->profile_cached;

	return sysfs_emit(buf, "%s\n", profile_names[v]);
}

static ssize_t profile_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct asus_ec *ec = dev_get_drvdata(dev);
	int idx = profile_name_to_idx(buf, count);
	int ret;

	if (idx < 0)
		return idx;

	ret = asus_ec_profile_apply(ec, (u8)idx);
	return ret ? ret : count;
}

static ssize_t profile_choices_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	return sysfs_emit(buf, "%s %s %s %s\n",
			  profile_names[EC_PROFILE_WHISPER],
			  profile_names[EC_PROFILE_STANDARD],
			  profile_names[EC_PROFILE_PERFORMANCE],
			  profile_names[EC_PROFILE_FULL_SPEED]);
}

static DEVICE_ATTR_RW(profile);
static DEVICE_ATTR_RO(profile_choices);

static struct attribute *asus_ec_profile_attrs[] = {
	&dev_attr_profile.attr,
	&dev_attr_profile_choices.attr,
	NULL,
};

static const struct attribute_group asus_ec_profile_group = {
	.attrs = asus_ec_profile_attrs,
};

/* ------------------------------------------------------------------ */
/* CPU frequency QoS                                                  */
/* ------------------------------------------------------------------ */

static void asus_ec_freq_qos_cleanup(struct asus_ec *ec)
{
	int i;

	for (i = 0; i < ec->n_freq_req; i++)
		freq_qos_remove_request(&ec->freq_req[i]);

	ec->n_freq_req = 0;
}

static void asus_ec_freq_qos_init(struct asus_ec *ec)
{
	struct cpufreq_policy *policy;
	int cpu, ret;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		/* A policy shared by several cores only needs one request. */
		if (cpu != cpumask_first(policy->related_cpus)) {
			cpufreq_cpu_put(policy);
			continue;
		}

		if (ec->n_freq_req == PP_MAX_POLICIES) {
			dev_warn(ec->dev, "freq_qos: too many CPU policies\n");
			cpufreq_cpu_put(policy);
			break;
		}

		ret = freq_qos_add_request(&policy->constraints,
					   &ec->freq_req[ec->n_freq_req],
					   FREQ_QOS_MAX,
					   FREQ_QOS_MAX_DEFAULT_VALUE);
		cpufreq_cpu_put(policy);
		if (ret < 0) {
			dev_warn(ec->dev,
				 "freq_qos: failed to register CPU%d: %d\n",
				 cpu, ret);
			continue;
		}

		ec->n_freq_req++;
	}

	dev_info(ec->dev, "freq_qos: registered %d CPU policies\n",
		 ec->n_freq_req);
}

static int asus_ec_freq_qos_set(struct asus_ec *ec, s32 max_khz)
{
	int i, ret;

	if (!ec->n_freq_req)
		return -ENODEV;

	for (i = 0; i < ec->n_freq_req; i++) {
		ret = freq_qos_update_request(&ec->freq_req[i], max_khz);
		if (ret < 0) {
			dev_warn(ec->dev,
				 "freq_qos: policy %d update to %d kHz failed: %d\n",
				 i, max_khz, ret);
			return ret;
		}
	}

	return 0;
}

static int asus_ec_set_profile_freq(struct asus_ec *ec,
				    enum platform_profile_option profile)
{
	s32 max_khz = FREQ_QOS_MAX_DEFAULT_VALUE;

	if (profile == PLATFORM_PROFILE_QUIET)
		max_khz = PP_QUIET_FREQ_KHZ;

	return asus_ec_freq_qos_set(ec, max_khz);
}

/* ------------------------------------------------------------------ */
/* platform_profile integration                                       */
/* ------------------------------------------------------------------ */

/*
 * Since the A14 EC profile register (0x01,0x0b) is read-only and the
 * Vivobook protocol (0x24/0x76) NACKs, we implement platform_profile
 * by controlling the fan and CPU frequency directly:
 *   QUIET        → WEBC quiet curve, or fallback PWM 0 + 1.44 GHz cap
 *   BALANCED     → auto mode (EC default thermal curve), CPU uncapped
 *   PERFORMANCE  → WEBC 0x04, or fallback manual PWM 180, CPU uncapped
 *   MAX_POWER    → WEBC 0x10, or fallback manual PWM 69, CPU uncapped
 */

#define PP_PERF_PWM	180
#define PP_MAX_POWER_PWM	69

static int asus_ec_pp_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_QUIET, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_MAX_POWER, choices);
	return 0;
}

static int asus_ec_pp_get(struct device *dev,
			  enum platform_profile_option *profile)
{
	struct asus_ec *ec = dev_get_drvdata(dev);

	*profile = ec->pp_active;
	return 0;
}

static int asus_ec_pp_set(struct device *dev,
			  enum platform_profile_option profile)
{
	struct asus_ec *ec = dev_get_drvdata(dev);
	int ret;
	u8 byte;

	/*
	 * QA path: try the DSDT WEBC(0x11, 1, [byte]) block-write first.
	 * On QA the EC accepts it and applies the OEM thermal table; on RA
	 * it NACKs and proven_bad sticks, so subsequent calls drop straight
	 * into the fan-mode dressup below.
	 */
	if (!ec->webc_profile_proven_bad) {
		switch (profile) {
		case PLATFORM_PROFILE_QUIET:
			byte = EC_PROFILE_BYTE_QUIET; break;
		case PLATFORM_PROFILE_BALANCED:
			byte = EC_PROFILE_BYTE_BALANCED; break;
		case PLATFORM_PROFILE_PERFORMANCE:
			byte = EC_PROFILE_BYTE_PERFORMANCE; break;
		case PLATFORM_PROFILE_MAX_POWER:
			byte = EC_PROFILE_BYTE_MAX_POWER; break;
		default:
			byte = 0;
		}
		if (byte) {
			ret = asus_ec_webc(ec, WEBC_CMD_PROFILE, &byte, 1);
			if (!ret) {
				ec->webc_profile_works = true;
				/*
				 * If an earlier profile put the EC in manual
				 * mode, return control to the OEM curve.
				 */
				mutex_lock(&ec->mode_lock);
				if (ec->manual_active) {
					if (!asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO))
						ec->manual_active = false;
				}
				ret = asus_ec_set_profile_freq(ec, profile);
				if (ret) {
					mutex_unlock(&ec->mode_lock);
					return ret;
				}
				ec->pp_active = profile;
				mutex_unlock(&ec->mode_lock);
				return 0;
			}
			dev_warn(ec->dev,
				 "WEBC profile path NACK'd (%d); falling back to fan dressup\n",
				 ret);
			ec->webc_profile_proven_bad = true;
		}
	}

	mutex_lock(&ec->mode_lock);

	switch (profile) {
	case PLATFORM_PROFILE_QUIET:
		/* Cap first, then stop both fans for passive cooling. */
		ret = asus_ec_set_profile_freq(ec, profile);
		if (ret)
			goto out;
		if (!ec->manual_active) {
			ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_MANUAL);
			if (ret)
				goto out;
			ec->manual_active = true;
		}
		ret = asus_ec_set_pwm_both(ec, 0);
		if (ret)
			goto out;
		break;

	case PLATFORM_PROFILE_BALANCED:
		/* Auto fan curve with no driver-imposed frequency ceiling. */
		if (ec->manual_active) {
			ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO);
			if (ret)
				goto out;
			ec->manual_active = false;
		}
		ret = asus_ec_set_profile_freq(ec, profile);
		if (ret)
			goto out;
		break;

	case PLATFORM_PROFILE_PERFORMANCE:
		/* Manual mode with fixed high PWM for sustained cooling. */
		if (!ec->manual_active) {
			ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_MANUAL);
			if (ret)
				goto out;
			ec->manual_active = true;
		}
		ret = asus_ec_set_pwm_both(ec, PP_PERF_PWM);
		if (ret)
			goto out;
		ret = asus_ec_set_profile_freq(ec, profile);
		if (ret)
			goto out;
		break;

	case PLATFORM_PROFILE_MAX_POWER:
		/* Manual mode with maximum PWM for maximum cooling (~6000 RPM). */
		if (!ec->manual_active) {
			ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_MANUAL);
			if (ret)
				goto out;
			ec->manual_active = true;
		}
		ret = asus_ec_set_pwm_both(ec, PP_MAX_POWER_PWM);
		if (ret)
			goto out;
		ret = asus_ec_set_profile_freq(ec, profile);
		if (ret)
			goto out;
		break;

	default:
		ret = -EOPNOTSUPP;
		goto out;
	}

	ec->pp_active = profile;
	ret = 0;

out:
	mutex_unlock(&ec->mode_lock);
	return ret;
}

static const struct platform_profile_ops asus_ec_pp_ops = {
	.probe       = asus_ec_pp_probe,
	.profile_get = asus_ec_pp_get,
	.profile_set = asus_ec_pp_set,
};

/* ------------------------------------------------------------------ */
/* Thermal zone bookkeeping + watchdog                                */
/* ------------------------------------------------------------------ */

static void asus_ec_lookup_thermal_zones(struct asus_ec *ec)
{
	int i;

	ec->n_zones = 0;
	for (i = 0; i < ARRAY_SIZE(asus_ec_thermal_zones); i++) {
		struct thermal_zone_device *tz;

		tz = thermal_zone_get_zone_by_name(asus_ec_thermal_zones[i]);
		if (IS_ERR(tz)) {
			dev_warn(ec->dev, "thermal zone '%s' not found: %ld\n",
				 asus_ec_thermal_zones[i], PTR_ERR(tz));
			continue;
		}
		ec->zones[ec->n_zones++] = tz;
		dev_dbg(ec->dev, "thermal zone '%s' attached\n",
			asus_ec_thermal_zones[i]);
	}

	if (ec->n_zones == 0)
		dev_warn(ec->dev,
			 "no thermal zones available; manual mode will fall back to EC temp\n");
}

/* Returns max temp in m°C across registered zones, or -1 on total failure. */
static int asus_ec_max_temp_mc(struct asus_ec *ec)
{
	int max = -1;
	int i;

	for (i = 0; i < ec->n_zones; i++) {
		int t;

		if (thermal_zone_get_temp(ec->zones[i], &t))
			continue;
		if (t > max)
			max = t;
	}

	if (max < 0) {
		/* Fallback: EC's own temp register, in °C → m°C. */
		u8 v;

		if (!asus_ec_read_reg(ec, EC_REG_TEMP_MAJ,
				      EC_REG_TEMP_MIN, &v))
			max = (int)v * 1000;
	}

	return max;
}

static int asus_ec_send_current_temp(struct asus_ec *ec)
{
	int mc = asus_ec_max_temp_mc(ec);
	u16 dc;

	if (mc < 0)
		return -ENODATA;

	/* m°C → deci-°C. Clamp to 2000 (200 °C) — guaranteed cold otherwise. */
	dc = (u16)clamp(mc / 100, 0, 2000);
	return fan_send_temp_dc(ec, dc);
}

static int asus_ec_watchdog_fn(void *data)
{
	struct asus_ec *ec = data;

	dev_info(ec->dev, "watchdog: started (period %d ms)\n",
		 WATCHDOG_PERIOD_MS);

	while (!kthread_should_stop()) {
		int ret = asus_ec_send_current_temp(ec);

		if (ret)
			dev_warn_ratelimited(ec->dev,
					     "watchdog: temp send failed: %d\n",
					     ret);

		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			__set_current_state(TASK_RUNNING);
			break;
		}
		schedule_timeout(msecs_to_jiffies(WATCHDOG_PERIOD_MS));
	}

	dev_info(ec->dev, "watchdog: stopped\n");
	return 0;
}

/* Caller MUST hold ec->mode_lock */
static int __maybe_unused asus_ec_start_watchdog(struct asus_ec *ec)
{
	struct task_struct *t;
	int ret;

	if (ec->watchdog_task)
		return 0;

	/* Send one temp synchronously so the EC's watchdog starts fresh. */
	ret = asus_ec_send_current_temp(ec);
	if (ret) {
		dev_err(ec->dev,
			"watchdog: refusing to start, initial temp send failed: %d\n",
			ret);
		return ret;
	}

	t = kthread_run(asus_ec_watchdog_fn, ec, "asus_ec_wdt");
	if (IS_ERR(t))
		return PTR_ERR(t);

	ec->watchdog_task = t;
	return 0;
}

/* Caller MUST hold ec->mode_lock */
static void asus_ec_stop_watchdog(struct asus_ec *ec)
{
	if (!ec->watchdog_task)
		return;

	kthread_stop(ec->watchdog_task);
	ec->watchdog_task = NULL;
}

/* Caller MUST hold ec->mode_lock. Order matters for safety. */
static int asus_ec_enter_manual(struct asus_ec *ec)
{
	int ret;

	/*
	 * A14 has no watchdog timeout (verified 2026-05-07: 3+ min manual
	 * mode with no temp feed = no reboot). Vivobook needs watchdog;
	 * A14 doesn't. For now skip watchdog entirely (TODO: add model
	 * detection and gate per-model).
	 */
	ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_MANUAL);
	if (ret)
		return ret;

	ec->manual_active = true;
	return 0;
}

/* Caller MUST hold ec->mode_lock. */
static int asus_ec_leave_manual(struct asus_ec *ec)
{
	int ret;

	/* Always tell EC auto FIRST; only then is it safe to stop feeding. */
	ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO);
	if (ret) {
		dev_err(ec->dev,
			"failed to restore auto fan mode: %d\n", ret);
		return ret;
	}

	/* A14: no watchdog, nothing to stop. */
	/* asus_ec_stop_watchdog(ec); */
	ec->manual_active = false;
	return 0;
}

/* ------------------------------------------------------------------ */
/* hwmon callbacks                                                    */
/* ------------------------------------------------------------------ */

static umode_t asus_ec_hwmon_is_visible(const void *drvdata,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		if (channel >= EC_NUM_FANS)
			return 0;
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
			return 0444;
		default:
			return 0;
		}
	case hwmon_pwm:
		if (channel >= EC_NUM_FANS)
			return 0;
		switch (attr) {
		case hwmon_pwm_input:
			return 0644;
		case hwmon_pwm_enable:
			return channel == 0 ? 0644 : 0;
		default:
			return 0;
		}
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_label:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static int asus_ec_hwmon_read(struct device *dev,
			      enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct asus_ec *ec = dev_get_drvdata(dev);
	u8 v;
	int ret;

	switch (type) {
	case hwmon_fan:
		if (attr != hwmon_fan_input || channel >= EC_NUM_FANS)
			return -EOPNOTSUPP;
		ret = asus_ec_read_fan_reg(ec, channel,
					   EC_REG_FAN_TACH_MIN, &v);
		if (ret)
			return ret;
		*val = (long)v * EC_TACH_RPM_MULT;
		return 0;

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			if (channel >= EC_NUM_FANS)
				return -EOPNOTSUPP;
			ret = asus_ec_read_fan_reg(ec, channel,
						   EC_REG_PWM_RMIN, &v);
			if (ret)
				return ret;
			*val = v;
			return 0;
		case hwmon_pwm_enable:
			if (channel != 0)
				return -EOPNOTSUPP;
			ret = asus_ec_read_reg(ec, EC_REG_FAN_MODE_MAJ,
					       EC_REG_FAN_MODE_RMIN, &v);
			if (ret)
				return ret;
			/* hwmon convention: 1=manual, 2=auto */
			if (v == EC_FAN_MODE_MANUAL)
				*val = 1;
			else if (v == EC_FAN_MODE_AUTO)
				*val = 2;
			else
				*val = 0;
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		ret = asus_ec_read_reg(ec, EC_REG_TEMP_MAJ,
				       EC_REG_TEMP_MIN, &v);
		if (ret)
			return ret;
		*val = (long)v * 1000;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static int asus_ec_hwmon_write(struct device *dev,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel, long val)
{
	struct asus_ec *ec = dev_get_drvdata(dev);
	int ret;
	u8 speed;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_enable:
		if (channel != 0)
			return -EOPNOTSUPP;
		mutex_lock(&ec->mode_lock);
		switch (val) {
		case 1: /* manual */
			if (ec->manual_active)
				ret = 0;
			else
				ret = asus_ec_enter_manual(ec);
			break;
		case 2: /* auto */
			if (!ec->manual_active)
				ret = 0;
			else
				ret = asus_ec_leave_manual(ec);
			break;
		default:
			ret = -EINVAL;
		}
		mutex_unlock(&ec->mode_lock);
		return ret;

	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;
		if (channel >= EC_NUM_FANS)
			return -EOPNOTSUPP;
		speed = (u8)val;

		mutex_lock(&ec->mode_lock);
		if (!ec->manual_active) {
			mutex_unlock(&ec->mode_lock);
			return -EBUSY;	/* set pwm1_enable=1 first */
		}
		ret = asus_ec_set_pwm(ec, channel, speed);
		mutex_unlock(&ec->mode_lock);
		if (ret)
			return ret;

		if (speed > 0 && speed < EC_PWM_SPIN_FLOOR)
			dev_info_ratelimited(ec->dev,
					     "fan%d pwm=%u below spin floor (%u); fan likely idle\n",
					     channel, speed, EC_PWM_SPIN_FLOOR);
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static const char * const fan_labels[] = { "left", "right" };

static int asus_ec_hwmon_read_string(struct device *dev,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel,
				     const char **str)
{
	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_label && channel < EC_NUM_FANS) {
			*str = fan_labels[channel];
			return 0;
		}
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_label) {
			*str = "ec";
			return 0;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops asus_ec_hwmon_ops = {
	.is_visible	= asus_ec_hwmon_is_visible,
	.read		= asus_ec_hwmon_read,
	.write		= asus_ec_hwmon_write,
	.read_string	= asus_ec_hwmon_read_string,
};

static const struct hwmon_channel_info * const asus_ec_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_chip_info asus_ec_hwmon_chip_info = {
	.ops	= &asus_ec_hwmon_ops,
	.info	= asus_ec_hwmon_info,
};

/* ------------------------------------------------------------------ */
/* probe / remove                                                     */
/* ------------------------------------------------------------------ */

static int asus_ec_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	const struct hwmon_chip_info *chip_info = &asus_ec_hwmon_chip_info;
	struct asus_ec *ec;
	u8 tach[EC_NUM_FANS] = { 0 };
	u8 pwm[EC_NUM_FANS] = { 0 };
	u8 temp = 0, mode = 0;
	int ret;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->dev = dev;
	mutex_init(&ec->lock);
	mutex_init(&ec->mode_lock);

	/* We are the i2c_client at 0x5b — adapter comes from there. */
	ec->adapter = client->adapter;
	ec->ec_client = client;

	/*
	 * The companion fan controller at 0x76 is not in DT. Claim it as a
	 * dummy client so no other kernel driver can race this mailbox.
	 */
	ec->fan_client = devm_i2c_new_dummy_device(dev, ec->adapter,
						   FAN_I2C_ADDR);
	if (IS_ERR(ec->fan_client)) {
		dev_err(dev, "failed to claim fan controller at 0x%02x\n",
			FAN_I2C_ADDR);
		return PTR_ERR(ec->fan_client);
	}

	i2c_set_clientdata(client, ec);
	dev_set_drvdata(dev, ec);

	asus_ec_lookup_thermal_zones(ec);

	/* Do not expose writable controls until the complete EC path responds. */
	ret = asus_ec_read_fan_reg(ec, 0, EC_REG_FAN_TACH_MIN, &tach[0]);
	if (ret)
		return dev_err_probe(dev, ret,
				     "EC sanity read failed (left tach)\n");

	ret = asus_ec_read_fan_reg(ec, 1, EC_REG_FAN_TACH_MIN, &tach[1]);
	if (ret)
		return dev_err_probe(dev, ret,
				     "EC sanity read failed (right tach)\n");

	ret = asus_ec_read_fan_reg(ec, 0, EC_REG_PWM_RMIN, &pwm[0]);
	if (ret)
		return dev_err_probe(dev, ret,
				     "EC sanity read failed (left PWM)\n");

	ret = asus_ec_read_fan_reg(ec, 1, EC_REG_PWM_RMIN, &pwm[1]);
	if (ret)
		return dev_err_probe(dev, ret,
				     "EC sanity read failed (right PWM)\n");

	ret = asus_ec_read_reg(ec, EC_REG_TEMP_MAJ, EC_REG_TEMP_MIN, &temp);
	if (ret)
		return dev_err_probe(dev, ret,
				     "EC sanity read failed (temperature)\n");

	ret = asus_ec_read_reg(ec, EC_REG_FAN_MODE_MAJ,
			       EC_REG_FAN_MODE_RMIN, &mode);
	if (ret)
		return dev_err_probe(dev, ret,
				     "EC sanity read failed (fan mode)\n");

	dev_info(dev,
		 "online: left tach=%u (~%u RPM) pwm=%u; right tach=%u (~%u RPM) pwm=%u; temp=%u°C mode=0x%02x zones=%d\n",
		 tach[0], tach[0] * EC_TACH_RPM_MULT, pwm[0],
		 tach[1], tach[1] * EC_TACH_RPM_MULT, pwm[1],
		 temp, mode, ec->n_zones);

	if (mode == EC_FAN_MODE_MANUAL) {
		dev_warn(dev,
			 "EC found in MANUAL mode at probe; forcing AUTO for safety\n");
		(void)asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO);
	}

	ec->hwmon_dev =
		devm_hwmon_device_register_with_info(dev, DRV_NAME, ec, chip_info, NULL);
	if (IS_ERR(ec->hwmon_dev)) {
		ret = PTR_ERR(ec->hwmon_dev);
		dev_err(dev, "hwmon registration failed: %d\n", ret);
		return ret;
	}

	dev_info(dev, "hwmon registered\n");

	/* Initialise profile cache from EC readback (best-effort). */
	{
		u8 v;

		if (!asus_ec_read_reg(ec, EC_REG_PROFILE_MAJ,
				      EC_REG_PROFILE_RMIN, &v) &&
		    v <= EC_PROFILE_MAX)
			ec->profile_cached = v;
		else
			ec->profile_cached = EC_PROFILE_STANDARD;
	}

	/*
	 * Profile sysfs disabled on A14: register (0x01,0x0b) is read-only;
	 * writes via eccw(0x01,0x8b,n) succeed but produce no state change.
	 * Profile appears firmware-controlled (may require ACPI method or is
	 * baked into thermal tables). Vivobook opcode 0x24/0x76 also NACK on A14.
	 * TODO: investigate ACPI WMI methods or Windows driver behavior.
	 */
	dev_info(dev, "profile read-only (fw-controlled); current=%s\n",
		 profile_names[ec->profile_cached]);

	/* Install unconstrained requests before exposing profile controls. */
	asus_ec_freq_qos_init(ec);

	/*
	 * Register platform_profile to map the firmware profiles to fan
	 * and CPU control. Userspace discovers it through
	 * /sys/class/platform-profile/.
	 */
	ec->pp_active = PLATFORM_PROFILE_BALANCED;
	ec->ppdev = devm_platform_profile_register(dev,
						   "asus-zenbook-a14-ec", ec,
						   &asus_ec_pp_ops);
	if (IS_ERR(ec->ppdev)) {
		dev_warn(dev, "platform_profile registration failed: %ld (continuing)\n",
			 PTR_ERR(ec->ppdev));
		ec->ppdev = NULL;
	} else {
		dev_info(dev, "platform_profile registered (quiet/balanced/performance/max-power)\n");
	}

	return 0;
}

static void asus_ec_remove(struct i2c_client *client)
{
	struct asus_ec *ec = i2c_get_clientdata(client);

	if (!ec)
		return;

	/* Always restore auto + stop watchdog before tearing down. */
	mutex_lock(&ec->mode_lock);
	if (ec->manual_active)
		(void)asus_ec_leave_manual(ec);
	else
		asus_ec_stop_watchdog(ec);
	mutex_unlock(&ec->mode_lock);

	/* Removing our requests restores each policy's firmware maximum. */
	(void)asus_ec_freq_qos_set(ec, FREQ_QOS_MAX_DEFAULT_VALUE);
	asus_ec_freq_qos_cleanup(ec);

	/* hwmon_dev and fan_client are devm-managed. */

	dev_info(&client->dev, "removed\n");
}

/* ------------------------------------------------------------------ */
/* PM                                                                 */
/* ------------------------------------------------------------------ */

static int __maybe_unused asus_ec_suspend(struct device *dev)
{
	struct asus_ec *ec = dev_get_drvdata(dev);
	bool was_manual;
	int ret;

	mutex_lock(&ec->mode_lock);
	was_manual = ec->manual_active;
	if (was_manual) {
		/* A14: no watchdog. Just set auto mode before suspend. */
		ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO);
		if (ret)
			dev_warn(dev,
				 "suspend: failed to set auto: %d (proceeding)\n",
				 ret);
		/* asus_ec_stop_watchdog(ec); */
		/* Keep manual_active=true so resume restores it. */
	}
	mutex_unlock(&ec->mode_lock);

	return 0;
}

static int __maybe_unused asus_ec_resume(struct device *dev)
{
	struct asus_ec *ec = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&ec->mode_lock);
	if (ec->manual_active) {
		/* A14: no watchdog, just restore manual mode directly. */
		ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_MANUAL);
		if (ret) {
			dev_err(dev,
				"resume: cannot restore manual (%d); leaving in auto\n",
				ret);
			ec->manual_active = false;
		}
	}
	mutex_unlock(&ec->mode_lock);

	return 0;
}

static SIMPLE_DEV_PM_OPS(asus_ec_pm_ops, asus_ec_suspend, asus_ec_resume);

static const struct of_device_id asus_ec_of_match[] = {
	{ .compatible = "asus,zenbook-a14-ec" },
	{ }
};
MODULE_DEVICE_TABLE(of, asus_ec_of_match);

static const struct i2c_device_id asus_ec_i2c_id[] = {
	{ DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, asus_ec_i2c_id);

static struct i2c_driver asus_ec_driver = {
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= asus_ec_of_match,
		.pm		= &asus_ec_pm_ops,
	},
	.probe		= asus_ec_probe,
	.remove		= asus_ec_remove,
	.id_table	= asus_ec_i2c_id,
};
module_i2c_driver(asus_ec_driver);

MODULE_AUTHOR("Sombre-Osmoze <sombre@osmoze.xyz>");
MODULE_AUTHOR("Ramshouriesh <rshouriesh@gmail.com>");
MODULE_DESCRIPTION("ASUS Zenbook A14 (UX3407QA / UX3407RA) Embedded Controller driver");
MODULE_LICENSE("GPL");
