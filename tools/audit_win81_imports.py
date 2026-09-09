"""Inventory PE imports and reject known Windows-10-only startup dependencies.

This static guard complements, and does not replace, execution on Windows 8.1.
"""
from pathlib import Path
import json
import struct
import sys

class PE:
    def __init__(self, path):
        self.data = path.read_bytes()
        pe = self.u32(0x3c)
        assert self.data[pe:pe+4] == b'PE\0\0'
        self.machine = self.u16(pe+4)
        count = self.u16(pe+6)
        opt_size = self.u16(pe+20)
        opt = pe+24
        assert self.u16(opt) == 0x20b
        self.subsystem_version = [self.u16(opt+48), self.u16(opt+50)]
        self.directories = opt+112
        self.sections = []
        for i in range(count):
            s = opt+opt_size+i*40
            self.sections.append((self.u32(s+12), max(self.u32(s+8), self.u32(s+16)), self.u32(s+20)))
    def u16(self, off): return struct.unpack_from('<H', self.data, off)[0]
    def u32(self, off): return struct.unpack_from('<I', self.data, off)[0]
    def offset(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va+size: return raw+rva-va
        raise ValueError(hex(rva))
    def string(self, rva):
        off = self.offset(rva)
        return self.data[off:self.data.index(b'\0', off)].decode('ascii')
    def imports(self, delay=False):
        rva = self.u32(self.directories+(13 if delay else 1)*8)
        if not rva: return {}
        pos = self.offset(rva)
        result = {}
        while self.u32(pos+(4 if delay else 12)):
            name = self.string(self.u32(pos+(4 if delay else 12)))
            thunk = self.u32(pos+16) if delay else self.u32(pos) or self.u32(pos+16)
            addr = self.offset(thunk)
            entries = []
            while value := struct.unpack_from('<Q', self.data, addr)[0]:
                entries.append('#'+str(value & 0xffff) if value >> 63 else self.string(value+2))
                addr += 8
            result[name] = entries
            pos += 32 if delay else 20
        return result

def main():
    directory = Path(sys.argv[1] if len(sys.argv)>1 else 'build-win81')
    names = ['pulse.exe','Pulse.Index.exe','Pulse.Preview.exe','pulse_shell.exe','lumatext.dll']
    forbidden = {
        'GetDpiForWindow','GetSystemMetricsForDpi','SetProcessDpiAwarenessContext',
        'SetThreadDpiAwarenessContext','AdjustWindowRectExForDpi','GetDpiForSystem',
        'GetThreadDescription','SetThreadDescription','GetSystemCpuSetInformation',
        'GetTempPath2W','GetTempPath2A','VirtualAlloc2','MapViewOfFile3',
        'IsWow64Process2','GetProcessInformation','SetProcessInformation',
    }
    result = []
    failed = False
    for name in names:
        pe = PE(directory/name)
        imports, delayed = pe.imports(), pe.imports(True)
        bad = sorted({entry for entries in imports.values() for entry in entries} & forbidden)
        runtimes = [dll for dll in imports if dll.lower().startswith(('msvcp','vcruntime'))]
        runtimes += [dll for dll in imports if dll.lower().startswith('api-ms-win-shcore-')]
        errors = bad + runtimes
        if name == 'pulse.exe' and 'lumatext.dll' in {dll.lower() for dll in imports}:
            errors.append('lumatext.dll must be delay-loaded')
        if pe.machine != 0x8664 or pe.subsystem_version > [6,3]: errors.append('PE target exceeds Windows 8.1 x64')
        failed |= bool(errors)
        result.append(dict(file=name, subsystem=pe.subsystem_version, imports=imports,
                           delay_imports=delayed, errors=errors))
        print(name+': '+('FAIL '+str(errors) if errors else 'PASS startup import guard'))
    dest = Path('bench_data/win81_imports.json')
    dest.write_text(json.dumps(result, indent=2), encoding='utf-8')
    return int(failed)

if __name__ == '__main__': sys.exit(main())
