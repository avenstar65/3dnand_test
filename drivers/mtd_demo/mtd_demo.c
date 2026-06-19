#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>

#define MTD_DEMO_SCAN_LIMIT 32

static int __init mtd_demo_init(void)
{
	struct mtd_info *mtd;
	int i;
	int count = 0;

	pr_info("mtd_demo: init\n");

	for (i = 0; i < MTD_DEMO_SCAN_LIMIT; i++) {
		mtd = get_mtd_device(NULL, i);
		if (IS_ERR(mtd))
			continue;

		pr_info("mtd_demo: mtd%d name=%s size=%llu erasesize=%u writesize=%u\n",
			mtd->index,
			mtd->name ? mtd->name : "(null)",
			(unsigned long long)mtd->size,
			mtd->erasesize,
			mtd->writesize);
		put_mtd_device(mtd);
		count++;
	}

	pr_info("mtd_demo: found %d MTD device(s)\n", count);
	return 0;
}

static void __exit mtd_demo_exit(void)
{
	pr_info("mtd_demo: exit\n");
}

module_init(mtd_demo_init);
module_exit(mtd_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("Minimal MTD inspection module for QEMU kernel development");
