# WCDB 依赖库说明

本目录收录各目标平台的底层 WCDB 动态链接依赖库：

## 1. 来源声明
* 本目录下所有 `WCDB.dll` / `libWCDB.dylib` 均为腾讯官方开源数据库项目 [Tencent/wcdb](https://github.com/Tencent/wcdb) 的 **`v2.1.15` 原生构建版本**。
* 内置 SQLCipher 加密引擎与标准 SQLite 支持，具备完整的 C++ ABI 导出。

## 2. 目录架构结构
* `macos/universal/libWCDB.dylib`：适用于 macOS (Apple Silicon arm64 与 Intel x64 Universal 架构)。
* `win32/x64/WCDB.dll` & `WCDB.def`：适用于 Windows 64位 (x64) 架构。
* `win32/arm64/WCDB.dll` & `WCDB.def`：适用于 Windows ARM64 架构。

> **说明**：Linux 平台不依赖单独的预编译 WCDB 库，构建脚本在 Linux 下会自动拉取腾讯开源 `Tencent/wcdb` 并静态合并打包为全自包含的 `libwcdb_api.so`。

## 3. 使用方式（部署到程序目录）
构建产出的 `wcdb_api` 动态库需要与 `deps` 下对应的 `WCDB` 库一同部署到宿主程序（如 WeFlow）指定的 `resources/wcdb/` 目录中：

| 平台 | 构建产物 | 配套依赖（来自本目录） | 目标部署路径 |
| :--- | :--- | :--- | :--- |
| **macOS** | `libwcdb_api.dylib` | `libWCDB.dylib` | `resources/wcdb/macos/universal/` |
| **Windows x64** | `wcdb_api.dll` | `WCDB.dll` | `resources/wcdb/win32/x64/` |
| **Windows ARM64**| `wcdb_api.dll` | `WCDB.dll` | `resources/wcdb/win32/arm64/` |
| **Linux (x64/ARM64)** | `libwcdb_api.so` | *(已静态内置，无额外依赖)* | `resources/wcdb/linux/x64/` (或对应架构目录) |
