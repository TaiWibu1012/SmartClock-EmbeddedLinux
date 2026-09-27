/**
 * @file btn_driver.c
 * @brief [P2-M1 Refactored] Linux Input Subsystem Driver for Push Button (evdev, EV_KEY & Sysfs Metrics)
 * @author PHUC TAI
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/input.h>
#include <linux/ktime.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#define DRIVER_NAME         "btn_driver"
#define DEBOUNCE_TIME_NS    (20 * 1000000ULL) /* 20 ms debounce threshold */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PHUC TAI");
MODULE_DESCRIPTION("SmartClock Push Button Linux Input Subsystem Driver");
MODULE_VERSION("2.0");

struct btn_device_data {
    int gpio_pin;
    int irq_number;
    struct input_dev *input_dev;
    ktime_t last_irq_time;

    /* Telemetry metrics exposed via sysfs */
    atomic_t press_count;
    atomic_t debounce_drops;
};

static struct btn_device_data btn_data;

/* Interrupt Service Routine (Top-Half) */
static irqreturn_t btn_irq_handler(int irq, void *dev_id)
{
    (void)irq;
    (void)dev_id;
    ktime_t now = ktime_get();
    s64 delta_ns = ktime_to_ns(ktime_sub(now, btn_data.last_irq_time));
    int pin_val;
    int is_pressed;

    /* 
     * 1. Software Debounce Filter (20ms threshold):
     * Hardware contact chatter typically lasts 5-15ms. 
     * A 20ms threshold cleanly drops mechanical bouncing without human-perceptible lag.
     */
    if (delta_ns < DEBOUNCE_TIME_NS) {
        atomic_inc(&btn_data.debounce_drops);
        return IRQ_HANDLED;
    }
    btn_data.last_irq_time = now;

    /* 2. Read physical pin state (Active-Low from Device Tree: 0 = Pressed, 1 = Released) */
    pin_val = gpio_get_value(btn_data.gpio_pin);
    is_pressed = (pin_val == 0) ? 1 : 0;

    if (is_pressed) {
        atomic_inc(&btn_data.press_count);
    }

    /* 
     * 3. Linux Input Core Event Generation:
     * Report standard EV_KEY event with KEY_POWER.
     * input_report_key() and input_sync() are lock-safe in interrupt context.
     */
    input_report_key(btn_data.input_dev, KEY_POWER, is_pressed);
    input_sync(btn_data.input_dev);

    return IRQ_HANDLED;
}

/* Sysfs Attribute: /sys/devices/platform/.../press_count */
static ssize_t press_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sprintf(buf, "%d\n", atomic_read(&btn_data.press_count));
}
static DEVICE_ATTR_RO(press_count);

/* Sysfs Attribute: /sys/devices/platform/.../debounce_drops */
static ssize_t debounce_drops_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sprintf(buf, "%d\n", atomic_read(&btn_data.debounce_drops));
}
static DEVICE_ATTR_RO(debounce_drops);

static struct attribute *btn_attrs[] = {
    &dev_attr_press_count.attr,
    &dev_attr_debounce_drops.attr,
    NULL,
};

static const struct attribute_group btn_attr_group = {
    .attrs = btn_attrs,
};

/* Platform Driver Probe */
static int btn_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    pr_info(DRIVER_NAME ": Probing SmartClock Input Subsystem Button...\n");

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

    /* 3. Allocate Linux Input Device with managed lifecycle */
    btn_data.input_dev = devm_input_allocate_device(dev);
    if (!btn_data.input_dev) {
        dev_err(dev, "Failed to allocate input device\n");
        return -ENOMEM;
    }

    btn_data.input_dev->name = "SmartClock Push Button";
    btn_data.input_dev->phys = "smartclock/button";
    btn_data.input_dev->id.bustype = BUS_HOST;
    btn_data.input_dev->id.vendor = 0x0001;
    btn_data.input_dev->id.product = 0x0001;
    btn_data.input_dev->id.version = 0x0200;

    /* Declare capabilities: EV_KEY with KEY_POWER */
    input_set_capability(btn_data.input_dev, EV_KEY, KEY_POWER);

    /* 4. Register Input Device with Linux Input Core */
    ret = input_register_device(btn_data.input_dev);
    if (ret) {
        dev_err(dev, "Failed to register input device: %d\n", ret);
        return ret;
    }

    /* 5. Initialize telemetry counters and debounce state */
    atomic_set(&btn_data.press_count, 0);
    atomic_set(&btn_data.debounce_drops, 0);
    btn_data.last_irq_time = ktime_set(0, 0);

    /* 6. Create Sysfs Attribute Group for observability */
    ret = sysfs_create_group(&dev->kobj, &btn_attr_group);
    if (ret) {
        dev_warn(dev, "Failed to create sysfs attribute group: %d\n", ret);
    }

    /* 7. Request Dual-Edge GPIO Interrupt using devm */
    btn_data.irq_number = gpio_to_irq(btn_data.gpio_pin);
    ret = devm_request_irq(dev,
                           btn_data.irq_number,
                           btn_irq_handler,
                           IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                           "smartclock_btn_irq",
                           &btn_data);
    if (ret) {
        dev_err(dev, "Failed to request IRQ %d\n", btn_data.irq_number);
        sysfs_remove_group(&dev->kobj, &btn_attr_group);
        return ret;
    }

    dev_info(dev, "Driver loaded successfully! GPIO: %d, IRQ: %d, Input Device: '%s'\n",
             btn_data.gpio_pin, btn_data.irq_number, btn_data.input_dev->name);
    return 0;
}

/* Platform Driver Remove */
static int btn_remove(struct platform_device *pdev)
{
    sysfs_remove_group(&pdev->dev.kobj, &btn_attr_group);
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