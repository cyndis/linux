/*
 * Copyright (C) 2010 Google, Inc.
 * Author: Erik Gilling <konkers@android.com>
 *
 * Copyright (C) 2011-2017 NVIDIA Corporation
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

#include "../dev.h"
#include "../debug.h"
#include "../cdma.h"
#include "../channel.h"

static void host1x_debug_show_channel_cdma(struct host1x *host,
					   struct host1x_channel *ch,
					   struct output *o)
{
	struct host1x_cdma *cdma = &ch->cdma;
	u32 dmaput, dmaget, dmactrl;
	u32 offset, class;
	u32 cf_read, cf_stat;
	u32 ch_stat;

	dmaput = host1x_ch_readl(ch, HOST1X_CHANNEL_DMAPUT);
	dmaget = host1x_ch_readl(ch, HOST1X_CHANNEL_DMAGET);
	dmactrl = host1x_ch_readl(ch, HOST1X_CHANNEL_DMACTRL);
	cf_read = host1x_ch_readl(ch, HOST1X_CHANNEL_CMDFIFO_RDATA);
	cf_stat = host1x_ch_readl(ch, HOST1X_CHANNEL_CMDFIFO_STAT);
	offset = host1x_ch_readl(ch, HOST1X_CHANNEL_CMDP_OFFSET);
	class = host1x_ch_readl(ch, HOST1X_CHANNEL_CMDP_CLASS);
	ch_stat = host1x_ch_readl(ch, HOST1X_CHANNEL_CHANNELSTAT);

	host1x_debug_output(o, "%u-%s: ", ch->id, dev_name(ch->dev));

	if (dmactrl & HOST1X_CHANNEL_DMACTRL_DMASTOP ||
	    !ch->cdma.push_buffer.mapped) {
		host1x_debug_output(o, "inactive\n\n");
		return;
	}

	if (class == HOST1X_CLASS_HOST1X && offset == HOST1X_UCLASS_WAIT_SYNCPT)
		host1x_debug_output(o, "waiting on syncpt\n");
	else
		host1x_debug_output(o, "active class %02x, offset %04x\n",
				    class, offset);

	host1x_debug_output(o, "DMAPUT %08x, DMAGET %08x, DMACTL %08x\n",
			    dmaput, dmaget, dmactrl);
	host1x_debug_output(o, "CMDFIFO_READ %08x, CMDFIFO_STAT %08x\n",
			    cf_read, cf_stat);
	host1x_debug_output(o, "CHANNELSTAT %02x\n", ch_stat);

	show_channel_gathers(o, cdma);
	host1x_debug_output(o, "\n");
}

static void host1x_debug_show_channel_fifo(struct host1x *host,
					   struct host1x_channel *ch,
					   struct output *o)
{
	/* TODO */
}

static void host1x_debug_show_mlocks(struct host1x *host, struct output *o)
{
	/* TODO */
}
