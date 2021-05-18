#include <asm/errno.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include "bn.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH (~(1 << 31))

#define __swap(x, y) \
    do {             \
        x = x ^ y;   \
        y = x ^ y;   \
        x = x ^ y;   \
    } while (0)

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);
static ktime_t kt;
static bn *fibnum;

static bn *fib_sequence(uint64_t n)
{
    /* FIXME: use clz/ctz and fast algorithms to speed up */
    if (unlikely(n <= 2)) {
        if (n == 0)
            return bn_new_from_digit(0);
        return bn_new_from_digit(1);
    }

    bn *a0 = bn_new_from_digit(0); /*  a0 = 0 */
    bn *a1 = bn_new_from_digit(1); /*  a1 = 1 */
    bn *const2 = bn_new_from_digit(2);

    /* Start at second-highest bit set. */
    for (uint64_t k = ((uint64_t) 1) << (62 - __builtin_clzll(n)); k; k >>= 1) {
        /* Both ways use two squares, two adds, one multipy and one shift. */
        bn *t1, *t2, *t3, *tmp1, *tmp2;
        tmp1 = bn_mul(a0, const2);
        t1 = bn_add(tmp1, a1);
        Bn_DECREF(tmp1);
        t2 = bn_mul(a0, a0);
        t3 = bn_mul(a1, a1);
        tmp1 = a0, tmp2 = a1;
        a1 = bn_mul(a1, t1);
        a0 = bn_add(t2, t3);
        Bn_DECREF(t1);
        Bn_DECREF(t2);
        Bn_DECREF(t3);
        Bn_DECREF(tmp1);
        Bn_DECREF(tmp2);
        if (k & n) {
            /*  a1 <-> a0 */
            tmp1 = a1;
            a1 = a0;
            a0 = tmp1;
            /*  a1 += a0 */
            tmp1 = a1;
            a1 = bn_add(a0, a1);
            Bn_DECREF(tmp1);
        }
    }
    /* Now a1 (alias of output parameter fib) = F[n] */
    Bn_DECREF(a0);
    Bn_DECREF(const2);
    return a1;
}


static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    unsigned long remains;
    bn *dec;
    char *str;
    size_t len;

    kt = ktime_get();
    fibnum = fib_sequence(*offset);
    kt = ktime_sub(ktime_get(), kt);

    dec = bn_to_dec(fibnum);
    str = bn_to_str(dec);
    remains = copy_to_user(buf, str, len = strlen(str) + 1);
    bfree(str);
    Bn_DECREF(dec);

    return (ssize_t) remains ? -EFAULT : len - 1;
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return 1;
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

/*
 * The "fib" file where a output of "fib_sequence()" is read from.
 */
static ssize_t f_show(struct kobject *kobj,
                      struct kobj_attribute *attr,
                      char *buf)
{
    bn *dec;
    char *str;
    int count = 0;

    if (!fibnum)
        return count;

    dec = bn_to_dec(fibnum);
    str = bn_to_str(dec);
    count = scnprintf(buf, PAGE_SIZE, "%s\n", str);
    bfree(str);
    Bn_DECREF(dec);
    return count;
}

static ssize_t f_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf,
                       size_t count)
{
    int ret, input;
    ret = kstrtoint(buf, 10, &input);
    if (ret < 0)
        return ret;
    if (fibnum)
        Bn_DECREF(fibnum);
    fibnum = fib_sequence(input);
    return count;
}

static struct kobj_attribute fib_attribute = __ATTR(fib, 0664, f_show, f_store);

/*
 * The "time" file where total number of CPU-nanoseconds used by
 * "fib_sequence()" is read from.
 */
static ssize_t k_show(struct kobject *kobj,
                      struct kobj_attribute *attr,
                      char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%lld\n", ktime_to_ns(kt));
}

static ssize_t k_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf,
                       size_t count)
{
    return count;
}

static struct kobj_attribute ktime_attribute =
    __ATTR(time, 0444, k_show, k_store);


static struct attribute *attrs[] = {
    &ktime_attribute.attr,
    &fib_attribute.attr,
    NULL,
};

static struct attribute_group attr_group = {
    .attrs = attrs,
};

static struct kobject *fib_kobj;

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    fib_cdev->ops = &fib_fops;
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }

    fib_kobj = kobject_create_and_add("fibdrv", kernel_kobj);
    if (!fib_kobj) {
        printk(KERN_ALERT "Failed to create kobject");
        rc = -ENOMEM;
        goto failed_device_create;
    }

    rc = sysfs_create_group(fib_kobj, &attr_group);
    if (rc) {
        printk(KERN_ALERT "Failed to create group");
        goto failed_file_create;
    }


    return rc;
failed_file_create:
    kobject_put(fib_kobj);
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    if (fibnum)
        Bn_DECREF(fibnum);
    kobject_put(fib_kobj);
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
