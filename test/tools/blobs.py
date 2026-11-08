# blobs.py <png> r g b tol [x0 y0 x1 y1]: centers of connected pixel blobs of a color
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
r, g, b, tol = map(int, sys.argv[2:6])
x0, y0, x1, y1 = map(int, sys.argv[6:10]) if len(sys.argv) > 9 else (0, 0, im.width, im.height)
px = im.load(); seen = set(); out = []
for y in range(y0, y1):
    for x in range(x0, x1):
        if (x, y) in seen: continue
        c = px[x, y]
        if abs(c[0]-r) > tol or abs(c[1]-g) > tol or abs(c[2]-b) > tol: continue
        stack = [(x, y)]; seen.add((x, y)); pts = []
        while stack:
            a, bb = stack.pop(); pts.append((a, bb))
            for n in ((a+1, bb), (a-1, bb), (a, bb+1), (a, bb-1)):
                if x0 <= n[0] < x1 and y0 <= n[1] < y1 and n not in seen:
                    cc = px[n]
                    if abs(cc[0]-r) <= tol and abs(cc[1]-g) <= tol and abs(cc[2]-b) <= tol:
                        seen.add(n); stack.append(n)
        if len(pts) >= 15:
            out.append((len(pts), sum(p[0] for p in pts)//len(pts), sum(p[1] for p in pts)//len(pts)))
for o in sorted(out, reverse=True): print(*o)
