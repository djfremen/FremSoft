"""Patch the auto-generated forwarders.c for MSVC.

GCC/MinGW supports __attribute__((naked)) + GCC inline asm.
MSVC needs __declspec(naked) + __asm { ... } syntax.

Run after gen_shim.py, before build_msvc.bat (build_msvc.bat
also calls this for safety so manual ordering doesn't matter).
"""
import re
from pathlib import Path

path = Path(__file__).parent.parent / "src" / "forwarders.c"
content = path.read_text()

content = content.replace("__attribute__((naked))", "__declspec(naked)")

pattern = re.compile(
    r'__asm__\s*__volatile__\s*\(\s*"jmp\s*\*%0\\n"\s*:\s*:\s*"m"\s*\((g_real_[a-zA-Z0-9_]+)\)\s*\);'
)
content = pattern.sub(r'__asm { jmp dword ptr [\1] }', content)

path.write_text(content)
print(f"forwarders.c patched for MSVC: {path}")
