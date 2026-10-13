#!/usr/bin/env bash
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
generate_win_import_def.py
从 Windows PE32+ (DLL) 导出表生成模块定义文件 (.def)，
用于通过 MSVC `lib.exe /def:<output.def> /out:<output.lib>` 生成链接导入库，
使项目在无需底层源码的情况下直接与预编译 DLL 链接。
"""

import os
import sys

def print_help():
    print("""
========================================================================
Windows DLL 导入定义 (.def) 文件提取工具
========================================================================

【用途】
  解析指定 Windows DLL 的导出目录表 (Export Address Table)，
  提取出所有的导出函数名与序号 (Ordinal)，生成模块定义文件 (.def)。
  随后可配合 MSVC 的 `lib.exe` 工具生成对应的 `.lib` 导入库。

【运行方式】
  1. 推荐方式 (使用 uv，自动拉取 pefile 依赖，无需污染系统环境)：
     uv run --with pefile python scripts/generate_win_import_def.py <input.dll> [output.def]

  2. 传统 pip 方式：
     pip install pefile
     python scripts/generate_win_import_def.py <input.dll> [output.def]

【参数说明】
  <input.dll>   必选。输入的 Windows 动态库路径 (例如 deps/win32/x64/WCDB.dll)。
  [output.def]  可选。输出的 .def 文件路径。若省略，则默认在 DLL 同级目录下生成同名 .def 文件。

【使用示例】
  # 提取 x64 WCDB.dll 的导出定义：
  uv run --with pefile python scripts/generate_win_import_def.py deps/win32/x64/WCDB.dll deps/win32/x64/WCDB.def

  # 提取 ARM64 WCDB.dll 的导出定义：
  uv run --with pefile python scripts/generate_win_import_def.py deps/win32/arm64/WCDB.dll deps/win32/arm64/WCDB.def

  # 随后在 Visual Studio 开发者命令提示符下生成 .lib：
  lib /def:deps/win32/x64/WCDB.def /machine:x64 /out:deps/win32/x64/WCDB.lib
========================================================================
""")

def generate_def(dll_path, def_path):
    if not os.path.exists(dll_path):
        print(f"[错误] 找不到输入的 DLL 文件: {dll_path}")
        return False

    try:
        import pefile
    except ImportError:
        print("[错误] 未检测到 Python 库 `pefile`。")
        print("建议使用 uv 运行（自动处理依赖）：")
        print(f"  uv run --with pefile python {sys.argv[0]} {dll_path} {def_path}")
        print("或先手动安装：")
        print("  pip install pefile")
        return False

    try:
        print(f"[*] 正在解析 PE 文件: {dll_path} ...")
        pe = pefile.PE(dll_path)
    except Exception as e:
        print(f"[错误] 无法解析 PE 文件: {e}")
        return False

    if not hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        print(f"[错误] 文件 {dll_path} 中不包含任何导出符号目录 (Export Directory)！")
        return False

    dll_name = os.path.basename(dll_path)
    symbols = pe.DIRECTORY_ENTRY_EXPORT.symbols
    print(f"[*] 成功解析导出目录，共找到 {len(symbols)} 个导出符号。")

    out_dir = os.path.dirname(def_path)
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    with open(def_path, "w", encoding="utf-8") as f:
        f.write(f"LIBRARY \"{dll_name}\"\n")
        f.write("EXPORTS\n")
        for sym in symbols:
            if sym.name:
                name = sym.name.decode("utf-8", errors="replace")
                f.write(f"    {name} @{sym.ordinal}\n")
            else:
                f.write(f"    ordinal_{sym.ordinal} @{sym.ordinal} NONAME\n")

    print(f"[+] 成功生成 DEF 文件: {def_path}")
    print(f"[*] 后续在 VS 开发者终端中运行以下命令即可生成 .lib 导入库：")
    machine = "x64" if pe.FILE_HEADER.Machine == 0x8664 else ("arm64" if pe.FILE_HEADER.Machine == 0xAA64 else "x86")
    lib_path = os.path.splitext(def_path)[0] + ".lib"
    print(f"    lib /def:{def_path} /machine:{machine} /out:{lib_path}")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print_help()
        sys.exit(0)

    input_dll = sys.argv[1]
    if len(sys.argv) >= 3:
        output_def = sys.argv[2]
    else:
        output_def = os.path.splitext(input_dll)[0] + ".def"

    success = generate_def(input_dll, output_def)
    sys.exit(0 if success else 1)
