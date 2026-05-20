// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS Zenbook A14 (UX3407QA / X1P, UX3407RA / X1E) Embedded Controller
 *
 * Provides hwmon (fan tach, PWM, EC temperature) and platform_profile.
 *
 * Wire format (from osmo'z RA reverse-engineering, mirrors x1e-ec-tool):
 *   - I2C 0x5b register window with opcodes 0x10 (addr) / 0x11 (data).
 *   - "ECCR/ECCW" compound mailbox at major 0xc4: write regsel(0x31),
 *     write data(0x32) for set, kick busy(0x30), poll busy==0.
 *   - Companion fan-controller client at I2C 0x76 (claimed via dummy).
 *
 * QA-specific deltas vs RA (Sombre-Osmoze's driver):
 *   - DT-bound i2c_client (compatible "asus,zenbook-a14-ec") rather than
 *     manual platform_device_register_simple + bus_find_device_by_name.
 *   - Profile set tries the QA DSDT's WEBC(0x11, 1, [byte]) block-write
 *     at 0x76 first; falls back to fan-mode/PWM dressup on NACK so RA
 *     keeps working with the same source.
 *   - Major-allowlist check on every reg/cmd path (0x01 sensors/fan,
 *     0x05 temp, 0xc4 mailbox, 0xc6 sensors window, 0xc9 block window).
 *
 * The "+0x80 destructive" guard on ECCR (refuses any compound-read with
 * sub-register >= 0x80) is osmo'z's idea and is preserved verbatim — it
 * neutralises a whole class of write-aliased read mistakes.
 *
 * Watchdog kthread (RA Vivobook needs it; A14 verified safe without)
 * is kept dormant. See inline notes.
 *
 * Copyright (C) 2026 Sombre-Osmoze <sombre@osmoze.xyz> (RA driver)
 * Copyright (C) 2026 (QA fork, DT/WEBC adaptation)
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_profile.h>
#include <linux/pm.h>
#include <linux/sched.h>
#include <linux/thermal.h>

#define DRV_NAME		"asus_zenbook_a14_ec"

#define EC_I2C_ADDR		0x5b
#define FAN_I2C_ADDR		0x76

/* Raw register-window opcodes */
#define EC_OP_ADDR		0x10
#define EC_OP_DATA		0x11

/* Compound-op mailboxes at major 0xc4 */
#define EC_CC_BUSY		0x30
#define EC_CC_REGSEL		0x31
#define EC_CC_DATA		0x32

#define EC_SETTLE_INTERVAL_US	50000
#define EC_SETTLE_TIMEOUT_MS	2000

/* hwmon registers */
#define EC_REG_FAN_MODE_MAJ	0x01
#define EC_REG_FAN_MODE_RMIN	0x02	/* read:  0=auto, 2=manual */
#define EC_REG_FAN_MODE_WMIN	0x82	/* write: 0=auto, 2=manual */
#define EC_REG_FAN_TACH_MAJ	0x01
#define EC_REG_FAN_TACH_MIN	0x09	/* RPM = value × 88 */
#define EC_REG_PWM_MAJ		0x01
#define EC_REG_PWM_RMIN		0x0a
#define EC_REG_PWM_WMIN		0x8a
#define EC_REG_FAN_SEL_WMIN	0x8c	/* fan-id selector */
#define EC_REG_TEMP_MAJ		0x05
#define EC_REG_TEMP_MIN		0x02	/* signed °C */
#define EC_REG_TEMP2_MIN	0x01	/* DSDT shows a second channel; best-effort */

#define EC_TACH_RPM_MULT	88
#define EC_PWM_SPIN_FLOOR	0x4b	/* 75; fan stalls below */

#define EC_FAN_MODE_AUTO	0
#define EC_FAN_MODE_MANUAL	2

/* 0x76 fan-controller opcodes */
#define FAN_OP_PUSH_TEMP	0x20	/* [0x20, 0x01, 0x02, lo, hi] */
#define FAN_OP_VIVOBOOK_PROFILE	0x24	/* [0x24, idx]; NACKs on A14 per RE */

/*
 * QA DSDT WEBC mailbox profile path. The ACPI _WEBC method writes a
 * profile byte through the 0xc9 block window with command 0x11; the
 * payload is one of 0x01 balanced, 0x02 quiet, 0x04 performance,
 * 0x10 max-power. We try this first before falling back to fan dressup.
 */
#define EC_BLK_MAJ		0xc9
#define EC_BLK_DATA0		0x40	/* payload byte 0 */
#define EC_BLK_BMCR		0x6f	/* busy/kick register */
#define EC_BLK_CMD		0x6e	/* command register */
#define EC_BLK_BMCR_BUSY	0x40
#define EC_BLK_BMCR_KICK	0x80
#define EC_BLK_PRE_KICK_TRIES	200
#define EC_BLK_PRE_KICK_INTERVAL_US 100

#define WEBC_CMD_PROFILE	0x11

/* Profile byte vocabulary on the wire (matches QA DSDT). */
#define PROFILE_BYTE_BALANCED	0x01
#define PROFILE_BYTE_QUIET	0x02
#define PROFILE_BYTE_PERFORMANCE 0x04
#define PROFILE_BYTE_MAX_POWER	0x10

/* Watchdog (Vivobook only — A14 doesn't need it). */
#define WATCHDOG_PERIOD_MS	2000

#define ASUS_EC_MAX_ZONES	4
static const char * const asus_ec_thermal_zones[] = {
	"cpu0-0-top-thermal",
	"cpu1-0-top-thermal",
	"cpu2-0-top-thermal",
	"gpuss-0-thermal",
};

/* PWM applied in performance fallback when WEBC profile path is unavailable. */
#define FALLBACK_PERF_PWM	180
#define FALLBACK_MAX_POWER_PWM	69

struct asus_ec {
	struct device		*dev;
	struct i2c_client	*ec_client;	/* 0x5b */
	struct i2c_client	*fan_client;	/* 0x76, claimed via dummy */
	struct device		*hwmon_dev;
	struct mutex		bus_lock;	/* serialises EC compound access */

	struct task_struct	*watchdog_task;
	struct mutex		mode_lock;
	bool			manual_active;
	struct thermal_zone_device *zones[ASUS_EC_MAX_ZONES];
	int			n_zones;

	enum platform_profile_option pp_active;
	bool			webc_profile_works;	/* set after first OK */
	bool			webc_profile_proven_bad; /* set after a NACK */

	u8			tx[3];	/* DMA-safe scratch */
	u8			rx[1];
};

static bool ec_major_allowed(u8 maj)
{
	return maj == 0x01 || maj == 0x05 ||
	       maj == 0xc4 || maj == 0xc6 || maj == 0xc9;
}

/* ------------------------------------------------------------------ */
/* EC primitives — caller MUST hold ec->bus_lock                      */
/* ------------------------------------------------------------------ */

static int __ec_rb(struct asus_ec *ec, u8 maj, u8 min, u8 *out)
{
	struct i2c_msg msgs[3];
	static const u8 op_data = EC_OP_DATA;
	int ret;

	if (!ec_major_allowed(maj))
		return -EINVAL;

	ec->tx[0] = EC_OP_ADDR;
	ec->tx[1] = maj;
	ec->tx[2] = min;

	msgs[0].addr  = ec->ec_client->addr;
	msgs[0].flags = 0;
	msgs[0].len   = 3;
	msgs[0].buf   = ec->tx;

	msgs[1].addr  = ec->ec_client->addr;
	msgs[1].flags = 0;
	msgs[1].len   = 1;
	msgs[1].buf   = (u8 *)&op_data;

	msgs[2].addr  = ec->ec_client->addr;
	msgs[2].flags = I2C_M_RD;
	msgs[2].len   = 1;
	msgs[2].buf   = ec->rx;

	ret = i2c_transfer(ec->ec_client->adapter, msgs, 3);
	if (ret < 0)
		return ret;
	if (ret != 3)
		return -EIO;

	*out = ec->rx[0];
	return 0;
}

static int __ec_wb(struct asus_ec *ec, u8 maj, u8 min, u8 val)
{
	struct i2c_msg msgs[2];
	u8 data[2];
	int ret;

	if (!ec_major_allowed(maj))
		return -EINVAL;

	ec->tx[0] = EC_OP_ADDR;
	ec->tx[1] = maj;
	ec->tx[2] = min;

	data[0] = EC_OP_DATA;
	data[1] = val;

	msgs[0].addr  = ec->ec_client->addr;
	msgs[0].flags = 0;
	msgs[0].len   = 3;
	msgs[0].buf   = ec->tx;

	msgs[1].addr  = ec->ec_client->addr;
	msgs[1].flags = 0;
	msgs[1].len   = 2;
	msgs[1].buf   = data;

	ret = i2c_transfer(ec->ec_client->adapter, msgs, 2);
	if (ret < 0)
		return ret;
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

	/* Write-aliased subregisters (>= 0x80) must never be read; doing so
	 * has historically wedged the EC. Reject early.
	 */
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

	mutex_lock(&ec->bus_lock);
	ret = __ec_cr(ec, maj, min, out);
	mutex_unlock(&ec->bus_lock);
	return ret;
}

static int asus_ec_write_reg(struct asus_ec *ec, u8 maj, u8 min, u8 val)
{
	int ret;

	mutex_lock(&ec->bus_lock);
	ret = __ec_cw(ec, maj, min, val);
	mutex_unlock(&ec->bus_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* WEBC block-write profile path (QA DSDT)                            */
/* ------------------------------------------------------------------ */

/*
 * Mirrors the QA DSDT WEBC sequence (drop the post-kick wait that
 * deadlocked an earlier attempt — DSDT polls BMCR after the kick;
 * we don't have to, the EC absorbs the command asynchronously).
 *
 * Returns 0 on success, -EBUSY if the pre-kick busy poll never cleared,
 * any other errno from the underlying register writes.
 */
static int asus_ec_webc(struct asus_ec *ec, u8 cmd, const u8 *buf, size_t len)
{
	u8 bmcr = 0;
	int ret, i;

	if (len > (EC_BLK_BMCR - EC_BLK_DATA0))
		return -EINVAL;

	mutex_lock(&ec->bus_lock);

	/* Pre-kick: BMCR must be zero before we can stuff the payload. */
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
		/* Set the abort bit and let the EC recover. Don't wait. */
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

	ret = __ec_wb(ec, EC_BLK_MAJ, EC_BLK_BMCR,
		      bmcr | EC_BLK_BMCR_KICK);
	if (ret)
		goto out;

	ret = __ec_wb(ec, EC_BLK_MAJ, EC_BLK_CMD, cmd);
	/* Fire and forget — releasing the mutex before the EC drains. */

out:
	mutex_unlock(&ec->bus_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Fan-controller (0x76) writes                                       */
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

/* ------------------------------------------------------------------ */
/* High-level fan helpers                                             */
/* ------------------------------------------------------------------ */

static int asus_ec_set_fan_mode(struct asus_ec *ec, u8 mode)
{
	return asus_ec_write_reg(ec, EC_REG_FAN_MODE_MAJ,
				 EC_REG_FAN_MODE_WMIN, mode);
}

static int asus_ec_set_pwm(struct asus_ec *ec, u8 speed)
{
	int ret;

	mutex_lock(&ec->bus_lock);
	ret = __ec_cw(ec, EC_REG_PWM_MAJ, EC_REG_FAN_SEL_WMIN, 0);
	if (ret)
		goto out;
	ret = __ec_cw(ec, EC_REG_PWM_MAJ, EC_REG_PWM_WMIN, speed);
out:
	mutex_unlock(&ec->bus_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* platform_profile                                                   */
/* ------------------------------------------------------------------ */

static u8 profile_to_byte(enum platform_profile_option p)
{
	switch (p) {
	case PLATFORM_PROFILE_QUIET:		return PROFILE_BYTE_QUIET;
	case PLATFORM_PROFILE_BALANCED:		return PROFILE_BYTE_BALANCED;
	case PLATFORM_PROFILE_PERFORMANCE:	return PROFILE_BYTE_PERFORMANCE;
	case PLATFORM_PROFILE_MAX_POWER:	return PROFILE_BYTE_MAX_POWER;
	default:				return 0;
	}
}

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

/*
 * Profile dressup fallback: when WEBC NACKs we cannot move the EC's
 * own thermal table, so we approximate by toggling fan auto/manual and
 * a fixed PWM on "performance".
 */
static int asus_ec_pp_fallback(struct asus_ec *ec,
			       enum platform_profile_option profile)
{
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_QUIET:
	case PLATFORM_PROFILE_BALANCED:
		if (ec->manual_active) {
			ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO);
			if (ret)
				return ret;
			ec->manual_active = false;
		}
		return 0;

	case PLATFORM_PROFILE_PERFORMANCE:
		if (!ec->manual_active) {
			ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_MANUAL);
			if (ret)
				return ret;
			ec->manual_active = true;
		}
		return asus_ec_set_pwm(ec, FALLBACK_PERF_PWM);

	case PLATFORM_PROFILE_MAX_POWER:
		if (!ec->manual_active) {
			ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_MANUAL);
			if (ret)
				return ret;
			ec->manual_active = true;
		}
		return asus_ec_set_pwm(ec, FALLBACK_MAX_POWER_PWM);

	default:
		return -EOPNOTSUPP;
	}
}

static int asus_ec_pp_set(struct device *dev,
			  enum platform_profile_option profile)
{
	struct asus_ec *ec = dev_get_drvdata(dev);
	u8 byte;
	int ret = -EOPNOTSUPP;

	byte = profile_to_byte(profile);
	if (!byte)
		return -EOPNOTSUPP;

	mutex_lock(&ec->mode_lock);

	/*
	 * 1) Try the QA DSDT WEBC path. RA's NACK is sticky once we've
	 *    seen it; afterwards we go straight to the fallback so we
	 *    don't keep poking a dead path.
	 */
	if (!ec->webc_profile_proven_bad) {
		ret = asus_ec_webc(ec, WEBC_CMD_PROFILE, &byte, 1);
		if (!ret) {
			ec->webc_profile_works = true;
			/* If we previously fell back to manual, restore auto
			 * so the EC's own thermal table takes over. */
			if (ec->manual_active) {
				int r2 = asus_ec_set_fan_mode(ec,
							      EC_FAN_MODE_AUTO);
				if (!r2)
					ec->manual_active = false;
			}
			goto done;
		}
		/* ret is errno from i2c_transfer; treat any failure as
		 * "this platform doesn't speak WEBC profile" once we've
		 * confirmed the EC is otherwise alive (we did sanity reads
		 * at probe). Stop trying. */
		dev_info(dev,
			 "WEBC profile path NACK'd (%d); falling back to fan dressup\n",
			 ret);
		ec->webc_profile_proven_bad = true;
	}

	/* 2) Fallback. */
	ret = asus_ec_pp_fallback(ec, profile);

done:
	if (!ret)
		ec->pp_active = profile;
	mutex_unlock(&ec->mode_lock);
	return ret;
}

static const struct platform_profile_ops asus_ec_pp_ops = {
	.probe       = asus_ec_pp_probe,
	.profile_get = asus_ec_pp_get,
	.profile_set = asus_ec_pp_set,
};

/* ------------------------------------------------------------------ */
/* Thermal zones + (dormant) watchdog                                 */
/* ------------------------------------------------------------------ */

static void asus_ec_lookup_thermal_zones(struct asus_ec *ec)
{
	int i;

	ec->n_zones = 0;
	for (i = 0; i < ARRAY_SIZE(asus_ec_thermal_zones); i++) {
		struct thermal_zone_device *tz;

		tz = thermal_zone_get_zone_by_name(asus_ec_thermal_zones[i]);
		if (IS_ERR(tz))
			continue;
		ec->zones[ec->n_zones++] = tz;
	}
}

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
		u8 v;

		if (!asus_ec_read_reg(ec, EC_REG_TEMP_MAJ,
				      EC_REG_TEMP_MIN, &v))
			max = (s8)v * 1000;
	}

	return max;
}

static int asus_ec_send_current_temp(struct asus_ec *ec)
{
	int mc = asus_ec_max_temp_mc(ec);
	u16 dc;

	if (mc < 0)
		return -ENODATA;

	dc = (u16)clamp(mc / 100, 0, 2000);
	return fan_send_temp_dc(ec, dc);
}

/*
 * Watchdog kthread.
 *
 * Vivobook S 15 hard-resets after ~2 min of manual mode without a temp
 * push to (0x76, 0x20). A14 was empirically verified safe (3+ min in
 * manual with no feed = no reboot). Code is kept around in case a
 * future variant flips back to needing it; never started on A14.
 */
static int __maybe_unused asus_ec_watchdog_fn(void *data)
{
	struct asus_ec *ec = data;

	while (!kthread_should_stop()) {
		(void)asus_ec_send_current_temp(ec);
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			__set_current_state(TASK_RUNNING);
			break;
		}
		schedule_timeout(msecs_to_jiffies(WATCHDOG_PERIOD_MS));
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* hwmon                                                              */
/* ------------------------------------------------------------------ */

static umode_t asus_ec_hwmon_is_visible(const void *drvdata,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
			return 0444;
		default:
			return 0;
		}
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;
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
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		ret = asus_ec_read_reg(ec, EC_REG_FAN_TACH_MAJ,
				       EC_REG_FAN_TACH_MIN, &v);
		if (ret)
			return ret;
		*val = (long)v * EC_TACH_RPM_MULT;
		return 0;

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			ret = asus_ec_read_reg(ec, EC_REG_PWM_MAJ,
					       EC_REG_PWM_RMIN, &v);
			if (ret)
				return ret;
			*val = v;
			return 0;
		case hwmon_pwm_enable:
			ret = asus_ec_read_reg(ec, EC_REG_FAN_MODE_MAJ,
					       EC_REG_FAN_MODE_RMIN, &v);
			if (ret)
				return ret;
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

	case hwmon_temp: {
		u8 sub = (channel == 0) ? EC_REG_TEMP_MIN : EC_REG_TEMP2_MIN;

		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		ret = asus_ec_read_reg(ec, EC_REG_TEMP_MAJ, sub, &v);
		if (ret)
			return ret;
		*val = (long)(s8)v * 1000;
		return 0;
	}

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
		mutex_lock(&ec->mode_lock);
		switch (val) {
		case 1:
			if (ec->manual_active) {
				ret = 0;
			} else {
				ret = asus_ec_set_fan_mode(ec,
							   EC_FAN_MODE_MANUAL);
				if (!ret)
					ec->manual_active = true;
			}
			break;
		case 2:
			if (!ec->manual_active) {
				ret = 0;
			} else {
				ret = asus_ec_set_fan_mode(ec,
							   EC_FAN_MODE_AUTO);
				if (!ret)
					ec->manual_active = false;
			}
			break;
		default:
			ret = -EINVAL;
		}
		mutex_unlock(&ec->mode_lock);
		return ret;

	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;
		speed = (u8)val;

		mutex_lock(&ec->mode_lock);
		if (!ec->manual_active) {
			mutex_unlock(&ec->mode_lock);
			return -EBUSY;	/* set pwm1_enable=1 first */
		}
		ret = asus_ec_set_pwm(ec, speed);
		mutex_unlock(&ec->mode_lock);
		if (ret)
			return ret;

		if (speed > 0 && speed < EC_PWM_SPIN_FLOOR)
			dev_info_ratelimited(ec->dev,
				"pwm=%u below spin floor (%u); fan likely idle\n",
				speed, EC_PWM_SPIN_FLOOR);
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static int asus_ec_hwmon_read_string(struct device *dev,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel,
				     const char **str)
{
	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_label) {
			*str = "fan";
			return 0;
		}
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_label) {
			*str = (channel == 0) ? "ec" : "ec2";
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
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_chip_info asus_ec_hwmon_chip_info = {
	.ops	= &asus_ec_hwmon_ops,
	.info	= asus_ec_hwmon_info,
};

/* ------------------------------------------------------------------ */
/* Probe / remove                                                     */
/* ------------------------------------------------------------------ */

static int asus_ec_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct asus_ec *ec;
	struct device *ppdev;
	u8 tach, pwm, temp, mode;
	int ret;

	if (client->addr != EC_I2C_ADDR)
		return dev_err_probe(dev, -EINVAL,
				     "EC must bind at 0x%02x, got 0x%02x\n",
				     EC_I2C_ADDR, client->addr);

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->dev = dev;
	ec->ec_client = client;
	mutex_init(&ec->bus_lock);
	mutex_init(&ec->mode_lock);
	ec->pp_active = PLATFORM_PROFILE_BALANCED;

	ec->fan_client = devm_i2c_new_dummy_device(dev, client->adapter,
						   FAN_I2C_ADDR);
	if (IS_ERR(ec->fan_client))
		return dev_err_probe(dev, PTR_ERR(ec->fan_client),
				     "cannot claim fan controller at 0x%02x\n",
				     FAN_I2C_ADDR);

	i2c_set_clientdata(client, ec);
	dev_set_drvdata(dev, ec);

	asus_ec_lookup_thermal_zones(ec);

	/* Probe-time sanity reads — must succeed before we let userspace touch
	 * anything. */
	ret = asus_ec_read_reg(ec, EC_REG_FAN_TACH_MAJ,
			       EC_REG_FAN_TACH_MIN, &tach);
	if (ret)
		return dev_err_probe(dev, ret,
				     "EC sanity read (fan tach) failed\n");
	(void)asus_ec_read_reg(ec, EC_REG_PWM_MAJ, EC_REG_PWM_RMIN, &pwm);
	(void)asus_ec_read_reg(ec, EC_REG_TEMP_MAJ, EC_REG_TEMP_MIN, &temp);
	(void)asus_ec_read_reg(ec, EC_REG_FAN_MODE_MAJ,
			       EC_REG_FAN_MODE_RMIN, &mode);

	dev_info(dev,
		 "online: tach=%u (~%u RPM) pwm=%u temp=%d°C mode=0x%02x zones=%d\n",
		 tach, tach * EC_TACH_RPM_MULT, pwm, (int)(s8)temp,
		 mode, ec->n_zones);

	if (mode == EC_FAN_MODE_MANUAL) {
		dev_warn(dev,
			 "EC found in MANUAL mode at probe; forcing AUTO\n");
		(void)asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO);
	}

	ec->hwmon_dev = devm_hwmon_device_register_with_info(dev,
				DRV_NAME, ec,
				&asus_ec_hwmon_chip_info, NULL);
	if (IS_ERR(ec->hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(ec->hwmon_dev),
				     "hwmon registration failed\n");

	ppdev = devm_platform_profile_register(dev, "asus-zenbook-a14",
					       ec, &asus_ec_pp_ops);
	if (IS_ERR(ppdev))
		dev_warn(dev,
			 "platform_profile registration failed: %ld (continuing)\n",
			 PTR_ERR(ppdev));
	else
		dev_info(dev,
			 "platform_profile registered (quiet/balanced/performance/max-power)\n");

	return 0;
}

static void asus_ec_remove(struct i2c_client *client)
{
	struct asus_ec *ec = i2c_get_clientdata(client);

	if (!ec)
		return;

	mutex_lock(&ec->mode_lock);
	if (ec->manual_active) {
		(void)asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO);
		ec->manual_active = false;
	}
	mutex_unlock(&ec->mode_lock);
}

/* ------------------------------------------------------------------ */
/* PM                                                                 */
/* ------------------------------------------------------------------ */

static int asus_ec_suspend(struct device *dev)
{
	struct asus_ec *ec = dev_get_drvdata(dev);

	mutex_lock(&ec->mode_lock);
	if (ec->manual_active) {
		int ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_AUTO);

		if (ret)
			dev_warn(dev,
				 "suspend: failed to set auto: %d (proceeding)\n",
				 ret);
		/* Keep manual_active=true so resume restores it. */
	}
	mutex_unlock(&ec->mode_lock);
	return 0;
}

static int asus_ec_resume(struct device *dev)
{
	struct asus_ec *ec = dev_get_drvdata(dev);

	mutex_lock(&ec->mode_lock);
	if (ec->manual_active) {
		int ret = asus_ec_set_fan_mode(ec, EC_FAN_MODE_MANUAL);

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

static DEFINE_SIMPLE_DEV_PM_OPS(asus_ec_pm_ops,
				asus_ec_suspend, asus_ec_resume);

static const struct of_device_id asus_ec_of_match[] = {
	{ .compatible = "asus,zenbook-a14-ec" },
	{ }
};
MODULE_DEVICE_TABLE(of, asus_ec_of_match);

static const struct i2c_device_id asus_ec_i2c_id[] = {
	{ "asus-zenbook-a14-ec" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, asus_ec_i2c_id);

static struct i2c_driver asus_ec_driver = {
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= asus_ec_of_match,
		.pm		= pm_sleep_ptr(&asus_ec_pm_ops),
	},
	.probe		= asus_ec_probe,
	.remove		= asus_ec_remove,
	.id_table	= asus_ec_i2c_id,
};
module_i2c_driver(asus_ec_driver);

MODULE_AUTHOR("Sombre-Osmoze <sombre@osmoze.xyz>");
MODULE_DESCRIPTION("ASUS Zenbook A14 (UX3407QA / UX3407RA) Embedded Controller driver");
MODULE_LICENSE("GPL v2");
