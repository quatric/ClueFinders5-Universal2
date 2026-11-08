# Print the first opcodes of each PICT: pictops.py <rsrcfile>
import struct, sys
d = open(sys.argv[1], 'rb').read()
doff, moff, dlen, mlen = struct.unpack('>IIII', d[:16])
m = d[moff:moff + mlen]
toff = struct.unpack('>H', m[24:26])[0]
ntypes = struct.unpack('>H', m[toff:toff + 2])[0] + 1
ops = {}
for i in range(ntypes):
    t, n, ref = struct.unpack('>4sHH', m[toff + 2 + i * 8: toff + 10 + i * 8])
    if t != b'PICT': continue
    for j in range(n + 1):
        rid, nameoff, ao = struct.unpack('>hHI', m[toff + ref + j * 12: toff + ref + j * 12 + 8])
        off = doff + (ao & 0xffffff)
        size = struct.unpack('>I', d[off:off+4])[0]
        p = d[off+4: off+4+size]
        # v2 header at 10: 0011 02ff 0c00 ...; scan opcodes roughly after header (40 bytes)
        seq = []
        pos = 10
        while pos < len(p) - 2 and len(seq) < 12:
            op = struct.unpack('>H', p[pos:pos+2])[0]; seq.append('%04x' % op)
            pos += 2
            if op == 0x0011: pos += 2
            elif op == 0x0c00: pos += 24
            elif op == 0x001e: pass
            elif op == 0x0001: rl = struct.unpack('>H', p[pos:pos+2])[0]; pos += rl
            elif op in (0x00a0,): pos += 2
            elif op == 0x00a1: pos += 2; l = struct.unpack('>H', p[pos:pos+2])[0]; pos += 2 + l
            else: break
        ops[rid] = (size, struct.unpack('>4h', p[2:10]), ' '.join(seq))
for k, v in sorted(ops.items())[:400]:
    print(k, v)
