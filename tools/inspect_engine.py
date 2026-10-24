"""Read-only PE/address-library inspection for the SPS Fallout port."""
import argparse
import json
import struct
from pathlib import Path

import capstone
import pefile

LIB = Path(r'C:\Development\Tools\BethesdaGhidraScripts\addresslibrary\f4')
GAMES = {
    'og': Path(r'C:\Games\Fallout.4 1.10.163\Fallout4.exe'),
    'ae': Path(r'C:\Games\Steam\steamapps\common\Fallout 4\Fallout4.exe'),
    # Next-Gen 1.10.984: no installed copy; this is the user's Steam executable with its SteamStub
    # wrapper removed, read here for code bytes only (the wrapped file's code is encrypted).
    'ng': Path(r'C:\Games\Steam\steamapps\common\Fallout4.exe.unpacked.exe'),
    # VR 1.2.72: the installed executable's code is SteamStub-encrypted; this copy next to it has the
    # wrapper removed (same version and section layout), read for code bytes only.
    'vr': Path(r'C:\Games\Steam\steamapps\common\Fallout 4 VR\Fallout4VR.exe.unpacked.exe'),
}


class Engine:
    def __init__(self, path):
        self.path = Path(path)
        self.pe = pefile.PE(str(path), fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
        v = self.pe.VS_FIXEDFILEINFO[0]
        self.version = (v.FileVersionMS >> 16, v.FileVersionMS & 65535,
                        v.FileVersionLS >> 16, v.FileVersionLS & 65535)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        db = LIB / ('version-' + '-'.join(map(str, self.version)) + '.bin')
        self.mapping = dict(struct.iter_unpack('<QQ', db.read_bytes()[8:])) if db.exists() else {}
        self.dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.dis.detail = True
        self.text = next(s for s in self.pe.sections if s.Name.rstrip(b'\0') == b'.text')
        self.code = self.text.get_data()

    def read(self, rva, size):
        return self.pe.get_data(rva, size)

    def instructions(self, rva, size=256):
        return list(self.dis.disasm(self.read(rva, size), self.base + rva))

    def show(self, rva, size=256):
        print(f'RVA {rva:#x}')
        for ins in self.instructions(rva, size):
            print(f'  {ins.address - self.base:08x}: {ins.mnemonic:8} {ins.op_str}')

    def vtable(self, ident, count=32):
        rva = self.mapping.get(ident, ident)
        print(f'VTABLE {ident} RVA {rva:#x}')
        for i, (address,) in enumerate(struct.iter_unpack('<Q', self.read(rva, count*8))):
            body = self.instructions(address-self.base, 12) if self.base <= address < self.base+self.pe.OPTIONAL_HEADER.SizeOfImage else []
            print(f'  {i:02x}: {address-self.base:#x} ' + '; '.join(f'{x.mnemonic} {x.op_str}' for x in body[:3]))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('runtime', choices=GAMES)
    p.add_argument('mode', choices=['vtable', 'disasm'])
    p.add_argument('address', type=lambda x: int(x, 0))
    p.add_argument('--size', type=int, default=256)
    a = p.parse_args()
    e = Engine(GAMES[a.runtime])
    print('.'.join(map(str, e.version)))
    if a.mode == 'vtable':
        e.vtable(a.address, a.size)
    else:
        e.show(a.address, a.size)
