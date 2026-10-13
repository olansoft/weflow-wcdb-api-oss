# WeFlow WCDB 跨平台编译指南

本项目是 WeFlow 微信数据提取组件的 C++17 实现，构建产物通过 C ABI 向宿主程序提供接口。接口声明见 [`include/wcdb_api.h`](include/wcdb_api.h)。

构建配置包含 macOS、Windows 和 Linux 分支。下文给出各平台的构建方式；配置支持不代表所有系统和架构组合均已完成构建及运行验证。

## 1. 构建产物与部署目录

| 操作系统 | 目标架构 | 生成库文件名 | WCDB 部署方式 | WeFlow 部署目录 |
| :--- | :--- | :--- | :--- | :--- |
| macOS | arm64；x64 行为待确认 | `libwcdb_api.dylib` | 同目录部署 `libWCDB.dylib` | `resources/wcdb/macos/universal/` |
| Windows | x64 | `wcdb_api.dll` | 同目录部署 x64 `WCDB.dll` | `resources/wcdb/win32/x64/` |
| Windows | ARM64 | `wcdb_api.dll` | 同目录部署 ARM64 `WCDB.dll` | `resources/wcdb/win32/arm64/` |
| Linux | x64 | `libwcdb_api.so` | 推荐静态链接 WCDB；动态链接时另行部署 WCDB | `resources/wcdb/linux/x64/` |
| Linux | ARM64（aarch64） | `libwcdb_api.so` | 推荐静态链接 WCDB；动态链接时另行部署 WCDB | `resources/wcdb/linux/arm64/` |

macOS arm64 上，WeFlow 主程序默认从 `macos/universal/` 读取库，因此保留该目录名。**目录名不代表库实际包含 arm64 和 x86_64 两种架构**。当前 macOS 构建命令使用默认目标架构；WeFlow 在 x64 系统上的加载行为尚未确认，不能据此承诺 x64 或 Universal 支持。

`deps` 中的预编译 WCDB 库版本标注为 `v2.1.15`，来源为 [Tencent/wcdb](https://github.com/Tencent/wcdb)。目录说明见 [`deps/README.md`](deps/README.md)。

表格只列出 WCDB 的部署方式。curl、zstd 及其传递依赖是否需要另行部署，取决于实际链接结果。发布前应检查产物的架构、动态依赖，并在目标环境执行加载验证。

## 2. macOS 构建

前置环境：

- Xcode Command Line Tools：`xcode-select --install`
- CMake >= 3.16
- Homebrew 依赖：`brew install zstd curl`
- 与目标架构匹配的 WCDB、zstd 和 curl 库

在工程根目录执行：

```bash
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=Release \
  -DHOMEBREW_PREFIX="$(brew --prefix)"
cmake --build build-macos --config Release --parallel "$(sysctl -n hw.ncpu)"
```

产物位于 `build-macos/libwcdb_api.dylib`。默认 WCDB 路径为 `deps/macos/universal/libWCDB.dylib`；该文件不存在时，配置会尝试使用本机 WeFlow 安装目录中的库。可通过 `-DWCDB_DYLIB_PATH=/实际路径/libWCDB.dylib` 指定其他库。

CMake 优先使用 Homebrew 下的 `libzstd.a`，不存在时查找动态库。curl 的实际来源由 `find_package(CURL)` 决定；如需指定 Homebrew curl，可添加 `-DCMAKE_PREFIX_PATH="$(brew --prefix curl)"`。

如需构建双架构产物，需要先准备包含两种架构的全部依赖，再配置 `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`。仅设置该参数不足以保证构建或宿主加载成功。

## 3. Windows 构建

前置环境：

- Visual Studio 2019 或 2022，安装“使用 C++ 的桌面开发”及目标架构工具链
- CMake >= 3.16
- vcpkg；下例假设安装在 `C:\vcpkg`，请按实际位置调整

以下命令使用 **Visual Studio Developer Command Prompt（CMD）**。其中 `^` 是 CMD 的续行符。示例使用 VS 2022；VS 2019 请将生成器替换为 `Visual Studio 16 2019`。

### 3.1 构建 x64

在工程根目录执行：

```cmd
C:\vcpkg\vcpkg.exe install curl:x64-windows zstd:x64-windows

lib /def:deps\win32\x64\WCDB.def /machine:x64 /out:deps\win32\x64\WCDB.lib

cmake -S . -B build-win-x64 -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DWCDB_LIB_PATH="%CD%\deps\win32\x64\WCDB.lib"

cmake --build build-win-x64 --config Release
```

产物位于 `build-win-x64\Release\wcdb_api.dll`。

### 3.2 构建 ARM64

在工程根目录执行，确保已安装 MSVC ARM64 构建工具：

```cmd
C:\vcpkg\vcpkg.exe install curl:arm64-windows zstd:arm64-windows

lib /def:deps\win32\arm64\WCDB.def /machine:arm64 /out:deps\win32\arm64\WCDB.lib

cmake -S . -B build-win-arm64 -G "Visual Studio 17 2022" -A ARM64 ^
  -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=arm64-windows ^
  -DWCDB_LIB_PATH="%CD%\deps\win32\arm64\WCDB.lib"

cmake --build build-win-arm64 --config Release
```

产物位于 `build-win-arm64\Release\wcdb_api.dll`。各架构使用独立构建目录，避免复用不匹配的 CMake 缓存。

部署时除 `WCDB.dll` 外，还应检查 zstd、curl 及其传递依赖 DLL 和 MSVC 运行库。当前 CMake 在找到 zstd 包时使用共享库目标，不能假设仅复制两个 DLL 即可运行。

### 3.3 更换 WCDB.dll 时重新生成 DEF

仓库已提供 x64 和 ARM64 的 `WCDB.def`，正常构建直接使用。更换 `WCDB.dll` 后，应使用 [`scripts/generate_win_import_def.py`](scripts/generate_win_import_def.py) 重新生成 DEF，再执行对应架构的 `lib` 命令。

安装 uv 后，在工程根目录执行：

```cmd
uv run --with pefile python scripts/generate_win_import_def.py deps/win32/x64/WCDB.dll
uv run --with pefile python scripts/generate_win_import_def.py deps/win32/arm64/WCDB.dll
```

也可以在 Python 虚拟环境中执行：

```cmd
python -m pip install pefile
python scripts/generate_win_import_def.py deps/win32/x64/WCDB.dll
```

脚本默认在 DLL 同目录生成同名 `.def`。第二个位置参数可指定输出路径，`--help` 显示完整帮助。

## 4. Linux 构建

以下步骤在目标架构的 Linux 环境中执行，适用于 x64 和 ARM64（aarch64）的本机构建；跨架构构建需要另外配置工具链。

前置环境：GCC/G++ >= 9 或支持 C++17 的 Clang、CMake >= 3.16、Git、Bash，以及 curl 和 zstd 开发包。

```bash
# Ubuntu / Debian
sudo apt-get update
sudo apt-get install -y build-essential cmake git libcurl4-openssl-dev libzstd-dev

# 使用 dnf 的 RHEL / Fedora 等系统，包名和版本请按发行版确认
sudo dnf install -y gcc-c++ cmake git libcurl-devel libzstd-devel
```

### 4.1 静态链接 WCDB（推荐）

在工程根目录执行：

```bash
bash scripts/build_linux_standalone.sh
```

脚本将 Tencent/wcdb `v2.1.15` 下载到 `third_party/wcdb`，初始化 `sqlcipher` 和 `zstd` 子模块，构建 `libWCDB.a`，再链接本项目。首次下载需要访问 GitHub。已有源码目录只有在 HEAD 与指定版本一致时才会复用，版本不符则停止并提示。

产物位于 `build_linux/libwcdb_api.so`。此方式无需单独部署 WCDB 动态库，但没有保证 curl、zstd、C/C++ 运行库等依赖全部静态链接；实际运行时依赖见验证步骤。

### 4.2 链接已有 WCDB 动态库

准备与目标架构及本项目 ABI 匹配的 WCDB 动态库，在工程根目录执行。将下例路径替换为实际文件路径：

```bash
cmake -S . -B build-linux-shared -DCMAKE_BUILD_TYPE=Release \
  -DWCDB_LIB_PATH="/path/to/libWCDB.so"
cmake --build build-linux-shared --config Release --parallel "$(nproc)"
```

产物位于 `build-linux-shared/libwcdb_api.so`。CMake 配置了 `$ORIGIN` RPATH，可从产物所在目录查找依赖。部署 WCDB 时，文件名或符号链接需与实际依赖名称（SONAME）匹配；curl、zstd 等依赖也需要在目标环境可用。

## 5. 构建与部署验证

先检查目标架构和动态依赖，再将产物部署到上表目录。以下命令假设使用各平台的默认构建目录。

macOS：

```bash
lipo -archs build-macos/libwcdb_api.dylib
lipo -archs deps/macos/universal/libWCDB.dylib
otool -L build-macos/libwcdb_api.dylib
```

检查宿主程序和所有库的架构匹配，WCDB 加载路径符合部署布局，并确认是否存在依赖本机 Homebrew 路径的动态库。

Windows（Developer Command Prompt）：

```cmd
dumpbin /headers build-win-x64\Release\wcdb_api.dll
dumpbin /dependents build-win-x64\Release\wcdb_api.dll
```

ARM64 构建请换用 `build-win-arm64` 路径。还需对依赖 DLL 检查架构和传递依赖。

Linux：

```bash
file build_linux/libwcdb_api.so
readelf -d build_linux/libwcdb_api.so
ldd build_linux/libwcdb_api.so
```

检查 `NEEDED`、RPATH/RUNPATH 和 `ldd` 输出，确保没有 `not found`。系统运行库及其最低版本要求应以目标环境验证结果为准。

安装 Python 3 后，可对已构建或已部署的库进行最小加载检查。以 Linux 产物为例，在工程根目录执行：

```bash
python3 -c 'import ctypes; ctypes.CDLL("./build_linux/libwcdb_api.so"); print("Library loaded")'
```

macOS 请替换为 `./build-macos/libwcdb_api.dylib`。Windows 可在 CMD 中使用：

```cmd
python -c "import ctypes, os; dll_dir = os.add_dll_directory(os.path.abspath('resources/wcdb/win32/x64')); ctypes.CDLL(os.path.abspath('resources/wcdb/win32/x64/wcdb_api.dll')); print('Library loaded')"
```

Windows 示例要求 Python >= 3.8，且库及配套 DLL 已部署到该目录；ARM64 请替换目录并使用匹配架构的 Python。

加载成功仅说明动态库及依赖可被当前进程加载。发布前还应在目标系统上通过 WeFlow 验证账号打开、查询及资源释放流程。

## 6. 常见问题

- **找不到 curl 或 zstd**：确认开发包已安装，CMake 搜索路径或 vcpkg triplet 与目标架构匹配。macOS 可指定 `HOMEBREW_PREFIX` 和 `CMAKE_PREFIX_PATH`。
- **架构不匹配**：检查 WCDB、curl、zstd、封装库和宿主程序的架构；切换架构时使用独立构建目录。
- **缺少 DLL 或共享库**：检查动态依赖及其传递依赖，按实际依赖名称部署。仅复制 WCDB 库未必足够。
- **Linux 缓存源码版本不符**：保留有用的本地修改，将 `third_party/wcdb` 移到其他目录后重新运行脚本，或手动切换到 `v2.1.15` 后重试。
- **更换 WCDB 后出现未定义符号**：确认版本和 ABI 匹配；Windows 重新生成 DEF 和导入库。C ABI 是封装层的对外接口，底层 WCDB C++ ABI 仍需匹配。
