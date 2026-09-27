/**
 * @file buzzer_driver.c
 * @brief [P2-M2] Character Device Driver for Passive Buzzer using Kernel High-Resolution Timers (hrtimer 2000Hz PWM Tone Generator)
 * @author PHUC TAI
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/version.h>

#define DRIVER_NAME         "buzzer_driver"
#define CLASS_NAME          "smartclock_buzzer"

/* 250 microseconds half-period = 500us full period = 2000 Hz (2 kHz) tone */
#define BUZZER_HALF_PERIOD_NS   (250 * 1000ULL)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PHUC TAI");
MODULE_DESCRIPTION("Passive Buzzer PWM Tone Generator via hrtimer");
MODULE_VERSION("2.0");

struct buzzer_device_data {
    int gpio_pin;
    dev_t dev_num;
    struct cdev cdev;
    struct class *dev_class;
    struct device *dev_device;

    /* Waveform generation context */
    struct hrtimer tone_timer;
    int pin_state;
    bool is_ringing;
    spinlock_t lock;
};

static struct buzzer_device_data buzzer_data;

/* High-Resolution Timer Callback for generating square wave */
static enum hrtimer_restart buzzer_timer_callback(struct hrtimer *timer)
{
    unsigned long flags;
    bool active;

    spin_lock_irqsave(&buzzer_data.lock, flags);
    active = buzzer_data.is_ringing;
    if (active) {
        buzzer_data.pin_state = !buzzer_data.pin_state;
        gpio_set_value(buzzer_data.gpio_pin, buzzer_data.pin_state);
    } else {
        gpio_set_value(buzzer_data.gpio_pin, 0);
    }
    spin_unlock_irqrestore(&buzzer_data.lock, flags);

    if (!active) {
        return HRTIMER_NORESTART;
    }

    /* Forward timer by half-period to generate next edge */
    hrtimer_forward_now(timer, ns_to_ktime(BUZZER_HALF_PERIOD_NS));
    return HRTIMER_RESTART;
}

/* File Operations: write ('1' to Start Tone, '0' to Stop Tone) */
static ssize_t buzzer_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[4];
    size_t len = min(count, sizeof(kbuf) - 1);
    unsigned long flags;
    bool should_start = false;
    bool should_stop = false;

    if (copy_from_user(kbuf, buf, len))
        return -EFAULT;

    kbuf[len] = '\0';

    spin_lock_irqsave(&buzzer_data.lock, flags);
    if (kbuf[0] == '1') {
        if (!buzzer_data.is_ringing) {
            buzzer_data.is_ringing = true;
            buzzer_data.pin_state = 0;
            should_start = true;
        }
    } else if (kbuf[0] == '0') {
        if (buzzer_data.is_ringing) {
            buzzer_data.is_ringing = false;
            should_stop = true;
        }
    }
    spin_unlock_irqrestore(&buzzer_data.lock, flags);

    /* Timer operations executed outside spinlock to avoid sleeping/deadlock */
    if (should_start) {
        hrtimer_start(&buzzer_data.tone_timer, ns_to_ktime(BUZZER_HALF_PERIOD_NS), HRTIMER_MODE_REL);
    } else if (should_stop) {
        hrtimer_cancel(&buzzer_data.tone_timer);
        gpio_set_value(buzzer_data.gpio_pin, 0);
    }

    return count;
}

static int buzzer_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int buzzer_release(struct inode *inode, struct file *file)
{
    unsigned long flags;

    /* Ensure buzzer is silenced and timer stopped when userspace closes node */
    spin_lock_irqsave(&buzzer_data.lock, flags);
    buzzer_data.is_ringing = false;
    spin_unlock_irqrestore(&buzzer_data.lock, flags);

    hrtimer_cancel(&buzzer_data.tone_timer);
    gpio_set_value(buzzer_data.gpio_pin, 0);
    return 0;
}

static const struct file_operations buzzer_fops = {
    .owner   = THIS_MODULE,
    .open    = buzzer_open,
    .release = buzzer_release,
    .write   = buzzer_write,
};

static int buzzer_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    pr_info(DRIVER_NAME ": Probing SmartClock Passive Buzzer Device...\n");

    /* 1. Extract GPIO Pin dynamically from Device Tree */
    buzzer_data.gpio_pin = of_get_named_gpio(dev->of_node, "gpios", 0);
    if (!gpio_is_valid(buzzer_data.gpio_pin)) {
        dev_err(dev, "Invalid GPIO pin from Device Tree node\n");
        return -EINVAL;
    }

    /* 2. Request GPIO and set initial direction LOW */
    ret = devm_gpio_request_one(dev, buzzer_data.gpio_pin, GPIOF_OUT_INIT_LOW, "smartclock_buzzer_gpio");
    if (ret) {
        dev_err(dev, "Failed to request GPIO %d\n", buzzer_data.gpio_pin);
        return ret;
    }

    /* 3. Register Character Device region */
    ret = alloc_chrdev_region(&buzzer_data.dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        dev_err(dev, "Failed to allocate chrdev region\n");
        return ret;
    }

    cdev_init(&buzzer_data.cdev, &buzzer_fops);
    buzzer_data.cdev.owner = THIS_MODULE;
    ret = cdev_add(&buzzer_data.cdev, buzzer_data.dev_num, 1);
    if (ret < 0) {
        dev_err(dev, "Failed to add cdev\n");
        goto unregister_chrdev;
    }

    /* 4. Create sysfs class & device node (/dev/buzzer_driver) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    buzzer_data.dev_class = class_create(CLASS_NAME);
#else
    buzzer_data.dev_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(buzzer_data.dev_class)) {
        dev_err(dev, "Failed to create class\n");
        ret = PTR_ERR(buzzer_data.dev_class);
        goto del_cdev;
    }

    buzzer_data.dev_device = device_create(buzzer_data.dev_class, NULL, buzzer_data.dev_num, NULL, DRIVER_NAME);
    if (IS_ERR(buzzer_data.dev_device)) {
        dev_err(dev, "Failed to create device /dev/%s\n", DRIVER_NAME);
        ret = PTR_ERR(buzzer_data.dev_device);
        goto destroy_class;
    }

    /* 5. Initialize synchronization and high-resolution timer */
    spin_lock_init(&buzzer_data.lock);
    buzzer_data.is_ringing = false;
    buzzer_data.pin_state = 0;

    hrtimer_init(&buzzer_data.tone_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    buzzer_data.tone_timer.function = buzzer_timer_callback;

    dev_info(dev, "Passive Buzzer Driver loaded! GPIO: %d, Frequency: 2000Hz, Node: /dev/%s\n",
             buzzer_data.gpio_pin, DRIVER_NAME);
    return 0;

destroy_class:
    class_destroy(buzzer_data.dev_class);
del_cdev:
    cdev_del(&buzzer_data.cdev);
unregister_chrdev:
    unregister_chrdev_region(buzzer_data.dev_num, 1);
    return ret;
}

static int buzzer_remove(struct platform_device *pdev)
{
    unsigned long flags;

    spin_lock_irqsave(&buzzer_data.lock, flags);
    buzzer_data.is_ringing = false;
    spin_unlock_irqrestore(&buzzer_data.lock, flags);

    hrtimer_cancel(&buzzer_data.tone_timer);
    gpio_set_value(buzzer_data.gpio_pin, 0);

    device_destroy(buzzer_data.dev_class, buzzer_data.dev_num);
    class_destroy(buzzer_data.dev_class);
    cdev_del(&buzzer_data.cdev);
    unregister_chrdev_region(buzzer_data.dev_num, 1);

    pr_info(DRIVER_NAME ": Driver unloaded safely.\n");
    return 0;
}

static const struct of_device_id buzzer_of_match[] = {
    { .compatible = "devlinux,smartclock-buzzer", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, buzzer_of_match);

static struct platform_driver buzzer_platform_driver = {
    .probe  = buzzer_probe,
    .remove = buzzer_remove,
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = buzzer_of_match,
        .owner          = THIS_MODULE,
    },
};

module_platform_driver(buzzer_platform_driver);