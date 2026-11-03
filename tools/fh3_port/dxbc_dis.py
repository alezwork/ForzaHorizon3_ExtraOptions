"""Disassemble DXBC blobs embedded in Forza shader files with the system d3dcompiler."""
import ctypes, sys
d3d = ctypes.WinDLL('d3dcompiler_47.dll')
class Blob(ctypes.Structure): pass
def disasm(data):
    i = data.find(b'DXBC')
    if i < 0: return None
    size = int.from_bytes(data[i+24:i+28], 'little')
    buf = ctypes.create_string_buffer(data[i:i+size], size)
    out = ctypes.c_void_p()
    hr = d3d.D3DDisassemble(buf, ctypes.c_size_t(size), 0, None, ctypes.byref(out))
    if hr != 0: return 'hr=%x' % (hr & 0xffffffff)
    vt = ctypes.cast(out, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    getptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    getsize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    return ctypes.string_at(getptr(out), getsize(out)).decode(errors='replace')
if __name__ == '__main__':
    print(disasm(open(sys.argv[1], 'rb').read()))
