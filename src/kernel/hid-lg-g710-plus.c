/*
 *  Logitech G710+ Keyboard Input Driver
 *
 *  Driver generates additional key events for the keys M1-MR, G1-G6
 *  and supports setting the backlight levels of the keyboard
 *
 *  Copyright (c) 2013 Filip Wieladke <Wattos@gmail.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/version.h>

#include "hid-ids.h"
#include "usbhid/usbhid.h"

#define USB_DEVICE_ID_LOGITECH_KEYBOARD_G710_PLUS 0xc24d

// 20 seeconds timeout
#define WAIT_TIME_OUT 20000

#define LOGITECH_KEY_MAP_SIZE 16

static const u8 g710_plus_key_map[LOGITECH_KEY_MAP_SIZE] = {
    0,       /* unused */
    0,       /* unused */
    0,       /* unused */
    0,       /* unused */
    KEY_F19, /* M1 */
    KEY_F20, /* M2 */
    KEY_F21, /* M3 */
    KEY_F22, /* MR */
    KEY_F13, /* G1 */
    KEY_F14, /* G2 */
    KEY_F15, /* G3 */
    KEY_F16, /* G4 */
    KEY_F17, /* G5 */
    KEY_F18, /* G6 */
    0,       /* unused */
    0,       /* unused */
};

/* Convenience macros */
#define lg_g710_plus_get_data(hdev) \
        ((struct lg_g710_plus_data *)(hid_get_drvdata(hdev)))

#define BIT_AT(var,pos) ((var) & (1<<(pos)))

struct lg_g710_plus_data {
    struct hid_report *g_mr_buttons_support_report; /* Needs to be written to enable G1-G6 and M1-MR keys */
    struct hid_report *mr_buttons_led_report; /* Controls the backlight of M1-MR buttons */
    struct hid_report *other_buttons_led_report; /* Controls the backlight of other buttons */
    struct hid_report *gamemode_report; /* Controls the backlight of other buttons */

    u8 modifier_state; /* holds the state of the 4 modifier keys, ie which one is active */
    u16 macro_button_state; /* Holds the last state of the G1-G6, M1-MR buttons. Required to know which buttons were pressed and which were released */
    struct hid_device *hdev; 
    struct input_dev *input_dev;
    struct attribute_group attr_group;

    u8 led_macro; /* state of the M1-MR macro leds as returned by the keyboard ==> binary coded 0 -> 0xF*/
    u8 led_keys; /* state of the WASD key leds as returned by the keyboard  ==> 0 -> 4 */

    spinlock_t lock; /* lock for communication with user space */
    struct completion ready; /* ready indicator */
};

static ssize_t lg_g710_plus_show_led_macro(struct device *device, struct device_attribute *attr, char *buf);
static ssize_t lg_g710_plus_store_led_macro(struct device *device, struct device_attribute *attr, const char *buf, size_t count);
static ssize_t lg_g710_plus_show_led_keys(struct device *device, struct device_attribute *attr, char *buf);
static ssize_t lg_g710_plus_store_led_keys(struct device *device, struct device_attribute *attr, const char *buf, size_t count);
static void lg_g710_plus_store_led_macro_internal(struct lg_g710_plus_data* data, unsigned long key_mask);

// registers virtual files
// TODO Create one to show active modifier key
static DEVICE_ATTR(led_macro, 0660, lg_g710_plus_show_led_macro, lg_g710_plus_store_led_macro);
static DEVICE_ATTR(led_keys,  0660, lg_g710_plus_show_led_keys,  lg_g710_plus_store_led_keys);

static struct attribute *lg_g710_plus_attrs[] = {
        &dev_attr_led_macro.attr,
        &dev_attr_led_keys.attr,
        NULL,
};

// get the mapped keycode for the active m key
// static int lg_g710_plus_extra_mr_active(struct hid_device *hdev) {
//     struct lg_g710_plus_data* g710_data = lg_g710_plus_get_data(hdev);
//     for (i = 4; i <= 7; i++) {
//         if (BIT_AT(g710_data->macro_button_state, i)) {
//             // store the keycode so it can easily emit later when G# keys are pressed
//             g710_data->modifier_key = g710_plus_key_map[i];
//
// //             return g710_plus_key_map[i];
//             break; // this means that the first (sorted by lowest number) M# key will win
//         }
//     }
//     return -1;
// }

// handles key presses
static int lg_g710_plus_extra_key_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    u8 i, j;
    u16 keys_pressed;
    struct lg_g710_plus_data* g710_data = lg_g710_plus_get_data(hdev);
    bool modifier_changed = false;
    if (g710_data == NULL || size < 3 || data[0] != 3) {
        return 1; /* cannot handle the event */
    }

    keys_pressed= data[1] << 8 | data[2];
    for (i = 0; i < LOGITECH_KEY_MAP_SIZE; i++) {
        // != -> mapped key
        // BIT_AT() -> key state changed from last state
        if (g710_plus_key_map[i] != 0 && (BIT_AT(keys_pressed, i) != BIT_AT(g710_data->macro_button_state, i)))
        {
            switch (g710_plus_key_map[i])
            {
            // G1 to G6
            case KEY_F13:
            case KEY_F14:
            case KEY_F15:
            case KEY_F16:
            case KEY_F17:
            case KEY_F18:
                if (BIT_AT(keys_pressed, i) != 0)
                {
                    // changing to pressed: modifiers first then g key
                    for (j = 0; j < 4; j++)
                        if (BIT_AT(g710_data->modifier_state, j))
                            input_report_key(g710_data->input_dev, g710_plus_key_map[j + 4], true);
                    input_report_key(g710_data->input_dev, g710_plus_key_map[i], true);
                }
                else {
                    // changing to non-pressed: g key then modifier
                    input_report_key(g710_data->input_dev, g710_plus_key_map[i], false);
                    for (j = 0; j < 4; j++)
                        if (BIT_AT(g710_data->modifier_state, j))
                            input_report_key(g710_data->input_dev, g710_plus_key_map[j + 4], false);
                }
                break;

                // M1
            case KEY_F19:
                if (BIT_AT(keys_pressed, i) != 0)
                {
                    modifier_changed = true;
                    g710_data->modifier_state ^= 1 << 0;
                }
                break;
                // M2
            case KEY_F20:
                if (BIT_AT(keys_pressed, i) != 0)
                {
                    modifier_changed = true;
                    g710_data->modifier_state ^= 1 << 1;
                }
                break;
                // M3
            case KEY_F21:
                if (BIT_AT(keys_pressed, i) != 0)
                {
                    modifier_changed = true;
                    g710_data->modifier_state ^= 1 << 2;
                }
                break;
                // MR
            case KEY_F22:
                if (BIT_AT(keys_pressed, i) != 0)
                {
                    modifier_changed = true;
                    g710_data->modifier_state ^= 1 << 3;
                }
                break;

            default:
                break;
            }
            if (modifier_changed)
                lg_g710_plus_store_led_macro_internal(g710_data, g710_data->modifier_state);
        }
    }
    input_sync(g710_data->input_dev);
    g710_data->macro_button_state= keys_pressed;
    return 1;
}

// gets macro lights state from keyboard
static int lg_g710_plus_extra_led_mr_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    struct lg_g710_plus_data* g710_data = lg_g710_plus_get_data(hdev);
    g710_data->led_macro= (data[1] >> 4) & 0xF;
    complete_all(&g710_data->ready);
    return 1;
}

// gets leds lights state from keyboard
static int lg_g710_plus_extra_led_keys_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    struct lg_g710_plus_data* g710_data = lg_g710_plus_get_data(hdev);
    g710_data->led_keys= data[1] << 4 | data[2];
    complete_all(&g710_data->ready);
    return 1;
}

// main input handler
static int lg_g710_plus_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
    switch(report->id) {
        case 3: return lg_g710_plus_extra_key_event(hdev, report, data, size);
        case 6: return lg_g710_plus_extra_led_mr_event(hdev, report, data, size);
        case 8: return lg_g710_plus_extra_led_keys_event(hdev, report, data, size);
        default: return 0;
    }
}

static int lg_g710_plus_input_mapping(struct hid_device *hdev, struct hid_input *hi, struct hid_field *field, struct hid_usage *usage, unsigned long **bit, int *max) 
{
    struct lg_g710_plus_data* data = lg_g710_plus_get_data(hdev);
    if (data != NULL && data->input_dev == NULL) {
        data->input_dev= hi->input;
    }
    return 0;
}

enum req_type {
    REQTYPE_READ,
    REQTYPE_WRITE
};

static void hidhw_request(struct hid_device *hdev, struct hid_report *report, enum req_type reqtype) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    hid_hw_request(hdev, report, reqtype == REQTYPE_READ ? HID_REQ_GET_REPORT : HID_REQ_SET_REPORT);
#else
    usbhid_submit_report(hdev, report, reqtype == REQTYPE_READ ? USB_DIR_IN : USB_DIR_OUT);
#endif
}

static int lg_g710_plus_initialize(struct hid_device *hdev) {
    int ret = 0;
    struct lg_g710_plus_data *data;
    struct list_head *feature_report_list = &hdev->report_enum[HID_FEATURE_REPORT].report_list;
    struct hid_report *report;

    if (list_empty(feature_report_list)) {
        return 0; /* Currently, the keyboard registers as two different devices */
    }

    data = lg_g710_plus_get_data(hdev);
    list_for_each_entry(report, feature_report_list, list) {
        switch(report->id) {
            case 6: data->mr_buttons_led_report= report; break;
            case 8: data->other_buttons_led_report= report; break;
            case 9:
                data->g_mr_buttons_support_report= report;
                hidhw_request(hdev, report, REQTYPE_WRITE);
                break;
        }
    }

    ret= sysfs_create_group(&hdev->dev.kobj, &data->attr_group);
    return ret;
}

static struct lg_g710_plus_data* lg_g710_plus_create(struct hid_device *hdev)
{
    struct lg_g710_plus_data* data;
    data= kzalloc(sizeof(struct lg_g710_plus_data), GFP_KERNEL);
    if (data == NULL) {
        return NULL;
    }

    data->attr_group.name= "logitech-g710";
    data->attr_group.attrs= lg_g710_plus_attrs;
    data->hdev= hdev;

    spin_lock_init(&data->lock);
    init_completion(&data->ready);
    return data;
}

static int lg_g710_plus_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;
    struct lg_g710_plus_data *data;

    data = lg_g710_plus_create(hdev);
    if (data == NULL) {
        dev_err(&hdev->dev, "can't allocate space for Logitech G710+ device attributes\n");
        ret= -ENOMEM;
        goto err_free;
    }
    hid_set_drvdata(hdev, data);

    /*
     * Without this, the device would send a first report with a key down event for
     * certain buttons, but never the key up event
     */
    hdev->quirks |= HID_QUIRK_NOGET;

    ret = hid_parse(hdev);
    if (ret) {
        hid_err(hdev, "parse failed\n");
        goto err_free;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) {
        hid_err(hdev, "hw start failed\n");
        goto err_free;
    }

    ret= lg_g710_plus_initialize(hdev);
    if (ret) {
        ret = -ret;
        hid_hw_stop(hdev);
        goto err_free;
    }

    return 0;

err_free:
    if (data != NULL) {
        kfree(data);
    }
    return ret;
}

static void lg_g710_plus_remove(struct hid_device *hdev)
{
    struct lg_g710_plus_data* data = lg_g710_plus_get_data(hdev);
    struct list_head *feature_report_list = &hdev->report_enum[HID_FEATURE_REPORT].report_list;

    if (data != NULL && !list_empty(feature_report_list))
        sysfs_remove_group(&hdev->dev.kobj, &data->attr_group);

    hid_hw_stop(hdev);
    if (data != NULL) {
        kfree(data);
    }
}

static ssize_t lg_g710_plus_show_led_macro(struct device *device, struct device_attribute *attr, char *buf)
{
    struct lg_g710_plus_data* data = hid_get_drvdata(dev_get_drvdata(device->parent));
    if (data != NULL) {
        spin_lock(&data->lock);
        init_completion(&data->ready);
        hidhw_request(data->hdev, data->mr_buttons_led_report, REQTYPE_READ);
        wait_for_completion_timeout(&data->ready, WAIT_TIME_OUT);
        spin_unlock(&data->lock);
        return sprintf(buf, "%d\n", data->led_macro);
    }
    return 0;
}

static ssize_t lg_g710_plus_show_led_keys(struct device *device, struct device_attribute *attr, char *buf)
{
    struct lg_g710_plus_data* data = hid_get_drvdata(dev_get_drvdata(device->parent));
    if (data != NULL) {
        spin_lock(&data->lock);
        init_completion(&data->ready);
        hidhw_request(data->hdev, data->other_buttons_led_report, REQTYPE_READ);
        wait_for_completion_timeout(&data->ready, WAIT_TIME_OUT);
        spin_unlock(&data->lock);
        return sprintf(buf, "%d\n", data->led_keys);
    }
    return 0;
}

static void lg_g710_plus_store_led_macro_internal(struct lg_g710_plus_data* data, unsigned long key_mask)
{
//     struct lg_g710_plus_data* data = lg_g710_plus_get_data(hdev);
    spin_lock(&data->lock);
    data->mr_buttons_led_report->field[0]->value[0]= (key_mask & 0xF) << 4;
    hidhw_request(data->hdev, data->mr_buttons_led_report, REQTYPE_WRITE);
    spin_unlock(&data->lock);
}

// presumably the store and show correspond to fio opperations on virtual file, read/write.
static ssize_t lg_g710_plus_store_led_macro(struct device *device, struct device_attribute *attr, const char *buf, size_t count)
{
    unsigned long key_mask;
    int retval;
    struct lg_g710_plus_data* data = hid_get_drvdata(dev_get_drvdata(device->parent));
    retval = kstrtoul(buf, 10, &key_mask);
    if (retval)
        return retval;

    spin_lock(&data->lock);
    data->mr_buttons_led_report->field[0]->value[0]= (key_mask & 0xF) << 4;
    hidhw_request(data->hdev, data->mr_buttons_led_report, REQTYPE_WRITE);
    spin_unlock(&data->lock);
    return count;
}

static ssize_t lg_g710_plus_store_led_keys(struct device *device, struct device_attribute *attr, const char *buf, size_t count)
{
    int retval;
    unsigned long key_mask;
    u8 wasd_mask, keys_mask;
    struct lg_g710_plus_data* data = hid_get_drvdata(dev_get_drvdata(device->parent));
    retval = kstrtoul(buf, 10, &key_mask);
    if (retval)
        return retval;

    wasd_mask= (key_mask >> 4) & 0xF;
    keys_mask= (key_mask) & 0xF;

    wasd_mask= wasd_mask > 4 ? 4 : wasd_mask;
    keys_mask= keys_mask > 4 ? 4 : keys_mask;

    spin_lock(&data->lock);
    data->other_buttons_led_report->field[0]->value[0]= wasd_mask;
    data->other_buttons_led_report->field[0]->value[1]= keys_mask;
    hidhw_request(data->hdev, data->other_buttons_led_report, REQTYPE_WRITE);
    spin_unlock(&data->lock);
    return count;
}

static const struct hid_device_id lg_g710_plus_devices[] = {
    { HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_KEYBOARD_G710_PLUS) },
    { }
};

MODULE_DEVICE_TABLE(hid, lg_g710_plus_devices);
static struct hid_driver lg_g710_plus_driver = {
    .name = "hid-lg-g710-plus",
    .id_table = lg_g710_plus_devices,
    .raw_event = lg_g710_plus_raw_event,
    .input_mapping = lg_g710_plus_input_mapping,
    .probe= lg_g710_plus_probe,
    .remove= lg_g710_plus_remove,
};

static int __init lg_g710_plus_init(void)
{
    return hid_register_driver(&lg_g710_plus_driver);
}

static void __exit lg_g710_plus_exit(void)
{
    hid_unregister_driver(&lg_g710_plus_driver);
}

module_init(lg_g710_plus_init);
module_exit(lg_g710_plus_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Wieladek <Wattos@gmail.com>");
MODULE_DESCRIPTION("Logitech G710+ driver");
