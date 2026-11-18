"""Bounded PE dependency reader, adapted from tools/audit_win81_imports.py.
Only names are inventoried; no module is loaded or executed.
"""
import re
import struct
from pathlib import Path

class PEError(ValueError):
    pass

NAME = re.compile(r"[A-Za-z0-9_.-]+\.(?:dll|drv)", re.IGNORECASE)

class PE:
    def __init__(self, path):
        if Path(path).stat().st_size > 512 * 1024 * 1024:
            raise PEError("image exceeds bound")
        self.data = Path(path).read_bytes()
        if len(self.data) > 512 * 1024 * 1024:
            raise PEError("image exceeds bound")
        if self.data[:2] != b'MZ':
            raise PEError("DOS signature")
        pe = self.u32(0x3c)
        if self.slice(pe, 4) != b'PE\0\0':
            raise PEError("PE signature")
        self.machine = self.u16(pe + 4)
        count, opt_size = self.u16(pe + 6), self.u16(pe + 20)
        opt = pe + 24
        self.slice(opt, opt_size)
        magic = self.u16(opt)
        if (self.machine, magic) == (0x8664, 0x20b):
            directory_offset, count_offset = 112, 108
            self.image_base = self.u64(opt + 24)
        elif (self.machine, magic) == (0x14c, 0x10b):
            directory_offset, count_offset = 96, 92
            self.image_base = self.u32(opt + 28)
        else:
            raise PEError("unsupported or inconsistent machine/magic")
        if not 1 <= count <= 96 or opt_size < directory_offset:
            raise PEError("section/optional-header bound")
        self.header_size = self.u32(opt + 60)
        if not self.header_size or self.header_size > len(self.data):
            raise PEError("header size")
        directories = self.u32(opt + count_offset)
        if directories > 16 or directory_offset + directories * 8 > opt_size:
            raise PEError("directory count")
        self.directories = [(self.u32(opt + directory_offset + i * 8),
                             self.u32(opt + directory_offset + i * 8 + 4)) for i in range(directories)]
        if opt + opt_size + count * 40 > self.header_size:
            raise PEError("section table exceeds headers")
        self.sections = []
        for i in range(count):
            s = opt + opt_size + i * 40
            self.slice(s, 40)
            va, raw_size, raw = self.u32(s + 12), self.u32(s + 16), self.u32(s + 20)
            if raw_size:
                self.slice(raw, raw_size)
                if raw < self.header_size:
                    raise PEError("section overlaps headers")
                if any(va < old_va + old_size and old_va < va + raw_size for old_va, old_size, _ in self.sections):
                    raise PEError("overlapping sections")
            self.sections.append((va, raw_size, raw))
        self.normal = self.imports(False)
        self.delay = self.imports(True)

    def slice(self, offset, size):
        if offset < 0 or size < 0 or offset > len(self.data) or size > len(self.data) - offset:
            raise PEError("truncated image")
        return self.data[offset:offset + size]

    def u16(self, off): return struct.unpack('<H', self.slice(off, 2))[0]
    def u32(self, off): return struct.unpack('<I', self.slice(off, 4))[0]
    def u64(self, off): return struct.unpack('<Q', self.slice(off, 8))[0]

    def offset(self, rva, size):
        if rva < self.header_size and size <= self.header_size - rva:
            return rva
        matches = [raw + rva - va for va, count, raw in self.sections if va <= rva and size <= count - (rva - va)]
        if len(matches) != 1:
            raise PEError("unmapped/ambiguous RVA")
        return matches[0]

    def string(self, rva):
        value = bytearray()
        for i in range(261):
            c = self.data[self.offset(rva + i, 1)]
            if not c:
                try: text = value.decode('ascii')
                except UnicodeDecodeError as error: raise PEError("non-ASCII DLL name") from error
                if not NAME.fullmatch(text): raise PEError("unsafe DLL dependency name")
                return text.lower()
            value.append(c)
        raise PEError("unterminated DLL name")

    def imports(self, delay):
        index = 13 if delay else 1
        if index >= len(self.directories): return []
        rva, size = self.directories[index]
        if not rva and not size: return []
        stride = 32 if delay else 20
        if not rva or size < stride or size > 1024 * 1024: raise PEError("import directory bound")
        names = set()
        for step in range(0, size - stride + 1, stride):
            pos = self.offset(rva + step, stride)
            values = struct.unpack('<' + 'I' * (stride // 4), self.slice(pos, stride))
            if not any(values): return sorted(names)
            name = values[1 if delay else 3]
            if delay:
                if values[0] not in (0, 1): raise PEError("unknown delay attributes")
                if not values[0]:
                    if name < self.image_base: raise PEError("invalid delay VA")
                    name -= self.image_base
            if not name: raise PEError("missing dependency name")
            names.add(self.string(name))
        raise PEError("import directory lacks terminator")

