# WCDB 依赖库说明

本目录提供 macOS 和 Windows 的预编译 WCDB 动态库及 Windows 导出定义文件。各平台的完整构建步骤见[项目 README](../README.md)。

## 来源与目录

预编译库版本标注为 [Tencent/wcdb](https://github.com/Tencent/wcdb) `v2.1.15`。更换依赖时，需要确认版本、目标架构和 C++ ABI 与封装层匹配。

| 路径 | 用途 |
| :--- | :--- |
| `macos/universal/libWCDB.dylib` | macOS WCDB 动态库；目录名不保证双架构 |
| `win32/x64/WCDB.dll`、`WCDB.def` | Windows x64 动态库及导出定义 |
| `win32/arm64/WCDB.dll`、`WCDB.def` | Windows ARM64 动态库及导出定义 |

macOS arm64 上，WeFlow 主程序默认从 `resources/wcdb/macos/universal/` 读取库，因此保留该目录名。实际库架构应通过 `lipo -archs` 检查；x64 系统上的主程序加载行为尚未确认。

Windows 构建前需要从对应的 `WCDB.def` 生成 `WCDB.lib`。更换 DLL 后，应重新提取 DEF 并生成导入库，具体命令见项目 README。

Linux 不在本目录提供预编译 WCDB 库。构建脚本固定获取 `v2.1.15` 源码并静态链接 WCDB，也可以通过 `WCDB_LIB_PATH` 链接已有动态库。

## 部署

| 平台 | 封装库 | WCDB 部署方式 | WeFlow 部署目录 |
| :--- | :--- | :--- | :--- |
| macOS arm64 | `libwcdb_api.dylib` | 同目录部署 `libWCDB.dylib` | `resources/wcdb/macos/universal/` |
| Windows x64 | `wcdb_api.dll` | 同目录部署 x64 `WCDB.dll` | `resources/wcdb/win32/x64/` |
| Windows ARM64 | `wcdb_api.dll` | 同目录部署 ARM64 `WCDB.dll` | `resources/wcdb/win32/arm64/` |
| Linux x64 / ARM64 | `libwcdb_api.so` | 静态链接时无需单独部署 WCDB；动态链接时按实际依赖名称部署 | `resources/wcdb/linux/x64/` 或 `resources/wcdb/linux/arm64/` |

以上只列出 WCDB 依赖。curl、zstd、MSVC 或 C/C++ 运行库等其他依赖是否需要另行部署，应以实际链接结果为准。发布前请按项目 README 中的验证步骤检查架构、动态依赖及宿主加载结果。
