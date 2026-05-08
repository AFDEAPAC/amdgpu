#!/bin/bash
# verify_gpu_util_stuck.sh — verification harness keyed on customer's
# headline complaint: "GPU 使用率永遠卡在 100%".
#
# Theory of detection:
#   When a queue gets evicted by mmu_notifier reclaim (the failure mode
#   v17.5-cwsr-resilient Item 2 closes), the userspace runtime keeps
#   submitting AQL packets to a queue the driver has torn down. The HW
#   gpu_busy_percent counter sticks at 100% even though no actual work
#   retires. The legitimate way to distinguish "real busy" from "stuck
#   busy" is to cross-check gpu_busy_percent against the GFX activity
#   counter delta over the same window: if util>=99% but the activity
#   counter advances less than EXPECTED_TICKS_PER_SEC * window_sec,
#   that's the smoking gun.
#
# Output:
#   $LOG_DIR/util.csv       : timestamp, gpu_id, busy_pct
#   $LOG_DIR/activity.csv   : timestamp, gpu_id, gfx_activity
#   $LOG_DIR/verdict.json   : per-GPU pass/fail + max stuck duration
#
# Verdict thresholds (override via env):
#   STUCK_PCT_THRESHOLD   = 99      (busy% threshold)
#   STUCK_DURATION_SEC    = 30      (consecutive seconds at >= threshold)
#   ACTIVITY_MIN_DELTA    = 1000    (min counter delta over the window;
#                                    if util>=99% but delta<this -> stuck)
#   SAMPLE_INTERVAL_SEC   = 1
#   TOTAL_DURATION_SEC    = 600     (default 10 minutes)
#
# Usage:
#   bash verify_gpu_util_stuck.sh
#   TOTAL_DURATION_SEC=1800 bash verify_gpu_util_stuck.sh   # 30 min
#
# Run in parallel with the reproducer (multistream_combo, customer_hang,
# torch_service_sim) under cgroup-32GB. Compare verdict between
# v17.5-cgroup-aware (baseline) and v17.5-cwsr-resilient (after rebuild
# + module reload).

set -uo pipefail

STUCK_PCT_THRESHOLD=${STUCK_PCT_THRESHOLD:-99}
STUCK_DURATION_SEC=${STUCK_DURATION_SEC:-30}
ACTIVITY_MIN_DELTA=${ACTIVITY_MIN_DELTA:-1000}
SAMPLE_INTERVAL_SEC=${SAMPLE_INTERVAL_SEC:-1}
TOTAL_DURATION_SEC=${TOTAL_DURATION_SEC:-600}

DIR="$(cd "$(dirname "$0")" && pwd)"
TAG=${TAG:-$(date +%Y%m%d_%H%M%S)}
LOG_DIR=${LOG_DIR:-$DIR/runs/util_${TAG}}
mkdir -p "$LOG_DIR"

# Auto-detect AMD GPU cards.
AMD_CARDS=()
for c in $(ls /sys/class/drm/ 2>/dev/null | grep "^card[0-9]*$"); do
    v=$(cat /sys/class/drm/$c/device/vendor 2>/dev/null || true)
    [ "$v" = "0x1002" ] || continue
    [ -r /sys/class/drm/$c/device/gpu_busy_percent ] || continue
    AMD_CARDS+=("$c")
done

if [ ${#AMD_CARDS[@]} -eq 0 ]; then
    echo "ERROR: no AMD GPU cards with gpu_busy_percent visible (need amdgpu loaded)"
    exit 2
fi

echo "=== verify_gpu_util_stuck ==="
echo "tag         : $TAG"
echo "log_dir     : $LOG_DIR"
echo "gpus        : ${AMD_CARDS[*]}"
echo "duration    : ${TOTAL_DURATION_SEC}s"
echo "stuck rule  : >=${STUCK_PCT_THRESHOLD}% for >=${STUCK_DURATION_SEC}s, with activity delta < $ACTIVITY_MIN_DELTA"
echo "driver tag  : $(cat /sys/module/amdgpu/srcversion 2>/dev/null || echo unknown)"
echo "Item 2 mod  : kfd_protect_cwsr_vma=$(cat /sys/module/amdgpu/parameters/kfd_protect_cwsr_vma 2>/dev/null || echo unset)"
echo "Item 1 mod  : kfd_cwsr_in_vram=$(cat /sys/module/amdgpu/parameters/kfd_cwsr_in_vram 2>/dev/null || echo unset)"
echo

# Banner into log.
{
    echo "tag=$TAG"
    echo "duration_sec=$TOTAL_DURATION_SEC"
    echo "stuck_pct_threshold=$STUCK_PCT_THRESHOLD"
    echo "stuck_duration_sec=$STUCK_DURATION_SEC"
    echo "activity_min_delta=$ACTIVITY_MIN_DELTA"
    echo "amdgpu_srcversion=$(cat /sys/module/amdgpu/srcversion 2>/dev/null || echo unknown)"
    echo "kfd_protect_cwsr_vma=$(cat /sys/module/amdgpu/parameters/kfd_protect_cwsr_vma 2>/dev/null || echo unset)"
    echo "kfd_cwsr_in_vram=$(cat /sys/module/amdgpu/parameters/kfd_cwsr_in_vram 2>/dev/null || echo unset)"
    for c in "${AMD_CARDS[@]}"; do
        echo "card=$c device=$(cat /sys/class/drm/$c/device/device)"
    done
} > "$LOG_DIR/HEADER.txt"

# Sample GFX activity counter. rocm-smi exposes a monotonic counter via
# `--showuse` ("GFX Activity"). Sysfs equivalent on 5.10/6.x is the
# `gpu_busy_percent` for instantaneous + the counter from rocm-smi for
# delta. We capture both.
sample_once() {
    local now_ms=$(date +%s%3N)
    local idx=0
    for c in "${AMD_CARDS[@]}"; do
        local pct=$(cat /sys/class/drm/$c/device/gpu_busy_percent 2>/dev/null || echo -1)
        echo "$now_ms,$idx,$c,$pct" >> "$LOG_DIR/util.csv"
        idx=$((idx+1))
    done
    # GFX activity counter — best obtained from rocm-smi, but parse cost is
    # ~150ms which throws off the 1s cadence. Sample every 5th tick.
    if (( SAMPLE_TICK % 5 == 0 )); then
        rocm-smi --showuse 2>/dev/null \
          | awk -v ts="$now_ms" '
              /^GPU\[([0-9]+)\][\t ]*: GFX Activity:/ {
                  match($0, /GPU\[([0-9]+)\]/, m); g=m[1];
                  act=$NF; print ts","g","act
              }' >> "$LOG_DIR/activity.csv"
    fi
}

# CSV headers.
echo "timestamp_ms,gpu_idx,card,busy_pct" > "$LOG_DIR/util.csv"
echo "timestamp_ms,gpu_idx,gfx_activity" > "$LOG_DIR/activity.csv"

end_ts=$(( $(date +%s) + TOTAL_DURATION_SEC ))
SAMPLE_TICK=0
while [ "$(date +%s)" -lt "$end_ts" ]; do
    sample_once
    SAMPLE_TICK=$((SAMPLE_TICK+1))
    sleep "$SAMPLE_INTERVAL_SEC"
done

# Verdict computation.
python3 - "$LOG_DIR" "$STUCK_PCT_THRESHOLD" "$STUCK_DURATION_SEC" "$ACTIVITY_MIN_DELTA" <<'PYEOF'
import csv, json, sys, collections

log_dir, pct_thr, dur_sec, act_min = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])

util_rows = []
with open(f"{log_dir}/util.csv") as f:
    r = csv.DictReader(f)
    for row in r:
        util_rows.append((int(row["timestamp_ms"]), int(row["gpu_idx"]), row["card"], int(row["busy_pct"])))

# Group by gpu_idx, sorted by ts.
by_gpu = collections.defaultdict(list)
for ts, idx, card, pct in util_rows:
    by_gpu[idx].append((ts, pct, card))

# Activity counter rows.
act_rows = collections.defaultdict(list)
try:
    with open(f"{log_dir}/activity.csv") as f:
        r = csv.DictReader(f)
        for row in r:
            act_rows[int(row["gpu_idx"])].append((int(row["timestamp_ms"]), int(row["gfx_activity"])))
except Exception:
    pass

verdict = {}
for idx, samples in sorted(by_gpu.items()):
    samples.sort()
    card = samples[0][2] if samples else "?"
    # Find longest run of >= pct_thr.
    longest_ms = 0
    longest_start_ms = 0
    longest_end_ms = 0
    cur_start = None
    cur_end = None
    for ts, pct, _c in samples:
        if pct >= pct_thr:
            if cur_start is None:
                cur_start = ts
            cur_end = ts
        else:
            if cur_start is not None:
                run = cur_end - cur_start
                if run > longest_ms:
                    longest_ms, longest_start_ms, longest_end_ms = run, cur_start, cur_end
                cur_start, cur_end = None, None
    if cur_start is not None:
        run = cur_end - cur_start
        if run > longest_ms:
            longest_ms, longest_start_ms, longest_end_ms = run, cur_start, cur_end

    longest_sec = longest_ms / 1000.0
    # Activity delta over the longest stuck window.
    act = act_rows.get(idx, [])
    act_in_window = [(ts, v) for ts, v in act if longest_start_ms <= ts <= longest_end_ms]
    if len(act_in_window) >= 2:
        act_delta = act_in_window[-1][1] - act_in_window[0][1]
    else:
        act_delta = -1   # not enough samples to decide

    is_stuck = (longest_sec >= dur_sec) and (act_delta != -1) and (act_delta < act_min)
    verdict[f"gpu{idx}"] = {
        "card": card,
        "longest_high_util_sec": round(longest_sec, 1),
        "activity_counter_delta_in_window": act_delta,
        "verdict": "STUCK_AT_100" if is_stuck else "PASS",
    }

with open(f"{log_dir}/verdict.json", "w") as f:
    json.dump(verdict, f, indent=2)

stuck_count = sum(1 for v in verdict.values() if v["verdict"] == "STUCK_AT_100")
print(f"\n=== verdict ({len(verdict)} GPUs sampled) ===")
for k, v in verdict.items():
    flag = "[!]" if v["verdict"] == "STUCK_AT_100" else "[ok]"
    print(f"  {flag} {k}: longest>= {pct_thr}% = {v['longest_high_util_sec']}s, "
          f"activity_delta = {v['activity_counter_delta_in_window']}  -> {v['verdict']}")
print(f"\n{stuck_count} GPU(s) flagged stuck, {len(verdict)-stuck_count} PASS")
sys.exit(1 if stuck_count > 0 else 0)
PYEOF
RC=$?
echo
echo "verdict file: $LOG_DIR/verdict.json"
echo "raw util:     $LOG_DIR/util.csv"
echo "raw activity: $LOG_DIR/activity.csv"
exit $RC
