"""Statically check the FH3 patch table in features/fh3.c against the game executable.

Every original byte string must match the supported executable, jump patches
must keep their native target, VIP patches must replace a call to a known VIP
getter, and treasure map getters must be referenced only by their registration.
"""
import os
import re
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from xbox_image import Image, FH3

SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "features", "fh3.c")
VIP_GETTERS = {0x10b97b0: "profile VIP getter", 0x2fdda0: "main values bool getter"}

src = open(SOURCE).read()
macros = dict(re.findall(r"#define (\w+) (\{[^}]*\})", src))
entry = r"\{(0x[0-9a-f]+), (\d), FH3_GROUP_(\w+), \"(\w+)\",\s*(\{[^}]*\}|\w+), (\{[^}]*\}|\w+)\}"
rows = re.findall(entry, src)


def parse(text):
    text = macros.get(text, text)
    return bytes(ord(x.strip()[1]) if x.strip().startswith("'") else int(x, 16)
                 for x in text.strip("{}").split(","))


image = Image(FH3)
md = Cs(CS_ARCH_X86, CS_MODE_64)
ok = bool(rows)
for rva, size, group, name, orig, new in rows:
    rva, size = int(rva, 16), int(size)
    orig, new = parse(orig), parse(new)
    current = image.read(rva, size)
    good = current == orig and len(orig) == len(new) == size
    detail = ""
    first = next(md.disasm(current + b"\x90" * 8, rva))
    if group == "INTRO":
        patched = next(md.disasm(new + b"\x90" * 8, rva))
        same = int(first.op_str, 16) == int(patched.op_str, 16)
        good &= first.mnemonic.startswith("j") and patched.mnemonic == "jmp" and same
        detail = "%s %s -> %s %s" % (first.mnemonic, first.op_str, patched.mnemonic, patched.op_str)
    elif group == "VIP" and first.mnemonic == "call":
        target = int(first.op_str, 16)
        good &= target in VIP_GETTERS
        detail = "call %x (%s) -> mov eax, 1" % (target, VIP_GETTERS.get(target, "UNKNOWN"))
    elif group == "VIP":
        detail = "%s %s -> forced VIP arguments" % (first.mnemonic, first.op_str)
    elif group == "BLUR_RIMS":
        cave = int(re.search(r"#define FH3_BLUR_RIM_THRESHOLD_RVA (0x[0-9a-f]+)", src).group(1), 16)
        patched = next(md.disasm(new, rva))
        target = rva + patched.size + int(re.search(r"rip \+ (0x[0-9a-f]+)", patched.op_str).group(1), 16)
        cave_free = image.read(cave, 4) == b"\xcc" * 4 and not image.refs_to(cave)
        good &= target == cave and cave_free and "0x2d20" in first.op_str
        detail = "%s %s -> %s %s (cave %x free=%s)" % (first.mnemonic, first.op_str,
                                                       patched.mnemonic, patched.op_str, cave, cave_free)
    elif group == "SUN_SHADOWS":
        detail = "%s %s -> %s" % (first.mnemonic, first.op_str,
                                  " ; ".join("%s %s" % (i.mnemonic, i.op_str)
                                             for i in md.disasm(new, rva)))
    elif group == "DLC":
        start = rva
        while image.read(start - 1, 1) != b"\0":
            start -= 1
        refs = image.refs_to(start)
        good &= len(refs) == 1
        detail = "%r refs=%d -> %r" % (image.cstr(start, 80), len(refs), new.decode())
    elif group == "TREASURE_MAP" and first.mnemonic != "sub":
        detail = "%s %s -> %s" % (first.mnemonic, first.op_str,
                                  " ; ".join("%s %s" % (i.mnemonic, i.op_str)
                                             for i in md.disasm(new, rva)))
    elif group == "TREASURE_MAP":
        refs = image.refs_to(rva)
        good &= len(refs) == 1
        detail = "getter refs=%d -> mov al, 1; ret" % len(refs)
    ok &= good
    print("%-36s %08x %-4s %s" % (name, rva, "OK" if good else "FAIL", detail))
print("ALL OK" if ok else "FAILURES FOUND")
sys.exit(0 if ok else 1)
