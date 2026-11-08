# List resource types in a resource-fork file: rsrc.py <file> [type]
import struct, sys, collections
d = open(sys.argv[1], 'rb').read()
doff, moff, dlen, mlen = struct.unpack('>IIII', d[:16])
m = d[moff:moff + mlen]
toff, noff = struct.unpack('>HH', m[24:28])
ntypes = struct.unpack('>H', m[toff:toff + 2])[0] + 1
out = collections.OrderedDict()
for i in range(ntypes):
    t, n, ref = struct.unpack('>4sHH', m[toff + 2 + i * 8: toff + 10 + i * 8])
    items = []
    for j in range(n + 1):
        rid, nameoff, attr_off = struct.unpack('>hHI', m[toff + ref + j * 12: toff + ref + j * 12 + 8])
        off = attr_off & 0xffffff
        size = struct.unpack('>I', d[doff + off: doff + off + 4])[0]
        name = ''
        if nameoff != 0xffff:
            l = m[noff + nameoff]; name = m[noff + nameoff + 1: noff + nameoff + 1 + l].decode('mac_roman')
        items.append((rid, name, size, doff + off + 4))
    out[t.decode('mac_roman')] = items
want = sys.argv[2] if len(sys.argv) > 2 else None
for t, items in out.items():
    if want and t != want: continue
    print('%s x%d %d bytes' % (t, len(items), sum(i[2] for i in items)), [(i[0], i[1], i[2]) for i in items[:6]] if want or len(items) < 4 else '')
