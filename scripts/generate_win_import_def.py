#!/usr/bin/env python3
"""
Generate Windows Module-Definition (.def) file from a PE32+ DLL.
Used to create import libraries (.lib) using MSVC `lib.exe /def:WCDB.def /out:WCDB.lib`
without needing full upstream WCDB source code.
"""

import sys
import os

try:
    import pefile
except ImportError:
    print("pefile not installed. Run via `uv run --with pefile python generate_win_import_def.py`")
    sys.exit(1)

def generate_def(dll_path, def_path):
    if not os.path.exists(dll_path):
        print(f"Error: DLL not found: {dll_path}")
        return False

    pe = pefile.PE(dll_path)
    if not hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        print(f"Error: No export directory in {dll_path}")
        return False

    dll_name = os.path.basename(dll_path)
    print(f"Reading exports from {dll_path}...")
    symbols = pe.DIRECTORY_ENTRY_EXPORT.symbols
    print(f"Found {len(symbols)} exported symbols.")

    with open(def_path, "w", encoding="utf-8") as f:
        f.write(f"LIBRARY \"{dll_name}\"\n")
        f.write("EXPORTS\n")
        for sym in symbols:
            if sym.name:
                name = sym.name.decode("utf-8")
                f.write(f"    {name} @{sym.ordinal}\n")
            else:
                f.write(f"    ordinal_{sym.ordinal} @{sym.ordinal} NONAME\n")

    print(f"Successfully generated DEF file: {def_path}")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: uv run --with pefile python generate_win_import_def.py <input.dll> <output.def>")
        sys.exit(1)

    generate_def(sys.argv[1], sys.argv[2])
