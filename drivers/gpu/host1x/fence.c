/*
 * Copyright (c) 2015, NVIDIA Corporation.
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

#include "fence.h"
#include "intr.h"
#include "syncpt.h"
#include "cdma.h"
#include "channel.h"
#include "dev.h"

#include "../../staging/android/sync.h"

struct host1x_sync_timeline {
	struct sync_timeline base;
	struct host1x *host;
	struct host1x_syncpt *syncpt;
};

struct host1x_sync_pt {
	struct sync_pt base;
	u32 threshold;
};

static inline struct host1x_sync_pt *to_host1x_pt(struct sync_pt *pt)
{
	return (struct host1x_sync_pt *)pt;
}

static inline struct host1x_sync_timeline *to_host1x_timeline(
		struct sync_pt *pt)
{
	return (struct host1x_sync_timeline *)sync_pt_parent(pt);
}

static struct sync_pt *host1x_sync_pt_dup(struct sync_pt *pt)
{
	struct sync_pt *new = sync_pt_create(sync_pt_parent(pt),
					     sizeof(struct host1x_sync_pt));
	memcpy(new, pt, sizeof(struct host1x_sync_pt));

	return new;
}

static int host1x_sync_pt_has_signaled(struct sync_pt *spt)
{
	struct host1x_sync_pt *pt = to_host1x_pt(spt);
	struct host1x_sync_timeline *tl = to_host1x_timeline(spt);

	return host1x_syncpt_is_expired(tl->syncpt, pt->threshold);
}

static int host1x_sync_pt_compare(struct sync_pt *a, struct sync_pt *b)
{
	struct host1x_sync_pt *pt_a = to_host1x_pt(a), *pt_b = to_host1x_pt(b);
	struct host1x_sync_timeline *tl = to_host1x_timeline(a);

	WARN_ON(tl != to_host1x_timeline(b));

	return host1x_syncpt_compare(tl->syncpt, pt_a->threshold,
				     pt_b->threshold);

}

static const struct sync_timeline_ops host1x_timeline_ops = {
	.driver_name = "host1x",
	.dup = host1x_sync_pt_dup, /* marked required but never used anywhere */
	.has_signaled = host1x_sync_pt_has_signaled,
	.compare = host1x_sync_pt_compare,
};

struct host1x_sync_pt *host1x_sync_pt_create(struct host1x *host,
				      struct host1x_syncpt *syncpt,
				      u32 threshold)
{
	struct host1x_sync_pt *pt;
	struct host1x_waitlist *waiter;
	int err;

	pt = (struct host1x_sync_pt *) sync_pt_create(
			(struct sync_timeline *)syncpt->timeline, sizeof(*pt));
	if (!pt)
		return NULL;

	pt->threshold = threshold;

	waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
	if (!waiter)
		goto free_sync_pt;

	err = host1x_intr_add_action(host, syncpt->id, threshold,
				     HOST1X_INTR_ACTION_SIGNAL_TIMELINE,
				     syncpt->timeline, waiter, NULL);
	if (err)
		goto free_sync_pt;

	return pt;

free_sync_pt:
	sync_pt_free((struct sync_pt *)pt);

	return NULL;
}

bool host1x_sync_pt_extract(struct sync_pt *pt, struct host1x_syncpt **syncpt,
			    u32 *threshold)
{
	struct sync_timeline *tl = sync_pt_parent(pt);

	if (tl->ops != &host1x_timeline_ops)
		return false;

	*syncpt = to_host1x_timeline(pt)->syncpt;
	*threshold = to_host1x_pt(pt)->threshold;

	return true;
}

bool host1x_sync_fence_wait(struct sync_fence *fence,
			    struct host1x *host,
			    struct host1x_channel *ch)
{
	struct host1x_syncpt *syncpt;
	bool non_host1x = false;
	u32 threshold;
	int i;

	for (i = 0; i < fence->num_fences; ++i) {
		struct sync_fence_cb *cb = &fence->cbs[i];
		struct sync_pt *spt = (struct sync_pt *)cb->sync_pt;

		if (!host1x_sync_pt_extract(spt, &syncpt, &threshold)) {
			non_host1x = true;
			continue;
		}

		if (host1x_syncpt_is_expired(syncpt, threshold))
			continue;

		host1x_hw_channel_push_wait(host, ch, syncpt->id, threshold);
	}

	return non_host1x;
}

struct host1x_sync_timeline *host1x_sync_timeline_create(
	struct host1x *host, struct host1x_syncpt* syncpt)
{
	struct host1x_sync_timeline *tl;

	tl = (struct host1x_sync_timeline *)sync_timeline_create(
			&host1x_timeline_ops, sizeof(*tl), "host1x");
	if (!tl)
		return NULL;

	tl->host = host;
	tl->syncpt = syncpt;

	return tl;
}

void host1x_sync_timeline_destroy(struct host1x_sync_timeline *timeline)
{
	sync_timeline_destroy((struct sync_timeline *)timeline);
}

void host1x_sync_timeline_signal(struct host1x_sync_timeline *timeline)
{
	sync_timeline_signal((struct sync_timeline *)timeline);
}
