#!/usr/bin/env bash
# ==============================================================================
# Linux 一键构建全自包含 libwcdb_api.so 脚本 (Monolithic All-in-One Build)
# 支持 Ubuntu / Debian / CentOS / Arch Linux 等 x64 与 aarch64 环境
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPS_DIR="${ROOT_DIR}/third_party"

echo "=== [1/4] 检查系统环境与基础依赖 ==="
if [ -f /etc/debian_version ]; then
    echo "检测到 Debian/Ubuntu 系统..."
    # sudo apt-get update && sudo apt-get install -y cmake build-essential git libcurl4-openssl-dev libzstd-dev
elif [ -f /etc/redhat-release ]; then
    echo "检测到 RHEL/CentOS 系统..."
    # sudo dnf install -y cmake gcc-c++ git libcurl-devel libzstd-devel
fi

mkdir -p "${DEPS_DIR}"
cd "${DEPS_DIR}"

echo "=== [2/4] 获取并编译腾讯开源 WCDB 静态库 ==="
if [ ! -d "wcdb" ]; then
    echo "克隆 Tencent/wcdb 仓库..."
    git clone --depth 1 https://github.com/Tencent/wcdb.git
    cd wcdb
    git submodule update --init --depth 1 sqlcipher zstd
else
    echo "WCDB 源码已存在，跳过克隆。"
    cd wcdb
fi

echo "配置并编译 WCDB 静态库..."
mkdir -p src/build_static
cd src/build_static
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF

cmake --build . -j"$(nproc)"

WCDB_STATIC_LIB="$(find "${PWD}" -name "libWCDB.a" | head -n 1)"
if [ -z "${WCDB_STATIC_LIB}" ] || [ ! -f "${WCDB_STATIC_LIB}" ]; then
    echo "错误: 未找到生成的 libWCDB.a 静态库！"
    exit 1
fi
echo "成功构建 WCDB 静态库: ${WCDB_STATIC_LIB}"

echo "=== [3/4] 编译全自包含 libwcdb_api.so ==="
cd "${ROOT_DIR}"
mkdir -p build_linux
cd build_linux

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DWCDB_LIB_PATH="${WCDB_STATIC_LIB}"

cmake --build . --config Release -j"$(nproc)"

echo "=== [4/4] 验证产物 ==="
OUTPUT_SO="${ROOT_DIR}/build_linux/libwcdb_api.so"
if [ -f "${OUTPUT_SO}" ]; then
    echo "构建成功！"
    echo "产物路径: ${OUTPUT_SO}"
    ls -lh "${OUTPUT_SO}"
    echo "导出的 wcdb API 数量: $(nm -gD "${OUTPUT_SO}" | grep -c " T wcdb_")"
else
    echo "构建失败，产物不存在。"
    exit 1
fi
