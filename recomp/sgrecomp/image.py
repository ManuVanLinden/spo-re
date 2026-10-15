"""Guest image access for the translator: a PE32 file mapped by section, or a flat blob.

Only bytes are read here; nothing is executed.
"""
import mmap
import re
import struct


def imp_symbol(dll, name, ordinal):
    """Host symbol of an import shim, per runtime-abi.md 5.1: sg_imp_<dll stem>_<export|ordN>.

    The DLL stem is lower-case ("KERNEL32.dll" -> kernel32). Export names that are not C
    identifiers (two MSVCR90 C++ exports such as "?terminate@@YAXXZ") have every other
    character replaced by '_'. tools/hle/gen_thunks.py generates the shims and must use the same rule.
    """
    stem = dll.rsplit(".", 1)[0].lower()
    stem = re.sub(r"[^0-9A-Za-z_]", "_", stem)
    if name:
        nm = re.sub(r"[^0-9A-Za-z_]", "_", name)
    else:
        nm = "ord%d" % ordinal
    return "sg_imp_%s_%s" % (stem, nm)


class Image:
    """Common interface: read(va, n) -> bytes (b'' outside the image), imports {iat_va: symbol}."""

    imports = {}
    text = (0, 0)

    def read(self, va, n):
        raise NotImplementedError

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else None


class FlatImage(Image):
    """A fixture: code and data bytes at fixed guest addresses."""

    def __init__(self, chunks, imports=None):
        self.chunks = sorted(chunks)  # [(va, bytes)]
        self.imports = imports or {}
        lo = min(va for va, _ in self.chunks)
        hi = max(va + len(b) for va, b in self.chunks)
        self.text = (lo, hi)

    def read(self, va, n):
        for base, b in self.chunks:
            if base <= va < base + len(b):
                o = va - base
                return bytes(b[o:o + n])
        return b""


class PEImage(Image):
    def __init__(self, path):
        self.f = open(path, "rb")
        self.d = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        o = pe + 24
        self.base = struct.unpack_from("<I", d, o + 28)[0]
        self.dirs = [struct.unpack_from("<II", d, o + 96 + 8 * i) for i in range(16)]
        self.secs = []
        for i in range(nsec):
            name, vsz, va, rsz, raw = struct.unpack_from("<8sIIII", d, o + optsz + 40 * i)
            self.secs.append((name.rstrip(b"\0").decode(), self.base + va, vsz, raw, rsz))
        t = [s for s in self.secs if s[0] == ".text"][0]
        self.text = (t[1], t[1] + t[2])
        self.imports = self._imports()

    def read(self, va, n):
        for name, sva, vsz, raw, rsz in self.secs:
            if sva <= va < sva + rsz:
                o = raw + va - sva
                n = min(n, sva + rsz - va)
                return bytes(self.d[o:o + n])
        return b""

    def cstr(self, va, n=256):
        b = self.read(va, n)
        return b.split(b"\0", 1)[0]

    def _imports(self):
        rva, size = self.dirs[1]
        res = {}
        va = self.base + rva
        while True:
            ilt, ts, fc, name, iat = struct.unpack("<IIIII", self.read(va, 20))
            if name == 0:
                break
            dll = self.cstr(self.base + name).decode()
            thunk = ilt or iat
            i = 0
            while True:
                v = self.u32(self.base + thunk + 4 * i)
                if not v:
                    break
                if v & 0x80000000:
                    sym = imp_symbol(dll, None, v & 0xFFFF)
                else:
                    sym = imp_symbol(dll, self.cstr(self.base + v + 2).decode(), None)
                res[self.base + iat + 4 * i] = sym
                i += 1
            va += 20
        return res
