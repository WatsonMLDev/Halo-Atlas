#!/usr/bin/env bash
# Run the Ticket #5 Zephyr RTOS & Cortex-M55 build/QEMU verification gates
# using Brilliant Labs' pre-baked halo-firmware-ci Docker image.
#
# Usage:
#   ./tools/run_zephyr_gate.sh [qemu|halo|all]

set -euo pipefail

MODE="${1:-all}"
IMAGE="${HALO_FIRMWARE_CI_IMAGE:-ghcr.io/brilliantlabsar/halo-firmware-ci:latest}"
QEMU_TIMEOUT_SEC="${QEMU_TIMEOUT_SEC:-60}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HALO_FIRMWARE_DIR="${HALO_FIRMWARE_DIR:-${REPO_ROOT}/../halo-firmware}"

run_qemu_gate() {
    echo "=== [Gate 1] Zephyr Cortex-M55 QEMU (mps3_an547) ==="
    docker run --rm \
        -e "QEMU_TIMEOUT_SEC=${QEMU_TIMEOUT_SEC}" \
        -v "${REPO_ROOT}:/halo_atlas:ro" \
        "${IMAGE}" \
        bash -c '
            set -euo pipefail
            export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
            west build -b mps3_an547 /halo_atlas/hal/zephyr/qemu_app -d /tmp/build-qemu -p
            python3 - << "PY"
import os, select, signal, subprocess, sys, time

timeout_sec = float(os.environ.get("QEMU_TIMEOUT_SEC", "60"))
deadline = time.monotonic() + timeout_sec
proc = subprocess.Popen(
    ["west", "build", "-t", "run", "-d", "/tmp/build-qemu"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    start_new_session=True,
)
assert proc.stdout is not None
fd = proc.stdout.fileno()
buf = b""
passed = False
failed = False
timed_out = False
try:
    while True:
        rem = deadline - time.monotonic()
        if rem <= 0:
            timed_out = True
            sys.stderr.write(f"PEBBLE_QEMU_GATE: TIMEOUT after {timeout_sec:.0f}s\n")
            break
        ready, _, _ = select.select([fd], [], [], min(rem, 0.5))
        if not ready:
            if proc.poll() is not None:
                break
            continue
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            raw_line, buf = buf.split(b"\n", 1)
            line = raw_line.decode("utf-8", errors="replace") + "\n"
            sys.stdout.write(line)
            sys.stdout.flush()
            if "PEBBLE_QEMU_GATE: PASS" in line:
                passed = True
                break
            if "PEBBLE_QEMU_GATE: FAIL" in line or "ZEPHYR FATAL ERROR" in line or "MPU FAULT" in line:
                failed = True
                break
        if passed or failed:
            break
finally:
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()
sys.exit(0 if (passed and not failed and not timed_out) else 1)
PY
        '
}

run_halo_gate() {
    echo "=== [Gate 2] halo-firmware (-b halo, arm-zephyr-eabi-gcc, CONFIG_FP_HARDABI=y) ==="
    local extra_mount=()
    local sync_cmd=""
    if [[ -d "${HALO_FIRMWARE_DIR}" ]]; then
        extra_mount=(-v "${HALO_FIRMWARE_DIR}:/host_halo:ro")
        sync_cmd="rsync -a --exclude=.git /host_halo/ /opt/workspace/project/;"
    fi
    docker run --rm \
        -v "${REPO_ROOT}:/halo_atlas:ro" \
        "${extra_mount[@]}" \
        "${IMAGE}" \
        bash -c "
            set -euo pipefail
            ${sync_cmd}
            cd /opt/workspace/project
            export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
            west build -b halo applications/halo -d /tmp/build-halo -p -- \
                -DZEPHYR_EXTRA_MODULES=/halo_atlas \
                -DCONFIG_PEBBLE_COMPAT=y \
                -DCONFIG_PEBBLE_COMPAT_KEEP_ENTRYPOINTS=y
            NM_BIN=\"\$(command -v arm-zephyr-eabi-nm || true)\"
            if [[ -z \"\${NM_BIN}\" && -n \"\${ZEPHYR_SDK_INSTALL_DIR:-}\" ]]; then
                NM_BIN=\"\${ZEPHYR_SDK_INSTALL_DIR}/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm\"
            fi
            if [[ -z \"\${NM_BIN}\" || ! -x \"\${NM_BIN}\" ]]; then
                NM_BIN=\"\$(find /opt/toolchains -name arm-zephyr-eabi-nm -type f 2>/dev/null | head -n 1)\"
            fi
            \"\${NM_BIN}\" /tmp/build-halo/zephyr/zephyr.elf \
                | grep -E ' (pebble_runtime_run|pebble_zephyr_hal_init|pebble_loader_exec|pebble_syscall_table_init_defaults)$'
        "
}

case "${MODE}" in
    qemu)
        run_qemu_gate
        ;;
    halo)
        run_halo_gate
        ;;
    all)
        run_qemu_gate
        run_halo_gate
        ;;
    *)
        echo "usage: $0 [qemu|halo|all]" >&2
        exit 2
        ;;
esac
