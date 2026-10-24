#define pr_fmt(fmt) "geekbook_ec: " fmt

#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>

#define NUM_FANS	2
#define EC_RPM(n)	((n) ? 0x9C : 0x9A)
#define EC_TARGET(n)	((n) ? 0xBD : 0xBB)
#define EC_MANUAL(n)	((n) ? 0xA2 : 0xA0)
#define EC_DUTY(n)	((n) ? 0xA3 : 0xA1)

static const u8 temp_reg[] = { 0x79, 0x72 };
static const char * const temp_label[] = { "CPU DTS", "CPU sensor" };
static const char * const fan_label[] = { "Fan 1", "Fan 2" };

static int min_duty = 10;
module_param(min_duty, int, 0444);
MODULE_PARM_DESC(min_duty, "Lowest duty percent that may be set (1-100, default 10)");

static int failsafe_temp = 100;
module_param(failsafe_temp, int, 0644);
MODULE_PARM_DESC(failsafe_temp, "Return fans to firmware control at this CPU temp in degC (default 100)");

static DEFINE_MUTEX(gb_lock);
static struct delayed_work gb_wd;
static struct device *gb_hwdev;
static struct platform_device *gb_pdev;

static int gb_read16(u8 reg, long *val)
{
	u8 hi, lo;
	int ret;

	ret = ec_read(reg, &hi);
	if (ret)
		return ret;
	ret = ec_read(reg + 1, &lo);
	if (ret)
		return ret;
	*val = (hi << 8) | lo;
	return 0;
}

/* The helpers below expect gb_lock to be held. */
static int gb_auto(int fan)
{
	int ret = ec_write(EC_MANUAL(fan), 0);

	if (ret)
		return ret;
	return ec_write(EC_DUTY(fan), 0);
}

static int gb_manual(int fan, int pct)
{
	int ret;

	pct = clamp(pct, min_duty, 100);
	ret = ec_write(EC_MANUAL(fan), 1);
	if (ret)
		return ret;
	return ec_write(EC_DUTY(fan), pct);
}

static void gb_all_auto(void)
{
	int i;

	for (i = 0; i < NUM_FANS; i++)
		gb_auto(i);
}

static umode_t gb_visible(const void *d, enum hwmon_sensor_types t, u32 a, int c)
{
	return t == hwmon_pwm ? 0644 : 0444;
}

static int gb_read(struct device *dev, enum hwmon_sensor_types type,
		   u32 attr, int ch, long *val)
{
	u8 flag, duty, t;
	int ret = -EOPNOTSUPP;

	mutex_lock(&gb_lock);
	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_input)
			ret = gb_read16(EC_RPM(ch), val);
		else if (attr == hwmon_fan_target)
			ret = gb_read16(EC_TARGET(ch), val);
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input) {
			ret = ec_read(temp_reg[ch], &t);
			if (!ret)
				*val = t * 1000;
		}
		break;
	case hwmon_pwm:
		ret = ec_read(EC_MANUAL(ch), &flag);
		if (ret)
			break;
		if (attr == hwmon_pwm_enable) {
			*val = flag ? 1 : 2;	/* 1 = manual, 2 = firmware auto */
		} else if (attr == hwmon_pwm_input) {
			ret = ec_read(EC_DUTY(ch), &duty);
			if (!ret)
				*val = flag ? DIV_ROUND_CLOSEST(duty * 255, 100) : 0;
		} else {
			ret = -EOPNOTSUPP;
		}
		break;
	default:
		break;
	}
	mutex_unlock(&gb_lock);
	return ret;
}

static int gb_read_string(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int ch, const char **str)
{
	if (type == hwmon_temp && attr == hwmon_temp_label)
		*str = temp_label[ch];
	else if (type == hwmon_fan && attr == hwmon_fan_label)
		*str = fan_label[ch];
	else
		return -EOPNOTSUPP;
	return 0;
}

static int gb_write(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int ch, long val)
{
	u8 flag;
	int ret;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	mutex_lock(&gb_lock);
	switch (attr) {
	case hwmon_pwm_input:
		/* Writing a duty switches the fan to manual mode. */
		if (val < 0 || val > 255) {
			ret = -EINVAL;
			break;
		}
		ret = gb_manual(ch, DIV_ROUND_CLOSEST(val * 100, 255));
		break;
	case hwmon_pwm_enable:
		if (val == 2) {			/* firmware control */
			ret = gb_auto(ch);
			break;
		}
		if (val != 0 && val != 1) {
			ret = -EINVAL;
			break;
		}
		ret = ec_read(EC_MANUAL(ch), &flag);
		if (ret)
			break;
		/* 0 = full speed; 1 = manual, start at 100% unless already manual */
		if (val == 0 || !flag)
			ret = gb_manual(ch, 100);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&gb_lock);
	return ret;
}

static const struct hwmon_channel_info * const gb_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_TARGET,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_TARGET),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops gb_ops = {
	.is_visible = gb_visible,
	.read = gb_read,
	.read_string = gb_read_string,
	.write = gb_write,
};

static const struct hwmon_chip_info gb_chip = {
	.ops = &gb_ops,
	.info = gb_info,
};

static void gb_watchdog(struct work_struct *work)
{
	u8 t = 0, flag;
	bool manual = false;
	int i;

	mutex_lock(&gb_lock);
	for (i = 0; i < NUM_FANS; i++)
		if (!ec_read(EC_MANUAL(i), &flag) && flag)
			manual = true;

	/* An unreadable temperature counts as a trip: fall back to firmware. */
	if (manual && (ec_read(temp_reg[0], &t) || t >= failsafe_temp)) {
		gb_all_auto();
		pr_warn("CPU at %u C with manual fan control, handing fans back to firmware\n", t);
	}
	mutex_unlock(&gb_lock);

	schedule_delayed_work(&gb_wd, HZ);
}

static int gb_reboot(struct notifier_block *nb, unsigned long action, void *data)
{
	mutex_lock(&gb_lock);
	gb_all_auto();
	mutex_unlock(&gb_lock);
	return NOTIFY_DONE;
}

static struct notifier_block gb_nb = { .notifier_call = gb_reboot };

static int __init gb_init(void)
{
	int ret;

	if (!dmi_match(DMI_SYS_VENDOR, "GEEKOM") ||
	    !dmi_match(DMI_PRODUCT_NAME, "NM16DM"))
		return -ENODEV;

	min_duty = clamp(min_duty, 1, 100);
	INIT_DELAYED_WORK(&gb_wd, gb_watchdog);

	gb_pdev = platform_device_register_simple("geekbook_ec", -1, NULL, 0);
	if (IS_ERR(gb_pdev))
		return PTR_ERR(gb_pdev);

	gb_hwdev = hwmon_device_register_with_info(&gb_pdev->dev, "geekbook_ec",
						   NULL, &gb_chip, NULL);
	if (IS_ERR(gb_hwdev)) {
		ret = PTR_ERR(gb_hwdev);
		platform_device_unregister(gb_pdev);
		return ret;
	}

	register_reboot_notifier(&gb_nb);
	schedule_delayed_work(&gb_wd, HZ);
	return 0;
}

static void __exit gb_exit(void)
{
	unregister_reboot_notifier(&gb_nb);
	cancel_delayed_work_sync(&gb_wd);
	hwmon_device_unregister(gb_hwdev);

	mutex_lock(&gb_lock);
	gb_all_auto();
	mutex_unlock(&gb_lock);

	platform_device_unregister(gb_pdev);
}

module_init(gb_init);
module_exit(gb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GEEKOM GeekBook M16 EC fan control and temperature monitor");
