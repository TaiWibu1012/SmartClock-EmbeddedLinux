/**
 * @file btn_driver.c
 * @brief [P2-M1] Character Device Driver for Smart Weather Alarm Clock Push Button (GPIO Dual-Edge IRQ, Debounce & Timestamp)
 * @author PHUC TAI
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/wait.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <linux/version.h>

#include "../../include/smartclock_common.h"

#define DRIVER_NAME         "btn_driver"
#define CLASS_NAME          "smartclock_btn"
#define BUFFER_SIZE         16
#define DEBOUNCE_TIME_NS    (20 * 1000000ULL) /* 20 ms debounce threshold */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PHUC TAI");
MODULE_DESCRIPTION("GPIO Interrupt-driven Button Driver with Timestamping");
MODULE_VERSION("1.0");

struct btn_device_data {
    int gpio_pin;
    int irq_number;
    dev_t dev_num;
    struct cdev cdev;
    struct class *dev_class;
    struct device *dev_device;

    /* Circular event buffer */
    struct button_event event_buf[BUFFER_SIZE];
    int head;
    int tail;
    spinlock_t lock;
    wait_queue_head_t wait_queue;

    /* Debounce state */
    ktime_t last_irq_time;
};

static struct btn_device_data btn_data;

/* Interrupt Service Routine (Top-Half) */
static irqreturn_t btn_irq_handler(int irq, void *dev_id)
{
    ktime_t now = ktime_get();
    s64 delta_ns;
    int pin_val;
    int next_head;
    unsigned long flags;

    /* 
     * 1. Software Debounce Filter (20ms threshold):
     * Mechanical tactile buttons physically produce contact bounce noise (spurious transitions)
     * lasting typically 5-15ms upon pressing or releasing. A 20ms threshold (DEBOUNCE_TIME_NS)
     * effectively filters 100% of contact chatter while guaranteeing immediate responsiveness
     * to intentional human button presses (human clicks are typically > 50ms).
     */
    delta_ns = ktime_to_ns(ktime_sub(now, btn_data.last_irq_time));
    if (delta_ns < DEBOUNCE_TIME_NS) {
        return IRQ_HANDLED; /* Ignore contact bounce noise */
    }
    btn_data.last_irq_time = now;

    /* 2. Read physical pin state (Active-Low: 0 = Pressed, 1 = Released) */
    pin_val = gpio_get_value(btn_data.gpio_pin);

    spin_lock_irqsave(&btn_data.lock, flags);

    next_head = (btn_data.head + 1) % BUFFER_SIZE;
    if (next_head != btn_data.tail) {
        btn_data.event_buf[btn_data.head].state = (pin_val == 0) ? BTN_STATE_PRESSED : BTN_STATE_RELEASED;
        btn_data.event_buf[btn_data.head].timestamp_ns = ktime_to_ns(now);
        btn_data.head = next_head;
    } else {
        pr_warn(DRIVER_NAME ": Event buffer overflow! head=%d tail=%d capacity=%d. Dropping event.\n",
                btn_data.head, btn_data.tail, BUFFER_SIZE);
    }

    spin_unlock_irqrestore(&btn_data.lock, flags);

    /* 3. Wake up userspace blocking read thread */
    wake_up_interruptible(&btn_data.wait_queue);

    return IRQ_HANDLED;
}

/* File Operations: read */
static ssize_t btn_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct button_event ev;
    unsigned long flags;
    int ret;

    if (count < sizeof(struct button_event))
        return -EINVAL;

    /* Block until an event is available in the ring buffer */
    ret = wait_event_interruptible(btn_data.wait_queue, (btn_data.head != btn_data.tail));
    if (ret != 0)
        return -ERESTARTSYS; /* Interrupted by signal */

    spin_lock_irqsave(&btn_data.lock, flags);
    ev = btn_data.event_buf[btn_data.tail];
    btn_data.tail = (btn_data.tail + 1) % BUFFER_SIZE;
    spin_unlock_irqrestore(&btn_data.lock, flags);

    if (copy_to_user(buf, &ev, sizeof(struct button_event)))
        return -EFAULT;

    return sizeof(struct button_event);
}

static int btn_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int btn_release(struct inode *inode, struct file *file)
{
    return 0;
}

static __poll_t btn_poll(struct file *file, struct poll_table_struct *wait)
{
    __poll_t mask = 0;
    unsigned long flags;

    poll_wait(file, &btn_data.wait_queue, wait);

    spin_lock_irqsave(&btn_data.lock, flags);
    if (btn_data.head != btn_data.tail) {
        mask |= (EPOLLIN | EPOLLRDNORM);
    }
    spin_unlock_irqrestore(&btn_data.lock, flags);

    return mask;
}

static const struct file_operations btn_fops = {
    .owner   = THIS_MODULE,
    .open    = btn_open,
    .release = btn_release,
    .read    = btn_read,
    .poll    = btn_poll,
};

/* Platform Driver Probe */
static int btn_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    pr_info(DRIVER_NAME ": Probing SmartClock Button Device...\n");

    /* 1. Extract GPIO Pin dynamically from Device Tree */
    btn_data.gpio_pin = of_get_named_gpio(dev->of_node, "gpios", 0);
    if (!gpio_is_valid(btn_data.gpio_pin)) {
        dev_err(dev, "Invalid GPIO pin from Device Tree node\n");
        return -EINVAL;
    }

    /* 2. Request GPIO and set direction input */
    ret = devm_gpio_request_one(dev, btn_data.gpio_pin, GPIOF_IN, "smartclock_btn_gpio");
    if (ret) {
        dev_err(dev, "Failed to request GPIO %d\n", btn_data.gpio_pin);
        return ret;
    }

    /* 3. Allocate Char Device Region */
    ret = alloc_chrdev_region(&btn_data.dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        dev_err(dev, "Failed to allocate chrdev region\n");
        return ret;
    }

    cdev_init(&btn_data.cdev, &btn_fops);
    btn_data.cdev.owner = THIS_MODULE;
    ret = cdev_add(&btn_data.cdev, btn_data.dev_num, 1);
    if (ret < 0) {
        dev_err(dev, "Failed to add cdev\n");
        goto unregister_chrdev;
    }

    /* 4. Create sysfs class & device node (/dev/btn_driver) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    btn_data.dev_class = class_create(CLASS_NAME);
#else
    btn_data.dev_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(btn_data.dev_class)) {
        dev_err(dev, "Failed to create class\n");
        ret = PTR_ERR(btn_data.dev_class);
        goto del_cdev;
    }

    btn_data.dev_device = device_create(btn_data.dev_class, NULL, btn_data.dev_num, NULL, DRIVER_NAME);
    if (IS_ERR(btn_data.dev_device)) {
        dev_err(dev, "Failed to create device /dev/%s\n", DRIVER_NAME);
        ret = PTR_ERR(btn_data.dev_device);
        goto destroy_class;
    }

    /* 5. Initialize synchronization primitives */
    spin_lock_init(&btn_data.lock);
    init_waitqueue_head(&btn_data.wait_queue);
    btn_data.head = 0;
    btn_data.tail = 0;
    btn_data.last_irq_time = ktime_set(0, 0);

    /* 6. Request Dual-Edge GPIO Interrupt */
    btn_data.irq_number = gpio_to_irq(btn_data.gpio_pin);
    ret = request_irq(btn_data.irq_number,
                      btn_irq_handler,
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                      "smartclock_btn_irq",
                      NULL);
    if (ret) {
        dev_err(dev, "Failed to request IRQ %d\n", btn_data.irq_number);
        goto destroy_device;
    }

    dev_info(dev, "Driver loaded successfully! GPIO: %d, IRQ: %d, Node: /dev/%s\n",
             btn_data.gpio_pin, btn_data.irq_number, DRIVER_NAME);
    return 0;

destroy_device:
    device_destroy(btn_data.dev_class, btn_data.dev_num);
destroy_class:
    class_destroy(btn_data.dev_class);
del_cdev:
    cdev_del(&btn_data.cdev);
unregister_chrdev:
    unregister_chrdev_region(btn_data.dev_num, 1);
    return ret;
}

/* Platform Driver Remove */
static int btn_remove(struct platform_device *pdev)
{
    free_irq(btn_data.irq_number, NULL);
    device_destroy(btn_data.dev_class, btn_data.dev_num);
    class_destroy(btn_data.dev_class);
    cdev_del(&btn_data.cdev);
    unregister_chrdev_region(btn_data.dev_num, 1);
    pr_info(DRIVER_NAME ": Driver unloaded safely.\n");
    return 0;
}

static const struct of_device_id btn_of_match[] = {
    { .compatible = "devlinux,smartclock-button", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, btn_of_match);

static struct platform_driver btn_platform_driver = {
    .probe  = btn_probe,
    .remove = btn_remove,
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = btn_of_match,
        .owner          = THIS_MODULE,
    },
};

module_platform_driver(btn_platform_driver);