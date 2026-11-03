"""Resolve MSVC RTTI class names for functions via vtables, and walk callers."""
import re, struct
from xbox_image import Image, FH2, FH3

def vtable_owners(img, func):
    base = img.pe.OPTIONAL_HEADER.ImageBase
    out = []
    for m in re.finditer(re.escape(struct.pack('<Q', base + func)), img.data):
        slot = img.rva(m.start())
        if slot is None: continue
        # walk back to vtable start: previous qword is COL pointer when it points into rdata COL (signature 1)
        p = slot
        for _ in range(400):
            prev = struct.unpack_from('<Q', img.data, img.off(p - 8))[0] - base
            col_off = img.off(prev) if 0 < prev < 0x7fffffff else None
            if col_off is not None and struct.unpack_from('<I', img.data, col_off)[0] == 1:
                td = struct.unpack_from('<I', img.data, col_off + 12)[0]
                name = img.cstr(td + 0x10, 200)
                if name.startswith('.?A'):
                    out.append((name, (slot - p) // 8, hex(p)))
                    break
            p -= 8
    return out

def callers(img, func):
    return [c for c in img.refs_to(func) if img.read(c, 1) in (b'\xe8', b'\xe9')]

def describe(img, func, depth=3, seen=None):
    seen = seen or set()
    if func in seen or depth < 0: return
    seen.add(func)
    owners = vtable_owners(img, func)
    print('  ' * (3 - depth) + 'func %x owners=%s' % (func, owners[:3]))
    for c in callers(img, func)[:4]:
        f = img.func_start(c)
        if f: describe(img, f[0], depth - 1, seen)

if __name__ == '__main__':
    a = Image(FH2)
    for r in (0x1131c02, 0x1153e5a, 0x11556b5):
        print('== FH2 patch', hex(r)); describe(a, a.func_start(r)[0])
