/*
 * Copyright (c) 2011-2023 Columbia University, System Level Design Group
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/esp/esp_cache.h>
#include <linux/printk.h>

#define DRV_NAME	"lastlevel_cache"

static DEFINE_SPINLOCK(esp_cache_list_lock);
static LIST_HEAD(esp_cache_list);

struct esp_cache_device {
	struct device *pdev; /* platform device */
	struct module *module;
	struct mutex lock;
	void __iomem *iomem; /* mmapped registers */
	struct list_head list;
};


static struct of_device_id esp_cache_device_ids[] = {
	{
		.name = "SLD_ESP_CACHE",
	},
	{
		.name = "eb_021",
	},
	{
		.compatible = "sld,llc_cache",
	},
	{
		.name = "ee_021",
	},
	{
		.compatible = "uiuc,spandex_llc",
	},
	{ },
};


static void esp_cache_do_flush(struct esp_cache_device *esp_cache)
{
	int cmd = 1 << ESP_CACHE_CMD_FLUSH_BIT;
	u32 cmd_reg;
	u32 satus_reg;

	/* Check if flush is already in progress */
	cmd_reg = ioread32be(esp_cache->iomem + ESP_CACHE_REG_CMD);

	if (!cmd_reg) {

		/* Set flush due for LLC cache */
		iowrite32be(cmd, esp_cache->iomem + ESP_CACHE_REG_CMD);

		/* Wait for completion */
		do {
			satus_reg = ioread32be(esp_cache->iomem + ESP_CACHE_REG_STATUS);
			satus_reg &= ESP_CACHE_STATUS_DONE_MASK;
		} while (!satus_reg);
	}

	/* Clear command register */
	iowrite32be(0x0, esp_cache->iomem + ESP_CACHE_REG_CMD);
}

int esp_cache_flush()
{
	struct esp_cache_device *esp_cache = NULL;
	list_for_each_entry(esp_cache, &esp_cache_list, list) {
		if (mutex_lock_interruptible(&esp_cache->lock))
			return -EINTR;
		esp_cache_do_flush(esp_cache);
		mutex_unlock(&esp_cache->lock);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(esp_cache_flush);


static int esp_cache_probe(struct platform_device *pdev)
{
	struct esp_cache_device *esp_cache;
	struct resource *res;
	const char *compatible;
	int cplen;
	int rc = 0;

	esp_cache = kzalloc(sizeof(*esp_cache), GFP_KERNEL);
	if (esp_cache == NULL)
		return -ENOMEM;

	esp_cache->pdev = &pdev->dev;

	esp_cache->module = THIS_MODULE;

	mutex_init(&esp_cache->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	esp_cache->iomem = devm_ioremap_resource(&pdev->dev, res);
	if (esp_cache->iomem == NULL) {
		dev_info(esp_cache->pdev, "cannot map registers for I/O\n");
		rc = -ENODEV;
		goto out_iomem;
	}

	/* Determine which type of cache is present */
#ifdef __sparc
	compatible = of_get_property(esp_cache->pdev->of_node, "name", &cplen);
	if (strcmp(compatible, "eb_021") == 0)
#else
	compatible = of_get_property(esp_cache->pdev->of_node, "compatible", &cplen);
	if (strcmp(compatible, "sld,llc_cache") == 0)
#endif
		dev_info(esp_cache->pdev, "ESP LLC cache registered.\n");
	else
		dev_info(esp_cache->pdev, "Spandex LLC cache registered.\n");

	platform_set_drvdata(pdev, esp_cache);

	spin_lock(&esp_cache_list_lock);
	list_add(&esp_cache->list, &esp_cache_list);
	spin_unlock(&esp_cache_list_lock);

	return 0;

 out_iomem:
	kfree(esp_cache);
	return rc;
}

static int __exit esp_cache_remove(struct platform_device *pdev)
{
	struct esp_cache_device *esp_cache = platform_get_drvdata(pdev);

	spin_lock(&esp_cache_list_lock);
	list_del(&esp_cache->list);
	spin_unlock(&esp_cache_list_lock);

	dev_info(esp_cache->pdev, "device unregistered.\n");

	devm_iounmap(&pdev->dev, esp_cache->iomem);
	kfree(esp_cache);

	return 0;
}

static struct platform_driver esp_cache_driver = {
	.probe		= esp_cache_probe,
	.remove		= esp_cache_remove,
	.driver		= {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = esp_cache_device_ids,
	},
};

int esp_cache_init(void)
{
	int rc;
	printk("esp_cache_init(); from kernel boot\n");
	rc = platform_driver_register(&esp_cache_driver);
	return rc;
}

void esp_cache_exit(void)
{
	platform_driver_unregister(&esp_cache_driver);
}

#undef DRV_NAME
#define DRV_NAME	"private_cache"

static DEFINE_MUTEX(esp_private_cache_list_lock);
static LIST_HEAD(esp_private_cache_list);

struct esp_private_cache_device {
	struct device *pdev; /* platform device */
	struct module *module;
	void __iomem *iomem; /* mmapped registers */
	struct list_head list;
};


static struct of_device_id esp_private_cache_device_ids[] = {
	{
		.name = "SLD_ESP_PRIVACE_CACHE",
	},
	{
		.name = "eb_020",
	},
	{
		.compatible = "sld,l2_cache",
	},
	{
		.name = "ee_020",
	},
	{
		.compatible = "uiuc,spandex_l2",
	},
	{ },
};


/*
 * Note: we know the leon3 has write-through caches, so this may not seem like
 * it's needed. However, I cannot find a way to flush the write buffers, so I'm
 * hoping flushing the cache does flush the write buffers too.
 *
 * The leon3 code flushes the entire cache even if we just want to flush a
 * single line, so we call the flush function below only once.
 */
static void leon3_flush_dcache_all(void)
{
#ifdef __sparc
	__asm__ __volatile__(".align 32\nflush\n.align 32\n");	/*iflush*/
	__asm__ __volatile__("sta %%g0, [%%g0] %0\n\t" : :
			     "i"(ASI_LEON_DFLUSH) : "memory");
        /* __asm__ __volatile__("sta %%g0, [%%g0] %0\n\t" : : */
        /*                 "i"(ASI_LEON_DFLUSH) : "memory"); */
#endif
}


static void esp_flush_l1(void *info)
{
	leon3_flush_dcache_all();
}

int esp_private_cache_flush(void)
{
	struct esp_private_cache_device *esp_private_cache = NULL;
	const int cmd = (1 << ESP_CACHE_CMD_FLUSH_INST_BIT) | (1 << ESP_CACHE_CMD_FLUSH_BIT);
	u32 cmd_reg;
	u32 status_reg = 0;

	mutex_lock(&esp_private_cache_list_lock);

	list_for_each_entry(esp_private_cache, &esp_private_cache_list, list) {

#ifndef CONFIG_SMP
		/* Only CPU0 running whith no SMP */
		status_reg    = ioread32be(esp_private_cache->iomem + ESP_CACHE_REG_STATUS);
		status_reg   &= ESP_CACHE_STATUS_CPUID_MASK;
		status_reg >>= ESP_CACHE_STATUS_CPUID_SHIFT;
#endif

		/* Check if flush is already in progress */
		cmd_reg = ioread32be(esp_private_cache->iomem + ESP_CACHE_REG_CMD);

		BUG_ON(cmd_reg);

		/* if (!cmd_reg && !status_reg) { */
		if (!status_reg) {

			/* Set flush due for L2 private cache */
			iowrite32be(cmd, esp_private_cache->iomem + ESP_CACHE_REG_CMD);

			/* Wait for L2 flush to be set (may loose synch flush with L1 otherwise) */
			do {
				cmd_reg = ioread32be(esp_private_cache->iomem + ESP_CACHE_REG_CMD);
			} while (cmd_reg != cmd);

		}

	}

	/* Flush all L1 caches: start synchronized L2 flush as well */
	on_each_cpu(esp_flush_l1, NULL, 1);

	list_for_each_entry(esp_private_cache, &esp_private_cache_list, list) {

		/* Check if cache command register has already been reset */
		cmd_reg = ioread32be(esp_private_cache->iomem + ESP_CACHE_REG_CMD);

		if (cmd_reg) {
			/* Wait for L2 caches to complete flush */
			do {
				status_reg = ioread32be(esp_private_cache->iomem + ESP_CACHE_REG_STATUS);
				status_reg &= ESP_CACHE_STATUS_DONE_MASK;
			} while (!status_reg);


			/* Clear command register */
			iowrite32be(0x0, esp_private_cache->iomem + ESP_CACHE_REG_CMD);
		}

	}

	mutex_unlock(&esp_private_cache_list_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(esp_private_cache_flush);


static int esp_private_cache_probe(struct platform_device *pdev)
{
	struct esp_private_cache_device *esp_private_cache;
	struct resource *res;
	const char *compatible;
	int cplen;
	int rc = 0;

	esp_private_cache = kzalloc(sizeof(*esp_private_cache), GFP_KERNEL);
	if (esp_private_cache == NULL)
		return -ENOMEM;

	esp_private_cache->pdev = &pdev->dev;

	esp_private_cache->module = THIS_MODULE;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	esp_private_cache->iomem = devm_ioremap_resource(&pdev->dev, res);
	if (esp_private_cache->iomem == NULL) {
		dev_info(esp_private_cache->pdev, "cannot map registers for I/O\n");
		rc = -ENODEV;
		goto out_iomem;
	}

	/* Determine which type of cache is present */
#ifdef __sparc
	compatible = of_get_property(esp_private_cache->pdev->of_node, "name", &cplen);
	if (strcmp(compatible, "eb_020") == 0)
#else
	compatible = of_get_property(esp_private_cache->pdev->of_node, "compatible", &cplen);
	if (strcmp(compatible, "sld,l2_cache") == 0)
#endif
		dev_info(esp_private_cache->pdev, "ESP L2 cache registered.\n");
	else
		dev_info(esp_private_cache->pdev, "Spandex L2 cache registered.\n");

	platform_set_drvdata(pdev, esp_private_cache);

	mutex_lock(&esp_private_cache_list_lock);
	list_add(&esp_private_cache->list, &esp_private_cache_list);
	mutex_unlock(&esp_private_cache_list_lock);

	return 0;

 out_iomem:
	kfree(esp_private_cache);
	return rc;
}

static int __exit esp_private_cache_remove(struct platform_device *pdev)
{
	struct esp_private_cache_device *esp_private_cache = platform_get_drvdata(pdev);

	mutex_lock(&esp_private_cache_list_lock);
	list_del(&esp_private_cache->list);
	mutex_unlock(&esp_private_cache_list_lock);

	dev_info(esp_private_cache->pdev, "device unregistered.\n");

	devm_iounmap(&pdev->dev, esp_private_cache->iomem);
	kfree(esp_private_cache);

	return 0;
}

static struct platform_driver esp_private_cache_driver = {
	.probe		= esp_private_cache_probe,
	.remove		= esp_private_cache_remove,
	.driver		= {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = esp_private_cache_device_ids,
	},
};

int __init esp_private_cache_init(void)
{
	int rc;

    printk("esp_private_cache() from kernel boot;\n");

    rc = platform_driver_register(&esp_private_cache_driver);
	return rc;
}

void __exit esp_private_cache_exit(void)
{
	platform_driver_unregister(&esp_private_cache_driver);
}

// module_init(esp_private_cache_init)
// module_exit(esp_private_cache_exit)

// MODULE_DEVICE_TABLE(of, esp_private_cache_device_ids);

// MODULE_AUTHOR("Paolo Mantovani <paolo@cs.columbia.edu>");
// MODULE_LICENSE("GPL");
// MODULE_DESCRIPTION("esp_private_cache driver");
