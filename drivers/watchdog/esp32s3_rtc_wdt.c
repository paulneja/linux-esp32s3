// SPDX-License-Identifier: GPL-2.0
/*
 * ESP32-S3 RTC watchdog (RWDT). The firmware on the other core keeps the two
 * timer-group watchdogs for itself; the RTC one is left off by the
 * bootloader and resets the whole chip, both cores, when it runs out.
 */
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/watchdog.h>

#define STORE1		0x54	/* the firmware's RTC_SLOW_CLK_CAL_REG */
#define WDTCONFIG0	0x98
#define WDTCONFIG1	0x9c
#define WDTFEED		0xac
#define WDTWPROTECT	0xb0

#define WDT_EN		BIT(31)
#define WDT_STG0(a)	((a) << 28)
#define WDT_CPU_RESET_LEN(l)	((l) << 16)
#define WDT_SYS_RESET_LEN(l)	((l) << 13)
#define WDT_FEED	BIT(31)
#define WDT_WKEY	0x50d83aa1

#define STG_RESET_RTC	4
#define RESET_LEN_3_2US	7
/* RC_SLOW is only roughly 136 kHz; the firmware measures it at boot */
#define SLOW_CLK_HZ	136000

struct esp32s3_wdt {
	struct watchdog_device wdd;
	struct regmap *rtc;
	u32 hz;
};

static void unlocked(struct esp32s3_wdt *w, unsigned int reg, unsigned int val)
{
	regmap_write(w->rtc, WDTWPROTECT, WDT_WKEY);
	regmap_write(w->rtc, reg, val);
	regmap_write(w->rtc, WDTWPROTECT, 0);
}

static int esp32s3_wdt_ping(struct watchdog_device *wdd)
{
	unlocked(watchdog_get_drvdata(wdd), WDTFEED, WDT_FEED);
	return 0;
}

static int esp32s3_wdt_start(struct watchdog_device *wdd)
{
	struct esp32s3_wdt *w = watchdog_get_drvdata(wdd);

	regmap_write(w->rtc, WDTWPROTECT, WDT_WKEY);
	regmap_write(w->rtc, WDTCONFIG0, 0);
	/* stage 0 counts two slow clock cycles per step, as ESP-IDF's ticks >> 1 */
	regmap_write(w->rtc, WDTCONFIG1, wdd->timeout * (w->hz / 2));
	regmap_write(w->rtc, WDTFEED, WDT_FEED);
	regmap_write(w->rtc, WDTCONFIG0, WDT_EN | WDT_STG0(STG_RESET_RTC) |
		     WDT_CPU_RESET_LEN(RESET_LEN_3_2US) | WDT_SYS_RESET_LEN(RESET_LEN_3_2US));
	regmap_write(w->rtc, WDTWPROTECT, 0);
	return 0;
}

static int esp32s3_wdt_stop(struct watchdog_device *wdd)
{
	unlocked(watchdog_get_drvdata(wdd), WDTCONFIG0, 0);
	return 0;
}

static int esp32s3_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{
	wdd->timeout = t;
	if (watchdog_active(wdd))
		return esp32s3_wdt_start(wdd);
	return 0;
}

static const struct watchdog_info esp32s3_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "ESP32-S3 RTC watchdog",
};

static const struct watchdog_ops esp32s3_wdt_ops = {
	.owner = THIS_MODULE,
	.start = esp32s3_wdt_start,
	.stop = esp32s3_wdt_stop,
	.ping = esp32s3_wdt_ping,
	.set_timeout = esp32s3_wdt_set_timeout,
};

static int esp32s3_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s3_wdt *w;
	unsigned int cfg;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->rtc = syscon_regmap_lookup_by_phandle(dev->of_node, "regmap");
	if (IS_ERR(w->rtc))
		return dev_err_probe(dev, PTR_ERR(w->rtc), "no rtc regmap\n");

	/* microseconds per tick, 19 fraction bits; 0 or nonsense: the nominal */
	regmap_read(w->rtc, STORE1, &cfg);
	w->hz = cfg ? div_u64(1000000ULL << 19, cfg) : 0;
	if (w->hz < 10000 || w->hz > 200000)
		w->hz = SLOW_CLK_HZ;

	w->wdd.info = &esp32s3_wdt_info;
	w->wdd.ops = &esp32s3_wdt_ops;
	w->wdd.parent = dev;
	w->wdd.min_timeout = 1;
	w->wdd.max_timeout = U32_MAX / (w->hz / 2);
	w->wdd.timeout = 30;
	watchdog_init_timeout(&w->wdd, 0, dev);
	dev_info(dev, "slow clock %u Hz, %u s\n", w->hz, w->wdd.timeout);
	watchdog_set_drvdata(&w->wdd, w);
	watchdog_stop_on_reboot(&w->wdd);

	/* left running from before (a kexec, or a bootloader that kept it on) */
	regmap_read(w->rtc, WDTCONFIG0, &cfg);
	if (cfg & WDT_EN) {
		esp32s3_wdt_start(&w->wdd);
		set_bit(WDOG_HW_RUNNING, &w->wdd.status);
	}
	return devm_watchdog_register_device(dev, &w->wdd);
}

static const struct of_device_id esp32s3_wdt_of_match[] = {
	{ .compatible = "esp,esp32s3-rtc-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s3_wdt_of_match);

static struct platform_driver esp32s3_wdt_driver = {
	.probe = esp32s3_wdt_probe,
	.driver = {
		.name = "esp32s3-rtc-wdt",
		.of_match_table = esp32s3_wdt_of_match,
	},
};
module_platform_driver(esp32s3_wdt_driver);

MODULE_DESCRIPTION("ESP32-S3 RTC watchdog");
MODULE_LICENSE("GPL");
