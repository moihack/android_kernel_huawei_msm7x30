/*
 * Huawei Battery Driver
 * Copyright (C) 2014  Rudolf Tammekivi <rtammekivi@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see [http://www.gnu.org/licenses/].
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <mach/msm_rpcrouter.h>

#include <linux/huawei_battery.h>

#define PM_LIB_RPC_PROG			0x30000061
#define PM_LIB_RPC_VERS			0x00030005

#define PM_CURRENT_CONSUMER_NOTIFY_PROC	200

struct huawei_bat_data {
	struct device *dev;
	struct msm_rpc_endpoint *endpoint;
	struct workqueue_struct *workqueue;
};
static struct huawei_bat_data *bat_data = NULL;

struct work_entry {
	struct work_struct work;
	struct huawei_bat_data *data;
	enum huawei_bat_consumer consumer;
	union huawei_bat_state state;
};

/* Corresponding RPC entry for the consumer. */
static const uint32_t rpc_consumer_entry[HW_BAT_CONSUMER_MAX] = {
	[HW_BAT_CONSUMER_LCD]			= 0,
	[HW_BAT_CONSUMER_FRONT_CAMERA]		= 1,
	[HW_BAT_CONSUMER_BACK_CAMERA]		= 2,
	[HW_BAT_CONSUMER_CAMERA]		= 3,
	[HW_BAT_CONSUMER_WIFI]			= 4,
	[HW_BAT_CONSUMER_BT]			= 5,
	[HW_BAT_CONSUMER_FM]			= 6,
	[HW_BAT_CONSUMER_ADSP]			= 7,
	[HW_BAT_CONSUMER_CAMERA_FLASH]		= 8,
	[HW_BAT_CONSUMER_KEYPAD]		= 20,
	[HW_BAT_CONSUMER_VIBRATOR]		= 21,
	[HW_BAT_CONSUMER_GSM850_GSM900]		= 22,
	[HW_BAT_CONSUMER_GSM1800_GSM1900]	= 23,
	[HW_BAT_CONSUMER_WCDMA]			= 24,
	[HW_BAT_CONSUMER_CDMA1X]		= 25,
	[HW_BAT_CONSUMER_SPEAKER]		= 26,
	[HW_BAT_CONSUMER_CPU]			= 27,
	[HW_BAT_CONSUMER_GPS]			= 28,

	[HW_BAT_CONSUMER_NONE]			= 0xff,
};

static int huawei_bat_notify_rpc(uint32_t consumer,
	uint32_t state)
{
	int ret;
	struct huawei_bat_data *data = bat_data;
	struct device *dev = data->dev;

	struct {
		struct rpc_request_hdr hdr;
		uint32_t consumer;
		uint32_t state;
	} req;

	req.consumer = cpu_to_be32(consumer);
	req.state = cpu_to_be32(state);

	ret = msm_rpc_call(data->endpoint, PM_CURRENT_CONSUMER_NOTIFY_PROC,
		&req, sizeof(req), msecs_to_jiffies(5000));
	if (ret) {
		dev_err(dev, "failed to do rpc call for consumer %d ret=%d\n",
			consumer, ret);
		return ret;
	}

	return 0;
}

static void huawei_bat_work(struct work_struct *work)
{
	int ret = 0;
	struct work_entry *entry = (struct work_entry *)work;
	struct huawei_bat_data *data = entry->data;
	struct device *dev = data->dev;
	uint32_t rpc_consumer = rpc_consumer_entry[entry->consumer];
	union huawei_bat_state state = entry->state;

	switch(entry->consumer) {
	case HW_BAT_CONSUMER_LCD:
		dev_vdbg(dev, "Notifying LCD level %d\n",
			state.backlight_level);
		ret = huawei_bat_notify_rpc(rpc_consumer,
			state.backlight_level);
		break;
	case HW_BAT_CONSUMER_FRONT_CAMERA:
	case HW_BAT_CONSUMER_BACK_CAMERA:
	case HW_BAT_CONSUMER_CAMERA:
		dev_vdbg(dev, "Notifying camera on %d\n", state.on);
		ret = huawei_bat_notify_rpc(rpc_consumer, state.on);
		break;
	case HW_BAT_CONSUMER_WIFI:
	case HW_BAT_CONSUMER_BT:
	case HW_BAT_CONSUMER_FM:
	case HW_BAT_CONSUMER_ADSP:
		break;
	case HW_BAT_CONSUMER_CAMERA_FLASH:
		dev_vdbg(dev, "Notifying flash current %d\n",
			state.flash_current);
		ret = huawei_bat_notify_rpc(rpc_consumer, state.flash_current);
		break;
	case HW_BAT_CONSUMER_KEYPAD:
	case HW_BAT_CONSUMER_VIBRATOR:
	case HW_BAT_CONSUMER_GSM850_GSM900:
	case HW_BAT_CONSUMER_GSM1800_GSM1900:
	case HW_BAT_CONSUMER_WCDMA:
	case HW_BAT_CONSUMER_CDMA1X:
	case HW_BAT_CONSUMER_SPEAKER:
	case HW_BAT_CONSUMER_CPU:
	case HW_BAT_CONSUMER_GPS:
	default:
		break;
	}

	if (ret) {
		dev_err(dev, "failed to notify rpc ret=%d\n", ret);
	}

	kfree(work);
}

int huawei_bat_notify(enum huawei_bat_consumer consumer,
	union huawei_bat_state state)
{
	int ret;
	struct huawei_bat_data *data = bat_data;
	struct device *dev;
	struct work_entry *work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;

	INIT_WORK((struct work_struct *)work, huawei_bat_work);

	if (!data || IS_ERR_OR_NULL(data->endpoint) || !data->workqueue)
		return -EPERM;
	work->data = data;

	dev = data->dev;

	if (consumer < 0 || consumer > HW_BAT_CONSUMER_MAX) {
		dev_err(dev, "invalid consumer %d\n", consumer);
		return -EINVAL;
	}
	work->consumer = consumer;
	work->state = state;

	ret = queue_work(data->workqueue, (struct work_struct *)work);
	if (ret < 0) {
		dev_err(dev, "failed to queue work ret=%d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(huawei_bat_notify);

static int huawei_bat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct huawei_bat_data *data;
	int ret = 0;

	if (pdev->id != -1) {
		dev_err(dev, "multiple devices not supported\n");
		goto err_exit;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		dev_err(dev, "failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_exit;
	}

	platform_set_drvdata(pdev, data);

	data->dev = dev;

	/* Connect to RPC service. */
	data->endpoint = msm_rpc_connect(PM_LIB_RPC_PROG, PM_LIB_RPC_VERS, 0);
	if (IS_ERR_OR_NULL(data->endpoint)) {
		ret = PTR_ERR(data->endpoint);
		dev_err(dev, "failed to connect to rpc service ret=%d\n", ret);
		goto err_free;
	}

	data->workqueue = create_workqueue("huawei_bat_queue");
	if (!data->workqueue) {
		dev_err(dev, "failed to create workqueue\n");
		ret = -EPERM;
		goto err_close_rpc;
	}

	bat_data = data;

	return 0;
err_close_rpc:
	msm_rpc_close(data->endpoint);
err_free:
	bat_data = NULL;
	kfree(data);
err_exit:
	return ret;
}

static int huawei_bat_remove(struct platform_device *pdev)
{
	struct huawei_bat_data *data = platform_get_drvdata(pdev);

	flush_workqueue(data->workqueue);
	destroy_workqueue(data->workqueue);

	msm_rpc_close(data->endpoint);

	bat_data = NULL;
	kfree(data);

	return 0;
}

static int huawei_bat_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int huawei_bat_resume(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver huawei_bat_driver = {
	.probe		= huawei_bat_probe,
	.remove		= huawei_bat_remove,
	.suspend	= huawei_bat_suspend,
	.resume		= huawei_bat_resume,
	.driver		= {
		.name	= "huawei_battery",
		.owner	= THIS_MODULE,
	},
};

static int __init huawei_bat_init(void)
{
	return platform_driver_register(&huawei_bat_driver);
}

static void __exit huawei_bat_exit(void)
{
	platform_driver_unregister(&huawei_bat_driver);
}

module_init(huawei_bat_init);
module_exit(huawei_bat_exit);

MODULE_DESCRIPTION("Huawei Battery Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_AUTHOR("Rudolf Tammekivi <rtammekivi@gmail.com>");
