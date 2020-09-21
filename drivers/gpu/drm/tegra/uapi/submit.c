// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2020 NVIDIA Corporation */

#include <linux/dma-fence-array.h>
#include <linux/file.h>
#include <linux/host1x.h>
#include <linux/iommu.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/nospec.h>
#include <linux/pm_runtime.h>
#include <linux/sync_file.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include "../uapi.h"
#include "../drm.h"
#include "../gem.h"

static struct tegra_drm_mapping *
tegra_drm_mapping_get(struct tegra_drm_channel_ctx *ctx, u32 id)
{
	struct tegra_drm_mapping *mapping;

	xa_lock(&ctx->mappings);
	mapping = xa_load(&ctx->mappings, id);
	if (mapping)
		kref_get(&mapping->ref);
	xa_unlock(&ctx->mappings);

	return mapping;
}

struct gather_bo {
	struct host1x_bo base;

	struct kref ref;

	u32 *gather_data;
	size_t gather_data_len;
};

static struct host1x_bo *gather_bo_get(struct host1x_bo *host_bo)
{
	struct gather_bo *bo = container_of(host_bo, struct gather_bo, base);

	kref_get(&bo->ref);

	return host_bo;
}

static void gather_bo_release(struct kref *ref)
{
	struct gather_bo *bo = container_of(ref, struct gather_bo, ref);

	kfree(bo->gather_data);
	kfree(bo);
}

static void gather_bo_put(struct host1x_bo *host_bo)
{
	struct gather_bo *bo = container_of(host_bo, struct gather_bo, base);

	kref_put(&bo->ref, gather_bo_release);
}

static struct sg_table *
gather_bo_pin(struct device *dev, struct host1x_bo *host_bo, dma_addr_t *phys)
{
	struct gather_bo *bo = container_of(host_bo, struct gather_bo, base);
	struct sg_table *sgt;
	int err;

	if (phys) {
		*phys = virt_to_phys(bo->gather_data);
		return NULL;
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	err = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (err) {
		kfree(sgt);
		return ERR_PTR(err);
	}

	sg_init_one(sgt->sgl, bo->gather_data, bo->gather_data_len);

	return sgt;
}

static void gather_bo_unpin(struct device *dev, struct sg_table *sgt)
{
	if (sgt) {
		sg_free_table(sgt);
		kfree(sgt);
	}
}

static void *gather_bo_mmap(struct host1x_bo *host_bo)
{
	struct gather_bo *bo = container_of(host_bo, struct gather_bo, base);

	return bo->gather_data;
}

static void gather_bo_munmap(struct host1x_bo *host_bo, void *addr)
{
}

static const struct host1x_bo_ops gather_bo_ops = {
	.get = gather_bo_get,
	.put = gather_bo_put,
	.pin = gather_bo_pin,
	.unpin = gather_bo_unpin,
	.mmap = gather_bo_mmap,
	.munmap = gather_bo_munmap,
};

struct tegra_drm_used_mapping {
	struct tegra_drm_mapping *mapping;
	u32 flags;
};

struct tegra_drm_job_data {
	struct tegra_drm_used_mapping *used_mappings;
	u32 num_used_mappings;
};

static int submit_copy_gather_data(struct drm_device *drm,
				   struct gather_bo **pbo,
				   struct drm_tegra_channel_submit *args)
{
	unsigned long copy_err;
	struct gather_bo *bo;

	if (args->gather_data_words == 0) {
		drm_info(drm, "gather_data_words can't be 0");
		return -EINVAL;
	}
	if (args->gather_data_words > 1024) {
		drm_info(drm, "gather_data_words can't be over 1024");
		return -E2BIG;
	}

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;

	kref_init(&bo->ref);
	host1x_bo_init(&bo->base, &gather_bo_ops);

	bo->gather_data =
		kmalloc(args->gather_data_words*4, GFP_KERNEL | __GFP_NOWARN);
	if (!bo->gather_data) {
		kfree(bo);
		return -ENOMEM;
	}

	copy_err = copy_from_user(bo->gather_data,
				  u64_to_user_ptr(args->gather_data_ptr),
				  args->gather_data_words*4);
	if (copy_err) {
		kfree(bo->gather_data);
		kfree(bo);
		return -EFAULT;
	}

	bo->gather_data_len = args->gather_data_words;

	*pbo = bo;

	return 0;
}

static int submit_write_reloc(struct gather_bo *bo,
			      struct drm_tegra_submit_buf *buf,
			      struct tegra_drm_mapping *mapping)
{
	/* TODO check that target_offset is within bounds */
	dma_addr_t iova = mapping->iova + buf->reloc.target_offset;
	u32 written_ptr = (u32)(iova >> buf->reloc.shift);

	if (buf->flags & DRM_TEGRA_SUBMIT_BUF_RELOC_BLOCKLINEAR)
		written_ptr |= BIT(39);

	if (buf->reloc.gather_offset_words >= bo->gather_data_len)
		return -EINVAL;

	buf->reloc.gather_offset_words = array_index_nospec(
		buf->reloc.gather_offset_words, bo->gather_data_len);

	bo->gather_data[buf->reloc.gather_offset_words] = written_ptr;

	return 0;
}

static void submit_unlock_resv(struct tegra_drm_job_data *job_data,
			       struct ww_acquire_ctx *acquire_ctx)
{
	u32 i;

	for (i = 0; i < job_data->num_used_mappings; i++) {
		struct tegra_bo *bo = host1x_to_tegra_bo(
			job_data->used_mappings[i].mapping->bo);

		dma_resv_unlock(bo->gem.resv);
	}

	ww_acquire_fini(acquire_ctx);
}

static int submit_handle_resv(struct tegra_drm_job_data *job_data,
			      struct ww_acquire_ctx *acquire_ctx)
{
	int contended = -1;
	int err;
	u32 i;

	/* Based on drm_gem_lock_reservations */

	ww_acquire_init(acquire_ctx, &reservation_ww_class);

retry:
	if (contended != -1) {
		struct tegra_bo *bo = host1x_to_tegra_bo(
			job_data->used_mappings[contended].mapping->bo);

		err = dma_resv_lock_slow_interruptible(bo->gem.resv,
						       acquire_ctx);
		if (err) {
			ww_acquire_done(acquire_ctx);
			return err;
		}
	}

	for (i = 0; i < job_data->num_used_mappings; i++) {
		struct tegra_bo *bo = host1x_to_tegra_bo(
			job_data->used_mappings[contended].mapping->bo);

		if (i == contended)
			continue;

		err = dma_resv_lock_interruptible(bo->gem.resv, acquire_ctx);
		if (err) {
			int j;

			for (j = 0; j < i; j++) {
				bo = host1x_to_tegra_bo(
					job_data->used_mappings[j].mapping->bo);
				dma_resv_unlock(bo->gem.resv);
			}

			if (contended != -1 && contended >= i) {
				bo = host1x_to_tegra_bo(
					job_data->used_mappings[contended].mapping->bo);
				dma_resv_unlock(bo->gem.resv);
			}

			if (err == -EDEADLK) {
				contended = i;
				goto retry;
			}

			ww_acquire_done(acquire_ctx);
			return err;
		}
	}

	ww_acquire_done(acquire_ctx);

	for (i = 0; i < job_data->num_used_mappings; i++) {
		struct tegra_drm_used_mapping *um = &job_data->used_mappings[i];
		struct tegra_bo *bo = host1x_to_tegra_bo(
			job_data->used_mappings[i].mapping->bo);

		if (um->flags & DRM_TEGRA_SUBMIT_BUF_RESV_READ) {
			err = dma_resv_reserve_shared(bo->gem.resv, 1);
			if (err < 0)
				goto unlock_resv;
		}
	}

	return 0;

unlock_resv:
	submit_unlock_resv(job_data, acquire_ctx);

	return err;
}

static int submit_process_bufs(struct drm_device *drm, struct gather_bo *bo,
			       struct tegra_drm_job_data *job_data,
			       struct tegra_drm_channel_ctx *ctx,
			       struct drm_tegra_channel_submit *args,
			       struct ww_acquire_ctx *acquire_ctx)
{
	struct drm_tegra_submit_buf __user *user_bufs_ptr =
		u64_to_user_ptr(args->bufs_ptr);
	struct tegra_drm_mapping *mapping;
	struct drm_tegra_submit_buf buf;
	unsigned long copy_err;
	int err;
	u32 i;

	job_data->used_mappings =
		kcalloc(args->num_bufs, sizeof(*job_data->used_mappings), GFP_KERNEL);
	if (!job_data->used_mappings)
		return -ENOMEM;

	for (i = 0; i < args->num_bufs; i++) {
		copy_err = copy_from_user(&buf, user_bufs_ptr+i, sizeof(buf));
		if (copy_err) {
			err = -EFAULT;
			goto drop_refs;
		}

		if (buf.flags & ~(DRM_TEGRA_SUBMIT_BUF_RELOC_BLOCKLINEAR |
				  DRM_TEGRA_SUBMIT_BUF_RESV_READ |
				  DRM_TEGRA_SUBMIT_BUF_RESV_WRITE)) {
			err = -EINVAL;
			goto drop_refs;
		}

		if (buf.reserved[0] || buf.reserved[1]) {
			err = -EINVAL;
			goto drop_refs;
		}

		mapping = tegra_drm_mapping_get(ctx, buf.mapping_id);
		if (!mapping) {
			drm_info(drm, "invalid mapping_id for buf: %u",
				 buf.mapping_id);
			err = -EINVAL;
			goto drop_refs;
		}

		err = submit_write_reloc(bo, &buf, mapping);
		if (err) {
			tegra_drm_mapping_put(mapping);
			goto drop_refs;
		}

		job_data->used_mappings[i].mapping = mapping;
		job_data->used_mappings[i].flags = buf.flags;
	}

	return 0;

drop_refs:
	for (;;) {
		if (i-- == 0)
			break;

		tegra_drm_mapping_put(job_data->used_mappings[i].mapping);
	}

	kfree(job_data->used_mappings);
	job_data->used_mappings = NULL;

	return err;
}

static int submit_create_job(struct drm_device *drm, struct host1x_job **pjob,
			     struct gather_bo *bo,
			     struct tegra_drm_channel_ctx *ctx,
			     struct drm_tegra_channel_submit *args,
			     struct drm_file *file)
{
	struct drm_tegra_submit_cmd __user *user_cmds_ptr =
		u64_to_user_ptr(args->cmds_ptr);
	struct drm_tegra_submit_cmd cmd;
	struct host1x_job *job;
	unsigned long copy_err;
	u32 i, gather_offset = 0;
	int err = 0;

	job = host1x_job_alloc(ctx->channel, args->num_cmds, 0);
	if (!job)
		return -ENOMEM;

	job->client = &ctx->client->base;
	job->class = ctx->client->base.class;
	job->serialize = true;

	for (i = 0; i < args->num_cmds; i++) {
		copy_err = copy_from_user(&cmd, user_cmds_ptr+i, sizeof(cmd));
		if (copy_err) {
			err = -EFAULT;
			goto free_job;
		}

		if (cmd.type == DRM_TEGRA_SUBMIT_CMD_GATHER_UPTR) {
			if (cmd.gather_uptr.reserved[0] ||
			    cmd.gather_uptr.reserved[1] ||
			    cmd.gather_uptr.reserved[2]) {
				err = -EINVAL;
				goto free_job;
			}

			/* Check for maximum gather size */
			if (cmd.gather_uptr.words > 16383) {
				err = -EINVAL;
				goto free_job;
			}

			host1x_job_add_gather(job, &bo->base,
					      cmd.gather_uptr.words,
					      gather_offset*4);

			gather_offset += cmd.gather_uptr.words;

			if (gather_offset > bo->gather_data_len) {
				err = -EINVAL;
				goto free_job;
			}
		} else if (cmd.type == DRM_TEGRA_SUBMIT_CMD_WAIT_SYNCPT) {
			if (cmd.wait_syncpt.reserved[0] ||
			    cmd.wait_syncpt.reserved[1]) {
				err = -EINVAL;
				goto free_job;
			}

			host1x_job_add_wait(job, cmd.wait_syncpt.id,
					    cmd.wait_syncpt.threshold);
		} else if (cmd.type == DRM_TEGRA_SUBMIT_CMD_WAIT_SYNC_FILE) {
			struct dma_fence *f;

			if (cmd.wait_sync_file.reserved[0] ||
			    cmd.wait_sync_file.reserved[1] ||
			    cmd.wait_sync_file.reserved[2]) {
				err = -EINVAL;
				goto free_job;
			}

			f = sync_file_get_fence(cmd.wait_sync_file.fd);
			if (!f) {
				err = -EINVAL;
				goto free_job;
			}

			err = dma_fence_wait(f, true);
			dma_fence_put(f);

			if (err)
				goto free_job;
		} else {
			err = -EINVAL;
			goto free_job;
		}
	}

	if (gather_offset == 0) {
		drm_info(drm, "job must have at least one gather");
		err = -EINVAL;
		goto free_job;
	}

	*pjob = job;

	return 0;

free_job:
	host1x_job_put(job);

	return err;
}

static int submit_handle_syncpts(struct drm_device *drm, struct host1x_job *job,
				 struct drm_tegra_channel_submit *args)
{
	struct drm_tegra_submit_syncpt_incr *incr;
	struct host1x_syncpt *sp;

	if (args->syncpt_incrs[1].num_incrs != 0) {
		drm_info(drm, "Only 1 syncpoint supported for now");
		return -EINVAL;
	}

	incr = &args->syncpt_incrs[0];

	if ((incr->flags & ~DRM_TEGRA_SUBMIT_SYNCPT_INCR_CREATE_SYNC_FILE) ||
	    incr->reserved[0] || incr->reserved[1] || incr->reserved[2])
		return -EINVAL;

	/* Syncpt ref will be dropped on job release */
	sp = host1x_syncpt_fd_get(incr->syncpt_fd);
	if (IS_ERR(sp))
		return PTR_ERR(sp);

	job->syncpt = sp;
	job->syncpt_incrs = incr->num_incrs;

	return 0;
}

static int submit_create_postfences(struct host1x_job *job,
				    struct drm_tegra_channel_submit *args)
{
	struct drm_tegra_submit_syncpt_incr *incr = &args->syncpt_incrs[0];
	struct tegra_drm_job_data *job_data = job->user_data;
	struct dma_fence *fence;
	int err = 0;
	u32 i;

	fence = host1x_fence_create(job->syncpt, job->syncpt_end);
	if (IS_ERR(fence))
		return PTR_ERR(fence);

	incr->fence_value = job->syncpt_end;

	for (i = 0; i < job_data->num_used_mappings; i++) {
		struct tegra_drm_used_mapping *um = &job_data->used_mappings[i];
		struct tegra_bo *bo = host1x_to_tegra_bo(um->mapping->bo);

		if (um->flags & DRM_TEGRA_SUBMIT_BUF_RESV_READ)
			dma_resv_add_shared_fence(bo->gem.resv, fence);

		if (um->flags & DRM_TEGRA_SUBMIT_BUF_RESV_WRITE)
			dma_resv_add_excl_fence(bo->gem.resv, fence);
	}

	if (incr->flags & DRM_TEGRA_SUBMIT_SYNCPT_INCR_CREATE_SYNC_FILE) {
		struct sync_file *sf;

		err = get_unused_fd_flags(O_CLOEXEC);
		if (err < 0)
			goto put_fence;

		sf = sync_file_create(fence);
		if (!sf) {
			err = -ENOMEM;
			goto put_fence;
		}

		fd_install(err, sf->file);
		incr->sync_file_fd = err;
		err = 0;
	}

put_fence:
	dma_fence_put(fence);

	return err;
}

static void release_job(struct host1x_job *job)
{
	struct tegra_drm_client *client =
		container_of(job->client, struct tegra_drm_client, base);
	struct tegra_drm_job_data *job_data = job->user_data;
	u32 i;

	for (i = 0; i < job_data->num_used_mappings; i++)
		tegra_drm_mapping_put(job_data->used_mappings[i].mapping);

	kfree(job_data->used_mappings);
	kfree(job_data);

	pm_runtime_put_autosuspend(client->base.dev);
}

int tegra_drm_ioctl_channel_submit(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_channel_submit *args = data;
	struct tegra_drm_job_data *job_data;
	struct ww_acquire_ctx acquire_ctx;
	struct tegra_drm_channel_ctx *ctx;
	struct host1x_job *job;
	struct gather_bo *bo;
	u32 i;
	int err;

	if (args->reserved0 || args->reserved1)
		return -EINVAL;

	ctx = tegra_drm_channel_ctx_lock(fpriv, args->channel_ctx);
	if (!ctx)
		return -EINVAL;

	err = submit_copy_gather_data(drm, &bo, args);
	if (err)
		goto unlock;

	job_data = kzalloc(sizeof(*job_data), GFP_KERNEL);
	if (!job_data) {
		err = -ENOMEM;
		goto put_bo;
	}

	err = submit_process_bufs(drm, bo, job_data, ctx, args, &acquire_ctx);
	if (err)
		goto free_job_data;

	err = submit_create_job(drm, &job, bo, ctx, args, file);
	if (err)
		goto free_job_data;

	err = submit_handle_syncpts(drm, job, args);
	if (err)
		goto put_job;

	err = host1x_job_pin(job, ctx->client->base.dev);
	if (err)
		goto put_job;

	err = pm_runtime_get_sync(ctx->client->base.dev);
	if (err)
		goto put_pm_runtime;

	job->user_data = job_data;
	job->release = release_job;
	job->timeout = 10000;

	/*
	 * job_data is now part of job reference counting, so don't release
	 * it from here.
	 */
	job_data = NULL;

	err = submit_handle_resv(job->user_data, &acquire_ctx);
	if (err)
		goto put_pm_runtime;

	err = host1x_job_submit(job);
	if (err)
		goto unlock_resv;

	err = submit_create_postfences(job, args);

	submit_unlock_resv(job->user_data, &acquire_ctx);

	goto put_job;

unlock_resv:
	submit_unlock_resv(job->user_data, &acquire_ctx);
put_pm_runtime:
	pm_runtime_put(ctx->client->base.dev);
	host1x_job_unpin(job);
put_job:
	host1x_job_put(job);
free_job_data:
	if (job_data && job_data->used_mappings) {
		for (i = 0; i < job_data->num_used_mappings; i++)
			tegra_drm_mapping_put(job_data->used_mappings[i].mapping);
		kfree(job_data->used_mappings);
	}
	if (job_data)
		kfree(job_data);
put_bo:
	kref_put(&bo->ref, gather_bo_release);
unlock:
	mutex_unlock(&fpriv->lock);
	return err;
}
