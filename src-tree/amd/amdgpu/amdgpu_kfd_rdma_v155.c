// SPDX-License-Identifier: MIT
/*
 * V15.5 RDMA BO-pin / hipFree decoupling helpers
 *
 * Implements four architectural gaps identified in the RDMA stress-test
 * write-up (hjbog-srdc-21/22, 2026-04-16) that caused customer-visible
 * "Freeing queue vital buffer" crashes and multi-GB VRAM leaks:
 *
 *   #4 pin watermark          dev_warn/err_ratelimited when pinned bytes
 *                             crosses warn / critical thresholds. Gives the
 *                             application a chance to quiesce before the
 *                             hard cap (dmabuf_pin_max_mb) returns -ENOSPC.
 *
 *   #3 orphan pin reaper      BOs that were freed by userspace while still
 *                             RDMA-pinned are queued on a per-adev list.
 *                             A delayed_work scans the list and force-unpins
 *                             + releases entries older than
 *                             amdgpu_pin_orphan_timeout_ms.
 *
 *   #2 hipFree EBUSY          free_memory_of_gpu calls wait_pin_drop for a
 *                             bounded window; if that fails, the BO is
 *                             queued onto the orphan list (policy =1) or
 *                             the ioctl returns -EBUSY (policy =2). Either
 *                             way hipFree stops silently leaking VRAM.
 *
 *   #1 strict dereg drain     Unpin drains *all* dma-fences on the resv
 *                             (TTM, CS, MES, IB), not just KFD-owned ones,
 *                             mirroring NVIDIA's nv_peer_mem free_callback.
 *
 * All four live in this file so the v15.3 -> v15.5 diff is reviewable in
 * one hunk. Hook points in amdgpu_amdkfd_gpuvm.c are kept minimal.
 */

#include <linux/delay.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "amdgpu.h"
#include "amdgpu_object.h"
#include "amdgpu_amdkfd.h"

/* ===== helpers shared between all four features =========================*/

static inline u64 amdgpu_kfd_pin_warn_bytes(void)
{
	u64 max_bytes = (u64)dmabuf_pin_max_mb << 20;
	u64 warn_bytes;

	if (!max_bytes)
		return 0;

	if (amdgpu_dmabuf_pin_warn_mb)
		warn_bytes = (u64)amdgpu_dmabuf_pin_warn_mb << 20;
	else
		warn_bytes = (max_bytes / 4) * 3;	/* 75% */

	if (warn_bytes >= max_bytes)
		warn_bytes = max_bytes - (max_bytes >> 4);
	return warn_bytes;
}

static inline u64 amdgpu_kfd_pin_critical_bytes(void)
{
	u64 max_bytes = (u64)dmabuf_pin_max_mb << 20;
	u64 crit_bytes;

	if (!max_bytes)
		return 0;

	if (amdgpu_dmabuf_pin_critical_mb)
		crit_bytes = (u64)amdgpu_dmabuf_pin_critical_mb << 20;
	else
		crit_bytes = (max_bytes / 10) * 9;	/* 90% */

	if (crit_bytes >= max_bytes)
		crit_bytes = max_bytes - (max_bytes >> 5);
	return crit_bytes;
}

/* ===== #4 pin watermark =================================================*/

static void amdgpu_kfd_rdma_quota_drop_saturated(struct amdgpu_device *adev,
							 u64 bytes, const char *reason)
{
	if (!dmabuf_pin_max_mb || !bytes)
		return;

	for (;;) {
		s64 old_s = atomic64_read(&adev->kfd.rdma_pinned_bytes);

		if (unlikely(old_s <= 0 || (u64)old_s < bytes)) {
			if (atomic64_cmpxchg(&adev->kfd.rdma_pinned_bytes, old_s, 0) == old_s) {
				atomic64_inc(&adev->kfd.rdma_pin_accounting_underflow);
				atomic64_inc(&adev->kfd.rdma_pin_accounting_clamp);
				dev_warn_ratelimited(adev->dev,
					"KFD RDMA quota drop underflow: pinned=%lld bytes drop=%lluMB reason=%s; clamped to 0\n",
					(long long)old_s, bytes >> 20, reason ? reason : "unknown");
				return;
			}
			continue;
		}

		if (atomic64_cmpxchg(&adev->kfd.rdma_pinned_bytes,
					     old_s, old_s - (s64)bytes) == old_s)
			return;
	}
}

void amdgpu_kfd_pin_watermark_check(struct amdgpu_device *adev, u64 new_total)
{
	u64 warn_b = amdgpu_kfd_pin_warn_bytes();
	u64 crit_b = amdgpu_kfd_pin_critical_bytes();
	u64 peak;

	if (!dmabuf_pin_max_mb)
		return;

	/* Update peak */
	peak = atomic64_read(&adev->kfd.rdma_pin_watermark_peak);
	while (new_total > peak) {
		if (atomic64_cmpxchg(&adev->kfd.rdma_pin_watermark_peak,
				     peak, new_total) == peak)
			break;
		peak = atomic64_read(&adev->kfd.rdma_pin_watermark_peak);
	}

	/* Critical first: edge-triggered via latched flag */
	if (crit_b && new_total >= crit_b) {
		if (!atomic_xchg(&adev->kfd.rdma_pin_critical_latched, 1)) {
			dev_err_ratelimited(adev->dev,
				"amdgpu V15.5 #4 CRITICAL pinned=%lluMB >= crit=%lluMB (max=%uMB). hipFree+ibv_dereg must quiesce NOW.\n",
				new_total >> 20, crit_b >> 20,
				dmabuf_pin_max_mb);
		}
	} else if (crit_b && new_total < (crit_b - (crit_b >> 3))) {
		/* hysteresis: 12.5% below critical to re-arm */
		atomic_set(&adev->kfd.rdma_pin_critical_latched, 0);
	}

	if (warn_b && new_total >= warn_b) {
		if (!atomic_xchg(&adev->kfd.rdma_pin_warn_latched, 1)) {
			dev_warn_ratelimited(adev->dev,
				"amdgpu V15.5 #4 WARN pinned=%lluMB >= warn=%lluMB (max=%uMB). Rotate/quiesce MR pool before critical.\n",
				new_total >> 20, warn_b >> 20,
				dmabuf_pin_max_mb);
		}
	} else if (warn_b && new_total < (warn_b - (warn_b >> 3))) {
		atomic_set(&adev->kfd.rdma_pin_warn_latched, 0);
	}
}

/* ===== #1 strict unpin drain + #2 pin-drop wait =========================*/

/*
 * Bounded wait on *all* dma-fences on the BO resv, not just KFD-owned.
 *
 * Caller must hold bo reserve (dma_resv_lock).
 *
 * Returns 0 on success, -ETIME on timeout (caller decides whether to
 * proceed or abort). On timeout we still let the caller continue -- the
 * reservation lock guarantees no new fences can appear, and subsequent
 * ttm_bo_wait/ttm_bo_unpin will block on the remainder anyway.
 */
int amdgpu_kfd_unpin_drain(struct amdgpu_bo *bo, int timeout_ms)
{
	struct dma_resv *resv;
	long timeout;
	long left;

	if (!bo || timeout_ms <= 0)
		return 0;

	resv = bo->tbo.base.resv;
	if (!resv)
		return 0;

	timeout = msecs_to_jiffies(timeout_ms);
	left = dma_resv_wait_timeout(resv, DMA_RESV_USAGE_BOOKKEEP,
				     true, timeout);
	if (left == 0) {
		struct amdgpu_device *adev = amdgpu_ttm_adev(bo->tbo.bdev);
		atomic64_inc(&adev->kfd.unpin_drain_timeouts);
		dev_warn_ratelimited(adev->dev,
			"amdgpu V15.5 #1 unpin_drain timeout bo=%p size=%lluMB timeout=%dms\n",
			bo, (u64)amdgpu_bo_size(bo) >> 20, timeout_ms);
		return -ETIME;
	} else if (left < 0) {
		return (int)left;
	}
	return 0;
}

/*
 * Poll bo->tbo.pin_count until it hits 0 or we time out.
 *
 * Caller must hold bo reserve.
 */
int amdgpu_kfd_wait_pin_drop(struct amdgpu_bo *bo, int timeout_ms)
{
	unsigned long deadline;
	int slept_us = 0;

	if (!bo || timeout_ms <= 0)
		return (bo && bo->tbo.pin_count) ? -EBUSY : 0;

	if (bo->tbo.pin_count == 0)
		return 0;

	deadline = jiffies + msecs_to_jiffies(timeout_ms);

	while (time_before(jiffies, deadline)) {
		/* exponential backoff: 100us, 200us, ... capped 5ms */
		int us = min(5000, 100 << min(slept_us / 5, 6));

		usleep_range(us, us + (us >> 2));
		slept_us++;

		if (bo->tbo.pin_count == 0)
			return 0;
	}

	return -EBUSY;
}

/* ===== #3 orphan pin reaper + #2 queue-to-orphan ========================*/

/*
 * Called by the release path when a BO with pin_count > 0 is being freed.
 * We take an extra ref on the BO and park it on adev->kfd.orphan_list;
 * the reaper delayed_work will force-unpin it after a timeout.
 *
 * Must be called with BO *unreserved* -- we re-reserve under the reaper.
 */
int amdgpu_kfd_orphan_queue(struct amdgpu_device *adev, struct amdgpu_bo *bo,
			    u64 bytes)
{
	struct kfd_orphan_pin *op;

	if (!adev || !bo)
		return -EINVAL;

	op = kzalloc(sizeof(*op), GFP_KERNEL);
	if (!op)
		return -ENOMEM;

	/* Keep the BO alive until the reaper runs. */
	amdgpu_bo_ref(bo);
	op->bo = bo;
	op->queued_at_jiffies = jiffies;
	op->bytes = bytes;
	op->owner_pid = current->pid;
	strscpy(op->owner_comm, current->comm, sizeof(op->owner_comm));

	spin_lock(&adev->kfd.orphan_lock);
	list_add_tail(&op->node, &adev->kfd.orphan_list);
	spin_unlock(&adev->kfd.orphan_lock);

	atomic64_inc(&adev->kfd.rdma_pin_orphans_queued);

	dev_warn_ratelimited(adev->dev,
		"amdgpu V15.5 #3 orphan queued bo=%p size=%lluMB pid=%d(%s) pin_count=%d. Will force-unpin after %dms.\n",
		bo, bytes >> 20, op->owner_pid, op->owner_comm,
		bo->tbo.pin_count, amdgpu_pin_orphan_timeout_ms);

	/* Kick the reaper in case it was sleeping. */
	if (adev->kfd.reaper_started)
		mod_delayed_work(system_wq, &adev->kfd.reaper_work, 0);
	return 0;
}

static void amdgpu_kfd_reaper_fn(struct work_struct *work)
{
	struct amdgpu_kfd_dev *kfd = container_of(to_delayed_work(work),
				struct amdgpu_kfd_dev, reaper_work);
	struct amdgpu_device *adev = container_of(kfd, struct amdgpu_device, kfd);
	struct kfd_orphan_pin *op, *tmp;
	LIST_HEAD(reap_now);
	unsigned long cutoff;
	int reaped = 0;

	if (amdgpu_pin_orphan_timeout_ms <= 0)
		goto reschedule;

	cutoff = jiffies - msecs_to_jiffies(amdgpu_pin_orphan_timeout_ms);

	spin_lock(&kfd->orphan_lock);
	list_for_each_entry_safe(op, tmp, &kfd->orphan_list, node) {
		if (time_after_eq(op->queued_at_jiffies, cutoff))
			break;	/* list is ordered by queue time */
		list_move_tail(&op->node, &reap_now);
	}
	spin_unlock(&kfd->orphan_lock);

	list_for_each_entry_safe(op, tmp, &reap_now, node) {
		struct amdgpu_bo *bo = op->bo;
		int r;

		dev_warn(adev->dev,
			"amdgpu V15.5 #3 reaping orphan bo=%p size=%lluMB aged %ums pid=%d(%s) pin_count=%d\n",
			bo, op->bytes >> 20,
			jiffies_to_msecs(jiffies - op->queued_at_jiffies),
			op->owner_pid, op->owner_comm, bo->tbo.pin_count);

		r = amdgpu_bo_reserve(bo, false);
		if (r) {
			dev_err_ratelimited(adev->dev,
				"amdgpu V15.5 #3 reap reserve failed bo=%p r=%d\n",
				bo, r);
		} else {
			/* Strict drain first (#1 semantics): make sure no
			 * fence references the pages before we decrement
			 * pin_count. */
			(void)amdgpu_kfd_unpin_drain(bo,
				amdgpu_kfd_unpin_drain_ms);

			while (bo->tbo.pin_count > 0)
				amdgpu_bo_unpin(bo);

			if (bo->tbo.resource &&
			    bo->tbo.resource->mem_type == TTM_PL_VRAM) {
				atomic64_sub((s64)amdgpu_bo_size(bo),
					&adev->kfd.vram_pinned);
				if (dmabuf_pin_max_mb)
					amdgpu_kfd_rdma_quota_drop_saturated(adev, op->bytes, "orphan_reap");
			}

			amdgpu_bo_unreserve(bo);
		}

		amdgpu_bo_unref(&op->bo);
		atomic64_inc(&kfd->rdma_pin_orphans_reaped);
		list_del(&op->node);
		kfree(op);
		reaped++;
	}

	if (reaped)
		dev_info(adev->dev,
			"amdgpu V15.5 #3 reaped %d orphan(s), queued_total=%lld reaped_total=%lld\n",
			reaped,
			(long long)atomic64_read(&kfd->rdma_pin_orphans_queued),
			(long long)atomic64_read(&kfd->rdma_pin_orphans_reaped));

reschedule:
	if (kfd->reaper_started && amdgpu_pin_reaper_interval_ms > 0)
		schedule_delayed_work(&kfd->reaper_work,
			msecs_to_jiffies(amdgpu_pin_reaper_interval_ms));
}

void amdgpu_kfd_reaper_start(struct amdgpu_device *adev)
{
	struct amdgpu_kfd_dev *kfd = &adev->kfd;

	if (kfd->reaper_started)
		return;

	spin_lock_init(&kfd->orphan_lock);
	INIT_LIST_HEAD(&kfd->orphan_list);
	INIT_DELAYED_WORK(&kfd->reaper_work, amdgpu_kfd_reaper_fn);
	atomic64_set(&kfd->rdma_pin_orphans_queued, 0);
	atomic64_set(&kfd->rdma_pin_orphans_reaped, 0);
	atomic64_set(&kfd->free_wait_pinned_count, 0);
	atomic64_set(&kfd->free_wait_pinned_timeout, 0);
	atomic64_set(&kfd->unpin_drain_timeouts, 0);
	atomic64_set(&kfd->rdma_pin_watermark_peak, 0);
	atomic64_set(&kfd->rdma_pin_accounting_underflow, 0);
	atomic64_set(&kfd->rdma_pin_accounting_clamp, 0);
	atomic_set(&kfd->rdma_pin_warn_latched, 0);
	atomic_set(&kfd->rdma_pin_critical_latched, 0);
	kfd->reaper_started = true;

	if (amdgpu_pin_reaper_interval_ms > 0)
		schedule_delayed_work(&kfd->reaper_work,
			msecs_to_jiffies(amdgpu_pin_reaper_interval_ms));

	dev_info(adev->dev,
		"amdgpu V15.5 reaper started: orphan_timeout=%dms interval=%dms warn_mb=%u crit_mb=%u\n",
		amdgpu_pin_orphan_timeout_ms, amdgpu_pin_reaper_interval_ms,
		amdgpu_dmabuf_pin_warn_mb, amdgpu_dmabuf_pin_critical_mb);
}

void amdgpu_kfd_reaper_stop(struct amdgpu_device *adev)
{
	struct amdgpu_kfd_dev *kfd = &adev->kfd;
	struct kfd_orphan_pin *op, *tmp;
	LIST_HEAD(drain);

	if (!kfd->reaper_started)
		return;

	kfd->reaper_started = false;
	cancel_delayed_work_sync(&kfd->reaper_work);

	/* Drain anything still on the orphan list with zero timeout. */
	spin_lock(&kfd->orphan_lock);
	list_splice_init(&kfd->orphan_list, &drain);
	spin_unlock(&kfd->orphan_lock);

	list_for_each_entry_safe(op, tmp, &drain, node) {
		struct amdgpu_bo *bo = op->bo;
		int r = amdgpu_bo_reserve(bo, false);
		if (!r) {
			while (bo->tbo.pin_count > 0)
				amdgpu_bo_unpin(bo);
			amdgpu_bo_unreserve(bo);
		}
		amdgpu_bo_unref(&op->bo);
		list_del(&op->node);
		kfree(op);
	}

	dev_info(adev->dev,
		"amdgpu V15.5 reaper stopped: queued_total=%lld reaped_total=%lld unpin_drain_timeouts=%lld free_wait_count=%lld free_wait_timeout=%lld peak_pinned=%lluMB accounting_underflow=%lld accounting_clamp=%lld\n",
		(long long)atomic64_read(&kfd->rdma_pin_orphans_queued),
		(long long)atomic64_read(&kfd->rdma_pin_orphans_reaped),
		(long long)atomic64_read(&kfd->unpin_drain_timeouts),
		(long long)atomic64_read(&kfd->free_wait_pinned_count),
		(long long)atomic64_read(&kfd->free_wait_pinned_timeout),
		(u64)atomic64_read(&kfd->rdma_pin_watermark_peak) >> 20,
		(long long)atomic64_read(&kfd->rdma_pin_accounting_underflow),
		(long long)atomic64_read(&kfd->rdma_pin_accounting_clamp));
}
