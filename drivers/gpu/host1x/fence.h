/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, NVIDIA Corporation.
 */

#ifndef HOST1X_FENCE_H
#define HOST1X_FENCE_H

struct host1x_syncpt_fence;

bool host1x_fence_signal(struct host1x_syncpt_fence *fence);

int host1x_fence_extract(struct dma_fence *fence, u32 *id, u32 *threshold);

#endif
