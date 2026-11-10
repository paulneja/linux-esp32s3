// SPDX-License-Identifier: GPL-2.0
/*
 * mkdir /sys/kernel/config/device-tree/overlays/x, write a .dtbo to x/dtbo
 * and it is applied; rmdir x takes it off. Same layout as the RPi kernels.
 */
#include <linux/configfs.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#define DTBO_MAX_SIZE	SZ_64K

struct dt_overlay {
	struct config_item item;
	struct mutex lock;
	void *dtbo;
	size_t size;
	int ovcs_id;
	int error;
};

static struct dt_overlay *to_overlay(struct config_item *item)
{
	return container_of(item, struct dt_overlay, item);
}

static ssize_t dt_overlay_dtbo_read(struct config_item *item, void *buf, size_t max)
{
	struct dt_overlay *ov = to_overlay(item);
	ssize_t ret;

	mutex_lock(&ov->lock);
	ret = ov->size;
	if (buf && ov->dtbo)
		memcpy(buf, ov->dtbo, min(max, ov->size));
	mutex_unlock(&ov->lock);
	return ret;
}

static ssize_t dt_overlay_dtbo_write(struct config_item *item, const void *buf, size_t count)
{
	struct dt_overlay *ov = to_overlay(item);
	ssize_t ret = count;
	void *copy;

	mutex_lock(&ov->lock);
	if (ov->ovcs_id) {
		ret = -EBUSY;
		goto out;
	}
	copy = kmemdup(buf, count, GFP_KERNEL);
	if (!copy) {
		ret = -ENOMEM;
		goto out;
	}
	ov->error = of_overlay_fdt_apply(copy, count, &ov->ovcs_id, NULL);
	if (ov->error) {
		pr_err("overlay %s: not applied (%d)\n", config_item_name(item), ov->error);
		kfree(copy);
		ov->ovcs_id = 0;
		ret = ov->error;
		goto out;
	}
	ov->dtbo = copy;
	ov->size = count;
out:
	mutex_unlock(&ov->lock);
	return ret;
}

CONFIGFS_BIN_ATTR(dt_overlay_, dtbo, NULL, DTBO_MAX_SIZE);

static ssize_t dt_overlay_status_show(struct config_item *item, char *page)
{
	struct dt_overlay *ov = to_overlay(item);
	ssize_t ret;

	mutex_lock(&ov->lock);
	if (ov->ovcs_id)
		ret = sysfs_emit(page, "applied\n");
	else if (ov->error)
		ret = sysfs_emit(page, "error %d\n", ov->error);
	else
		ret = sysfs_emit(page, "unapplied\n");
	mutex_unlock(&ov->lock);
	return ret;
}

CONFIGFS_ATTR_RO(dt_overlay_, status);

static struct configfs_attribute *dt_overlay_attrs[] = {
	&dt_overlay_attr_status,
	NULL,
};

static struct configfs_bin_attribute *dt_overlay_bin_attrs[] = {
	&dt_overlay_attr_dtbo,
	NULL,
};

static void dt_overlay_release(struct config_item *item)
{
	struct dt_overlay *ov = to_overlay(item);

	if (ov->ovcs_id && of_overlay_remove(&ov->ovcs_id))
		pr_err("overlay %s: could not be removed\n", config_item_name(item));
	kfree(ov->dtbo);
	kfree(ov);
}

static struct configfs_item_operations dt_overlay_item_ops = {
	.release = dt_overlay_release,
};

static const struct config_item_type dt_overlay_type = {
	.ct_item_ops = &dt_overlay_item_ops,
	.ct_attrs = dt_overlay_attrs,
	.ct_bin_attrs = dt_overlay_bin_attrs,
	.ct_owner = THIS_MODULE,
};

static struct config_item *dt_overlays_make_item(struct config_group *group, const char *name)
{
	struct dt_overlay *ov = kzalloc(sizeof(*ov), GFP_KERNEL);

	if (!ov)
		return ERR_PTR(-ENOMEM);
	mutex_init(&ov->lock);
	config_item_init_type_name(&ov->item, name, &dt_overlay_type);
	return &ov->item;
}

static void dt_overlays_drop_item(struct config_group *group, struct config_item *item)
{
	config_item_put(item);
}

static struct configfs_group_operations dt_overlays_ops = {
	.make_item = dt_overlays_make_item,
	.drop_item = dt_overlays_drop_item,
};

static const struct config_item_type dt_overlays_type = {
	.ct_group_ops = &dt_overlays_ops,
	.ct_owner = THIS_MODULE,
};

static const struct config_item_type dt_root_type = {
	.ct_owner = THIS_MODULE,
};

static struct config_group dt_overlays;

static struct configfs_subsystem dt_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "device-tree",
			.ci_type = &dt_root_type,
		},
	},
	.su_mutex = __MUTEX_INITIALIZER(dt_subsys.su_mutex),
};

static int __init of_cfs_init(void)
{
	int ret;

	config_group_init(&dt_subsys.su_group);
	config_group_init_type_name(&dt_overlays, "overlays", &dt_overlays_type);
	configfs_add_default_group(&dt_overlays, &dt_subsys.su_group);
	ret = configfs_register_subsystem(&dt_subsys);
	if (ret)
		pr_err("device-tree configfs: %d\n", ret);
	return ret;
}
late_initcall(of_cfs_init);
