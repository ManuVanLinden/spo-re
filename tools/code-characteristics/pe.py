"""Minimal PE32 reader (mmap based, low memory), and the default paths of the Spore folder and the EP1 exe.

Usage:   a library, imported by tools/manifest/build.py and the tools that share its EXE and GAME defaults."""
import mmap
import os
import struct

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# SPORE_WORK_DIR (default: the source tree) takes the generated outputs; SPORE_GAME_DIR / SPORE_EXE name the
# Spore folder / the EP1 exe instead of game/ (docs/design/convert.md 4).
GAME = os.environ.get("SPORE_GAME_DIR") or os.path.join(ROOT, "game")
EXE = os.environ.get("SPORE_EXE") or os.path.join(GAME, "SporebinEP1", "SporeApp.exe")


class PE:
    def __init__(self, path=EXE):
        self.f = open(path, "rb")
        self.d = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        o = pe + 24
        self.base = struct.unpack_from("<I", d, o + 28)[0]
        self.entry = self.base + struct.unpack_from("<I", d, o + 16)[0]
        self.dirs = [struct.unpack_from("<II", d, o + 96 + 8 * i) for i in range(16)]
        self.secs = []
        for i in range(nsec):
            name, vsz, va, rsz, raw = struct.unpack_from("<8sIIII", d, o + optsz + 40 * i)
            ch = struct.unpack_from("<I", d, o + optsz + 40 * i + 36)[0]
            self.secs.append((name.rstrip(b"\0").decode(), self.base + va, vsz, raw, rsz, ch))

    def sec(self, name):
        for s in self.secs:
            if s[0] == name:
                return s

    def secname(self, va):
        for n, sva, vsz, raw, rsz, ch in self.secs:
            if sva <= va < sva + max(vsz, rsz):
                return n
        return None

    def off(self, va):
        for n, sva, vsz, raw, rsz, ch in self.secs:
            if sva <= va < sva + rsz:
                return raw + va - sva
        return None

    def u32(self, va):
        o = self.off(va)
        return struct.unpack_from("<I", self.d, o)[0] if o is not None else None

    def cstr(self, va, n=200):
        o = self.off(va)
        if o is None:
            return None
        e = self.d.find(b"\0", o, o + n)
        return bytes(self.d[o:e if e >= 0 else o + n])

    def relocs(self):
        """Yield VA of every IMAGE_REL_BASED_HIGHLOW fixup; also counts other types."""
        rva, size = self.dirs[5]
        o = self.off(self.base + rva)
        end = o + size
        self.reloc_other = {}
        while o < end:
            page, bsz = struct.unpack_from("<II", self.d, o)
            if bsz == 0:
                break
            for i in range((bsz - 8) // 2):
                e = struct.unpack_from("<H", self.d, o + 8 + 2 * i)[0]
                t, off = e >> 12, e & 0xFFF
                if t == 3:
                    yield self.base + page + off
                else:
                    self.reloc_other[t] = self.reloc_other.get(t, 0) + 1
            o += bsz

    def imports(self):
        """Return {iat_va: 'dll!name'}."""
        rva, size = self.dirs[1]
        res = {}
        o = self.off(self.base + rva)
        while True:
            ilt, ts, fc, name, iat = struct.unpack_from("<IIIII", self.d, o)
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
                    fn = "#%d" % (v & 0xFFFF)
                else:
                    fn = self.cstr(self.base + v + 2).decode()
                res[self.base + iat + 4 * i] = dll.lower() + "!" + fn
                i += 1
            o += 20
        return res

DATA = os.path.join(ROOT, "data", "code-characteristics")
SCR = os.path.join(ROOT, "scratch", "code-characteristics")  # large intermediates, git-ignored
