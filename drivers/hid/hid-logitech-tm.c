/*
 *  HID driver for Logitech Touch Mice devices
 *
 *  Copyright (c) 2011 Logitech
 *  Copyright (c) 2012 Google
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so by e-mail send
 * your message to Benjamin Tissoires <benjamin.tissoires at gmail com>
 *
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Andrew de los Reyes <adlr@chromium.org>");
MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_AUTHOR("Nestor Lopez Casado <nlopezcasad@logitech.com>");
MODULE_DESCRIPTION("Logitech Wireless Touch Mice");
MODULE_LICENSE("GPL");

#include "hid-ids.h"
#include "hid-logitech-hidpp.h"

static bool use_raw_mode = true;
module_param(use_raw_mode, bool, 0644);
MODULE_PARM_DESC(use_raw_mode, "Use raw mode");

#define SOFTWARE_ID 0xB
#define FEATURE_TOUCH_MOUSE_RAW_POINTS 0x6110
#define FEATURE_TOUCH_MOUSE_1B03 0x1b03

#define MIN(__a, __b) ((__a) < (__b) ? (__a) : (__b))
#define MAX(__a, __b) ((__a) > (__b) ? (__a) : (__b))

struct tm_touchpad_info {
	__u16 x_size, y_size;
	__u16 resolution;
	__u8 origin_position;
	__u8 max_fingers;
	__u8 max_width;
};

/* These are values for button_depressor values below. */
#define DEPRESSOR_NONE 0
#define DEPRESSOR_MOUSE 1  /* From mouse report */
#define DEPRESSOR_RAWPTS 2  /* From HidPP 0x6110 TouchMouseRawTouchPoints */
#define DEPRESSOR_1B03 3  /* From HidPP 0x1b03 */

struct tm_data {
	struct input_dev *input;
	struct hidpp_device *hidpp_dev;
	struct work_struct work;
	struct tm_touchpad_info tp_info;
	__u8 mt_feature_index;
	__u8 feature_1b03;
	__u16 next_tracking_id;
	__u8 prev_slots_used;  /* bit mask: 1 << slot_num */
	/* Which type of report was responsible for pressing the button down */
	__u8 button_depressor[3];  /* index is 0:BTN_LEFT, 1:RIGHT, 2:MIDDLE */
	spinlock_t lock;
	unsigned ignore_mouse_report_buttons:1;
	unsigned hid_hw_started:1;
	unsigned in_raw_mode:1;
	unsigned raw_switch_requested:1;
};

static int tm_set_raw_report_state(struct hidpp_device *hidpp_dev);

/*
 * Some of these mice seem to report button presses in unusual ways:
 * They may report the same button multiple times via differnt reports,
 * some with extra delay. Also, the presense of a finger on the surface
 * may change which type of report is used to report button change.
 *
 * In this function, we look for many ways a button may be reported, updates
 * fd->button_state and fd->button_depressor, and sends the proper input
 * events for the new fd->button_state.
 */
static void emit_buttons(struct tm_data *fd, u8 *bytes) {
	u8 idx_left = 0, idx_middle = 2;
	int buttons[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
	int i;
	
	if (bytes[0] == 0x2 && !fd->ignore_mouse_report_buttons) {
		for (i = idx_left; i <= idx_middle; i++) {
			u8 mask = 1 << i;
			if (!fd->button_depressor[i] && bytes[1] & mask) {
				fd->button_depressor[i] = DEPRESSOR_MOUSE;
			} else if (fd->button_depressor[i] == DEPRESSOR_MOUSE &&
				!(bytes[1] & mask)) {
				fd->button_depressor[i] = DEPRESSOR_NONE;
			}
		}
	}
	/* TODO(adlr): use proper feature index instead of 0x1b */
	if (bytes[0] == 0x11 && bytes[2] == fd->feature_1b03) {
		if (!fd->button_depressor[idx_middle] && bytes[5] != 0) {
			fd->button_depressor[idx_middle] = DEPRESSOR_1B03;
		} else if (fd->button_depressor[idx_middle] == DEPRESSOR_1B03
			&& bytes[5] == 0) {
			fd->button_depressor[idx_middle] = DEPRESSOR_NONE;
		}
	}
	if (bytes[0] == 0x11 && bytes[2] == fd->mt_feature_index &&
		(bytes[3] >> 4) == 1) {
		if (!fd->button_depressor[idx_left] && (bytes[4] & 2)) {
			fd->button_depressor[idx_left] = DEPRESSOR_RAWPTS;
		} else if (fd->button_depressor[idx_left] == DEPRESSOR_RAWPTS
			&& !(bytes[4] & 2)) {
			fd->button_depressor[idx_left] = DEPRESSOR_NONE;
		}
	}

	/* Send input reports */
	for (i = idx_left; i <= idx_middle; i++) {
		dbg_hid("report key: 0x%x: %d\n", buttons[i], fd->button_depressor[i] != DEPRESSOR_NONE);
		input_report_key(fd->input, buttons[i],
			fd->button_depressor[i] != DEPRESSOR_NONE);
	}
}

#define ORIGIN_LOWER_LEFT 1
#define ORIGIN_LOWER_RIGHT 2
#define ORIGIN_UPPER_LEFT 3
#define ORIGIN_UPPER_RIGHT 4

static void emit_fingers(struct tm_data *fd, u8 *bytes)
{
	int i;
	u8 slots_used = 0;
	u8 swap_x = fd->tp_info.origin_position == ORIGIN_LOWER_RIGHT ||
		fd->tp_info.origin_position == ORIGIN_UPPER_RIGHT;
	u8 swap_y = fd->tp_info.origin_position == ORIGIN_LOWER_LEFT ||
		fd->tp_info.origin_position == ORIGIN_LOWER_RIGHT;
	for (i = 0; i < fd->tp_info.max_fingers; i++) {
		u8 *rec = bytes + 4 * i;
		int x_pos = ((int)rec[0])<<4 | (rec[2] & 0xf);
		int y_pos = ((int)rec[1])<<4 | (rec[2] >> 4);
		int width_x = (rec[3] >> 4) + 1;
		int width_y = (rec[3] & 0xf) + 1;
		u8 slot_mask = 1 << i;

		input_mt_slot(fd->input, i);
		if (rec[0] == 0xff && rec[1] == 0xff && rec[2] == 0xff && rec[3] == 0xff) {
			input_event(fd->input, EV_ABS, ABS_MT_TRACKING_ID, -1);
			continue;
		}
		slots_used |= slot_mask;
		if (!(fd->prev_slots_used & slot_mask)) {
			/* new finger. assign new tracking id */
			input_event(fd->input, EV_ABS, ABS_MT_TRACKING_ID,
				fd->next_tracking_id);
			fd->next_tracking_id++;
			if (fd->next_tracking_id == 0xffff)
				fd->next_tracking_id = 1;
		}
		if (swap_x)
			x_pos = fd->tp_info.x_size - x_pos;
		if (swap_y)
			y_pos = fd->tp_info.y_size - y_pos;
		input_event(fd->input, EV_ABS, ABS_MT_POSITION_X,
				x_pos);
		input_event(fd->input, EV_ABS, ABS_MT_POSITION_Y,
				y_pos);
		input_event(fd->input, EV_ABS, ABS_MT_PRESSURE,
				MIN(MAX(30, width_x * width_y * 3), 255));
		
	}

	input_event(fd->input, EV_KEY, BTN_TOOL_FINGER, slots_used == 1);
	input_event(fd->input, EV_KEY, BTN_TOOL_DOUBLETAP, slots_used == 2);
	input_event(fd->input, EV_KEY, BTN_TOOL_TRIPLETAP, slots_used == 3);
	input_event(fd->input, EV_KEY, BTN_TOOL_QUADTAP, slots_used == 4);

	fd->prev_slots_used = slots_used;
}

static int tm_raw_event(struct hidpp_device *hidpp_dev,
		struct hidpp_report *hidpp_report)
{
	struct tm_data *fd = (struct tm_data *)hidpp_dev->driver_data;
	dbg_hid("Got raw event %02x %02x\n",
		hidpp_report->report_id,
		hidpp_report->device_index);

	if (!fd->hid_hw_started) {
		dbg_hid("Early abort b/c hardware not ready yet\n");
		return 1;  // do nothing more
	}

	if (hidpp_report->report_id == 0x02) {
		int shift = sizeof(int)*8 - 12;
		int value = hidpp_report->rap.reg_address;
		value |= ((int)(hidpp_report->rap.params[0] & 0xf)) << 8;
		value = (value << shift) >> shift;
		input_report_rel(fd->input, REL_X, value);

		value = hidpp_report->rap.params[1];
		value = (value<<4) | (int)(hidpp_report->rap.params[0] >> 4);
		value = (value << shift) >> shift;

		input_report_rel(fd->input, REL_Y, value);
	} else if (hidpp_report->report_id == 0x11) {
		dbg_hid("got 0x11 (%02x %02x)\n", hidpp_report->rap.sub_id, fd->mt_feature_index);
		if (hidpp_report->rap.sub_id == fd->mt_feature_index) {
			dbg_hid("got 0x11 fidx\n");
			if ((hidpp_report->rap.reg_address >> 4) == 0) {
				// raw finger data
				emit_fingers(fd, hidpp_report->rap.params);
			}
		}
	}
	emit_buttons(fd, (u8*)hidpp_report);
	input_sync(fd->input);	
	return 1;  // do nothing more
}

static void delayedwork_callback(struct work_struct *work)
{
	unsigned long flags;
	struct tm_data *fd =
		container_of(work, struct tm_data, work);
	struct hidpp_device *hidpp_dev = fd->hidpp_dev;
	int ret;
	dbg_hid("%s START\n", __func__);

	spin_lock_irqsave(&fd->lock, flags);
	if (fd->in_raw_mode) {
		spin_unlock_irqrestore(&fd->lock, flags);
		dbg_hid("%s: already in raw mode\n", __func__);
		return;
	}
	spin_unlock_irqrestore(&fd->lock, flags);


	ret = tm_set_raw_report_state(hidpp_dev);
	
	if (ret) {
		hid_err(hidpp_dev->hid_dev, "unable to set to raw report mode. "
			"The device may not be in range.\n");
	} else {
		/* set up the input device */
		if (!fd->hid_hw_started) {
			ret = hid_hw_start(hidpp_dev->hid_dev, HID_CONNECT_DEFAULT);
			if (ret) {
				dbg_hid("hid_hw_start failed: %d\n", ret);
			} else {
				fd->hid_hw_started = 1;
			}
		}
	}

	spin_lock_irqsave(&fd->lock, flags);
	if (!ret)
		fd->in_raw_mode = 1;
	fd->raw_switch_requested = 0;
	spin_unlock_irqrestore(&fd->lock, flags);

	dbg_hid("%s END\n", __func__);
}

static int tm_hidpp_send_sync(struct hidpp_device *hidpp_dev,
				u8 type,
				u8 feature_index,
				u8 sub_index,
				u8 software_id,
				u8 *params,
				int params_count,
				struct hidpp_report *response)
{
	return hidpp_send_rap_command_sync(hidpp_dev,
					type,
					feature_index,
					(sub_index << 4) | software_id,
					params,
					params_count,
					response);
}

static int tm_set_raw_report_state(struct hidpp_device *hidpp_dev)
{
	struct tm_data *fd = (struct tm_data *)hidpp_dev->driver_data;
	struct hidpp_report response;
	int ret = 0;
	u8 params[2];
	u8 f_idx;
	
	// Get hid++ version number
	ret = tm_hidpp_send_sync(hidpp_dev, REPORT_ID_HIDPP_SHORT,
				0, 1,
				SOFTWARE_ID,
				NULL, 0, &response);
	if (ret) {
		dbg_hid("send root cmd returned: %d", ret);
		return -ret;
	}
	
	dbg_hid("HID++ version: %d.%d\n", response.rap.params[0],
		response.rap.params[1]);
	// Get Feature index of 0x6110
	params[0] = (FEATURE_TOUCH_MOUSE_RAW_POINTS >> 8);
	params[1] = (FEATURE_TOUCH_MOUSE_RAW_POINTS & 0xff);
	ret = tm_hidpp_send_sync(hidpp_dev, REPORT_ID_HIDPP_SHORT,
				0, 0,
				SOFTWARE_ID,
				params, 2, &response);
	if (ret)
		return -ret;
	f_idx = response.rap.params[0];
	fd->mt_feature_index = f_idx;
	dbg_hid("Feature index of 0x%x: %d\n", FEATURE_TOUCH_MOUSE_RAW_POINTS, f_idx);

	// Get Feature index of 0x1b03
	params[0] = (FEATURE_TOUCH_MOUSE_1B03 >> 8);
	params[1] = (FEATURE_TOUCH_MOUSE_1B03 & 0xff);
	ret = tm_hidpp_send_sync(hidpp_dev, REPORT_ID_HIDPP_SHORT,
				0, 0,
				SOFTWARE_ID,
				params, 2, &response);
	if (ret)
		return -ret;
	fd->feature_1b03 = response.rap.params[0];
	dbg_hid("Feature index of 0x%x: %d\n", FEATURE_TOUCH_MOUSE_1B03, fd->feature_1b03);
	
	// Get params
	ret = tm_hidpp_send_sync(hidpp_dev, REPORT_ID_HIDPP_SHORT,
				f_idx, 0,
				SOFTWARE_ID,
				NULL, 0, &response);
	if (ret)
		return -ret;
	fd->tp_info.x_size = ((__u16)response.rap.params[0]) << 8 |
		response.rap.params[1];
	fd->tp_info.y_size = ((__u16)response.rap.params[2]) << 8 |
		response.rap.params[3];
	fd->tp_info.resolution = ((__u16)response.rap.params[4]) << 8 |
		response.rap.params[5];
	fd->tp_info.origin_position = response.rap.params[6];
	fd->tp_info.max_fingers = response.rap.params[7];
	fd->tp_info.max_width = response.rap.params[8];

	// // Request raw mode
	params[0] = 3;
	ret = tm_hidpp_send_sync(hidpp_dev, REPORT_ID_HIDPP_SHORT,
				f_idx, 2,
				SOFTWARE_ID,
				params, 1, &response);
	if (ret)
		return -ret;
	dbg_hid("Requested raw mode!\n");
	
	return ret;
}

static int tm_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	struct hidpp_device *hidpp_dev = hid_get_drvdata(hdev);
	struct tm_data *fd = (struct tm_data *)hidpp_dev->driver_data;
	struct input_dev *input = hi->input;

	dbg_hid("%s:\n", __func__);

	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_BUTTON)
		return -1;
	
	fd->input = hi->input;
	
	__set_bit(EV_KEY, input->evbit);

	switch (fd->tp_info.max_fingers) {
	case 4: __set_bit(BTN_TOOL_QUADTAP, input->keybit);
	case 3: __set_bit(BTN_TOOL_TRIPLETAP, input->keybit);
	case 2: __set_bit(BTN_TOOL_DOUBLETAP, input->keybit);
	case 1: __set_bit(BTN_TOOL_FINGER, input->keybit);
		__set_bit(BTN_TOUCH, input->keybit);
	}
	
	input_set_capability(input, EV_KEY, BTN_TOUCH);

	__set_bit(BTN_LEFT, input->keybit);
	__set_bit(BTN_RIGHT, input->keybit);
	__set_bit(BTN_MIDDLE, input->keybit);

	__set_bit(EV_ABS, input->evbit);
	
	input_mt_init_slots(input, MAX(2, fd->tp_info.max_fingers));
	
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR,
		0, fd->tp_info.max_width, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR,
		0, fd->tp_info.max_width, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE,
		0, MAX(255, fd->tp_info.max_width * fd->tp_info.max_width),
		0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_X,
		0, fd->tp_info.x_size, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y,
		0, fd->tp_info.y_size, 0, 0);
	input_set_abs_params(input, ABS_X,
		0, fd->tp_info.x_size, 0, 0);
	input_set_abs_params(input, ABS_Y,
		0, fd->tp_info.y_size, 0, 0);
	input_set_capability(input, EV_REL, REL_X);
	input_set_capability(input, EV_REL, REL_Y);
	
	return 0;
}

static void tm_connect_change(struct hidpp_device *hidpp_dev, bool connected)
{
	struct tm_data *fd = (struct tm_data *)hidpp_dev->driver_data;
	dbg_hid("%s: connected:%d\n", __func__, connected);
	if (connected && fd && fd->hid_hw_started)
		tm_set_raw_report_state(hidpp_dev);
}

static int tm_device_init(struct hidpp_device *hidpp_dev)
{
	int ret = 0;
	// struct hidpp_touchpad_raw_info raw_info = {};
	// struct tm_data *fd = (struct tm_data *)hidpp_dev->driver_data;

	dbg_hid("%s\n", __func__);
	return ret;
}

static int tm_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct tm_data *fd;
	struct hidpp_device *hidpp_device;
	int ret;

	dbg_hid("%s START\n", __func__);

	if (!use_raw_mode) {
		dbg_hid("Using Standard mode for mouse\n");
		hdev->driver->input_mapping = NULL;
		hdev->driver->remove = NULL;
		hdev->driver->raw_event = NULL;
		ret = hid_parse(hdev);
		if (!ret)
			ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		return ret;
	}

	hidpp_device = kzalloc(sizeof(struct hidpp_device), GFP_KERNEL);
	if (!hidpp_device) {
		hid_err(hdev, "cannot allocate hidpp_device\n");
		return -ENOMEM;
	}

	fd = kzalloc(sizeof(struct tm_data), GFP_KERNEL);
	if (!fd) {
		hid_err(hdev, "cannot allocate tm Touch data\n");
		kfree(hidpp_device);
		return -ENOMEM;
	}
	INIT_WORK(&fd->work, delayedwork_callback);

	fd->hidpp_dev = hidpp_device;
	fd->prev_slots_used = 0;
	fd->next_tracking_id = 1;
	fd->ignore_mouse_report_buttons =
		hdev->product == UNIFYING_DEVICE_ID_TOUCH_MOUSE_T620;
	dbg_hid("Ignore mouse report buttons: %d\n", fd->ignore_mouse_report_buttons);
	spin_lock_init(&fd->lock);

	hidpp_device->driver_data = (void *)fd;
	hid_set_drvdata(hdev, hidpp_device);

	hidpp_device->device_init = tm_device_init;
	hidpp_device->connect_change = tm_connect_change;
	hidpp_device->raw_event = tm_raw_event;

	dbg_hid("%s calling hid_parse\n", __func__);
	ret = hid_parse(hdev);
	if (ret)
		goto parse_failed;

	/* Normally, a driver would call 
	 */

	dbg_hid("%s calling hidpp_init\n", __func__);
	ret = hidpp_init(hidpp_device, hdev);
	if (ret)
		goto failed;
	//hidpp_delayed_init(hidpp_device);

	dbg_hid("%s upping driver event lock\n", __func__);
	hid_device_io_start(hdev);

	dbg_hid("%s going to raw\n", __func__);
	ret = tm_set_raw_report_state(hidpp_device);
	if (ret) {
		dbg_hid("ERROR: tm_set_raw_report_state failed!!");
	}
	fd->in_raw_mode = 1;
	hid_device_io_stop(hdev);

	dbg_hid("%s calling hid_hw_start\n", __func__);
	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		goto failed;
	fd->hid_hw_started = 1;

	dbg_hid("%s END\n", __func__);
	return 0;

failed:
	//hid_hw_stop(hdev);
parse_failed:
	kfree(hidpp_device->driver_data);
	kfree(hidpp_device);
	hid_set_drvdata(hdev, NULL);
	return -ENODEV;
}

static void tm_remove(struct hid_device *hdev)
{
	struct hidpp_device *hidpp_dev = hid_get_drvdata(hdev);
	struct tm_data *fd = hidpp_dev->driver_data;
	dbg_hid("%s\n", __func__);
	cancel_work_sync(&fd->work);
	hid_hw_stop(hdev);
	hidpp_remove(hidpp_dev);
	kfree(fd);
	kfree(hidpp_dev);
	hid_set_drvdata(hdev, NULL);
}

static const struct hid_device_id tm_devices[] = {
	{HID_DEVICE(BUS_DJ, USB_VENDOR_ID_LOGITECH, UNIFYING_DEVICE_ID_ZONE_MOUSE_T400) },
	{HID_DEVICE(BUS_DJ, USB_VENDOR_ID_LOGITECH, UNIFYING_DEVICE_ID_TOUCH_MOUSE_T620) },
	{ }
};
MODULE_DEVICE_TABLE(hid, tm_devices);

static struct hid_driver tm_driver = {
	.name = "tm-touch",
	.id_table = tm_devices,
	.probe = tm_probe,
	.remove = tm_remove,
	.input_mapping = tm_input_mapping,
	.raw_event = hidpp_raw_event,
};

static int __init tm_init(void)
{
	return hid_register_driver(&tm_driver);
}

static void __exit tm_exit(void)
{
	hid_unregister_driver(&tm_driver);
}

module_init(tm_init);
module_exit(tm_exit);
