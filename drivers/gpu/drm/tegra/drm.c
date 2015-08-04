/*
 * Copyright (C) 2012 Avionic Design GmbH
 * Copyright (C) 2012-2015 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/host1x.h>
#include <linux/iommu.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>

#include "drm.h"
#include "gem.h"

#ifdef CONFIG_SYNC
#include "../../../staging/android/sync.h"
#endif

#define DRIVER_NAME "tegra"
#define DRIVER_DESC "NVIDIA Tegra graphics"
#define DRIVER_DATE "20120330"
#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 0

#define IOVA_AREA_SZ (1024 * 1024 * 64) /* 64 MiB */

struct tegra_drm_file {
	struct list_head contexts;
};

static void tegra_atomic_schedule(struct tegra_drm *tegra,
				  struct drm_atomic_state *state)
{
	tegra->commit.state = state;
	schedule_work(&tegra->commit.work);
}

static void tegra_atomic_complete(struct tegra_drm *tegra,
				  struct drm_atomic_state *state)
{
	struct drm_device *drm = tegra->drm;

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one condition: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout.
	 */

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_planes(drm, state);
	drm_atomic_helper_commit_modeset_enables(drm, state);

	drm_atomic_helper_wait_for_vblanks(drm, state);

	drm_atomic_helper_cleanup_planes(drm, state);
	drm_atomic_state_free(state);
}

static void tegra_atomic_work(struct work_struct *work)
{
	struct tegra_drm *tegra = container_of(work, struct tegra_drm,
					       commit.work);

	tegra_atomic_complete(tegra, tegra->commit.state);
}

static int tegra_atomic_commit(struct drm_device *drm,
			       struct drm_atomic_state *state, bool async)
{
	struct tegra_drm *tegra = drm->dev_private;
	int err;

	err = drm_atomic_helper_prepare_planes(drm, state);
	if (err)
		return err;

	/* serialize outstanding asynchronous commits */
	mutex_lock(&tegra->commit.lock);
	flush_work(&tegra->commit.work);

	/*
	 * This is the point of no return - everything below never fails except
	 * when the hw goes bonghits. Which means we can commit the new state on
	 * the software side now.
	 */

	drm_atomic_helper_swap_state(drm, state);

	if (async)
		tegra_atomic_schedule(tegra, state);
	else
		tegra_atomic_complete(tegra, state);

	mutex_unlock(&tegra->commit.lock);
	return 0;
}

static const struct drm_mode_config_funcs tegra_drm_mode_funcs = {
	.fb_create = tegra_fb_create,
#ifdef CONFIG_DRM_TEGRA_FBDEV
	.output_poll_changed = tegra_fb_output_poll_changed,
#endif
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = tegra_atomic_commit,
};

static int tegra_drm_load(struct drm_device *drm, unsigned long flags)
{
	struct host1x_device *device = to_host1x_device(drm->dev);
	struct tegra_drm *tegra;
	int err;

	tegra = kzalloc(sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	if (iommu_present(&platform_bus_type)) {
		struct iommu_domain_geometry *geometry;
		u64 start, end, iova_start;
		size_t bitmap_size;

		tegra->domain = iommu_domain_alloc(&platform_bus_type);
		if (!tegra->domain) {
			err = -ENOMEM;
			goto free;
		}

		geometry = &tegra->domain->geometry;
		start = geometry->aperture_start;
		end = geometry->aperture_end;
		iova_start = end - IOVA_AREA_SZ + 1;

		DRM_DEBUG("IOMMU context initialized (GEM aperture: %#llx-%#llx, IOVA aperture: %#llx-%#llx)\n",
			  start, iova_start-1, iova_start, end);
		bitmap_size = BITS_TO_LONGS(IOVA_AREA_SZ >> PAGE_SHIFT) *
			sizeof(long);
		tegra->iova_bitmap = devm_kzalloc(drm->dev, bitmap_size,
						  GFP_KERNEL);
		if (!tegra->iova_bitmap) {
			err = -ENOMEM;
			goto free;
		}
		tegra->iova_bitmap_bits = BITS_PER_BYTE * bitmap_size;
		tegra->iova_start = iova_start;
		mutex_init(&tegra->iova_lock);

		drm_mm_init(&tegra->mm, start, iova_start - start);
	}

	mutex_init(&tegra->clients_lock);
	INIT_LIST_HEAD(&tegra->clients);

	mutex_init(&tegra->commit.lock);
	INIT_WORK(&tegra->commit.work, tegra_atomic_work);

	drm->dev_private = tegra;
	tegra->drm = drm;

	drm_mode_config_init(drm);

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;

	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;

	drm->mode_config.funcs = &tegra_drm_mode_funcs;

	err = tegra_drm_fb_prepare(drm);
	if (err < 0)
		goto config;

	drm_kms_helper_poll_init(drm);

	err = host1x_device_init(device);
	if (err < 0)
		goto fbdev;

	drm_mode_config_reset(drm);

	/*
	 * We don't use the drm_irq_install() helpers provided by the DRM
	 * core, so we need to set this manually in order to allow the
	 * DRM_IOCTL_WAIT_VBLANK to operate correctly.
	 */
	drm->irq_enabled = true;

	/* syncpoints are used for full 32-bit hardware VBLANK counters */
	drm->max_vblank_count = 0xffffffff;

	err = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (err < 0)
		goto device;

	err = tegra_drm_fb_init(drm);
	if (err < 0)
		goto vblank;

	return 0;

vblank:
	drm_vblank_cleanup(drm);
device:
	host1x_device_exit(device);
fbdev:
	drm_kms_helper_poll_fini(drm);
	tegra_drm_fb_free(drm);
config:
	drm_mode_config_cleanup(drm);

	if (tegra->domain) {
		iommu_domain_free(tegra->domain);
		drm_mm_takedown(&tegra->mm);
	}
free:
	kfree(tegra);
	return err;
}

static int tegra_drm_unload(struct drm_device *drm)
{
	struct host1x_device *device = to_host1x_device(drm->dev);
	struct tegra_drm *tegra = drm->dev_private;
	int err;

	drm_kms_helper_poll_fini(drm);
	tegra_drm_fb_exit(drm);
	drm_mode_config_cleanup(drm);
	drm_vblank_cleanup(drm);

	err = host1x_device_exit(device);
	if (err < 0)
		return err;

	if (tegra->domain) {
		iommu_domain_free(tegra->domain);
		drm_mm_takedown(&tegra->mm);
	}

	kfree(tegra);

	return 0;
}

static int tegra_drm_open(struct drm_device *drm, struct drm_file *filp)
{
	struct tegra_drm_file *fpriv;

	fpriv = kzalloc(sizeof(*fpriv), GFP_KERNEL);
	if (!fpriv)
		return -ENOMEM;

	INIT_LIST_HEAD(&fpriv->contexts);
	filp->driver_priv = fpriv;

	return 0;
}

static void tegra_drm_context_free(struct tegra_drm_context *context)
{
	context->client->ops->close_channel(context);
	kfree(context);
}

static void tegra_drm_lastclose(struct drm_device *drm)
{
#ifdef CONFIG_DRM_TEGRA_FBDEV
	struct tegra_drm *tegra = drm->dev_private;

	tegra_fbdev_restore_mode(tegra->fbdev);
#endif
}

static struct host1x_bo *
host1x_bo_lookup(struct drm_device *drm, struct drm_file *file, u32 handle)
{
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	gem = drm_gem_object_lookup(drm, file, handle);
	if (!gem)
		return NULL;

	mutex_lock(&drm->struct_mutex);
	drm_gem_object_unreference(gem);
	mutex_unlock(&drm->struct_mutex);

	bo = to_tegra_bo(gem);
	return &bo->base;
}

static int host1x_reloc_copy_from_user(struct host1x_reloc *dest,
				       struct drm_tegra_reloc __user *src,
				       struct drm_device *drm,
				       struct drm_file *file)
{
	u32 cmdbuf, target;
	int err;

	err = get_user(cmdbuf, &src->cmdbuf.handle);
	if (err < 0)
		return err;

	err = get_user(dest->cmdbuf.offset, &src->cmdbuf.offset);
	if (err < 0)
		return err;

	err = get_user(target, &src->target.handle);
	if (err < 0)
		return err;

	err = get_user(dest->target.offset, &src->target.offset);
	if (err < 0)
		return err;

	err = get_user(dest->shift, &src->shift);
	if (err < 0)
		return err;

	dest->cmdbuf.bo = host1x_bo_lookup(drm, file, cmdbuf);
	if (!dest->cmdbuf.bo)
		return -ENOENT;

	dest->target.bo = host1x_bo_lookup(drm, file, target);
	if (!dest->target.bo)
		return -ENOENT;

	return 0;
}

#ifdef CONFIG_SYNC
struct syncpt_sync_fence_waiter {
	struct sync_fence_waiter base;
	struct host1x_syncpt *syncpt;
};

void syncpt_sync_fence_waiter_cb(struct sync_fence *fence,
				 struct sync_fence_waiter *waiter)
{
	struct syncpt_sync_fence_waiter *syncpt_waiter =
		container_of(waiter, struct syncpt_sync_fence_waiter, base);

	host1x_syncpt_incr(syncpt_waiter->syncpt);

	kfree(syncpt_waiter);
	sync_fence_put(fence);
}
#endif

int tegra_drm_submit(struct tegra_drm_context *context,
		     struct drm_tegra_submit *args, struct drm_device *drm,
		     struct drm_file *file)
{
#ifdef CONFIG_SYNC
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct sync_fence *fence = NULL;
#endif
	unsigned int num_cmdbufs = args->num_cmdbufs;
	unsigned int num_relocs = args->num_relocs;
	unsigned int num_syncpt_waits = 0;
	unsigned int num_inserted_waits = 0;
	unsigned int num_syncpt_incrs = 0;
	struct drm_tegra_cmdbuf __user *cmdbufs =
		(void __user *)(uintptr_t)args->cmdbufs;
	struct drm_tegra_reloc __user *relocs =
		(void __user *)(uintptr_t)args->relocs;
	struct drm_tegra_submit_syncpt_incr __user *syncpt_incrs =
		(void __user *)(uintptr_t)args->syncpt_incrs;
	struct drm_tegra_submit_syncpt_wait __user *syncpt_waits =
		(void __user *)(uintptr_t)args->syncpt_waits;
	u32 __user *syncpt_incr_ends =
		(void __user *)(uintptr_t)args->syncpt_incr_ends;
	struct host1x_job *job;
	int err, i;

	/* Calculate number of needed waitchks and syncpts */

	num_syncpt_waits = args->num_syncpt_waits;

	if (args->pre_fence) {
		fence = sync_fence_fdget(args->pre_fence);
		if (!fence)
			return -EINVAL;

		num_inserted_waits = host1x_sync_fence_count_waits(fence);
	}

	num_syncpt_incrs = args->num_syncpt_incrs;

	/* Create host1x job object */

	job = host1x_job_alloc(context->channel, args->num_cmdbufs,
			       args->num_relocs,
			       num_syncpt_waits + num_inserted_waits,
			       num_syncpt_incrs);
	if (!job) {
		err = -ENOMEM;
		goto put_fence;
	}

	job->num_relocs = args->num_relocs;
	job->num_waitchk = num_syncpt_waits + num_inserted_waits;
	job->num_syncpts = num_syncpt_incrs;
	job->client = (u32)args->context;
	job->class = context->client->base.class;
	job->serialize = true;
	job->is_addr_reg = context->client->ops->is_addr_reg;
	job->timeout = 10000;

	if (args->timeout && args->timeout < 10000)
		job->timeout = args->timeout;

	/* Setup postfences */

	if (copy_from_user(job->syncpts, syncpt_incrs,
			   num_syncpt_incrs * sizeof(*(job->syncpts)))) {
		err = -EFAULT;
		goto free_job;
	}

	/* Setup prefences */

	for (i = 0; i < num_syncpt_waits; ++i) {
		struct drm_tegra_submit_syncpt_wait wait;
		struct host1x_bo *bo;

		if (copy_from_user(&wait, syncpt_waits, sizeof(wait))) {
			err = -EFAULT;
			goto free_job;
		}

		bo = host1x_bo_lookup(drm, file, wait.handle);
		if (!bo) {
			err = -ENOENT;
			goto free_job;
		}

		job->waitchk[i].bo = bo;
		job->waitchk[i].offset = wait.offset;
		job->waitchk[i].syncpt_id = wait.syncpt;
		job->waitchk[i].thresh = wait.thresh;
	}

	if (fence) {
		host1x_sync_fence_unpack_waits(
			fence, job->waitchk + num_syncpt_waits);

		if (fence->num_fences > num_inserted_waits) {
			/*
			* Fence contains non-syncpoint-backed subfences.
			* Do these things:
			* 1) Increase syncpt max value on this channel. Due to
			*    synchronization this ensures that the submit will
			*    wait until the fence wait has completed.
			* 2) Create a fence that waits for the given prefence to
			*    complete and for the channel to finish its previous
			*    work. (REVIEW!)
			* 3) Create an async waiter for that fence that will,
			*    when signaled, increase the channel syncpoint, thus
			*    launching the waiting submission.
			*/
			struct sync_pt *prev_job_pt;
			struct sync_fence *prev_job_f, *merged_f;
			struct syncpt_sync_fence_waiter *waiter;
			struct host1x_syncpt *spt =
				context->client->base.syncpts[0];
			u32 thresh = host1x_syncpt_incr_max(spt, 1) - 1;

			prev_job_pt = (struct sync_pt *)host1x_sync_pt_create(
				host, spt, thresh);
			if (!prev_job_pt) {
				err = -ENOMEM;
				goto free_job;
			}

			prev_job_f = sync_fence_create("host1x_prev_job",
						       prev_job_pt);
			if (!prev_job_f) {
				sync_pt_free(prev_job_pt);
				err = -ENOMEM;
				goto free_job;
			}

			merged_f = sync_fence_merge("host1x_async_submit",
						    fence, prev_job_f);
			sync_fence_put(prev_job_f);
			if (!merged_f) {
				err = -ENOMEM;
				goto free_job;
			}

			waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
			if (!waiter) {
				sync_fence_put(merged_f);
				err = -ENOMEM;
				goto free_job;
			}

			sync_fence_waiter_init(&waiter->base,
					syncpt_sync_fence_waiter_cb);
			waiter->syncpt = spt;
			err = sync_fence_wait_async(merged_f, &waiter->base);
			if (err < 0) {
				kfree(waiter);
				goto free_job;
			}
			if (err == 1) {
				/* Fence was already signaled */
				syncpt_sync_fence_waiter_cb(merged_f,
							    &waiter->base);
			}
		} else {
			/*
			 * Fence completely unpacked as hw waits, no longer
			 * needed.
			 */
			sync_fence_put(fence);
		}

		/* Fence is being handled, must not be 'put' anymore */
		fence = NULL;
	}

	/* Setup command buffers and buffer relocations */

	while (num_cmdbufs) {
		struct drm_tegra_cmdbuf cmdbuf;
		struct host1x_bo *bo;

		if (copy_from_user(&cmdbuf, cmdbufs, sizeof(cmdbuf))) {
			err = -EFAULT;
			goto free_job;
		}

		bo = host1x_bo_lookup(drm, file, cmdbuf.handle);
		if (!bo) {
			err = -ENOENT;
			goto free_job;
		}

		host1x_job_add_gather(job, bo, cmdbuf.words, cmdbuf.offset);
		num_cmdbufs--;
		cmdbufs++;
	}

	while (num_relocs--) {
		err = host1x_reloc_copy_from_user(&job->relocarray[num_relocs],
						  &relocs[num_relocs], drm,
						  file);
		if (err < 0)
			goto free_job;
	}

	/* Submit job */

	err = host1x_job_pin(job, context->client->base.dev);
	if (err)
		goto free_job;

	err = host1x_job_submit(job);
	if (err)
		goto fail_submit;

	/* Return postfences to userspace */

	for (i = 0; i < num_syncpt_incrs; ++i) {
		u32 *end = syncpt_incr_ends + i;
		put_user(job->syncpts[i].end, end);
	}

	if (args->flags & DRM_TEGRA_SUBMIT_CREATE_POST_FENCE) {
		struct host1x_syncpt *syncpt;
		struct host1x_sync_pt *pt;
		struct sync_fence *a_fence, *b_fence, *merged_fence = NULL;
		int fd;

		for (i = 0; i < num_syncpt_incrs; ++i) {
			syncpt = host1x_syncpt_get(host, job->syncpts[0].id);
			pt = host1x_sync_pt_create(host, syncpt,
						   job->syncpts[0].end);
			if (!pt) {
				if (merged_fence)
					sync_fence_put(merged_fence);
				err = -ENOMEM;
				goto free_job;
			}

			a_fence = sync_fence_create("tegradrm",
						    (struct sync_pt *)pt);
			if (!a_fence) {
				if (merged_fence)
					sync_fence_put(merged_fence);
				sync_pt_free((struct sync_pt *)pt);
				err = -ENOMEM;
				goto free_job;
			}

			if (merged_fence) {
				b_fence = sync_fence_merge("tegradrm",
							   a_fence,
							   merged_fence);
				sync_fence_put(merged_fence);
				merged_fence = b_fence;
				if (!merged_fence) {
					err = -ENOMEM;
					goto free_job;
				}
			} else {
				merged_fence = a_fence;
			}
		}

		if (merged_fence) {
			fd = get_unused_fd_flags(O_CLOEXEC);
			sync_fence_install(merged_fence, fd);
			args->post_fence = fd;
		}
	}

	host1x_job_put(job);
	return 0;

fail_submit:
	host1x_job_unpin(job);
free_job:
	host1x_job_put(job);
put_fence:
	if (fence)
		sync_fence_put(fence);
	return err;
}


#ifdef CONFIG_DRM_TEGRA_STAGING
static struct tegra_drm_context *tegra_drm_get_context(__u64 context)
{
	return (struct tegra_drm_context *)(uintptr_t)context;
}

static bool tegra_drm_file_owns_context(struct tegra_drm_file *file,
					struct tegra_drm_context *context)
{
	struct tegra_drm_context *ctx;

	list_for_each_entry(ctx, &file->contexts, list)
		if (ctx == context)
			return true;

	return false;
}

static int tegra_gem_create(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct drm_tegra_gem_create *args = data;
	struct tegra_bo *bo;

	bo = tegra_bo_create_with_handle(file, drm, args->size, args->flags,
					 &args->handle);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	return 0;
}

static int tegra_gem_mmap(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_tegra_gem_mmap *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	gem = drm_gem_object_lookup(drm, file, args->handle);
	if (!gem)
		return -EINVAL;

	bo = to_tegra_bo(gem);

	args->offset = drm_vma_node_offset_addr(&bo->gem.vma_node);

	drm_gem_object_unreference(gem);

	return 0;
}

static int tegra_syncpt_read(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct drm_tegra_syncpt_read *args = data;
	struct host1x_syncpt *sp;

	sp = host1x_syncpt_get(host, args->id);
	if (!sp)
		return -EINVAL;

	args->value = host1x_syncpt_read_min(sp);
	return 0;
}

static int tegra_syncpt_incr(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct host1x *host1x = dev_get_drvdata(drm->dev->parent);
	struct drm_tegra_syncpt_incr *args = data;
	struct host1x_syncpt *sp;

	sp = host1x_syncpt_get(host1x, args->id);
	if (!sp)
		return -EINVAL;

	return host1x_syncpt_incr(sp);
}

static int tegra_syncpt_wait(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct host1x *host1x = dev_get_drvdata(drm->dev->parent);
	struct drm_tegra_syncpt_wait *args = data;
	struct host1x_syncpt *sp;

	sp = host1x_syncpt_get(host1x, args->id);
	if (!sp)
		return -EINVAL;

	return host1x_syncpt_wait(sp, args->thresh, args->timeout,
				  &args->value);
}

static int tegra_open_channel(struct drm_device *drm, void *data,
			      struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;
	struct drm_tegra_open_channel *args = data;
	struct tegra_drm_context *context;
	struct tegra_drm_client *client;
	int err = -ENODEV;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	list_for_each_entry(client, &tegra->clients, list)
		if (client->base.class == args->client) {
			err = client->ops->open_channel(client, context);
			if (err)
				break;

			list_add(&context->list, &fpriv->contexts);
			args->context = (uintptr_t)context;
			context->client = client;
			return 0;
		}

	kfree(context);
	return err;
}

static int tegra_close_channel(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_close_channel *args = data;
	struct tegra_drm_context *context;

	context = tegra_drm_get_context(args->context);

	if (!tegra_drm_file_owns_context(fpriv, context))
		return -EINVAL;

	list_del(&context->list);
	tegra_drm_context_free(context);

	return 0;
}

static int tegra_get_syncpt(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_get_syncpt *args = data;
	struct tegra_drm_context *context;
	struct host1x_syncpt *syncpt;

	context = tegra_drm_get_context(args->context);

	if (!tegra_drm_file_owns_context(fpriv, context))
		return -ENODEV;

	if (args->index >= context->client->base.num_syncpts)
		return -EINVAL;

	syncpt = context->client->base.syncpts[args->index];
	args->id = host1x_syncpt_id(syncpt);

	return 0;
}

static int tegra_submit(struct drm_device *drm, void *data,
			struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_submit *args = data;
	struct tegra_drm_context *context;

	context = tegra_drm_get_context(args->context);

	if (!tegra_drm_file_owns_context(fpriv, context))
		return -ENODEV;

	return context->client->ops->submit(context, args, drm, file);
}

static int tegra_get_syncpt_base(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_get_syncpt_base *args = data;
	struct tegra_drm_context *context;
	struct host1x_syncpt_base *base;
	struct host1x_syncpt *syncpt;

	context = tegra_drm_get_context(args->context);

	if (!tegra_drm_file_owns_context(fpriv, context))
		return -ENODEV;

	if (args->syncpt >= context->client->base.num_syncpts)
		return -EINVAL;

	syncpt = context->client->base.syncpts[args->syncpt];

	base = host1x_syncpt_get_base(syncpt);
	if (!base)
		return -ENXIO;

	args->id = host1x_syncpt_base_id(base);

	return 0;
}

static int tegra_gem_set_tiling(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_tegra_gem_set_tiling *args = data;
	enum tegra_bo_tiling_mode mode;
	struct drm_gem_object *gem;
	unsigned long value = 0;
	struct tegra_bo *bo;

	switch (args->mode) {
	case DRM_TEGRA_GEM_TILING_MODE_PITCH:
		mode = TEGRA_BO_TILING_MODE_PITCH;

		if (args->value != 0)
			return -EINVAL;

		break;

	case DRM_TEGRA_GEM_TILING_MODE_TILED:
		mode = TEGRA_BO_TILING_MODE_TILED;

		if (args->value != 0)
			return -EINVAL;

		break;

	case DRM_TEGRA_GEM_TILING_MODE_BLOCK:
		mode = TEGRA_BO_TILING_MODE_BLOCK;

		if (args->value > 5)
			return -EINVAL;

		value = args->value;
		break;

	default:
		return -EINVAL;
	}

	gem = drm_gem_object_lookup(drm, file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);

	bo->tiling.mode = mode;
	bo->tiling.value = value;

	drm_gem_object_unreference(gem);

	return 0;
}

static int tegra_gem_get_tiling(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_tegra_gem_get_tiling *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;
	int err = 0;

	gem = drm_gem_object_lookup(drm, file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);

	switch (bo->tiling.mode) {
	case TEGRA_BO_TILING_MODE_PITCH:
		args->mode = DRM_TEGRA_GEM_TILING_MODE_PITCH;
		args->value = 0;
		break;

	case TEGRA_BO_TILING_MODE_TILED:
		args->mode = DRM_TEGRA_GEM_TILING_MODE_TILED;
		args->value = 0;
		break;

	case TEGRA_BO_TILING_MODE_BLOCK:
		args->mode = DRM_TEGRA_GEM_TILING_MODE_BLOCK;
		args->value = bo->tiling.value;
		break;

	default:
		err = -EINVAL;
		break;
	}

	drm_gem_object_unreference(gem);

	return err;
}

static int tegra_gem_set_flags(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct drm_tegra_gem_set_flags *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	if (args->flags & ~DRM_TEGRA_GEM_FLAGS)
		return -EINVAL;

	gem = drm_gem_object_lookup(drm, file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);
	bo->flags = 0;

	if (args->flags & DRM_TEGRA_GEM_BOTTOM_UP)
		bo->flags |= TEGRA_BO_BOTTOM_UP;

	drm_gem_object_unreference(gem);

	return 0;
}

static int tegra_gem_get_flags(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct drm_tegra_gem_get_flags *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	gem = drm_gem_object_lookup(drm, file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);
	args->flags = 0;

	if (bo->flags & TEGRA_BO_BOTTOM_UP)
		args->flags |= DRM_TEGRA_GEM_BOTTOM_UP;

	drm_gem_object_unreference(gem);

	return 0;
}
#endif

static const struct drm_ioctl_desc tegra_drm_ioctls[] = {
#ifdef CONFIG_DRM_TEGRA_STAGING
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_CREATE, tegra_gem_create, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_MMAP, tegra_gem_mmap, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_READ, tegra_syncpt_read, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_INCR, tegra_syncpt_incr, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_WAIT, tegra_syncpt_wait, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_OPEN_CHANNEL, tegra_open_channel, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_CLOSE_CHANNEL, tegra_close_channel, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_GET_SYNCPT, tegra_get_syncpt, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_SUBMIT, tegra_submit, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_GET_SYNCPT_BASE, tegra_get_syncpt_base, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_SET_TILING, tegra_gem_set_tiling, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_GET_TILING, tegra_gem_get_tiling, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_SET_FLAGS, tegra_gem_set_flags, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_GET_FLAGS, tegra_gem_get_flags, DRM_UNLOCKED),
#endif
};

static const struct file_operations tegra_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = tegra_drm_mmap,
	.poll = drm_poll,
	.read = drm_read,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static struct drm_crtc *tegra_crtc_from_pipe(struct drm_device *drm,
					     unsigned int pipe)
{
	struct drm_crtc *crtc;

	list_for_each_entry(crtc, &drm->mode_config.crtc_list, head) {
		if (pipe == drm_crtc_index(crtc))
			return crtc;
	}

	return NULL;
}

static u32 tegra_drm_get_vblank_counter(struct drm_device *drm, int pipe)
{
	struct drm_crtc *crtc = tegra_crtc_from_pipe(drm, pipe);
	struct tegra_dc *dc = to_tegra_dc(crtc);

	if (!crtc)
		return 0;

	return tegra_dc_get_vblank_counter(dc);
}

static int tegra_drm_enable_vblank(struct drm_device *drm, int pipe)
{
	struct drm_crtc *crtc = tegra_crtc_from_pipe(drm, pipe);
	struct tegra_dc *dc = to_tegra_dc(crtc);

	if (!crtc)
		return -ENODEV;

	tegra_dc_enable_vblank(dc);

	return 0;
}

static void tegra_drm_disable_vblank(struct drm_device *drm, int pipe)
{
	struct drm_crtc *crtc = tegra_crtc_from_pipe(drm, pipe);
	struct tegra_dc *dc = to_tegra_dc(crtc);

	if (crtc)
		tegra_dc_disable_vblank(dc);
}

static void tegra_drm_preclose(struct drm_device *drm, struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm_context *context, *tmp;
	struct drm_crtc *crtc;

	list_for_each_entry(crtc, &drm->mode_config.crtc_list, head)
		tegra_dc_cancel_page_flip(crtc, file);

	list_for_each_entry_safe(context, tmp, &fpriv->contexts, list)
		tegra_drm_context_free(context);

	kfree(fpriv);
}

#ifdef CONFIG_DEBUG_FS
static int tegra_debugfs_framebuffers(struct seq_file *s, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)s->private;
	struct drm_device *drm = node->minor->dev;
	struct drm_framebuffer *fb;

	mutex_lock(&drm->mode_config.fb_lock);

	list_for_each_entry(fb, &drm->mode_config.fb_list, head) {
		seq_printf(s, "%3d: user size: %d x %d, depth %d, %d bpp, refcount %d\n",
			   fb->base.id, fb->width, fb->height, fb->depth,
			   fb->bits_per_pixel,
			   atomic_read(&fb->refcount.refcount));
	}

	mutex_unlock(&drm->mode_config.fb_lock);

	return 0;
}

static int tegra_debugfs_iova(struct seq_file *s, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)s->private;
	struct drm_device *drm = node->minor->dev;
	struct tegra_drm *tegra = drm->dev_private;

	return drm_mm_dump_table(s, &tegra->mm);
}

static struct drm_info_list tegra_debugfs_list[] = {
	{ "framebuffers", tegra_debugfs_framebuffers, 0 },
	{ "iova", tegra_debugfs_iova, 0 },
};

static int tegra_debugfs_init(struct drm_minor *minor)
{
	return drm_debugfs_create_files(tegra_debugfs_list,
					ARRAY_SIZE(tegra_debugfs_list),
					minor->debugfs_root, minor);
}

static void tegra_debugfs_cleanup(struct drm_minor *minor)
{
	drm_debugfs_remove_files(tegra_debugfs_list,
				 ARRAY_SIZE(tegra_debugfs_list), minor);
}
#endif

static struct drm_driver tegra_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME,
	.load = tegra_drm_load,
	.unload = tegra_drm_unload,
	.open = tegra_drm_open,
	.preclose = tegra_drm_preclose,
	.lastclose = tegra_drm_lastclose,

	.get_vblank_counter = tegra_drm_get_vblank_counter,
	.enable_vblank = tegra_drm_enable_vblank,
	.disable_vblank = tegra_drm_disable_vblank,

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = tegra_debugfs_init,
	.debugfs_cleanup = tegra_debugfs_cleanup,
#endif

	.gem_free_object = tegra_bo_free_object,
	.gem_vm_ops = &tegra_bo_vm_ops,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_export = tegra_gem_prime_export,
	.gem_prime_import = tegra_gem_prime_import,

	.dumb_create = tegra_bo_dumb_create,
	.dumb_map_offset = tegra_bo_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,

	.ioctls = tegra_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(tegra_drm_ioctls),
	.fops = &tegra_drm_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

int tegra_drm_register_client(struct tegra_drm *tegra,
			      struct tegra_drm_client *client)
{
	mutex_lock(&tegra->clients_lock);
	list_add_tail(&client->list, &tegra->clients);
	mutex_unlock(&tegra->clients_lock);

	return 0;
}

int tegra_drm_unregister_client(struct tegra_drm *tegra,
				struct tegra_drm_client *client)
{
	mutex_lock(&tegra->clients_lock);
	list_del_init(&client->list);
	mutex_unlock(&tegra->clients_lock);

	return 0;
}

void *tegra_drm_alloc(struct tegra_drm *tegra, size_t size,
			      dma_addr_t *iova)
{
	size_t aligned = PAGE_ALIGN(size);
	int num_pages = aligned >> PAGE_SHIFT;
	void *virt;
	unsigned int start;
	int err;

	virt = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
					get_order(aligned));
	if (!virt)
		return NULL;

	if (!tegra->domain) {
		/*
		 * If IOMMU is disabled, devices address physical memory
		 * directly.
		 */
		*iova = virt_to_phys(virt);
		return virt;
	}

	mutex_lock(&tegra->iova_lock);

	start = bitmap_find_next_zero_area(tegra->iova_bitmap,
					   tegra->iova_bitmap_bits, 0,
					   num_pages, 0);
	if (start > tegra->iova_bitmap_bits)
		goto free_pages;

	bitmap_set(tegra->iova_bitmap, start, num_pages);

	*iova = tegra->iova_start + (start << PAGE_SHIFT);
	err = iommu_map(tegra->domain, *iova, virt_to_phys(virt),
			aligned, IOMMU_READ | IOMMU_WRITE);
	if (err < 0)
		goto free_iova;

	mutex_unlock(&tegra->iova_lock);

	return virt;

free_iova:
	bitmap_clear(tegra->iova_bitmap, start, num_pages);
free_pages:
	mutex_unlock(&tegra->iova_lock);

	free_pages((unsigned long)virt, get_order(aligned));

	return NULL;
}

void tegra_drm_free(struct tegra_drm *tegra, size_t size, void *virt,
		    dma_addr_t iova)
{
	size_t aligned = PAGE_ALIGN(size);
	int num_pages = aligned >> PAGE_SHIFT;

	if (tegra->domain) {
		unsigned int start = (iova - tegra->iova_start) >> PAGE_SHIFT;

		iommu_unmap(tegra->domain, iova, aligned);

		mutex_lock(&tegra->iova_lock);
		bitmap_clear(tegra->iova_bitmap, start, num_pages);
		mutex_unlock(&tegra->iova_lock);
	}

	free_pages((unsigned long)virt, get_order(aligned));
}

static int host1x_drm_probe(struct host1x_device *dev)
{
	struct drm_driver *driver = &tegra_drm_driver;
	struct drm_device *drm;
	int err;

	drm = drm_dev_alloc(driver, &dev->dev);
	if (!drm)
		return -ENOMEM;

	drm_dev_set_unique(drm, dev_name(&dev->dev));
	dev_set_drvdata(&dev->dev, drm);

	err = drm_dev_register(drm, 0);
	if (err < 0)
		goto unref;

	DRM_INFO("Initialized %s %d.%d.%d %s on minor %d\n", driver->name,
		 driver->major, driver->minor, driver->patchlevel,
		 driver->date, drm->primary->index);

	return 0;

unref:
	drm_dev_unref(drm);
	return err;
}

static int host1x_drm_remove(struct host1x_device *dev)
{
	struct drm_device *drm = dev_get_drvdata(&dev->dev);

	drm_dev_unregister(drm);
	drm_dev_unref(drm);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int host1x_drm_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	drm_kms_helper_poll_disable(drm);

	return 0;
}

static int host1x_drm_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	drm_kms_helper_poll_enable(drm);

	return 0;
}
#endif

static const struct dev_pm_ops host1x_drm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(host1x_drm_suspend, host1x_drm_resume)
};

static const struct of_device_id host1x_drm_subdevs[] = {
	{ .compatible = "nvidia,tegra20-dc", },
	{ .compatible = "nvidia,tegra20-hdmi", },
	{ .compatible = "nvidia,tegra20-gr2d", },
	{ .compatible = "nvidia,tegra20-gr3d", },
	{ .compatible = "nvidia,tegra30-dc", },
	{ .compatible = "nvidia,tegra30-hdmi", },
	{ .compatible = "nvidia,tegra30-gr2d", },
	{ .compatible = "nvidia,tegra30-gr3d", },
	{ .compatible = "nvidia,tegra114-dsi", },
	{ .compatible = "nvidia,tegra114-hdmi", },
	{ .compatible = "nvidia,tegra114-gr3d", },
	{ .compatible = "nvidia,tegra124-dc", },
	{ .compatible = "nvidia,tegra124-sor", },
	{ .compatible = "nvidia,tegra124-hdmi", },
	{ .compatible = "nvidia,tegra124-vic", },
	{ /* sentinel */ }
};

static struct host1x_driver host1x_drm_driver = {
	.driver = {
		.name = "drm",
		.pm = &host1x_drm_pm_ops,
	},
	.probe = host1x_drm_probe,
	.remove = host1x_drm_remove,
	.subdevs = host1x_drm_subdevs,
};

static int __init host1x_drm_init(void)
{
	int err;

	err = host1x_driver_register(&host1x_drm_driver);
	if (err < 0)
		return err;

	err = platform_driver_register(&tegra_dc_driver);
	if (err < 0)
		goto unregister_host1x;

	err = platform_driver_register(&tegra_dsi_driver);
	if (err < 0)
		goto unregister_dc;

	err = platform_driver_register(&tegra_sor_driver);
	if (err < 0)
		goto unregister_dsi;

	err = platform_driver_register(&tegra_hdmi_driver);
	if (err < 0)
		goto unregister_sor;

	err = platform_driver_register(&tegra_dpaux_driver);
	if (err < 0)
		goto unregister_hdmi;

	err = platform_driver_register(&tegra_gr2d_driver);
	if (err < 0)
		goto unregister_dpaux;

	err = platform_driver_register(&tegra_gr3d_driver);
	if (err < 0)
		goto unregister_gr2d;

	err = platform_driver_register(&tegra_vic_driver);
	if (err < 0)
		goto unregister_gr3d;

	return 0;

unregister_gr3d:
	platform_driver_unregister(&tegra_gr3d_driver);
unregister_gr2d:
	platform_driver_unregister(&tegra_gr2d_driver);
unregister_dpaux:
	platform_driver_unregister(&tegra_dpaux_driver);
unregister_hdmi:
	platform_driver_unregister(&tegra_hdmi_driver);
unregister_sor:
	platform_driver_unregister(&tegra_sor_driver);
unregister_dsi:
	platform_driver_unregister(&tegra_dsi_driver);
unregister_dc:
	platform_driver_unregister(&tegra_dc_driver);
unregister_host1x:
	host1x_driver_unregister(&host1x_drm_driver);
	return err;
}
module_init(host1x_drm_init);

static void __exit host1x_drm_exit(void)
{
	platform_driver_unregister(&tegra_gr3d_driver);
	platform_driver_unregister(&tegra_gr2d_driver);
	platform_driver_unregister(&tegra_dpaux_driver);
	platform_driver_unregister(&tegra_hdmi_driver);
	platform_driver_unregister(&tegra_sor_driver);
	platform_driver_unregister(&tegra_dsi_driver);
	platform_driver_unregister(&tegra_dc_driver);
	host1x_driver_unregister(&host1x_drm_driver);
}
module_exit(host1x_drm_exit);

MODULE_AUTHOR("Thierry Reding <thierry.reding@avionic-design.de>");
MODULE_DESCRIPTION("NVIDIA Tegra DRM driver");
MODULE_LICENSE("GPL v2");
