// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mainstone-wm97xx.c  --  Mainstone Continuous Touch screen driver for
 *                         Wolfson WM97xx AC97 Codecs.
 *
 * Copyright 2004, 2007 Wolfson Microelectronics PLC.
 * Author: Liam Girdwood <lrg@slimlogic.co.uk>
 * Parts Copyright : Ian Molton <spyro@f2s.com>
 *                   Andrew Zabolotny <zap@homelink.ru>
 *
 * Notes:
 *     This is a wm97xx extended touch driver to capture touch
 *     data in a continuous manner on the Intel XScale architecture
 *
 *  Features:
 *       - codecs supported:- WM9705, WM9712, WM9713
 *       - processors supported:- Intel XScale PXA25x, PXA26x, PXA27x
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/soc/pxa/cpu.h>
#include <linux/wm97xx.h>

#include <sound/pxa2xx-lib.h>

#include <asm/mach-types.h>

struct continuous {
	u16 id;    /* codec id */
	u8 code;   /* continuous code */
	u8 reads;  /* number of coord reads per read cycle */
	u32 speed; /* number of coords per second */
};

#define WM_READS(sp) ((sp / HZ) + 1)

static const struct continuous cinfo[] = {
	{ WM9705_ID2, 0, WM_READS(94),  94  },
	{ WM9705_ID2, 1, WM_READS(188), 188 },
	{ WM9705_ID2, 2, WM_READS(375), 375 },
	{ WM9705_ID2, 3, WM_READS(750), 750 },
	{ WM9712_ID2, 0, WM_READS(94),  94  },
	{ WM9712_ID2, 1, WM_READS(188), 188 },
	{ WM9712_ID2, 2, WM_READS(375), 375 },
	{ WM9712_ID2, 3, WM_READS(750), 750 },
	{ WM9713_ID2, 0, WM_READS(94),  94  },
	{ WM9713_ID2, 1, WM_READS(120), 120 },
	{ WM9713_ID2, 2, WM_READS(154), 154 },
	{ WM9713_ID2, 3, WM_READS(188), 188 },
};

/* continuous speed index */
static int sp_idx;
static struct gpio_desc *gpiod_irq;

/*
 * Pen sampling frequency (Hz) in continuous mode.
 */
static int cont_rate = 200;
module_param(cont_rate, int, 0);
MODULE_PARM_DESC(cont_rate, "Sampling rate in continuous mode (Hz)");

/*
 * Pen down detection.
 *
 * This driver can either poll or use an interrupt to indicate a pen down
 * event. If the irq request fails then it will fall back to polling mode.
 */
static int pen_int;
module_param(pen_int, int, 0);
MODULE_PARM_DESC(pen_int, "Pen down detection (1 = interrupt, 0 = polling)");

/*
 * Pressure readback.
 *
 * Set to 1 to read back pen down pressure
 */
static int pressure;
module_param(pressure, int, 0);
MODULE_PARM_DESC(pressure, "Pressure readback (1 = pressure, 0 = no pressure)");

/*
 * AC97 touch data slot.
 *
 * Touch screen readback data ac97 slot
 */
static int ac97_touch_slot = 5;
module_param(ac97_touch_slot, int, 0);
MODULE_PARM_DESC(ac97_touch_slot, "Touch screen data slot AC97 number");


/* flush AC97 slot 5 FIFO on pxa machines */
static void wm97xx_acc_pen_up(struct wm97xx *wm)
{
	unsigned int count;

	msleep(1);

	if (cpu_is_pxa27x()) {
		while (pxa2xx_ac97_read_misr() & (1 << 2))
			pxa2xx_ac97_read_modr();
	} else if (cpu_is_pxa3xx()) {
		for (count = 0; count < 16; count++)
			pxa2xx_ac97_read_modr();
	}
}

static int wm97xx_acc_pen_down(struct wm97xx *wm)
{
	u16 x, y, p = 0x100 | WM97XX_ADCSEL_PRES;
	int reads = 0;
	static u16 last, tries;

	/* When the AC97 queue has been drained we need to allow time
	 * to buffer up samples otherwise we end up spinning polling
	 * for samples.  The controller can't have a suitably low
	 * threshold set to use the notifications it gives.
	 */
	msleep(1);

	if (tries > 5) {
		tries = 0;
		return RC_PENUP;
	}

	x = pxa2xx_ac97_read_modr();
	if (x == last) {
		tries++;
		return RC_AGAIN;
	}
	last = x;
	do {
		if (reads)
			x = pxa2xx_ac97_read_modr();
		y = pxa2xx_ac97_read_modr();
		if (pressure)
			p = pxa2xx_ac97_read_modr();

		dev_dbg(wm->dev, "Raw coordinates: x=%x, y=%x, p=%x\n",
			x, y, p);

		/* are samples valid */
		if ((x & WM97XX_ADCSEL_MASK) != WM97XX_ADCSEL_X ||
		    (y & WM97XX_ADCSEL_MASK) != WM97XX_ADCSEL_Y ||
		    (p & WM97XX_ADCSEL_MASK) != WM97XX_ADCSEL_PRES)
			goto up;

		/* coordinate is good */
		tries = 0;
		input_report_abs(wm->input_dev, ABS_X, x & 0xfff);
		input_report_abs(wm->input_dev, ABS_Y, y & 0xfff);
		input_report_abs(wm->input_dev, ABS_PRESSURE, p & 0xfff);
		input_report_key(wm->input_dev, BTN_TOUCH, (p != 0));
		input_sync(wm->input_dev);
		reads++;
	} while (reads < cinfo[sp_idx].reads);
up:
	return RC_PENDOWN | RC_AGAIN;
}

static int wm97xx_acc_startup(struct wm97xx *wm)
{
	int idx = 0, ret = 0;

	/* check we have a codec */
	if (wm->ac97 == NULL)
		return -ENODEV;

	/* Go you big red fire engine */
	for (idx = 0; idx < ARRAY_SIZE(cinfo); idx++) {
		if (wm->id != cinfo[idx].id)
			continue;
		sp_idx = idx;
		if (cont_rate <= cinfo[idx].speed)
			break;
	}
	wm->acc_rate = cinfo[sp_idx].code;
	wm->acc_slot = ac97_touch_slot;
	dev_info(wm->dev,
		 "mainstone accelerated touchscreen driver, %d samples/sec\n",
		 cinfo[sp_idx].speed);

	if (pen_int) {
		gpiod_irq = gpiod_get(wm->dev, "touch", GPIOD_IN);
		if (IS_ERR(gpiod_irq))
			pen_int = 0;
	}

	if (pen_int) {
		wm->pen_irq = gpiod_to_irq(gpiod_irq);
		irq_set_irq_type(wm->pen_irq, IRQ_TYPE_EDGE_BOTH);
	}

	/* codec specific irq config */
	if (pen_int) {
		switch (wm->id) {
		case WM9705_ID2:
			break;
		case WM9712_ID2:
		case WM9713_ID2:
			/* use PEN_DOWN GPIO 13 to assert IRQ on GPIO line 2 */
			wm97xx_config_gpio(wm, WM97XX_GPIO_13, WM97XX_GPIO_IN,
					   WM97XX_GPIO_POL_HIGH,
					   WM97XX_GPIO_STICKY,
					   WM97XX_GPIO_WAKE);
			wm97xx_config_gpio(wm, WM97XX_GPIO_2, WM97XX_GPIO_OUT,
					   WM97XX_GPIO_POL_HIGH,
					   WM97XX_GPIO_NOTSTICKY,
					   WM97XX_GPIO_NOWAKE);
			break;
		default:
			dev_err(wm->dev,
				"pen down irq not supported on this device\n");
			pen_int = 0;
			break;
		}
	}

	return ret;
}

static void wm97xx_acc_shutdown(struct wm97xx *wm)
{
	/* codec specific deconfig */
	if (pen_int) {
		if (gpiod_irq)
			gpiod_put(gpiod_irq);
		wm->pen_irq = 0;
	}
}

static struct wm97xx_mach_ops mainstone_mach_ops = {
	.acc_enabled	= 1,
	.acc_pen_up	= wm97xx_acc_pen_up,
	.acc_pen_down	= wm97xx_acc_pen_down,
	.acc_startup	= wm97xx_acc_startup,
	.acc_shutdown	= wm97xx_acc_shutdown,
	.irq_gpio	= WM97XX_GPIO_2,
};

static int mainstone_wm97xx_probe(struct platform_device *pdev)
{
	struct wm97xx *wm = platform_get_drvdata(pdev);

	return wm97xx_register_mach_ops(wm, &mainstone_mach_ops);
}

static void mainstone_wm97xx_remove(struct platform_device *pdev)
{
	struct wm97xx *wm = platform_get_drvdata(pdev);

	wm97xx_unregister_mach_ops(wm);
}

static struct platform_driver mainstone_wm97xx_driver = {
	.probe	= mainstone_wm97xx_probe,
	.remove	= mainstone_wm97xx_remove,
	.driver	= {
		.name	= "wm97xx-touch",
	},
};
module_platform_driver(mainstone_wm97xx_driver);

/* Module information */
MODULE_AUTHOR("Liam Girdwood <lrg@slimlogic.co.uk>");
MODULE_DESCRIPTION("wm97xx continuous touch driver for mainstone");
MODULE_LICENSE("GPL");
