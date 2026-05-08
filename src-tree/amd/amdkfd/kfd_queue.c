// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2014-2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/mmap_lock.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <kcl/backport/kcl_mm_backport.h>
#include "kfd_priv.h"
#include "kfd_topology.h"
#include "kfd_svm.h"

/*
 * V17.5 Phase C modparams.
 *
 * kfd_pin_queue_svm_pages:
 *   When 1 (default), kfd_queue_buffer_svm_get() pins the user-space CWSR
 *   pages with FOLL_LONGTERM so kernel direct reclaim cannot trigger
 *   MMU_NOTIFY_UNMAP and consequently quiesce the compute queue.
 *
 * kfd_pin_queue_svm_max_mb:
 *   Per-process aggregate cap. Once reached, additional queues are created
 *   but their CWSR ranges remain unpinned (legacy behavior). Prevents a
 *   misbehaving process from pinning unbounded host RAM.
 */
int kfd_pin_queue_svm_pages = 1;
module_param(kfd_pin_queue_svm_pages, int, 0644);
MODULE_PARM_DESC(kfd_pin_queue_svm_pages,
		 "Pin user CWSR SVM pages to prevent reclaim-driven queue eviction (default: 1)");

unsigned long kfd_pin_queue_svm_max_mb = 256;
module_param(kfd_pin_queue_svm_max_mb, ulong, 0644);
MODULE_PARM_DESC(kfd_pin_queue_svm_max_mb,
		 "Per-process cap on pinned CWSR pages, in MB (default: 256)");

/*
 * V17.5 Item 2 (cwsr-resilient): VMA-level CWSR protection.
 *
 * When 1 (default), kfd_queue_buffer_svm_get() sets VM_LOCKED|VM_DONTCOPY on
 * the VMAs backing the CWSR / queue-vital SVM ranges, preventing kernel
 * direct reclaim from picking those pages. This is the safe replacement for
 * Phase C (kfd_pin_queue_svm_pages) which used pin_user_pages_remote with
 * FOLL_LONGTERM and triggered svm_migrate_to_ram D-state on
 * GPU_ALWAYS_MAPPED ranges. VM_LOCKED uses the same kernel primitive as
 * userspace mlock(2) and never triggers the SVM migration path.
 *
 * Coexistence: when kfd_cwsr_in_vram=1 (Item 1), the CWSR range is owned by
 * a VRAM BO and there are no user pages to lock — Item 2 path is a no-op
 * for that range.
 *
 * Set to 0 to disable (legacy reclaim-driven eviction reappears, mitigated
 * only by Phase C2 defer-on-unmap).
 */
int kfd_protect_cwsr_vma = 1;
module_param(kfd_protect_cwsr_vma, int, 0644);
MODULE_PARM_DESC(kfd_protect_cwsr_vma,
		 "Set VM_LOCKED on CWSR/queue-vital VMAs to prevent reclaim (default: 1)");

/*
 * V17.5 Item 1 (cwsr-resilient): driver-allocated VRAM CWSR.
 *
 * Master toggle for the root-cause fix of the "Freeing queue vital
 * buffer 0x7f...." dmesg path. When 1, KFD honors the
 * KFD_IOC_QUEUE_FLAGS_USE_DRIVER_CWSR input flag from CREATE_QUEUE
 * by allocating the CWSR area from VRAM via amdgpu_amdkfd_alloc_gtt_mem
 * + AMDGPU_GEM_DOMAIN_VRAM and exposing a CPU-readable user VA via
 * dma_buf+vm_mmap. VRAM is not cgroup-accounted so kernel direct
 * reclaim cannot evict the CWSR pages and queue eviction from cgroup
 * pressure becomes structurally impossible.
 *
 * Default 0 (opt-in). Userspace also has to opt in via the ioctl
 * flag — modparam alone is insufficient — so a fleet-wide flip of
 * this value to 1 has no effect until libhsakmt also runs the new
 * code path (HSA_CWSR_IN_VRAM=auto/1 + new thunk).
 *
 * Coexistence with Item 2 (kfd_protect_cwsr_vma): when Item 1 grants
 * the request, kfd_queue_buffer_svm_get is bypassed for the CWSR
 * range (Item 1 d/5), so Item 2's vma-lock branch is naturally a
 * no-op for that range. Both can be enabled simultaneously for
 * mixed workloads.
 */
int kfd_cwsr_in_vram;	/* default 0 (off); module_param below */
module_param(kfd_cwsr_in_vram, int, 0644);
MODULE_PARM_DESC(kfd_cwsr_in_vram,
		 "Allocate CWSR buffer from VRAM (driver-managed) when userspace requests it (default: 0)");

void print_queue_properties(struct queue_properties *q)
{
	if (!q)
		return;

	pr_debug("Printing queue properties:\n");
	pr_debug("Queue Type: %u\n", q->type);
	pr_debug("Queue Size: %llu\n", q->queue_size);
	pr_debug("Queue percent: %u\n", q->queue_percent);
	pr_debug("Queue Address: 0x%llX\n", q->queue_address);
	pr_debug("Queue Id: %u\n", q->queue_id);
	pr_debug("Queue Process Vmid: %u\n", q->vmid);
	pr_debug("Queue Read Pointer: 0x%px\n", q->read_ptr);
	pr_debug("Queue Write Pointer: 0x%px\n", q->write_ptr);
	pr_debug("Queue Doorbell Pointer: 0x%p\n", q->doorbell_ptr);
	pr_debug("Queue Doorbell Offset: %u\n", q->doorbell_off);
}

void print_queue(struct queue *q)
{
	if (!q)
		return;
	pr_debug("Printing queue:\n");
	pr_debug("Queue Type: %u\n", q->properties.type);
	pr_debug("Queue Size: %llu\n", q->properties.queue_size);
	pr_debug("Queue percent: %u\n", q->properties.queue_percent);
	pr_debug("Queue Address: 0x%llX\n", q->properties.queue_address);
	pr_debug("Queue Id: %u\n", q->properties.queue_id);
	pr_debug("Queue Process Vmid: %u\n", q->properties.vmid);
	pr_debug("Queue Read Pointer: 0x%px\n", q->properties.read_ptr);
	pr_debug("Queue Write Pointer: 0x%px\n", q->properties.write_ptr);
	pr_debug("Queue Doorbell Pointer: 0x%p\n", q->properties.doorbell_ptr);
	pr_debug("Queue Doorbell Offset: %u\n", q->properties.doorbell_off);
	pr_debug("Queue MQD Address: 0x%p\n", q->mqd);
	pr_debug("Queue MQD Gart: 0x%llX\n", q->gart_mqd_addr);
	pr_debug("Queue Process Address: 0x%p\n", q->process);
	pr_debug("Queue Device Address: 0x%p\n", q->device);
}

int init_queue(struct queue **q, const struct queue_properties *properties)
{
	struct queue *tmp_q;

	tmp_q = kzalloc(sizeof(*tmp_q), GFP_KERNEL);
	if (!tmp_q)
		return -ENOMEM;

	memcpy(&tmp_q->properties, properties, sizeof(*properties));

	*q = tmp_q;
	return 0;
}

void uninit_queue(struct queue *q)
{
	kfree(q);
}

#if IS_ENABLED(CONFIG_HSA_AMD_SVM_AMDKCL)

/*
 * V17.5 Phase C: defensive cap on a single prange pin to keep kvmalloc
 * within sane bounds and avoid pinning oversized SVM ranges by accident.
 * CWSR per queue per XCC is at most a few hundred KB; 16MB is well above
 * the largest plausible total per prange.
 */
#define KFD_PIN_PRANGE_MAX_PAGES   (16ULL << (20 - PAGE_SHIFT))

/*
 * Pin the user-space pages backing the given SVM prange so kernel reclaim
 * skips them. On success the prange owns the pin until queue_refcount hits
 * zero and kfd_queue_unpin_svm_prange() releases it.
 *
 * Caller must hold p->svms.lock so the prange cannot be split or freed
 * concurrently. Pin failure is non-fatal — the caller logs and continues
 * with legacy behavior; queue creation must never be blocked by Phase C.
 */
static int kfd_queue_pin_svm_prange(struct kfd_process *p,
				    struct svm_range *prange)
{
	unsigned long uaddr = prange->start << PAGE_SHIFT;
	unsigned long npages = prange->last - prange->start + 1;
	unsigned long want_bytes = npages << PAGE_SHIFT;
	unsigned long cur_bytes, max_bytes;
	struct mm_struct *mm;
	struct page **pages;
	long pinned;
	int ret = 0;

	if (npages == 0 || npages > KFD_PIN_PRANGE_MAX_PAGES) {
		pr_warn_ratelimited("kfd: skip pin: prange size out of range (%lu pages)\n",
				    npages);
		return -E2BIG;
	}

	cur_bytes = atomic_long_read(&p->pinned_svm_bytes);
	max_bytes = kfd_pin_queue_svm_max_mb << 20;
	if (cur_bytes + want_bytes > max_bytes) {
		pr_warn_ratelimited("kfd: pin SVM cap reached (cur=%luKB want=%luKB max=%luKB)\n",
				    cur_bytes >> 10, want_bytes >> 10,
				    max_bytes >> 10);
		return -ENOMEM;
	}

	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	mm = get_task_mm(p->lead_thread);
	if (!mm) {
		ret = -ESRCH;
		goto err_free;
	}

	mmap_read_lock(mm);
	pinned = kcl_pin_user_pages_remote(mm, uaddr, npages,
					   FOLL_WRITE | FOLL_LONGTERM,
					   pages);
	mmap_read_unlock(mm);
	mmput(mm);

	if (pinned < 0) {
		ret = (int)pinned;
		goto err_free;
	}
	if (pinned != (long)npages) {
		pr_warn_ratelimited("kfd: partial pin %ld/%lu for prange [0x%lx-0x%lx]\n",
				    pinned, npages, prange->start, prange->last);
		kcl_unpin_user_pages(pages, pinned);
		ret = -EAGAIN;
		goto err_free;
	}

	prange->pinned_pages = pages;
	prange->pinned_npages = npages;
	prange->gup_pinned = true;
	atomic_long_add(want_bytes, &p->pinned_svm_bytes);
	atomic_inc(&p->pinned_svm_ranges);
	return 0;

err_free:
	kvfree(pages);
	return ret;
}

/*
 * Release the pin established by kfd_queue_pin_svm_prange(). Caller must
 * hold p->svms.lock and have observed prange->queue_refcount == 0.
 *
 * Exposed (non-static) so svm_range_free() can use it as a bottom guard
 * to recover from a leaked pin (which should never happen, but if it
 * does we must not leave pages pinned forever).
 */
void kfd_queue_unpin_svm_prange(struct kfd_process *p,
				struct svm_range *prange)
{
	if (!prange->gup_pinned)
		return;

	kcl_unpin_user_pages(prange->pinned_pages, prange->pinned_npages);
	atomic_long_sub(prange->pinned_npages << PAGE_SHIFT,
			&p->pinned_svm_bytes);
	atomic_dec(&p->pinned_svm_ranges);

	kvfree(prange->pinned_pages);
	prange->pinned_pages = NULL;
	prange->pinned_npages = 0;
	prange->gup_pinned = false;
}

/*
 * V17.5 Item 2 (cwsr-resilient): VMA-level CWSR protection.
 *
 * Walks every VMA in [uaddr, uaddr+size) and sets VM_LOCKED|VM_DONTCOPY
 * on it. Pages backed by VM_LOCKED VMAs are skipped by the kernel page
 * reclaim path (try_to_unmap / shrink_*_list), so MMU_NOTIFY_UNMAP from
 * direct reclaim can no longer fire for these ranges. VM_DONTCOPY also
 * keeps fork() COW-induced notifier callbacks from racing the parent.
 *
 * Unlike the disabled Phase C path (pin_user_pages_remote(FOLL_LONGTERM)),
 * VM_LOCKED never triggers svm_migrate_to_ram on GPU_ALWAYS_MAPPED ranges
 * — it is the same primitive userspace mlock(2) uses, so the kernel mm
 * core handles it without any AMD-specific migration logic.
 *
 * We deliberately do NOT update mm->locked_vm: the /proc/<pid>/smaps
 * VmLck field is derived from VMA flags, so it stays accurate, but we
 * avoid the RLIMIT_MEMLOCK accounting that would otherwise fail when a
 * container's RLIMIT_MEMLOCK is small.
 *
 * Caller must hold p->svms.lock and have prange->queue_refcount > 0.
 * Failure is non-fatal: queue creation must NEVER be blocked by this
 * optimization.
 */
static int kfd_queue_lock_vma_for_prange(struct kfd_process *p,
					 struct svm_range *prange)
{
	unsigned long uaddr = prange->start << PAGE_SHIFT;
	unsigned long size = (prange->last - prange->start + 1) << PAGE_SHIFT;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long cur = uaddr, end = uaddr + size;
	bool any_locked = false;
	int ret = 0;

	if (!size)
		return -EINVAL;

	mm = get_task_mm(p->lead_thread);
	if (!mm)
		return -ESRCH;

	mmap_write_lock(mm);
	vma = find_vma(mm, cur);
	while (vma && vma->vm_start < end) {
		/*
		 * Skip already-locked VMAs (e.g. user explicitly mlock()ed,
		 * or a sibling prange of the same allocation locked it
		 * earlier in this loop).
		 */
		if (!(vma->vm_flags & VM_LOCKED)) {
			vm_flags_set(vma, VM_LOCKED | VM_DONTCOPY);
			any_locked = true;
		}
		if (vma->vm_end >= end)
			break;
		cur = vma->vm_end;
		vma = find_vma(mm, cur);
	}
	mmap_write_unlock(mm);

	/*
	 * Materialize the lock. mm_populate faults pages in and pins them
	 * onto the unevictable LRU. Done outside mmap_write_lock because
	 * mm_populate takes mmap_read_lock internally.
	 *
	 * Errors are advisory only — VM_LOCKED is set, so reclaim still
	 * skips these pages on demand even if they're not all populated.
	 */
	if (any_locked)
		mm_populate(uaddr, size);
	else
		ret = -ENOENT;

	mmput(mm);

	if (any_locked) {
		prange->vma_locked = true;
		atomic_long_add(size, &p->pinned_svm_bytes);
		atomic_inc(&p->pinned_svm_ranges);
	}
	return ret;
}

/*
 * Mirror of kfd_queue_lock_vma_for_prange: clear VM_LOCKED|VM_DONTCOPY on
 * every VMA we touched. Idempotent: safe to call from the bottom guard
 * even when the user VMA has already been munmapped (find_vma simply
 * returns NULL for that range and we no-op).
 */
void kfd_queue_unlock_vma_for_prange(struct kfd_process *p,
				     struct svm_range *prange)
{
	unsigned long uaddr, size, cur, end;
	struct mm_struct *mm;
	struct vm_area_struct *vma;

	if (!prange->vma_locked)
		return;

	uaddr = prange->start << PAGE_SHIFT;
	size = (prange->last - prange->start + 1) << PAGE_SHIFT;
	cur = uaddr;
	end = uaddr + size;

	mm = get_task_mm(p->lead_thread);
	if (!mm) {
		/*
		 * Process is exiting; the mm has already been torn down
		 * along with all its VMAs. Just clear our bookkeeping.
		 */
		goto clear_state;
	}

	mmap_write_lock(mm);
	vma = find_vma(mm, cur);
	while (vma && vma->vm_start < end) {
		if (vma->vm_flags & VM_LOCKED)
			vm_flags_clear(vma, VM_LOCKED | VM_DONTCOPY);
		if (vma->vm_end >= end)
			break;
		cur = vma->vm_end;
		vma = find_vma(mm, cur);
	}
	mmap_write_unlock(mm);
	mmput(mm);

clear_state:
	atomic_long_sub(size, &p->pinned_svm_bytes);
	atomic_dec(&p->pinned_svm_ranges);
	prange->vma_locked = false;
}

static int kfd_queue_buffer_svm_get(struct kfd_process_device *pdd, u64 addr, u64 size)
{
	struct kfd_process *p = pdd->process;
	struct list_head update_list;
	struct svm_range *prange;
	int ret = -EINVAL;

	INIT_LIST_HEAD(&update_list);
	addr >>= PAGE_SHIFT;
	size >>= PAGE_SHIFT;

	mutex_lock(&p->svms.lock);

	/*
	 * range may split to multiple svm pranges aligned to granularity boundaery.
	 */
	while (size) {
		uint32_t gpuid, gpuidx;
		int r;

		prange = svm_range_from_addr(&p->svms, addr, NULL);
		if (!prange)
			break;

		if (!prange->mapped_to_gpu)
			break;

		r = kfd_process_gpuid_from_node(p, pdd->dev, &gpuid, &gpuidx);
		if (r < 0)
			break;
		if (!test_bit(gpuidx, prange->bitmap_access) &&
		    !test_bit(gpuidx, prange->bitmap_aip))
			break;

		if (!(prange->flags & KFD_IOCTL_SVM_FLAG_GPU_ALWAYS_MAPPED))
			break;

		list_add(&prange->update_list, &update_list);

		if (prange->last - prange->start + 1 >= size) {
			size = 0;
			break;
		}

		size -= prange->last - prange->start + 1;
		addr += prange->last - prange->start + 1;
	}
	if (size) {
		pr_debug("[0x%llx 0x%llx] not registered\n", addr, addr + size - 1);
		goto out_unlock;
	}

	list_for_each_entry(prange, &update_list, update_list) {
		atomic_inc(&prange->queue_refcount);

		/*
		 * V17.5 Phase C: pin user CWSR pages so kernel reclaim
		 * cannot trigger MMU_NOTIFY_UNMAP -> queue eviction.
		 *
		 * Pin failure is non-fatal: queue creation must not depend
		 * on this optimization. We log once at WARN level then fall
		 * back to legacy behavior (range may still be evicted under
		 * memory pressure, but service-survival ENV will keep the
		 * user-space wait bounded).
		 */
		if (kfd_pin_queue_svm_pages && !prange->gup_pinned) {
			int pin_ret = kfd_queue_pin_svm_prange(p, prange);

			if (pin_ret)
				pr_warn_ratelimited("kfd: phase-C pin failed prange %p ret=%d (legacy fallback)\n",
						    prange, pin_ret);
		}

		/*
		 * V17.5 Item 2 (cwsr-resilient): VMA-level CWSR protection.
		 *
		 * Set VM_LOCKED|VM_DONTCOPY on the user VMAs backing this
		 * prange. This is the safe replacement for Phase C — it
		 * uses the same mechanism as userspace mlock(2) and never
		 * triggers svm_migrate_to_ram on GPU_ALWAYS_MAPPED ranges.
		 *
		 * Skipped if Phase C already took the pin (gup_pinned) so
		 * we don't double-bookkeep pinned_svm_bytes. When Item 1
		 * (kfd_cwsr_in_vram) lands and the CWSR range is owned by
		 * a driver-allocated VRAM BO, kfd_queue_buffer_svm_get
		 * will not be invoked for that range at all (Item 1 d/5),
		 * so this branch never runs in that mode.
		 *
		 * Failure is non-fatal — queue creation must not be blocked
		 * by this optimization.
		 */
		if (kfd_protect_cwsr_vma && !prange->gup_pinned &&
		    !prange->vma_locked) {
			int lock_ret = kfd_queue_lock_vma_for_prange(p,
								     prange);

			if (lock_ret)
				pr_warn_ratelimited("kfd: item-2 vma-lock failed prange %p ret=%d (legacy fallback)\n",
						    prange, lock_ret);
		}
	}
	ret = 0;

out_unlock:
	mutex_unlock(&p->svms.lock);
	return ret;
}

static void kfd_queue_buffer_svm_put(struct kfd_process_device *pdd, u64 addr, u64 size)
{
	struct kfd_process *p = pdd->process;
	struct svm_range *prange, *pchild;
	struct interval_tree_node *node;
	unsigned long last;

	addr >>= PAGE_SHIFT;
	last = addr + (size >> PAGE_SHIFT) - 1;

	mutex_lock(&p->svms.lock);

	node = interval_tree_iter_first(&p->svms.objects, addr, last);
	while (node) {
		struct interval_tree_node *next_node;
		unsigned long next_start;

		prange = container_of(node, struct svm_range, it_node);
		next_node = interval_tree_iter_next(node, addr, last);
		next_start = min(node->last, last) + 1;

		if (atomic_add_unless(&prange->queue_refcount, -1, 0)) {
			list_for_each_entry(pchild, &prange->child_list, child_list)
				atomic_add_unless(&pchild->queue_refcount, -1, 0);

			/*
			 * V17.5 Phase C: drop the pin only when the last
			 * queue using this prange goes away. Multiple
			 * concurrent queues sharing a prange share a single
			 * pin; the gup_pinned flag prevents double-pin in
			 * kfd_queue_buffer_svm_get().
			 */
			if (atomic_read(&prange->queue_refcount) == 0 &&
			    prange->gup_pinned)
				kfd_queue_unpin_svm_prange(p, prange);

			/*
			 * V17.5 Item 2 (cwsr-resilient): mirror — drop the
			 * VMA-level lock only when the last queue using this
			 * prange goes away. Independent from Phase C: a prange
			 * can be in either mode (gup_pinned XOR vma_locked)
			 * but never both at once (the get() path enforces
			 * the precedence).
			 */
			if (atomic_read(&prange->queue_refcount) == 0 &&
			    prange->vma_locked)
				kfd_queue_unlock_vma_for_prange(p, prange);
		}

		node = next_node;
		addr = next_start;
	}

	mutex_unlock(&p->svms.lock);
}
#else

static int kfd_queue_buffer_svm_get(struct kfd_process_device *pdd, u64 addr, u64 size)
{
	return -EINVAL;
}

static void kfd_queue_buffer_svm_put(struct kfd_process_device *pdd, u64 addr, u64 size)
{
}

#endif

/*
 * V17.5 Item 1 (cwsr-resilient): driver-allocated VRAM CWSR helpers.
 *
 * These wrap the existing KFD GPUVM allocator with the parameters
 * appropriate for context-save-restore: VRAM-domain, host-visible
 * (PUBLIC), wiped on release. The thunk supplies the GPU virtual
 * address (drawn from its own per-process aperture, same way it does
 * for any other queue resource); the driver allocates the VRAM BO,
 * reserves cgroup VRAM via the existing KFD path, and maps the BO into
 * the process GPUVM at exactly that address so HW can write CWSR
 * data without any further translation.
 *
 * The dma_buf export and user-VA mmap are layered on top in Item 1
 * e/5 (so userspace's fill_cwsr_header pass can write the header).
 *
 * Failure here is fatal at the call site: when userspace explicitly
 * requested USE_DRIVER_CWSR and the modparam permits it, we either
 * give them a working driver-CWSR queue or fail with -ENOMEM. We do
 * NOT silently fall back to legacy host CWSR — that would defeat the
 * whole point of the feature (which is to escape cgroup pressure).
 * Old userspace simply doesn't set the flag and never enters this
 * path, so they retain the legacy fallback.
 */
int kfd_alloc_cwsr_vram(struct kfd_process_device *pdd, uint64_t gpu_va,
			size_t size, struct queue_properties *q_props)
{
	struct kfd_process *p = pdd->process;
	struct kgd_mem *mem = NULL;
	uint64_t mmap_offset = 0;
	uint32_t flags = KFD_IOC_ALLOC_MEM_FLAGS_VRAM |
			 KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC |
			 KFD_IOC_ALLOC_MEM_FLAGS_NO_SUBSTITUTE |
			 KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
			 KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE;
	int idr_handle = -1;
	int ret;

	if (!kfd_cwsr_in_vram)
		return -EOPNOTSUPP;
	if (!q_props || !pdd || !pdd->dev || !pdd->drm_priv)
		return -EINVAL;
	if (!size || (size & (PAGE_SIZE - 1)) || (gpu_va & (PAGE_SIZE - 1)))
		return -EINVAL;

	if (!kfd_dev_is_large_bar(pdd->dev)) {
		pr_warn_ratelimited("kfd: item-1 cwsr-vram requires large-bar; declining\n");
		return -EOPNOTSUPP;
	}

	ret = amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu(
			pdd->dev->adev, gpu_va, size,
			pdd->drm_priv, &mem, &mmap_offset, flags, false);
	if (ret) {
		pr_warn_ratelimited("kfd: item-1 cwsr-vram alloc failed gpu_va=0x%llx size=0x%zx ret=%d\n",
				    gpu_va, size, ret);
		return ret;
	}

	idr_handle = kfd_process_device_create_obj_handle(pdd, mem,
				gpu_va, size, 0,
				KFD_IOC_ALLOC_MEM_FLAGS_VRAM, -1);
	if (idr_handle < 0) {
		ret = -EFAULT;
		goto err_free_mem;
	}

	ret = amdgpu_amdkfd_gpuvm_map_memory_to_gpu(pdd->dev->adev, mem,
						    pdd->drm_priv);
	if (ret) {
		pr_warn_ratelimited("kfd: item-1 cwsr-vram map_to_gpu failed gpu_va=0x%llx ret=%d\n",
				    gpu_va, ret);
		goto err_free_handle;
	}

	/*
	 * V17.5 Item 1 e/5: export a dma_buf and mmap it into the user mm
	 * at the same VA we just mapped into GPUVM. Because thunk picks
	 * the VA from the SVM aperture (where user-VA == GPU-VA), one
	 * value of ctx_save_restore_address serves both:
	 *   - HW save/restore (programmed via MQD; accesses via GPUVM)
	 *   - thunk's fill_cwsr_header() (CPU writes via the BAR-backed
	 *     user mapping)
	 *
	 * MAP_FIXED_NOREPLACE returns -EEXIST if the VA is busy — never
	 * silently clobber an existing user mapping. Failure fails the
	 * whole alloc; we tear down the GPUVM mapping and return error.
	 *
	 * No dma_buf_{begin,end}_cpu_access dance is needed because the
	 * BO is allocated with KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC -> the
	 * VRAM BAR aperture is directly CPU-accessible via uncached
	 * memory mapping. Reads/writes are PCIe-coherent without an
	 * explicit invalidate/flush step.
	 */
	{
		struct dma_buf *dmabuf = NULL;
		unsigned long user_va;
		int dret;

		dret = amdgpu_amdkfd_gpuvm_export_dmabuf(mem, &dmabuf);
		if (dret || !dmabuf) {
			pr_warn_ratelimited("kfd: item-1 dmabuf export failed ret=%d\n",
					    dret);
			ret = dret ? dret : -ENOMEM;
			goto err_unmap;
		}

		user_va = vm_mmap(dmabuf->file, gpu_va, size,
				  PROT_READ | PROT_WRITE,
				  MAP_SHARED | MAP_FIXED_NOREPLACE, 0);

		if (IS_ERR_VALUE(user_va) || user_va != gpu_va) {
			pr_warn_ratelimited("kfd: item-1 vm_mmap@0x%llx size=0x%zx returned 0x%lx\n",
					    gpu_va, size, user_va);
			dma_buf_put(dmabuf);
			ret = IS_ERR_VALUE(user_va) ? (int)user_va : -EBUSY;
			goto err_unmap;
		}

		q_props->cwsr_drv_dmabuf = dmabuf;	/* refcount kept for lifetime */
		q_props->cwsr_drv_user_va = user_va;
	}

	atomic64_add(PAGE_ALIGN(size), &pdd->vram_usage);

	q_props->cwsr_drv_mem = mem;
	q_props->cwsr_drv_idr_handle = idr_handle;
	q_props->cwsr_drv_mmap_offset = mmap_offset;
	q_props->cwsr_drv_owned = true;
	q_props->ctx_save_restore_area_address = gpu_va;
	q_props->ctx_save_restore_area_size = size;

	pr_debug("kfd: item-1 cwsr-vram allocated gpu_va=0x%llx size=0x%zx mmap_off=0x%llx\n",
		 gpu_va, size, mmap_offset);
	return 0;

err_unmap:
	(void)amdgpu_amdkfd_gpuvm_unmap_memory_from_gpu(pdd->dev->adev, mem,
							pdd->drm_priv);
err_free_handle:
	kfd_process_device_remove_obj_handle(pdd, idr_handle);
err_free_mem:
	amdgpu_amdkfd_gpuvm_free_memory_of_gpu(pdd->dev->adev, mem,
					       pdd->drm_priv, NULL);
	(void)p;	/* reserved for future use (counters / tracing) */
	return ret;
}

/*
 * Mirror of kfd_alloc_cwsr_vram. Releases the VRAM BO, removes it from
 * the process kgd_mem table, and zeros the queue_properties bookkeeping.
 *
 * Idempotent: safe to call when q_props->cwsr_drv_owned is false (no-op).
 * Also safe to call when amdgpu_amdkfd_gpuvm_unmap_memory_from_gpu would
 * fail (we follow the existing error-tolerant teardown convention used
 * by the regular alloc-memory ioctl release path).
 */
void kfd_free_cwsr_vram(struct kfd_process_device *pdd,
			struct queue_properties *q_props)
{
	struct kgd_mem *mem;
	uint64_t size = 0;

	if (!q_props || !q_props->cwsr_drv_owned)
		return;

	/*
	 * V17.5 Item 1 e/5: tear down the user mmap first. We attempt
	 * vm_munmap from the calling task's mm, which is correct on the
	 * normal destroy_queue ioctl path (current is a thread in the
	 * owning process). On the process-exit path (pqm_uninit ->
	 * release_buffers), current->mm has already been replaced with
	 * the dying process' final mm or with init_mm — vm_munmap on
	 * init_mm is a no-op. Either way the dma_buf_put below releases
	 * the file ref and the mmap is torn down by the kernel during
	 * exit_mmap. So vm_munmap is best-effort.
	 */
	if (q_props->cwsr_drv_user_va && current && current->mm) {
		uint64_t munmap_size =
			PAGE_ALIGN(q_props->ctx_save_restore_area_size);

		if (munmap_size)
			(void)vm_munmap(q_props->cwsr_drv_user_va,
					munmap_size);
	}

	mem = q_props->cwsr_drv_mem;
	if (!mem || !pdd || !pdd->dev)
		goto clear_props;

	(void)amdgpu_amdkfd_gpuvm_unmap_memory_from_gpu(pdd->dev->adev, mem,
							pdd->drm_priv);

	/*
	 * Free returns the BO size in 'size' so we can keep the per-pdd
	 * vram_usage counter honest. Mirrors the existing free path in
	 * kfd_chardev.c::kfd_ioctl_free_memory_of_gpu.
	 */
	(void)amdgpu_amdkfd_gpuvm_free_memory_of_gpu(pdd->dev->adev, mem,
						     pdd->drm_priv, &size);

	if (q_props->cwsr_drv_idr_handle >= 0)
		kfd_process_device_remove_obj_handle(pdd,
				q_props->cwsr_drv_idr_handle);

	if (size)
		atomic64_sub(PAGE_ALIGN(size), &pdd->vram_usage);

clear_props:
	if (q_props->cwsr_drv_dmabuf) {
		dma_buf_put(q_props->cwsr_drv_dmabuf);
		q_props->cwsr_drv_dmabuf = NULL;
	}
	q_props->cwsr_drv_mem = NULL;
	q_props->cwsr_drv_idr_handle = -1;
	q_props->cwsr_drv_user_va = 0;
	q_props->cwsr_drv_mmap_offset = 0;
	q_props->cwsr_drv_owned = false;
}

int kfd_queue_buffer_get(struct amdgpu_vm *vm, void __user *addr, struct amdgpu_bo **pbo,
			 u64 expected_size)
{
	struct amdgpu_bo_va_mapping *mapping;
	u64 user_addr;
	u64 size;

	user_addr = (u64)addr >> AMDGPU_GPU_PAGE_SHIFT;
	size = expected_size >> AMDGPU_GPU_PAGE_SHIFT;

	mapping = amdgpu_vm_bo_lookup_mapping(vm, user_addr);
	if (!mapping)
		goto out_err;

	if (user_addr != mapping->start ||
	    (size != 0 && user_addr + size - 1 != mapping->last)) {
		pr_debug("expected size 0x%llx not equal to mapping addr 0x%llx size 0x%llx\n",
			expected_size, mapping->start << AMDGPU_GPU_PAGE_SHIFT,
			(mapping->last - mapping->start + 1) << AMDGPU_GPU_PAGE_SHIFT);
		goto out_err;
	}

	*pbo = amdgpu_bo_ref(mapping->bo_va->base.bo);
	mapping->bo_va->queue_refcount++;
	return 0;

out_err:
	*pbo = NULL;
	return -EINVAL;
}

/* FIXME: remove this function, just call amdgpu_bo_unref directly */
void kfd_queue_buffer_put(struct amdgpu_bo **bo)
{
	amdgpu_bo_unref(bo);
}

int kfd_queue_acquire_buffers(struct kfd_process_device *pdd, struct queue_properties *properties)
{
	struct kfd_topology_device *topo_dev;
	u64 expected_queue_size;
	struct amdgpu_vm *vm;
	u32 total_cwsr_size;
	int err;

	topo_dev = kfd_topology_device_by_id(pdd->dev->id);
	if (!topo_dev)
		return -EINVAL;

	/* AQL queues on GFX7 and GFX8 appear twice their actual size */
	if (properties->type == KFD_QUEUE_TYPE_COMPUTE &&
	    properties->format == KFD_QUEUE_FORMAT_AQL &&
	    topo_dev->node_props.gfx_target_version >= 70000 &&
	    topo_dev->node_props.gfx_target_version < 90000)
		expected_queue_size = properties->queue_size / 2;
	else
		expected_queue_size = properties->queue_size;

	vm = drm_priv_to_vm(pdd->drm_priv);
	err = amdgpu_bo_reserve(vm->root.bo, false);
	if (err)
		return err;

	err = kfd_queue_buffer_get(vm, properties->write_ptr, &properties->wptr_bo, PAGE_SIZE);
	if (err)
		goto out_err_unreserve;

	err = kfd_queue_buffer_get(vm, properties->read_ptr, &properties->rptr_bo, PAGE_SIZE);
	if (err)
		goto out_err_unreserve;

	err = kfd_queue_buffer_get(vm, (void *)properties->queue_address,
				   &properties->ring_bo, expected_queue_size);
	if (err)
		goto out_err_unreserve;

	/* only compute queue requires EOP buffer and CWSR area */
	if (properties->type != KFD_QUEUE_TYPE_COMPUTE)
		goto out_unreserve;

	/* EOP buffer is not required for all ASICs */
	if (properties->eop_ring_buffer_address) {
		if (properties->eop_ring_buffer_size != topo_dev->node_props.eop_buffer_size) {
			pr_debug("queue eop bo size 0x%x not equal to node eop buf size 0x%x\n",
				properties->eop_ring_buffer_size,
				topo_dev->node_props.eop_buffer_size);
			err = -EINVAL;
			goto out_err_unreserve;
		}
		err = kfd_queue_buffer_get(vm, (void *)properties->eop_ring_buffer_address,
					   &properties->eop_buf_bo,
					   properties->eop_ring_buffer_size);
		if (err)
			goto out_err_unreserve;
	}

	if (properties->ctl_stack_size != topo_dev->node_props.ctl_stack_size) {
		pr_debug("queue ctl stack size 0x%x not equal to node ctl stack size 0x%x\n",
			properties->ctl_stack_size,
			topo_dev->node_props.ctl_stack_size);
		err = -EINVAL;
		goto out_err_unreserve;
	}

	if (properties->ctx_save_restore_area_size != topo_dev->node_props.cwsr_size) {
		pr_debug("queue cwsr size 0x%x not equal to node cwsr size 0x%x\n",
			properties->ctx_save_restore_area_size,
			topo_dev->node_props.cwsr_size);
		err = -EINVAL;
		goto out_err_unreserve;
	}

	total_cwsr_size = (topo_dev->node_props.cwsr_size + topo_dev->node_props.debug_memory_size)
			  * NUM_XCC(pdd->dev->xcc_mask);
	total_cwsr_size = ALIGN(total_cwsr_size, PAGE_SIZE);

	/*
	 * V17.5 Item 1 (cwsr-resilient): if the driver allocated the CWSR
	 * BO from VRAM (Item 1 c/5 path), the kgd_mem is already registered
	 * with the process VM and refcount-tracked via the per-process IDR
	 * table — no kfd_queue_buffer_get / svm_get is needed (and would
	 * fail anyway because there is no SVM range for this VA).
	 *
	 * Skip both steps and return success directly. cwsr_bo stays NULL
	 * (the buffer-put path checks before deref) and queue_refcount on
	 * any prange is left untouched (none was incremented for this VA).
	 */
	if (properties->cwsr_drv_owned) {
		pr_debug("kfd: item-1 skipping cwsr buffer_get / svm_get (driver owns 0x%llx)\n",
			 properties->ctx_save_restore_area_address);
		goto out_unreserve;
	}

	err = kfd_queue_buffer_get(vm, (void *)properties->ctx_save_restore_area_address,
				   &properties->cwsr_bo, total_cwsr_size);
	if (!err)
		goto out_unreserve;

	amdgpu_bo_unreserve(vm->root.bo);

	err = kfd_queue_buffer_svm_get(pdd, properties->ctx_save_restore_area_address,
				       total_cwsr_size);
	if (err)
		goto out_err_release;

	return 0;

out_unreserve:
	amdgpu_bo_unreserve(vm->root.bo);
	return 0;

out_err_unreserve:
	amdgpu_bo_unreserve(vm->root.bo);
out_err_release:
	/* FIXME: make a _locked version of this that can be called before
	 * dropping the VM reservation.
	 */
	kfd_queue_unref_bo_vas(pdd, properties);
	kfd_queue_release_buffers(pdd, properties);
	return err;
}

int kfd_queue_release_buffers(struct kfd_process_device *pdd, struct queue_properties *properties)
{
	struct kfd_topology_device *topo_dev;
	u32 total_cwsr_size;

	kfd_queue_buffer_put(&properties->wptr_bo);
	kfd_queue_buffer_put(&properties->rptr_bo);
	kfd_queue_buffer_put(&properties->ring_bo);
	kfd_queue_buffer_put(&properties->eop_buf_bo);
	kfd_queue_buffer_put(&properties->cwsr_bo);

	topo_dev = kfd_topology_device_by_id(pdd->dev->id);
	if (!topo_dev)
		return -EINVAL;
	total_cwsr_size = (topo_dev->node_props.cwsr_size + topo_dev->node_props.debug_memory_size)
			  * NUM_XCC(pdd->dev->xcc_mask);
	total_cwsr_size = ALIGN(total_cwsr_size, PAGE_SIZE);

	/*
	 * V17.5 Item 1 (cwsr-resilient): if this queue's CWSR is owned
	 * by the driver, free the VRAM BO + GPUVM mapping here. Skip the
	 * legacy svm_put path in that mode — no SVM range was registered
	 * for this VA (Item 1 d/5 in kfd_queue_acquire_buffers), so a
	 * svm_put would just be a wasted lookup, and on multi-XCC systems
	 * with overlapping address layouts could decrement an unrelated
	 * range's queue_refcount.
	 */
	if (properties->cwsr_drv_owned) {
		kfd_free_cwsr_vram(pdd, properties);
		return 0;
	}

	kfd_queue_buffer_svm_put(pdd, properties->ctx_save_restore_area_address, total_cwsr_size);
	return 0;
}

void kfd_queue_unref_bo_va(struct amdgpu_vm *vm, struct amdgpu_bo **bo)
{
	if (*bo) {
		struct amdgpu_bo_va *bo_va;

		bo_va = amdgpu_vm_bo_find(vm, *bo);
		if (bo_va && bo_va->queue_refcount)
			bo_va->queue_refcount--;
	}
}

int kfd_queue_unref_bo_vas(struct kfd_process_device *pdd,
			   struct queue_properties *properties)
{
	struct amdgpu_vm *vm;
	int err;

	vm = drm_priv_to_vm(pdd->drm_priv);
	err = amdgpu_bo_reserve(vm->root.bo, false);
	if (err)
		return err;

	kfd_queue_unref_bo_va(vm, &properties->wptr_bo);
	kfd_queue_unref_bo_va(vm, &properties->rptr_bo);
	kfd_queue_unref_bo_va(vm, &properties->ring_bo);
	kfd_queue_unref_bo_va(vm, &properties->eop_buf_bo);
	kfd_queue_unref_bo_va(vm, &properties->cwsr_bo);

	amdgpu_bo_unreserve(vm->root.bo);
	return 0;
}

#define SGPR_SIZE_PER_CU	0x4000
#define LDS_SIZE_PER_CU		0x10000
#define HWREG_SIZE_PER_CU	0x1000
#define DEBUGGER_BYTES_ALIGN	64
#define DEBUGGER_BYTES_PER_WAVE	32

static u32 kfd_get_vgpr_size_per_cu(u32 gfxv)
{
	u32 vgpr_size = 0x40000;

	if (gfxv == 90402 ||			/* GFX_VERSION_AQUA_VANJARAM */
	    gfxv == 90010 ||			/* GFX_VERSION_ALDEBARAN */
	    gfxv == 90008 ||			/* GFX_VERSION_ARCTURUS */
	    gfxv == 90500)
		vgpr_size = 0x80000;
	else if (gfxv == 110000 ||		/* GFX_VERSION_PLUM_BONITO */
		 gfxv == 110001 ||		/* GFX_VERSION_WHEAT_NAS */
		 gfxv == 120000 ||		/* GFX_VERSION_GFX1200 */
		 gfxv == 120001)		/* GFX_VERSION_GFX1201 */
		vgpr_size = 0x60000;

	return vgpr_size;
}

#define WG_CONTEXT_DATA_SIZE_PER_CU(gfxv, props)	\
	(kfd_get_vgpr_size_per_cu(gfxv) + SGPR_SIZE_PER_CU +\
	 (((gfxv) == 90500) ? (props->lds_size_in_kb << 10) : LDS_SIZE_PER_CU) +\
	 HWREG_SIZE_PER_CU)

#define CNTL_STACK_BYTES_PER_WAVE(gfxv)	\
	((gfxv) >= 100100 ? 12 : 8)	/* GFX_VERSION_NAVI10*/

#define SIZEOF_HSA_USER_CONTEXT_SAVE_AREA_HEADER 40

void kfd_queue_ctx_save_restore_size(struct kfd_topology_device *dev)
{
	struct kfd_node_properties *props = &dev->node_props;
	u32 gfxv = props->gfx_target_version;
	u32 ctl_stack_size;
	u32 wg_data_size;
	u32 wave_num;
	u32 cu_num;

	if (gfxv < 80001)	/* GFX_VERSION_CARRIZO */
		return;

	cu_num = props->simd_count / props->simd_per_cu / NUM_XCC(dev->gpu->xcc_mask);
	wave_num = (gfxv < 100100) ?	/* GFX_VERSION_NAVI10 */
		    min(cu_num * 40, props->array_count / props->simd_arrays_per_engine * 512)
		    : cu_num * 32;

	wg_data_size = ALIGN(cu_num * WG_CONTEXT_DATA_SIZE_PER_CU(gfxv, props), PAGE_SIZE);
	ctl_stack_size = wave_num * CNTL_STACK_BYTES_PER_WAVE(gfxv) + 8;
	ctl_stack_size = ALIGN(SIZEOF_HSA_USER_CONTEXT_SAVE_AREA_HEADER + ctl_stack_size,
			       PAGE_SIZE);

	if ((gfxv / 10000 * 10000) == 100000) {
		/* HW design limits control stack size to 0x7000.
		 * This is insufficient for theoretical PM4 cases
		 * but sufficient for AQL, limited by SPI events.
		 */
		ctl_stack_size = min(ctl_stack_size, 0x7000);
	}

	props->ctl_stack_size = ctl_stack_size;
	props->debug_memory_size = ALIGN(wave_num * DEBUGGER_BYTES_PER_WAVE, DEBUGGER_BYTES_ALIGN);
	props->cwsr_size = ctl_stack_size + wg_data_size;

	if (gfxv == 80002)	/* GFX_VERSION_TONGA */
		props->eop_buffer_size = 0x8000;
	else if (gfxv == 90402)	/* GFX_VERSION_AQUA_VANJARAM */
		props->eop_buffer_size = 4096;
	else if (gfxv >= 80000)
		props->eop_buffer_size = 4096;
}
