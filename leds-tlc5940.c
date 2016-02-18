/*
 * Copyright 2016
 * Jordan Yelloz <jordan@yelloz.me>
 *
 * Based on:
 *  - leds-dac124s085.c by Guennadi Liakhovetski <lg@denx.de>
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License.  See the file COPYING in the main
 * directory of this archive for more details.
 *
 * LED driver for the TLC5940 SPI
 */

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pwm.h>

#include "tlc5940-timing.h"

#define TLC5940_MAX_LEDS   16
#define TLC5940_GS_CHANNEL_WIDTH 12

#define TLC5940_BITS_PER_WORD 12
#define TLC5940_MAX_SPEED_HZ ((u32) (1e6))

struct tlc5940_led {
	struct led_classdev ldev;
	int                 id;
	int                 brightness;
	char                name[sizeof("tlc5940-00")];
	u16                *fb;

	struct mutex       *mutex;
	struct work_struct  work;
	spinlock_t          lock;
};

struct tlc5940 {
	struct tlc5940_led leds[TLC5940_MAX_LEDS];
	u16                fb[TLC5940_MAX_LEDS];
	bool               new_gs_data;
	int                gpio_blank;
	struct hrtimer     timer;
	struct spi_device *spi;
	struct pwm_device *pwm;

	struct mutex       mutex;
};

static inline struct tlc5940*
tlc5940_led_get_tlc5940 (struct tlc5940_led *const led)
{
	/* the first element of leds array */
	struct tlc5940_led *const leds = (
	  led - (led->id * sizeof(struct tlc5940_led))
	);
	return container_of(leds, struct tlc5940, leds[0]);
}

static void
set_new_gs_data(struct tlc5940_led *const led, const bool value)
{
	struct tlc5940 *const tlc = tlc5940_led_get_tlc5940(led);
	tlc->new_gs_data = value;
}

static enum hrtimer_restart
tlc5940_timer_func(struct hrtimer *const timer)
{

	struct tlc5940 *const tlc = container_of(timer, struct tlc5940, timer);
	struct device *const dev = &tlc->spi->dev;
    const int gpio_blank = tlc->gpio_blank;

	if (!gpio_is_valid(gpio_blank)) {
		dev_err(dev, "invalid gpio %d, expiring timer\n", gpio_blank);
		return HRTIMER_NORESTART;
	}

	gpio_set_value(gpio_blank, 1);
	gpio_set_value(gpio_blank, 0);

	hrtimer_forward_now(timer, ktime_set(0, BLANK_PERIOD_NS));

    return HRTIMER_RESTART;

}

static void
tlc5940_led_work(struct work_struct *work)
{
	struct tlc5940_led *led = container_of(work, struct tlc5940_led, work);

	mutex_lock(led->mutex);

	set_new_gs_data (led, 1);

	mutex_unlock(led->mutex);

}

static void
tlc5940_set_brightness(struct led_classdev *const ldev,
					   const enum led_brightness brightness)
{

	struct tlc5940_led *const led = container_of(
	  ldev,
	  struct tlc5940_led,
	  ldev
	);

	u16 *const fb = led->fb;
	const int id = led->id;
	const u16 scaled_brightness = clamp(((u16) brightness) << 4, 0x000, 0xfff);

	spin_lock(&led->lock);
	{
		fb[id] = scaled_brightness;
		schedule_work(&led->work);
	}
	spin_unlock(&led->lock);

}

static int tlc5940_probe(struct spi_device *const spi)
{
	struct device *const dev = &(spi->dev);
	struct device_node *const np = dev->of_node;
	struct tlc5940 *const tlc = devm_kzalloc(
	  &spi->dev,
	  sizeof(struct tlc5940),
	  GFP_KERNEL
	);
	struct hrtimer *const timer = &tlc->timer;
	struct tlc5940_led *led;
	int i, ret;

	if (!tlc)
		return -ENOMEM;

	spi->bits_per_word = TLC5940_BITS_PER_WORD;
	ret = of_get_named_gpio(np, "blank-gpio", 0);
	if (ret < 0) {
		dev_err(dev, "failed to read property `blank-gpio': %d\n", ret);
		return ret;
	}
	tlc->gpio_blank = ret;
	ret = devm_gpio_request(dev, tlc->gpio_blank, "TLC5940 BLANK");
	if (ret) {
		dev_err(dev, "Failed to request BLANK pin:%d\n", ret);
		return ret;
	}
	/* this can be HIGH initially to avoid an initial flicker */
	gpio_direction_output(tlc->gpio_blank, 1);

	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->function = tlc5940_timer_func;
	hrtimer_start(timer, ktime_set(1, 0), HRTIMER_MODE_REL);

	tlc->spi = spi;

	mutex_init(&tlc->mutex);

	for (i = 0; i < TLC5940_MAX_LEDS; i++) {
		led = tlc->leds + i;
		led->id = i;
		led->brightness = LED_OFF;
		led->fb = tlc->fb;
		led->mutex = &tlc->mutex;
		snprintf(led->name, sizeof(led->name), "tlc5940-%d", i);
		spin_lock_init(&led->lock);
		INIT_WORK(&led->work, tlc5940_led_work);
		led->ldev.name = led->name;
		led->ldev.brightness = LED_OFF;
		led->ldev.max_brightness = 0xfff;
		led->ldev.brightness_set = tlc5940_set_brightness;
		ret = led_classdev_register(&spi->dev, &led->ldev);
		if (ret < 0)
			goto eledcr;
	}

	spi_set_drvdata(spi, tlc);

	return 0;

eledcr:
	while (i--)
		led_classdev_unregister(&tlc->leds[i].ldev);

	return ret;
}

static int
tlc5940_remove(struct spi_device *const spi)
{
	struct tlc5940 *const tlc = spi_get_drvdata(spi);
	struct hrtimer *const timer = &tlc->timer;
	struct tlc5940_led *led;
	int i;

	hrtimer_cancel(timer);

	for (i = 0; i < TLC5940_MAX_LEDS; i++) {
		led = &tlc->leds[i];
		led_classdev_unregister(&led->ldev);
		cancel_work_sync(&led->work);
	}

	return 0;
}

static const struct of_device_id tlc5940_dt_ids[] = {
	{
		.compatible = "linux,tlc5940",
	},
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, tlc5940_dt_ids);

static struct spi_driver tlc5940_driver = {
	.probe = tlc5940_probe,
	.remove = tlc5940_remove,
	.driver = {
		.name = "tlc5940",
		.owner = THIS_MODULE,
		.of_match_table = tlc5940_dt_ids,
	},
};

module_spi_driver(tlc5940_driver);

MODULE_AUTHOR("Jordan Yelloz <jordan@yelloz.me");
MODULE_DESCRIPTION("TLC5940 LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:tlc5940");
