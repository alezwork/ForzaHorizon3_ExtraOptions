"""Small static-analysis helpers for the FH2/FH3 Xbox One executables."""
import re, struct, sys, pickle, os
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

# Paths to the extracted Xbox One executables (Mount\forza_durango_release_final.exe).
FH2 = os.environ.get("FH2_EXE")
FH3 = os.environ.get("FH3_EXE")
CACHE = os.path.dirname(os.path.abspath(__file__))


class Image:
    def __init__(self, path):
        if not path:
            raise SystemExit("Set FH2_EXE / FH3_EXE to the game's forza_durango_release_final.exe")
        self.path = path
        self.data = open(path, "rb").read()
        self.pe = pefile.PE(data=self.data, fast_load=True)
        self.sections = [(s.Name.rstrip(b"\0").decode(errors="replace"), s.VirtualAddress,
                          max(s.Misc_VirtualSize, s.SizeOfRawData), s.PointerToRawData,
                          s.SizeOfRawData) for s in self.pe.sections]
        self.text = next(s for s in self.sections if s[0] == ".text")
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self._xrefs = None

    def off(self, rva):
        for name, va, vs, ra, rs in self.sections:
            if va <= rva < va + rs:
                return ra + rva - va
        return None

    def rva(self, off):
        for name, va, vs, ra, rs in self.sections:
            if ra <= off < ra + rs:
                return va + off - ra
        return None

    def read(self, rva, n):
        o = self.off(rva)
        return b'' if o is None else self.data[o:o + n]

    def dis(self, rva, n=0x80, out=True):
        lines = []
        for i in self.md.disasm(self.read(rva, n), rva):
            lines.append("%08x  %-24s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
        if out:
            print("\n".join(lines))
        return lines

    def strings(self, needle, wide=False):
        enc = needle.encode("utf-16le") if wide else needle.encode()
        res = []
        for m in re.finditer(re.escape(enc), self.data):
            r = self.rva(m.start())
            if r is not None:
                res.append(r)
        return res

    def xrefs(self):
        """Map target RVA -> list of instruction RVAs for RIP-relative operands in .text."""
        if self._xrefs is not None:
            return self._xrefs
        cache = os.path.join(CACHE, os.path.basename(os.path.dirname(os.path.dirname(self.path))).replace(" ", "_") + ".xrefs")
        if os.path.exists(cache):
            self._xrefs = pickle.load(open(cache, "rb"))
            return self._xrefs
        name, va, vs, ra, rs = self.text
        t = self.data[ra:ra + rs]
        x = {}
        # REX.W? 8D/8B/89/C7/80/83/39/3B/0F B6... with modrm mod=00 rm=101 (rip-relative)
        pat = re.compile(rb"(?:[\x40-\x4f])?(?:\x8d|\x8b|\x89|\x3b|\x39|\x80|\x83|\xc7|\xc6|\x0f\xb6|\x0f\x10|\x0f\x28|\x0f\x11|\x0f\x2e|\x0f\x2f|\xff)([\x05\x0d\x15\x1d\x25\x2d\x35\x3d])", re.S)
        for m in pat.finditer(t):
            p = m.end()
            disp = struct.unpack_from("<i", t, p)[0]
            ins_end = p + 4
            op0 = t[m.start():m.end()]
            if op0[-2:-1] in (b"\x80", b"\x83", b"\xc6"):
                ins_end += 1
            elif op0[-2:-1] == b"\xc7":
                ins_end += 4
            target = va + ins_end + disp
            x.setdefault(target, []).append(va + m.start())
        # E8 calls / E9 jumps
        for m in re.finditer(rb"[\xe8\xe9]", t):
            p = m.start()
            if p + 5 > len(t):
                break
            disp = struct.unpack_from("<i", t, p + 1)[0]
            target = va + p + 5 + disp
            if va <= target < va + rs:
                x.setdefault(target, []).append(va + p)
        pickle.dump(x, open(cache, "wb"))
        self._xrefs = x
        return x

    def refs_to(self, rva):
        return self.xrefs().get(rva, [])

    def func_start(self, rva):
        """Find the containing function using .pdata."""
        if not hasattr(self, "_pdata"):
            d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
            raw = self.read(d.VirtualAddress, d.Size)
            self._pdata = sorted(struct.unpack_from("<III", raw, i)[:2] for i in range(0, len(raw) - 11, 12))
        import bisect
        i = bisect.bisect_right(self._pdata, (rva, 0xffffffff)) - 1
        if i >= 0 and self._pdata[i][0] <= rva < self._pdata[i][1]:
            return self._pdata[i]
        return None

    def cstr(self, rva, n=200):
        b = self.read(rva, n)
        return b.split(b"\0")[0].decode(errors="replace")


if __name__ == "__main__":
    pass
