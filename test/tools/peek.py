# peek.py <macho> <vmaddr> <count32>: dump 32-bit LE words at a VM address
import struct, sys
d = open(sys.argv[1], 'rb').read()
addr = int(sys.argv[2], 16); n = int(sys.argv[3])
ncmds = struct.unpack('<I', d[16:20])[0]; off = 28
for _ in range(ncmds):
    cmd, size = struct.unpack('<II', d[off:off+8])
    if cmd == 1:
        vm, vs, fo, fs = struct.unpack('<IIII', d[off+24:off+40])
        if vm <= addr < vm + fs:
            p = fo + addr - vm
            words = struct.unpack('<%dI' % n, d[p:p+4*n])
            print(' '.join('%08x(%s)' % (w, struct.pack('>I', w).decode('latin1') if all(32 <= b < 127 for b in struct.pack('>I', w)) else '') for w in words))
    off += size
