/*
 * Copyright 2016
 * Jordan Yelloz <jordan@yelloz.me>
 *
 * Based on:
 *  - leds-dac124s085.c by Guennadi Liakhovetski <lg@denx.de>
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License. See the file LICENSE in the main
 * directory of this archive for more details.
 *
 * LED driver for the TLC5940 SPI LED Controller
 */

#include <linux/leds.h>
#include <linux/module.h>
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

#define TLC5940_GSCLK_SPEED_HZ  2500000
#define TLC5940_GSCLK_PERIOD_NS ((unsigned long) (1e9 / TLC5940_GSCLK_SPEED_HZ))
#define TLC5940_GSCLK_DUTY_CYCLE_NS (TLC5940_GSCLK_PERIOD_NS / 2)
#define TLC5940_BLANK_PERIOD_NS (4096 * TLC5940_GSCLK_PERIOD_NS)

#define TLC5940_MAX_LEDS   16
#define TLC5940_GS_CHANNEL_WIDTH 12

#define TLC5940_BITS_PER_WORD 8
#define TLC5940_MAX_SPEED_HZ ((u32) (1e6))

#define TLC5940_FB_SIZE_BITS ((TLC5940_MAX_LEDS) * TLC5940_GS_CHANNEL_WIDTH)
#define TLC5940_FB_SIZE (TLC5940_FB_SIZE_BITS >> 3)

#define GS_DUO(a, b)			((a) >> 4), ((a) << 4) | ((b) >> 8), (b)
#define DC_QUARTET(a, b, c, d)	((a) << 2) | ((b) >> 4), \
								((b) << 4) | ((c) >> 2), \
								((c) << 6) | (d)

#define FB_OFFSET_BITS(__led)   ( \
								  (TLC5940_FB_SIZE_BITS) - \
								  (TLC5940_GS_CHANNEL_WIDTH * ((__led) + 1)) \
								)
#define FB_OFFSET(__led)        (FB_OFFSET_BITS(__led) >> 3)

struct tlc5940_led {
	struct led_classdev ldev;
	int                 id;
	int                 brightness;
	const char         *name;
	struct tlc5940     *tlc;

	spinlock_t          lock;
};

struct tlc5940 {
	struct tlc5940_led  leds[TLC5940_MAX_LEDS];
	u8                  fb[TLC5940_FB_SIZE];
	bool                new_gs_data;

	int                 gpio_blank;
	struct hrtimer      timer;

	struct work_struct  work;
	struct spi_device  *spi;
	struct pwm_device  *pwm;

};

static enum hrtimer_restart
tlc5940_timer_func(struct hrtimer *const timer)
{

	struct tlc5940 *const tlc = container_of(timer, struct tlc5940, timer);
	struct spi_device *const spi = tlc->spi;
	struct device *const dev = &spi->dev;
	const int gpio_blank = tlc->gpio_blank;

	hrtimer_forward_now(timer, ktime_set(0, TLC5940_BLANK_PERIOD_NS));

	if (!gpio_is_valid(gpio_blank)) {
		dev_err(dev, "invalid gpio %d, expiring timer\n", gpio_blank);
		return HRTIMER_NORESTART;
	}

	gpio_set_value(gpio_blank, 1);
	gpio_set_value(gpio_blank, 0);

	if (tlc->new_gs_data) {
		schedule_work(&tlc->work);
	}

	return HRTIMER_RESTART;

}

static void
tlc5940_update_fb(struct tlc5940 *const tlc)
{

	u8 *const fb = &(tlc->fb[0]);
	int id;

	for (id = 0; id < TLC5940_MAX_LEDS; id++) {

		struct tlc5940_led *const led = &(tlc->leds[id]);

		const u16 brightness = led->brightness & 0xfff;
		const u8 offset = FB_OFFSET(id);
		const u8 mid_byte = id % 2 == 0;

		if (mid_byte) {
			fb[offset] = (fb[offset] & 0xf0) | brightness >> 8;
			fb[offset + 1] = brightness & 0xff;
		} else {
			fb[offset] = brightness >> 4;
			fb[offset + 1] = (fb[offset + 1] & 0x0f) | ((brightness << 4 & 0xf0));
		}

	}

}

static void
tlc5940_work(struct work_struct *const work)
{

	struct tlc5940 *const tlc = container_of(work, struct tlc5940, work);
	struct spi_device *const spi = tlc->spi;
	struct device *const dev = &spi->dev;
	u8 *const fb = &(tlc->fb[0]);
	int ret;

	tlc5940_update_fb(tlc);

	ret = spi_write(spi, fb, TLC5940_FB_SIZE);

	if (ret) {
		dev_err(dev, "spi transfer error: %d\n", ret);
		return;
	}

	tlc->new_gs_data = 0;

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

	led->tlc->new_gs_data = 1;

	spin_lock(&led->lock);
	{
		led->brightness = brightness;
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
	struct work_struct *const work = &tlc->work;
	struct pwm_device *pwm;
	struct tlc5940_led *led;
	struct device_node *child;
	int i, ret;

	if (!tlc) {
		return -ENOMEM;
	}

	spi->bits_per_word = TLC5940_BITS_PER_WORD;
	spi->max_speed_hz = TLC5940_MAX_SPEED_HZ;

	ret = of_get_named_gpio(np, "blank-gpio", 0);
	if (ret < 0) {
		dev_err(dev, "failed to read property `blank-gpio': %d\n", ret);
		return ret;
	}
	tlc->gpio_blank = ret;
	ret = devm_gpio_request(dev, tlc->gpio_blank, "TLC5940 BLANK");
	if (ret) {
		dev_err(dev, "failed to request BLANK pin: %d\n", ret);
		return ret;
	}
	/* this can be HIGH initially to avoid any startup flicker */
	ret = gpio_direction_output(tlc->gpio_blank, 1);
	if (ret) {
		dev_err(dev, "failed to configure BLANK pin for output: %d\n", ret);
		return ret;
	}

	pwm = devm_of_pwm_get(dev, np, NULL);
	if (IS_ERR(pwm)) {
		ret = PTR_ERR(pwm);
		dev_err(dev, "failed to get GSCLK PWM pin: %d\n", ret);
		return ret;
	}

	ret = pwm_config(pwm, TLC5940_GSCLK_DUTY_CYCLE_NS, TLC5940_GSCLK_PERIOD_NS);
	if (ret) {
		dev_err(
		  dev,
		  "failed to configure pwm with period %ld, duty cycle %ld: %d\n",
		  TLC5940_GSCLK_PERIOD_NS,
		  TLC5940_GSCLK_DUTY_CYCLE_NS,
		  ret
		);
		return ret;
	}

	pwm_enable(pwm);

	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->function = tlc5940_timer_func;
	hrtimer_start(timer, ktime_set(1, 0), HRTIMER_MODE_REL);

	INIT_WORK(work, tlc5940_work);

	tlc->new_gs_data = 1;

	tlc->spi = spi;
	tlc->pwm = pwm;

	i = 0;
	for_each_child_of_node(np, child) {
		led = &(tlc->leds[i]);
		led->name = of_get_property(child, "label", NULL) ? : child->name;
		led->id = i;
		led->tlc = tlc;
		led->brightness = LED_OFF;
		spin_lock_init(&led->lock);
		led->ldev.name = led->name;
		led->ldev.brightness = LED_OFF;
		led->ldev.max_brightness = 0xfff;
		led->ldev.brightness_set = tlc5940_set_brightness;
		ret = led_classdev_register(dev, &led->ldev);
		if (ret < 0)
			goto eledcr;
		i++;
	}

	spi_set_drvdata(spi, tlc);

	return 0;

eledcr:
	dev_err(dev, "failed to set up child LED #%d: %d\n", i, ret);
	while (i--)
		led_classdev_unregister(&tlc->leds[i].ldev);

	return ret;
}

static int
tlc5940_remove(struct spi_device *const spi)
{
	struct tlc5940 *const tlc = spi_get_drvdata(spi);
	struct hrtimer *const timer = &tlc->timer;
	struct work_struct *const work = &tlc->work;
	struct pwm_device *const pwm = tlc->pwm;
	struct tlc5940_led *led;
	int i;

	pwm_disable(pwm);
	hrtimer_cancel(timer);
	cancel_work_sync(work);

	for (i = 0; i < TLC5940_MAX_LEDS; i++) {
		led = &tlc->leds[i];
		led_classdev_unregister(&led->ldev);
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
