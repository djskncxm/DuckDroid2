#!/usr/bin/env bash
# 启动 DuckDroid2 System 网关，暴露 dy7.sock（抖音 40.8.0 / libmetasec_ml.so）。
#
# 用法：
#   ./scripts/start_dy7_system.sh
#   DUCKEMU_ROOT=/path/to/DuckDroid2 ./scripts/start_dy7_system.sh
#
# 可用环境变量：
#   DUCKEMU_ROOT         DuckDroid2 仓库根（默认与本仓库同级）
#   DUCKEMU_BUILD        构建目录（默认 $DUCKEMU_ROOT/build）
#   DUCKEMU_ROM_SYMBOLS  ROM 符号根（默认 ../../ROM/.../symbols）
#   DUCKEMU_SOCKET_DIR   socket 目录（默认 /tmp/duckemu-dy7）
#   DY7_INSTANCES        实例数（默认 1；每个实例约占数百 MB）
#   DY7_WORKERS          Worker 数（默认 1）

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"

DUCKEMU_ROOT="${DUCKEMU_ROOT:-$(cd "$root/../DuckDroid2" 2>/dev/null && pwd || true)}"
if [[ -z "${DUCKEMU_ROOT}" ]]; then
    echo "找不到 DuckDroid2；请设置 DUCKEMU_ROOT" >&2
    exit 1
fi

DUCKEMU_BUILD="${DUCKEMU_BUILD:-$DUCKEMU_ROOT/build}"
DUCKEMU_ROM_SYMBOLS="${DUCKEMU_ROM_SYMBOLS:-$DUCKEMU_ROOT/../ROM/out/target/product/lemonade/symbols}"
DUCKEMU_SOCKET_DIR="${DUCKEMU_SOCKET_DIR:-/tmp/duckemu-dy7}"
DY7_INSTANCES="${DY7_INSTANCES:-1}"
DY7_WORKERS="${DY7_WORKERS:-1}"

system_bin="$DUCKEMU_BUILD/duckemu_system"
worker_bin="$DUCKEMU_BUILD/duckemu_worker"
library="$DUCKEMU_ROOT/code/user_code/dy7_case/assets/libmetasec_ml.so"
properties="$DUCKEMU_ROOT/code/user_code/dy7_case/dy7.properties"
libc="$DUCKEMU_ROM_SYMBOLS/system/lib64/bootstrap/libc.so"
lib64="$DUCKEMU_ROM_SYMBOLS/system/lib64"

for path in "$system_bin" "$worker_bin" "$library" "$properties" "$libc"; do
    if [[ ! -e "$path" ]]; then
        echo "缺少文件: $path" >&2
        exit 1
    fi
done

mkdir -p "$DUCKEMU_SOCKET_DIR"
rm -f "$DUCKEMU_SOCKET_DIR/dy7.sock"

echo "[*] System     : $system_bin"
echo "[*] ROM libc   : $libc"
echo "[*] libmetasec : $library"
echo "[*] Socket     : $DUCKEMU_SOCKET_DIR/dy7.sock"
echo "[*] 实例       : workers=$DY7_WORKERS instances=$DY7_INSTANCES"

exec "$system_bin" \
    --socket-dir "$DUCKEMU_SOCKET_DIR" \
    --worker-exec "$worker_bin" \
    --app "id=dy7,library=$library,rom-libc=$libc,property-file=$properties,library-path=$lib64:$lib64/bootstrap,instances=$DY7_INSTANCES,workers=$DY7_WORKERS,init-timeout-us=120000000,invoke-timeout-us=120000000" \
    "$@"
