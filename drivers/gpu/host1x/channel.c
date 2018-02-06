/*
 * Tegra host1x Channel
 *
 * Copyright (c) 2010-2013, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/slab.h>
#include <linux/module.h>

#include "channel.h"
#include "dev.h"
#include "job.h"

/* Constructor for the host1x device list */
int host1x_channel_list_init(struct host1x_channel_list *chlist,
			     unsigned int num_channels)
{
	chlist->channels = kcalloc(num_channels, sizeof(struct host1x_channel),
				   GFP_KERNEL);
	if (!chlist->channels)
		return -ENOMEM;

	chlist->num_channels = num_channels;

	chlist->allocated_channels =
		kcalloc(BITS_TO_LONGS(num_channels), sizeof(unsigned long),
			GFP_KERNEL);
	if (!chlist->allocated_channels) {
		kfree(chlist->channels);
		return -ENOMEM;
	}

	bitmap_zero(chlist->allocated_channels, num_channels);

	mutex_init(&chlist->lock);
	sema_init(&chlist->sema, num_channels);

	return 0;
}

void host1x_channel_list_free(struct host1x_channel_list *chlist)
{
	int i;

	for (i = 0; i < chlist->num_channels; i++) {
		struct host1x_channel *ch = &chlist->channels[i];

		if (ch->cdma.push_buffer.mapped)
			host1x_pushbuffer_destroy(&ch->cdma.push_buffer);
	}

	kfree(chlist->allocated_channels);
	kfree(chlist->channels);
}

int host1x_job_submit(struct host1x_job *job)
{
	struct host1x *host = dev_get_drvdata(job->channel->dev->parent);

	return host1x_hw_channel_submit(host, job);
}
EXPORT_SYMBOL(host1x_job_submit);

struct host1x_channel *host1x_channel_get(struct host1x_channel *channel)
{
	kref_get(&channel->refcount);

	return channel;
}
EXPORT_SYMBOL(host1x_channel_get);

/**
 * host1x_channel_get_index() - Attempt to get channel reference by index
 * @host: Host1x device object
 * @index: Index of channel
 *
 * If channel number @index is currently allocated, increase its refcount
 * and return a pointer to it. Otherwise, return NULL.
 */
struct host1x_channel *host1x_channel_get_index(struct host1x *host,
						unsigned int index)
{
	struct host1x_channel *ch = &host->channel_list.channels[index];

	if (!kref_get_unless_zero(&ch->refcount))
		return NULL;

	return ch;
}

static void release_channel(struct kref *kref)
{
	struct host1x_channel *channel =
		container_of(kref, struct host1x_channel, refcount);
	struct host1x *host = dev_get_drvdata(channel->dev->parent);
	struct host1x_channel_list *chlist = &host->channel_list;

	host1x_hw_cdma_stop(host, &channel->cdma);
	host1x_cdma_deinit(&channel->cdma);

	clear_bit(channel->id, chlist->allocated_channels);

	up(&chlist->sema);
}

void host1x_channel_put(struct host1x_channel *channel)
{
	kref_put(&channel->refcount, release_channel);
}
EXPORT_SYMBOL(host1x_channel_put);

static struct host1x_channel *acquire_unused_channel(struct host1x *host,
						     bool wait)
{
	struct host1x_channel_list *chlist = &host->channel_list;
	unsigned int max_channels = host->info->nb_channels;
	unsigned int index;
	int err;

	if (wait) {
		err = down_interruptible(&chlist->sema);
		if (err)
			return ERR_PTR(err);
	} else {
		if (down_trylock(&chlist->sema))
			return ERR_PTR(-EBUSY);
	}

	mutex_lock(&chlist->lock);

	index = find_first_zero_bit(chlist->allocated_channels, max_channels);
	if (WARN(index >= max_channels, "failed to find free channel")) {
		mutex_unlock(&chlist->lock);
		dev_err(host->dev, "failed to find free channel\n");
		return ERR_PTR(-EBUSY);
	}

	chlist->channels[index].id = index;

	set_bit(index, chlist->allocated_channels);

	mutex_unlock(&chlist->lock);

	return &chlist->channels[index];
}

/**
 * host1x_channel_request() - Allocate a channel
 * @device: Host1x unit this channel will be used to send commands to
 * @wait: Whether to wait for a free channels if all are reserved
 *
 * Allocates a new host1x channel for @device. If all channels are in use,
 * and @wait is true, does an interruptible wait until one is available.
 *
 * If a channel was acquired, returns a pointer to it. Otherwise returns
 * an error pointer with -EINTR if the wait was interrupted, -EBUSY
 * if a channel could not be acquired or another error code if channel
 * initialization failed.
 */
struct host1x_channel *host1x_channel_request(struct device *dev, bool wait)
{
	struct host1x *host = dev_get_drvdata(dev->parent);
	struct host1x_channel_list *chlist = &host->channel_list;
	struct host1x_channel *channel;
	int err;

	channel = acquire_unused_channel(host, wait);
	if (IS_ERR(channel))
		return channel;

	kref_init(&channel->refcount);
	mutex_init(&channel->submitlock);
	channel->dev = dev;

	err = host1x_hw_channel_init(host, channel, channel->id);
	if (err < 0)
		goto fail;

	err = host1x_cdma_init(&channel->cdma);
	if (err < 0)
		goto fail;

	/* pushbuffers are retained to decrease channel allocation latency */
	if (!channel->cdma.push_buffer.mapped) {
		err = host1x_pushbuffer_init(&channel->cdma.push_buffer);
		if (err < 0)
			goto fail;
	}

	return channel;

fail:
	clear_bit(channel->id, chlist->allocated_channels);

	dev_err(dev, "failed to initialize channel\n");

	return ERR_PTR(err);
}
EXPORT_SYMBOL(host1x_channel_request);
