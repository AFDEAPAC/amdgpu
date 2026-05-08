# V17.5 cgroup-aware GPU protection — reviewer / deployment guide

> **Status**: ready for review. NOT yet validated on real GPU hardware.
> **Author**: agent A. **Reviewer**: agent B (please run validation matrix in §7).
> **Predecessor**: `v17.5-sdma-fix` (== v17.5-rc3 driver baseline).

---

## 1. Source

| | |
|---|---|
| Repo | `git@github.com:AFDEAPAC/amdgpu.git` |
| Branch | `v17.5-cgroup-aware` |
| Tip | `90b8aeb7e` (`Phase C fixup: move kfd_queue_unpin_svm_prange decl after struct kfd_process`) |
| Base | `v17.5-sdma-fix` (tip `fef6c2a9f`, FIX-K1 amdgpu_sync_wait bound) |
| Plan | `/home/chun-wan/.cursor/plans/cgroup-aware_gpu_protection_v2_corrected.plan.md` |
| DKMS srcversion (built host) | `3AAFCF7B100DAE0975C2469` |
| Built kernel (build host only) | `5.10.134-13.1.al8.x86_64` |
| Target kernel (customer/test) | `6.14.14` (KCL shim covers 5.6 → 6.5+ pin API) |

### Commit log on top of `v17.5-sdma-fix`

```
90b8aeb7e Phase C fixup: move kfd_queue_unpin_svm_prange decl after struct kfd_process
b45bf5a81 Phase A ttm: cgroup-aware GTT allocation gate (opt-in)
346421bee Phase C2 svm: defer queue eviction when CWSR VMA still mapped
0ff6de20f Phase C sysfs: pinned_svm_bytes / pinned_svm_ranges per process
72ef3d683 Phase C svm: split safety + bottom guard for pin lifecycle
9e487340b Phase C kfd_queue: pin user CWSR pages on queue acquire
cbf6a4352 Phase C kcl: pin_user_pages_remote / unpin_user_pages shim
88ac7c49e Phase C struct — svm_range pin tracking + kfd_process counters
```

### Diffstat vs `v17.5-sdma-fix`

```
src-tree/amd/amdgpu/amdgpu.h                    |   3 +
src-tree/amd/amdgpu/amdgpu_drv.c                |  18 ++-
src-tree/amd/amdgpu/amdgpu_ttm.c                |  79 +++++++++++
src-tree/amd/amdkfd/kfd_priv.h                  |  36 +++++
src-tree/amd/amdkfd/kfd_process.c               |  23 +++-
src-tree/amd/amdkfd/kfd_queue.c                 | 169 +++++++++++++++++++++++-
src-tree/amd/amdkfd/kfd_svm.c                   |  86 +++++++++++-
src-tree/amd/amdkfd/kfd_svm.h                   |  11 ++
src-tree/include/kcl/backport/kcl_mm_backport.h |  55 ++++++++
9 files changed, 472 insertions(+), 8 deletions(-)
```

---

## 2. Why this branch exists (root cause, corrected)

Customer dmesg (`/home/chun-wan/dmesg0507.log`) shows:

| Signal | Count | Interpretation |
|---|---:|---|
| `Freeing queue vital buffer 0x7f5355000000, queue evicted` | 2 | **user-space VA** anon CWSR region revoked |
| `gem_create LARGE` | 5574 | heavy BO churn, cgroup-32GB pressure |
| `survival slow WAIT_EVENTS` | 14134 | hipStreamSync stuck in timeout loop |
| `Out of memory` / `Killed process` / `oom-kill` | 0 | cgroup did **not** OOM-kill |
| `RDMA pin rejected` | 0 | rdma_pinned_bytes counter clean |
| `gpu reset` / `fence timeout` | 0 | no GPU/kernel hang |

Root cause: under cgroup-32GB pressure, the **mm subsystem reclaims the user-space CWSR pages** (anon mmap’d for ALWAYS_MAPPED queues). `mmu_notifier` fires `svm_range_unmap_from_cpu()`, which sees `queue_refcount > 0` and emits `Freeing queue vital buffer, queue evicted` then forcibly quiesces the queue. Userspace blocks in `WAIT_EVENTS` because the queue is gone.

**The original plan** (`bfb317e7.plan.md`) misdiagnosed this as kernel-side `amdgpu_amdkfd_alloc_gtt_mem()` MQD/EOP eviction. That is incorrect — the address `0x7f5355000000` is a user VA, not a TTM-managed BO. The corrected plan’s Phase C is the right surgery point.

---

## 3. What changed (per phase)

### Phase C — Pin user CWSR pages on queue acquire (default ON)

**Files**: `kfd_svm.h`, `kfd_priv.h`, `kfd_queue.c`, `kfd_svm.c`, `kfd_process.c`, `kcl_mm_backport.h`

- New per-prange state: `pinned_pages`, `pinned_npages`, `gup_pinned`.
- `kfd_queue_buffer_svm_get()` now calls `kcl_pin_user_pages_remote(FOLL_LONGTERM | FOLL_WRITE)` after `atomic_inc(queue_refcount)`. Pages are immune to `mmu_notifier` reclaim while pinned, so cgroup-32GB pressure can no longer evict the queue.
- `kfd_queue_buffer_svm_put()` calls `kcl_unpin_user_pages` + `kvfree(pages)` when `queue_refcount` drops to 0.
- Caps to keep memory bounded:
  - per-prange: `KFD_PIN_PRANGE_MAX_PAGES` = 16 MiB worth of pages (compile-time).
  - per-process: `kfd_pin_queue_svm_max_mb` modparam, default 256 MB.
- **Failure is non-fatal**: pin error → warn-rate-limited + skip pin (fall back to legacy unpinned behavior). Queue creation never fails because of this.
- Split safety: `svm_range_split_adjust` and `svm_range_split_pages` zero-init pin state for child ranges (do NOT inherit pin).
- Bottom guard: `svm_range_free` does `WARN_ON_ONCE(prange->gup_pinned)` and unpins if leaked.
- Sysfs observability per process (`/sys/class/kfd/kfd/proc/<pid>/`):
  - `pinned_svm_bytes` — total bytes pinned by Phase C
  - `pinned_svm_ranges` — number of pinned pranges

### Phase C2 — Defensive eviction gate (default ON)

**Files**: `kfd_svm.c`

- In `svm_range_unmap_from_cpu()`, before calling `kgd2kfd_quiesce_mm()`, check if the user VMA is still mapped (`find_vma`). If yes → call `svm_range_evict()` to route through the restore-worker path (quiesce + restore pair) instead of the immediate-quiesce-only path. This gives the queue a chance to come back even if Phase C didn't pin (legacy fallback path or upstream regression).
- New modparam: `kfd_defer_queue_eviction` (int, default 1).

### Phase A — cgroup-aware GTT allocation gate (default OFF, opt-in)

**Files**: `amdgpu_ttm.c`, `amdgpu_drv.c`, `amdgpu.h`

- New helper: `amdgpu_memcg_avail_bytes()` — reads `mem_cgroup_from_task` + `page_counter_read(&memcg->memory)`. cgroup v2 only; returns `ULONG_MAX` if `CONFIG_MEMCG=n` or no limit.
- In `amdgpu_ttm_tt_populate()` for `TTM_PL_TT`: if `amdgpu_gtt_cgroup_reserve_mb > 0` and `avail < need + reserve` → return `-ENOMEM` early instead of letting GTT alloc push cgroup over the wall.
- New modparam: `amdgpu_gtt_cgroup_reserve_mb` (uint, default **0** = OFF).
- This is opt-in because it changes ENOMEM semantics and customers may not want it without explicit testing.

### Phase D — kernel-infra BO no-account (NOT implemented, audited and dropped)

Audit result: `amdgpu_amdkfd_alloc_gtt_mem()` and friends do **not** pass `__GFP_ACCOUNT` by default in this tree (TTM uses `GFP_HIGHUSER`). The proposed `kernel_bo_no_account` modparam would have been a no-op. Dropping avoids dead code in driver. Re-evaluate only if upstream merges a TTM change that flips the default.

### KCL shim (kernel API compatibility)

**File**: `src-tree/include/kcl/backport/kcl_mm_backport.h`

`pin_user_pages_remote()` signature changed in 6.5: the `vmas` output parameter was removed. The shim dispatches on `HAVE_GET_USER_PAGES_REMOTE_REMOVE_VMAS` and exposes `kcl_pin_user_pages_remote` / `kcl_unpin_user_pages` so kfd_queue.c stays version-agnostic. For kernels < 5.6 (no FOLL_LONGTERM pin path), it gracefully returns `-ENOSYS` and Phase C falls back to legacy.

---

## 4. Modparam reference (post-cgroup-aware)

| Param | Default | Range | Meaning |
|---|---:|---|---|
| `kfd_pin_queue_svm_pages` | 1 | 0/1 | Phase C — pin user CWSR pages on queue acquire |
| `kfd_pin_queue_svm_max_mb` | 256 | 0..ULONG_MAX | Phase C — per-process aggregate pin cap (MB) |
| `kfd_defer_queue_eviction` | 1 | 0/1 | Phase C2 — defer queue eviction if VMA still mapped |
| `amdgpu_gtt_cgroup_reserve_mb` | 0 | 0..UINT_MAX | Phase A — opt-in cgroup-aware GTT reserve (MB) |

All v17.5-sdma-fix (rc3) modparams remain unchanged. See §5 for full gold modprobe line.

---

## 5. Gold deployment recipe (mi300x customer-equivalent)

### Driver `/etc/modprobe.d/amdgpu.conf`

```
options amdgpu kfd_pin_queue_svm_pages=1 kfd_pin_queue_svm_max_mb=256 kfd_defer_queue_eviction=1 \
               kfd_wait_max_ms_per_wall=5000 kfd_survival_slow_ioctl_ms=4000 \
               kfd_free_wait_ms=4000 kfd_unpin_drain_ms=3000 kfd_free_on_pinned=1 \
               pin_orphan_timeout_ms=10000 pin_reaper_interval_ms=2000 \
               gtt_lock_timeout_ms=4000 gtt_multi_window=32 \
               rdma_pin_debug=1 sdma_fence_watchdog_ms=30000
options amdkcl suballoc_timeout_ms=4000
```

> Phase A (`amdgpu_gtt_cgroup_reserve_mb=4096`) is **NOT** in the gold line — opt-in only after dedicated cgroup-stress validation per §7.

### Runtime ENV (unchanged from v17.5-rc3)

```
export ROCR_SERVICE_SURVIVAL=1
export HIP_SERVICE_SURVIVAL=1
export ROCR_SIGNAL_WAIT_MAX_MS=2000
export ROCR_SDMA_WRITE_ADDR_FAIL_MS=500
export HIP_FREE_SYNC_FAIL_MS=2000
export HIP_AWAIT_FAIL_MS=2000
export HIP_FREE_REJECT_ON_ACTIVE=1
```

---

## 6. Build / install (DKMS, kernel 6.14.14 customer profile)

```bash
# 1) Pull the branch
cd /usr/local/src && rm -rf amdgpu-kernel
git clone -b v17.5-cgroup-aware git@github.com:AFDEAPAC/amdgpu.git amdgpu-kernel
cd amdgpu-kernel

# 2) Stage as DKMS source
sudo cp -r src-tree /usr/src/amdgpu-6.14.14-2212064.el8.v17_5_cgroup_aware
sudo sed -i 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="6.14.14-2212064.el8.v17_5_cgroup_aware"/' \
  /usr/src/amdgpu-6.14.14-2212064.el8.v17_5_cgroup_aware/dkms.conf

# 3) Add / build / install
sudo dkms add     -m amdgpu -v 6.14.14-2212064.el8.v17_5_cgroup_aware
sudo dkms build   -m amdgpu -v 6.14.14-2212064.el8.v17_5_cgroup_aware -k $(uname -r)
sudo dkms install -m amdgpu -v 6.14.14-2212064.el8.v17_5_cgroup_aware -k $(uname -r) --force

# 4) Drop gold modprobe.conf (see §5), then reboot.
#    rmmod hot-swap is NOT recommended on this site; reboot is cleaner.
sudo reboot
```

### Post-boot smoke test

```bash
lsmod | grep -E "amdgpu|amdkfd|amdkcl"
modinfo amdgpu | grep -E "^version|^srcversion"
# expect srcversion 3AAFCF7B100DAE0975C2469 (or whatever your local rebuild produces)

cat /sys/module/amdgpu/parameters/kfd_pin_queue_svm_pages    # 1
cat /sys/module/amdgpu/parameters/kfd_pin_queue_svm_max_mb   # 256
cat /sys/module/amdgpu/parameters/kfd_defer_queue_eviction   # 1
cat /sys/module/amdgpu/parameters/amdgpu_gtt_cgroup_reserve_mb  # 0

dmesg | grep -E "amdgpu|kfd|amdkcl" | tail -50
# Must NOT see: WARN_ON, BUG, Oops, NULL deref.
```

---

## 7. Validation matrix (reviewer to run)

### 7.1 Regression (must pass before any new test)

Run the entire v17.5-rc2/rc3 reproducer suite under gold ENV + gold modprobe:

| Reproducer | Pass criterion |
|---|---|
| `multistream_combo` (8 streams, 30 min) | service alive, no abort, application errors graceful |
| `sdma_suballoc_hang` | bounded wait, returns with hipErrorOutOfResources |
| `customer_hang_repro` | no DEATH-A1/A2/C; service stays up |
| `d2h_perm_hang` | bounded; returns with hipErrorTimeout |
| `torch_service_sim` | survives ≥ 30 min, no abort |

If any of these regress vs v17.5-rc3 → **STOP**, do not proceed; report the diff to agent A.

### 7.2 Phase C — pin keeps queue alive under cgroup pressure

```bash
# Limit cgroup to 32 GB
sudo systemd-run --scope -p MemoryMax=32G \
    bash -c 'torch_service_sim & sleep 5 && \
             stress-ng --vm 4 --vm-bytes 4G --timeout 600s'
```

Expected:
- `dmesg` count of `Freeing queue vital buffer` == 0 (was 2 on customer site).
- `cat /sys/class/kfd/kfd/proc/$PID/pinned_svm_bytes` reports non-zero, ≤ 256 MB.
- Service alive ≥ 5 min after stressor exits.

### 7.3 Phase C2 — VMA-still-mapped → defer not evict

Synthetic test:
1. Spawn HIP process, create CWSR queue.
2. From a sibling thread, `madvise(MADV_DONTNEED, queue_va_range)`.
3. dmesg should show `deferring queue eviction` (Phase C2) instead of `queue evicted`.
4. Queue resumes after restore-worker tick.

### 7.4 Phase A — opt-in only

Set `amdgpu_gtt_cgroup_reserve_mb=4096` and re-run cgroup-32GB stress:
- `hipMemcpy`/large allocations should return `hipErrorOutOfMemory` early.
- No OOM kill, no GPU hang.
- Compare against `amdgpu_gtt_cgroup_reserve_mb=0` to confirm gate is the cause of early-fail.

---

## 8. Risk / regression surface (be explicit)

| Risk | Mitigation in this branch |
|---|---|
| pin failure in get-path → queue creation fails | non-fatal: warn + skip pin, queue still created (legacy behavior) |
| pin counter leak on abnormal exit | bottom guard in `svm_range_free` + WARN_ON_ONCE |
| split inherits stale pin → double-unpin | split helpers zero-init child pin state |
| 6.14.14 ABI for `pin_user_pages_remote` differs | KCL shim with version dispatch + degraded fallback |
| Phase C2 `find_vma` race | `mmap_assert_locked` + read under same lock as upstream code |
| Phase A early ENOMEM breaks unrelated paths | default 0 (off); only TTM_PL_TT path; only when modparam non-zero |
| Memory accounting drift in cgroup v1 | helper checks v2 cgroup; v1 returns ULONG_MAX (gate disabled) |

**Known gaps** (callout for reviewer):
- No real-hardware validation done by agent A. The build host has no AMD GPU.
- Phase D (kernel-infra BO `__GFP_ACCOUNT` audit) was dropped — re-audit if a future TTM change flips defaults.
- Pin caps are heuristics: 16 MB per prange / 256 MB per process. Tune via `kfd_pin_queue_svm_max_mb` if customer profile differs.

---

## 9. Sysfs observability cheat-sheet

```bash
# Per-process pin state
for p in /sys/class/kfd/kfd/proc/*/; do
    echo "=== pid=$(basename $p) ==="
    [ -r $p/pinned_svm_bytes  ] && cat $p/pinned_svm_bytes
    [ -r $p/pinned_svm_ranges ] && cat $p/pinned_svm_ranges
done

# Module params
grep . /sys/module/amdgpu/parameters/{kfd_pin_queue_svm_pages,kfd_pin_queue_svm_max_mb,kfd_defer_queue_eviction,amdgpu_gtt_cgroup_reserve_mb}

# Trigger / log signals
dmesg | grep -E "phase-C pin failed|deferring queue eviction|Freeing queue vital buffer" | tail
```

---

## 10. Rollback

To disable everything Phase C/C2/A in this branch *without* changing modules:

```bash
# Disable on running kernel
echo 0 | sudo tee /sys/module/amdgpu/parameters/kfd_pin_queue_svm_pages
echo 0 | sudo tee /sys/module/amdgpu/parameters/kfd_defer_queue_eviction
echo 0 | sudo tee /sys/module/amdgpu/parameters/amdgpu_gtt_cgroup_reserve_mb
```

Behavior reverts to **exactly** v17.5-rc3 (all new code paths gated by modparam). Use this as the regression A/B harness.

To fully roll back to v17.5-rc3 module:
```bash
sudo dkms remove amdgpu/6.14.14-2212064.el8.v17_5_cgroup_aware --all
sudo modprobe -r amdgpu  # or reboot
sudo modprobe amdgpu     # falls back to v17_5_rc3_fixk1
```

---

## 11. Sign-off checklist (for reviewer)

- [ ] §7.1 regression suite — all PASS
- [ ] §7.2 Phase C cgroup-pressure test — 0 occurrences of `Freeing queue vital buffer`
- [ ] §7.3 Phase C2 synthetic — defer message observed
- [ ] §7.4 Phase A opt-in — early ENOMEM observed, no OOM kill
- [ ] No new WARN_ON / BUG / Oops in dmesg over 30-min soak
- [ ] `pinned_svm_bytes` returns to 0 after process exit (no leak)
- [ ] After all pass → tag `v17.5-rc4` and update gold deployment doc
