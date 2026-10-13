#!/usr/bin/env bash
# ==============================================================================
# Linux 构建 libwcdb_api.so（静态链接 WCDB，其他依赖以链接结果为准）
# 支持 Ubuntu / Debian / CentOS / Arch Linux 等 x64 与 aarch64 环境
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPS_DIR="${ROOT_DIR}/third_party"
WCDB_VERSION="v2.1.15"

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
    git clone --branch "${WCDB_VERSION}" --depth 1 https://github.com/Tencent/wcdb.git
    cd wcdb
else
    echo "WCDB 源码已存在，检查版本。"
    cd wcdb
fi

WCDB_EXPECTED_COMMIT="$(git rev-parse --verify "refs/tags/${WCDB_VERSION}^{commit}" 2>/dev/null)" || {
    echo "错误: 缓存源码缺少 ${WCDB_VERSION} 标签，无法确认版本。"
    echo "请保留本地修改，将 ${DEPS_DIR}/wcdb 移到其他目录后重新运行。"
    exit 1
}
if [ "$(git rev-parse HEAD)" != "${WCDB_EXPECTED_COMMIT}" ]; then
    echo "错误: 缓存源码 HEAD 与 ${WCDB_VERSION} 不一致。"
    echo "请保留本地修改并切换到 ${WCDB_VERSION}，或将 ${DEPS_DIR}/wcdb 移走后重试。"
    exit 1
fi
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "错误: WCDB 缓存存在已跟踪文件的本地修改，无法保证使用 ${WCDB_VERSION} 原始源码。"
    echo "请保留修改并恢复干净源码后重试。"
    exit 1
fi
git submodule update --init --depth 1 sqlcipher zstd

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

echo "=== [3/4] 编译 libwcdb_api.so（静态链接 WCDB） ==="
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
