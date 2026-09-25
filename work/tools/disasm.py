"""Disassemble gundam.exe address ranges: disasm.py START END [START END ...]"""
import struct, sys, capstone
EXE = __file__.rsplit('work', 1)[0] + 'source_exe_01/gundam.exe'
data = open(EXE, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
opt = struct.unpack_from('<H', data, pe + 20)[0]
base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + i * 40
    vsz, va, rsz, raw = struct.unpack_from('<IIII', data, o + 8)
    secs.append((base + va, max(vsz, rsz), raw))
def read(addr, n):
    for va, sz, raw in secs:
        if va <= addr < va + sz:
            return data[raw + addr - va: raw + addr - va + n]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
if __name__ == '__main__':
    a = sys.argv[1:]
    for s, e in zip(a[::2], a[1::2]):
        s, e = int(s, 16), int(e, 16)
        for ins in md.disasm(read(s, e - s), s):
            print(f'{ins.address:08x}  {ins.mnemonic} {ins.op_str}')
        print('----')
