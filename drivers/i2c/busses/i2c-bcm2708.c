/*
 * Driver for Broadcom BCM2708 BSC Controllers
 *
 * Copyright (C) 2012 Chris Boot & Frank Buss
 *
 * This driver is inspired by:
 * i2c-ocores.c, by Peter Korsgaard <jacmet@sunsite.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/wait.h>

/* BSC register offsets */
#define BSC_C			0x00
#define BSC_S			0x04
#define BSC_DLEN		0x08
#define BSC_A			0x0c
#define BSC_FIFO		0x10
#define BSC_DIV			0x14
#define BSC_DEL			0x18
#define BSC_CLKT		0x1c

/* Bitfields in BSC_C */
#define BSC_C_I2CEN		0x00008000
#define BSC_C_INTR		0x00000400
#define BSC_C_INTT		0x00000200
#define BSC_C_INTD		0x00000100
#define BSC_C_ST		0x00000080
#define BSC_C_CLEAR_1		0x00000020
#define BSC_C_CLEAR_2		0x00000010
#define BSC_C_READ		0x00000001

/* Bitfields in BSC_S */
#define BSC_S_CLKT		0x00000200
#define BSC_S_ERR		0x00000100
#define BSC_S_RXF		0x00000080
#define BSC_S_TXE		0x00000040
#define BSC_S_RXD		0x00000020
#define BSC_S_TXD		0x00000010
#define BSC_S_RXR		0x00000008
#define BSC_S_TXW		0x00000004
#define BSC_S_DONE		0x00000002
#define BSC_S_TA		0x00000001

#define I2C_TIMEOUT_MS	150

#define DRV_NAME	"bcm2708_i2c"

static unsigned int baudrate = CONFIG_I2C_BCM2708_BAUDRATE;
module_param(baudrate, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(baudrate, "The I2C baudrate");


struct bcm2708_i2c {
	struct i2c_adapter adapter;

	spinlock_t lock;
	void __iomem *base;
	int irq;
	struct clk *clk;

	struct completion done;

	struct i2c_msg *msg;
	int pos;
	int nmsgs;
	bool error;
};

/*
 * This function sets the ALT mode on the I2C pins so that we can use them with
 * the BSC hardware.
 *
 * FIXME: This is a hack. Use pinmux / pinctrl.
 */
static void bcm2708_i2c_init_pinmode(int id)
{
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

	int pin;
	u32 *gpio = ioremap(0x20200000, SZ_16K);

        BUG_ON(id != 0 && id != 1);
	/* BSC0 is on GPIO 0 & 1, BSC1 is on GPIO 2 & 3 */
	for (pin = id*2+0; pin <= id*2+1; pin++) {
printk("bcm2708_i2c_init_pinmode(%d,%d)\n", id, pin);
		INP_GPIO(pin);		/* set mode to GPIO input first */
		SET_GPIO_ALT(pin, 0);	/* set mode to ALT 0 */
	}

	iounmap(gpio);

#undef INP_GPIO
#undef SET_GPIO_ALT
}

static inline u32 bcm2708_rd(struct bcm2708_i2c *bi, unsigned reg)
{
	return readl(bi->base + reg);
}

static inline void bcm2708_wr(struct bcm2708_i2c *bi, unsigned reg, u32 val)
{
	writel(val, bi->base + reg);
}

static inline void bcm2708_bsc_reset(struct bcm2708_i2c *bi)
{
	bcm2708_wr(bi, BSC_C, 0);
	bcm2708_wr(bi, BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);	
}

static inline void bcm2708_bsc_fifo_drain(struct bcm2708_i2c *bi)
{
	while ((bcm2708_rd(bi, BSC_S) & BSC_S_RXD) && (bi->pos < bi->msg->len))
		bi->msg->buf[bi->pos++] = bcm2708_rd(bi, BSC_FIFO);
}

static inline void bcm2708_bsc_fifo_fill(struct bcm2708_i2c *bi)
{
	while ((bcm2708_rd(bi, BSC_S) & BSC_S_TXD) && (bi->pos < bi->msg->len))
		bcm2708_wr(bi, BSC_FIFO, bi->msg->buf[bi->pos++]);
}

static inline void bcm2708_bsc_setup(struct bcm2708_i2c *bi)
{
	unsigned long bus_hz;
	u32 cdiv;
	u32 c = BSC_C_I2CEN | BSC_C_INTD | BSC_C_ST | BSC_C_CLEAR_1;

	bus_hz = clk_get_rate(bi->clk);
	cdiv = bus_hz / baudrate;

	if (bi->msg->flags & I2C_M_RD)
		c |= BSC_C_INTR | BSC_C_READ;
	else
		c |= BSC_C_INTT;

	bcm2708_wr(bi, BSC_DIV, cdiv);
	bcm2708_wr(bi, BSC_A, bi->msg->addr);
	bcm2708_wr(bi, BSC_DLEN, bi->msg->len);
	bcm2708_wr(bi, BSC_C, c);
}

static irqreturn_t bcm2708_i2c_interrupt(int irq, void *dev_id)
{
	struct bcm2708_i2c *bi = dev_id;
	bool handled = true;
	u32 s;

	spin_lock(&bi->lock);

	/* we may see camera interrupts on the "other" I2C channel
           Just return if we've not sent anything */
        if (!bi->nmsgs || !bi->msg )
		goto early_exit;

	s = bcm2708_rd(bi, BSC_S);

	if (s & (BSC_S_CLKT | BSC_S_ERR)) {
		bcm2708_bsc_reset(bi);
		bi->error = true;

		/* wake up our bh */
		complete(&bi->done);
	} else if (s & BSC_S_DONE) {
		bi->nmsgs--;

		if (bi->msg->flags & I2C_M_RD)
			bcm2708_bsc_fifo_drain(bi);

		bcm2708_bsc_reset(bi);

		if (bi->nmsgs) {
			/* advance to next message */
			bi->msg++;
			bi->pos = 0;
			bcm2708_bsc_setup(bi);
		} else {
			/* wake up our bh */
			complete(&bi->done);
		}
	} else if (s & BSC_S_TXW) {
		bcm2708_bsc_fifo_fill(bi);
	} else if (s & BSC_S_RXR) {
		bcm2708_bsc_fifo_drain(bi);
	} else {
		handled = false;
	}

early_exit:
	spin_unlock(&bi->lock);

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int bcm2708_i2c_master_xfer(struct i2c_adapter *adap,
	struct i2c_msg *msgs, int num)
{
	struct bcm2708_i2c *bi = adap->algo_data;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&bi->lock, flags);

	INIT_COMPLETION(bi->done);
	bi->msg = msgs;
	bi->pos = 0;
	bi->nmsgs = num;
	bi->error = false;

	spin_unlock_irqrestore(&bi->lock, flags);

	bcm2708_bsc_setup(bi);

	ret = wait_for_completion_timeout(&bi->done,
			msecs_to_jiffies(I2C_TIMEOUT_MS));
	if (ret == 0) {
		dev_err(&adap->dev, "transfer timed out\n");
		spin_lock_irqsave(&bi->lock, flags);
		bcm2708_bsc_reset(bi);
		spin_unlock_irqrestore(&bi->lock, flags);
		return -ETIMEDOUT;
	}

	return bi->error ? -EIO : num;
}

static u32 bcm2708_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | /*I2C_FUNC_10BIT_ADDR |*/ I2C_FUNC_SMBUS_EMUL;
}

static struct i2c_algorithm bcm2708_i2c_algorithm = {
	.master_xfer = bcm2708_i2c_master_xfer,
	.functionality = bcm2708_i2c_functionality,
};

static int bcm2708_i2c_probe(struct platform_device *pdev)
{
	struct resource *regs;
	int irq, err = -ENOMEM;
	struct clk *clk;
	struct bcm2708_i2c *bi;
	struct i2c_adapter *adap;

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!regs) {
		dev_err(&pdev->dev, "could not get IO memory\n");
		return -ENXIO;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "could not get IRQ\n");
		return irq;
	}

	clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "could not find clk: %ld\n", PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	bcm2708_i2c_init_pinmode(pdev->id);

	bi = kzalloc(sizeof(*bi), GFP_KERNEL);
	if (!bi)
		goto out_clk_put;

	platform_set_drvdata(pdev, bi);

	adap = &bi->adapter;
	adap->class = I2C_CLASS_HWMON | I2C_CLASS_DDC;
	adap->algo = &bcm2708_i2c_algorithm;
	adap->algo_data = bi;
	adap->dev.parent = &pdev->dev;
	adap->nr = pdev->id;
	strlcpy(adap->name, dev_name(&pdev->dev), sizeof(adap->name));

	switch (pdev->id) {
	case 0:
		adap->class = I2C_CLASS_HWMON;
		break;
	case 1:
		adap->class = I2C_CLASS_DDC;
		break;
	default:
		dev_err(&pdev->dev, "can only bind to BSC 0 or 1\n");
		err = -ENXIO;
		goto out_free_bi;
	}

	spin_lock_init(&bi->lock);
	init_completion(&bi->done);

	bi->base = ioremap(regs->start, resource_size(regs));
	if (!bi->base) {
		dev_err(&pdev->dev, "could not remap memory\n");
		goto out_free_bi;
	}

	bi->irq = irq;
	bi->clk = clk;

	err = request_irq(irq, bcm2708_i2c_interrupt, IRQF_SHARED,
			dev_name(&pdev->dev), bi);
	if (err) {
		dev_err(&pdev->dev, "could not request IRQ: %d\n", err);
		goto out_iounmap;
	}

	bcm2708_bsc_reset(bi);

	err = i2c_add_numbered_adapter(adap);
	if (err < 0) {
		dev_err(&pdev->dev, "could not add I2C adapter: %d\n", err);
		goto out_free_irq;
	}

	dev_info(&pdev->dev, "BSC%d Controller at 0x%08lx (irq %d) (baudrate %dk)\n",
		pdev->id, (unsigned long)regs->start, irq, baudrate/1000);

	return 0;

out_free_irq:
	free_irq(bi->irq, bi);
out_iounmap:
	iounmap(bi->base);
out_free_bi:
	kfree(bi);
out_clk_put:
	clk_put(clk);
	return err;
}

static int bcm2708_i2c_remove(struct platform_device *pdev)
{
	struct bcm2708_i2c *bi = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);

	i2c_del_adapter(&bi->adapter);
	free_irq(bi->irq, bi);
	iounmap(bi->base);
	clk_disable(bi->clk);
	clk_put(bi->clk);
	kfree(bi);

	return 0;
}

static struct platform_driver bcm2708_i2c_driver = {
	.driver		= {
		.name	= DRV_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= bcm2708_i2c_probe,
	.remove		= bcm2708_i2c_remove,
};

// module_platform_driver(bcm2708_i2c_driver);


static int __init bcm2708_i2c_init(void)
{
	return platform_driver_register(&bcm2708_i2c_driver);
}

static void __exit bcm2708_i2c_exit(void)
{
	platform_driver_unregister(&bcm2708_i2c_driver);
}

module_init(bcm2708_i2c_init);
module_exit(bcm2708_i2c_exit);



MODULE_DESCRIPTION("BSC controller driver for Broadcom BCM2708");
MODULE_AUTHOR("Chris Boot <bootc@bootc.net>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);