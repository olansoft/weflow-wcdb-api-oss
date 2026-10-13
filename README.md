# WeFlow WCDB 跨平台编译指南 (macOS / Windows / Linux)

本项目为 WeFlow 微信数据提取组件的开源跨平台实现，支持在 **macOS (arm64 / x64 Universal)**、**Windows (x64 / ARM64)** 以及 **Linux (x64 / ARM64)** 上编译为兼容 C ABI 的动态链接库。

---

## 1. 跨平台产物命名与部署规范

| 操作系统 | 架构 | 生成库文件名 | 需一同部署的底层依赖（来自 `./deps`） | 对应 WeFlow 程序部署目录 |
| :--- | :--- | :--- | :--- | :--- |
| **macOS** | Universal (arm64/x64) | `libwcdb_api.dylib` | `libWCDB.dylib` | `resources/wcdb/macos/universal/` |
| **Windows** | x64 | `wcdb_api.dll` | `WCDB.dll` | `resources/wcdb/win32/x64/` |
| **Windows** | ARM64 | `wcdb_api.dll` | `WCDB.dll` | `resources/wcdb/win32/arm64/` |
| **Linux** | x64 | `libwcdb_api.so` | *(已静态全内置，无需外部库)* | `resources/wcdb/linux/x64/` |
| **Linux** | ARM64 (aarch64) | `libwcdb_api.so` | *(已静态全内置，无需外部库)* | `resources/wcdb/linux/arm64/` |

> **重要说明：部署与运行时依赖**  
> * **WCDB 库来源**：本项目 `./deps` 目录下预置的 `libWCDB.dylib`、`WCDB.dll` 等依赖库均来自腾讯官方开源仓库 [Tencent/wcdb](https://github.com/Tencent/wcdb) 的 **`v2.1.15` 原生构建版本**。
> * **部署要求**：在 macOS 和 Windows 上，运行 WeFlow 时**必须将 `./deps` 下对应的底层 WCDB 库一同复制到程序指定目录**（如上表所示），否则程序启动时加载 `wcdb_api` 会因缺少符号解析而报错。

---

## 2. 各平台详细构建步骤

### 2.1 macOS (arm64 / x64 Universal)

#### 前置环境
- Xcode Command Line Tools (`xcode-select --install`)
- CMake >= 3.16
- Homebrew: `brew install zstd curl`

#### 构建命令
```bash
# 进入工程根目录
cd wcdb-api-oss

# 配置并编译 (默认自动定位 ./deps/macos/universal/libWCDB.dylib，并优先静态链接 libzstd.a)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j$(sysctl -n hw.ncpu)
```

产物位于 `build/libwcdb_api.dylib`。

---

### 2.2 Windows (x64 / ARM64)

#### 前置环境
- Visual Studio 2019 / 2022 (勾选“使用 C++ 的桌面开发”)
- CMake >= 3.16
- Python 3 (或使用 `uv`)
- 通过 vcpkg 安装依赖：
  ```cmd
  vcpkg install curl:x64-windows zstd:x64-windows
  ```
  *(ARM64 请安装 `curl:arm64-windows` 与 `zstd:arm64-windows`)*

#### 步骤 1：生成 MSVC 导入库 (`WCDB.lib`) 与 DEF 提取工具使用
Windows 下使用 MSVC 链接器链接第三方预编译 `WCDB.dll` 时，需要对应的导入库 `.lib`。

##### 1.1 使用 `generate_win_import_def.py` 提取导出定义文件 (`.def`)
本项目脚本 [`scripts/generate_win_import_def.py`](file:///Users/steven/Desktop/Weflow/lib/wcdb/wcdb-api-oss/scripts/generate_win_import_def.py) 可直接解析任何 PE32+ (DLL) 文件的导出表，自动提取全部导出函数名和序号（Ordinal）。

> **提示**：本项目 `./deps/win32/x64/` 和 `./deps/win32/arm64/` 目录下**已预置提取好的 `WCDB.def` 文件**。若未来更换了其他版本的 `WCDB.dll` 或需要重新提取，可运行此工具：

* **推荐命令 (使用 `uv`，自动配置依赖，无需污染系统 Python)**：
  ```bash
  # 提取 x64 WCDB.dll 导出表：
  uv run --with pefile python scripts/generate_win_import_def.py deps/win32/x64/WCDB.dll

  # 提取 ARM64 WCDB.dll 导出表：
  uv run --with pefile python scripts/generate_win_import_def.py deps/win32/arm64/WCDB.dll
  ```
* **传统 `pip` 方式**：
  ```bash
  pip install pefile
  python scripts/generate_win_import_def.py deps/win32/x64/WCDB.dll [可选自定义输出路径.def]
  ```
* **命令行参数说明**：
  * 参数 1 (`input.dll`)：输入的 Windows DLL 文件路径（必选）。
  * 参数 2 (`output.def`)：输出的 `.def` 路径（可选，省略时默认在 DLL 所在同级目录下输出同名 `.def`）。
  * 支持 `--help` / `-h` 查看完整内置帮助与建议的 `lib.exe` 生成命令。

##### 1.2 在 Visual Studio 开发者终端中生成 `.lib` 导入库
在 Visual Studio 开发者命令提示符 (Developer Command Prompt) 中执行 MSVC `lib.exe` 工具：

* **x64 架构**：
  ```cmd
  cd deps\win32\x64
  lib /def:WCDB.def /machine:x64 /out:WCDB.lib
  cd ..\..\..
  ```
* **ARM64 架构**：
  ```cmd
  cd deps\win32\arm64
  lib /def:WCDB.def /machine:arm64 /out:WCDB.lib
  cd ..\..\..
  ```

#### 步骤 2：编译动态库
```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake"

cmake --build build --config Release
```
*(ARM64 平台请将 `-A x64` 替换为 `-A ARM64`)*

> **关于 Windows 构建产物路径说明**：  
> * **多配置生成器 (Visual Studio，默认)**：MSVC 构建会自动按配置分类，产物位于 `build\Release\wcdb_api.dll`。  
> * **单配置生成器 (如 Ninja / MinGW)**：如果使用 `-G Ninja` 构建，产物直接位于 `build\wcdb_api.dll`。

---

### 2.3 Linux (x64 / ARM64 aarch64)

本项目全面支持 Linux x64 与 Linux ARM64 (aarch64)。

#### 前置环境
- GCC / G++ (>= 9.0) 或 Clang
- CMake >= 3.16
- Git
- 系统依赖库：
  ```bash
  # Ubuntu / Debian
  sudo apt-get update
  sudo apt-get install -y build-essential cmake git libcurl4-openssl-dev libzstd-dev

  # CentOS / RHEL / Fedora
  sudo dnf install -y gcc-c++ cmake git libcurl-devel libzstd-devel
  ```

#### 方式一：一键全自包含构建（推荐，全静态打包单个 .so）
在 Linux 下，为彻底避免动态库加载路径冲突与版本兼容问题，推荐采用与官方一致的全静态一体化编译：

```bash
# 进入工程根目录执行一键构建脚本
./scripts/build_linux_standalone.sh
```

> **自动化编译机制**：  
> `build_linux_standalone.sh` 脚本在执行时会**自动从 GitHub 克隆腾讯官方开源仓库 [Tencent/wcdb](https://github.com/Tencent/wcdb) (`v2.1.15` 及其 `sqlcipher`、`zstd` 子模块)**，根据当前 Linux 架构（x64 或 aarch64）在本地编译生成静态库 `libWCDB.a`，随后与本项目源码静态合并链接，最终在 `build_linux/` 目录下生成单个全自包含的 **`libwcdb_api.so`**。运行时无需额外安装任何 WCDB 动态库。

#### 方式二：已有 WCDB 动态库时链接构建
若您已有针对 Linux 平台编译好的 `libWCDB.so`：
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DWCDB_LIB_PATH="/path/to/libWCDB.so"

cmake --build build --config Release -j$(nproc)
```
产物位于 `build/libwcdb_api.so`，CMake 已配置 `$ORIGIN` RPATH，部署时只需将 `libWCDB.so` 与 `libwcdb_api.so` 放置于同级目录即可。
