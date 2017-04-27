/*
 * Copyright (c) 2015-2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author:
 *	Mikko Perttunen <mperttunen@nvidia.com>
 *	Aapo Vienamo	<avienamo@nvidia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/* TODO prune includes */
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/thermal.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include <soc/tegra/bpmp.h>
#include <soc/tegra/bpmp-abi.h>

struct tegra_bpmp_thermal_zone {
	struct tegra_bpmp_thermal *tegra;
	struct thermal_zone_device *tzd;
	struct work_struct tz_device_update_work;
	unsigned int idx;
};

struct tegra_bpmp_thermal {
	struct tegra_bpmp *bpmp;
	unsigned int num_zones;
	struct tegra_bpmp_thermal_zone *zones;
};

static int tegra_bpmp_thermal_get_temp(void *data, int *out_temp)
{
	struct tegra_bpmp_thermal_zone *zone = data;
	struct mrq_thermal_host_to_bpmp_request req;
	union mrq_thermal_bpmp_to_host_response reply;
	struct tegra_bpmp_message msg;
	int err;

	memset(&req, 0, sizeof(req));
	req.type = CMD_THERMAL_GET_TEMP;
	req.get_temp.zone = zone->idx;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_THERMAL;
	msg.tx.data = &req;
	msg.tx.size = sizeof(req);
	msg.rx.data = &reply;
	msg.rx.size = sizeof(reply);

	err = tegra_bpmp_transfer(zone->tegra->bpmp, &msg);
	if (err)
		return err;

	*out_temp = reply.get_temp.temp;

	return 0;
}

static int tegra_bpmp_thermal_set_trips(void *data, int low, int high)
{
	struct tegra_bpmp_thermal_zone *zone = data;
	struct mrq_thermal_host_to_bpmp_request req;
	struct tegra_bpmp_message msg;

	memset(&req, 0, sizeof(req));
	req.type = CMD_THERMAL_SET_TRIP;
	req.set_trip.zone = zone->idx;
	req.set_trip.enabled = true;
	req.set_trip.low = low;
	req.set_trip.high = high;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_THERMAL;
	msg.tx.data = &req;
	msg.tx.size = sizeof(req);

	return tegra_bpmp_transfer(zone->tegra->bpmp, &msg);
}

static void tz_device_update_work_fn(struct work_struct *work)
{
	struct tegra_bpmp_thermal_zone *zone;

	zone = container_of(work, struct tegra_bpmp_thermal_zone,
			    tz_device_update_work);

	thermal_zone_device_update(zone->tzd, THERMAL_TRIP_VIOLATED);
}

static void bpmp_mrq_thermal(unsigned int mrq, struct tegra_bpmp_channel *ch,
			     void *data)
{
	struct mrq_thermal_bpmp_to_host_request *req;
	struct tegra_bpmp_thermal *tegra = data;
	int zone_idx;

	req = (struct mrq_thermal_bpmp_to_host_request *)ch->ib->data;

	if (req->type != CMD_THERMAL_HOST_TRIP_REACHED) {
		dev_err(tegra->bpmp->dev, "%s: invalid request type: %d\n",
			__func__, req->type);
		tegra_bpmp_mrq_return(ch, -EINVAL, NULL, 0);
		return;
	}

	zone_idx = req->host_trip_reached.zone;
	if (zone_idx >= tegra->num_zones) {
		dev_err(tegra->bpmp->dev, "%s: invalid thermal zone: %d\n",
			__func__, zone_idx);
		tegra_bpmp_mrq_return(ch, -EINVAL, NULL, 0);
		return;
	}

	tegra_bpmp_mrq_return(ch, 0, NULL, 0);

	schedule_work(&tegra->zones[zone_idx].tz_device_update_work);
}

static int tegra_bpmp_thermal_get_num_zones(struct tegra_bpmp *bpmp,
					    int *num_zones)
{
	struct mrq_thermal_host_to_bpmp_request req;
	union mrq_thermal_bpmp_to_host_response reply;
	struct tegra_bpmp_message msg;
	int err;

	memset(&req, 0, sizeof(req));
	req.type = CMD_THERMAL_GET_NUM_ZONES;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_THERMAL;
	msg.tx.data = &req;
	msg.tx.size = sizeof(req);
	msg.rx.data = &reply;
	msg.rx.size = sizeof(reply);

	err = tegra_bpmp_transfer(bpmp, &msg);
	if (err)
		return err;

	*num_zones = reply.get_num_zones.num;

	return 0;
}

static struct thermal_zone_of_device_ops tegra_bpmp_of_thermal_ops = {
	.get_temp = tegra_bpmp_thermal_get_temp,
	.set_trips = tegra_bpmp_thermal_set_trips,
};

int tegra_bpmp_init_thermal(struct tegra_bpmp *bpmp)
{
	struct tegra_bpmp_thermal *tegra;
	struct thermal_zone_device *tzd;
	unsigned int i;
	int err;

	tegra = devm_kzalloc(bpmp->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

// 	err = tegra_bpmp_thermal_abi_probe();
// 	if (err) {
// 		dev_err(tegra->bpmp->dev,
// 			"%s: BPMP ABI probe failed\n", __func__);
// 		goto free_tegra;
// 	}

	err = tegra_bpmp_thermal_get_num_zones(bpmp, &tegra->num_zones);
	if (err) {
		dev_err(tegra->bpmp->dev,
			"%s: failed to get the number of zones: %d\n",
			__func__, err);
		goto free_tegra;
	}

	tegra->zones = devm_kcalloc(bpmp->dev, tegra->num_zones,
				    sizeof(*tegra->zones), GFP_KERNEL);
	if (!tegra->zones) {
		err = -ENOMEM;
		goto free_tegra;
	}

	for (i = 0; i < tegra->num_zones; ++i) {
		int temp;
		tegra->zones[i].idx = i;
		tegra->zones[i].tegra = tegra;

		err = tegra_bpmp_thermal_get_temp(&tegra->zones[i], &temp);
		if (err < 0)
			continue;

		tzd = thermal_zone_of_sensor_register(
			bpmp->dev, i, &tegra->zones[i],
			&tegra_bpmp_of_thermal_ops);
		if (IS_ERR(tzd)) {
			err = PTR_ERR(tzd);
			dev_notice(bpmp->dev, "Thermal zone %d not supported\n",
				   i);
			tzd = NULL;
		}

		tegra->zones[i].tzd = tzd;
		INIT_WORK(&tegra->zones[i].tz_device_update_work,
			  tz_device_update_work_fn);

		/* Ensure that HW trip points are set */
		thermal_zone_device_update(tzd, THERMAL_EVENT_UNSPECIFIED);
	}

	err = tegra_bpmp_request_mrq(bpmp, MRQ_THERMAL, bpmp_mrq_thermal,
				     tegra);
	if (err) {
		dev_err(bpmp->dev, "%s: failed to register mrq handler: %d\n",
			__func__, err);
		goto free_zones;
	}

	return 0;

free_zones:
	devm_kfree(bpmp->dev, tegra->zones);
free_tegra:
	devm_kfree(bpmp->dev, tegra);

	return err;
}
