// Palette Manager. The screen is always direct color, so palettes only
// matter as a source of colors for indexed GWorlds (Palette2CTab).
#include "emu.h"
#include "heap.h"
#include "qd.h"

#include <algorithm>

namespace {

// Palette: pmEntries (2), then fields up to 16; ColorInfo entries of 16
// bytes each: ciRGB (6), ciUsage (2), ciTolerance (2), private (6)
const uint32_t kHeader = 16, kEntry = 16;

uint32_t seedCounter = 0x1000;

int entries(gaddr pal) { return pal && heap::deref(pal) ? (int16_t)emu::r16(heap::deref(pal)) : 0; }
gaddr entryAddr(gaddr pal, int i) { return heap::deref(pal) + kHeader + kEntry * (uint32_t)i; }

} // namespace

IMPORT(NewPalette) {
	// (entries, srcColors CTabHandle, srcUsage, srcTolerance)
	int n = c.sarg16(0);
	gaddr pal = heap::newHandle(kHeader + kEntry * (uint32_t)std::max(n, 0), true);
	emu::w16(heap::deref(pal), (uint16_t)n);
	gaddr src = c.arg(1);
	for (int i = 0; i < n; i++) {
		gaddr e = entryAddr(pal, i);
		emu::w16(e + 6, (uint16_t)c.arg(2));
		emu::w16(e + 8, (uint16_t)c.arg(3));
		if (src && heap::deref(src) && i <= (int16_t)emu::r16(heap::deref(src) + 6))
			qd::writeRGB(e, qd::readRGB(heap::deref(src) + 8 + 8 * (uint32_t)i + 2));
	}
	c.ret(pal);
}
IMPORT(DisposePalette) { heap::disposeHandle(c.arg(0)); }
IMPORT(SetEntryColor) {
	if (c.sarg16(1) >= 0 && c.sarg16(1) < entries(c.arg(0)))
		qd::writeRGB(entryAddr(c.arg(0), c.sarg16(1)), qd::readRGB(c.arg(2)));
}
IMPORT(GetEntryColor) {
	qd::RGB rgb;
	if (c.sarg16(1) >= 0 && c.sarg16(1) < entries(c.arg(0)))
		rgb = qd::readRGB(entryAddr(c.arg(0), c.sarg16(1)));
	qd::writeRGB(c.arg(2), rgb);
}
IMPORT(SetPalette) {}
IMPORT(ActivatePalette) {}
IMPORT(Palette2CTab) {
	// (palette, CTabHandle): the table is resized to the palette
	gaddr pal = c.arg(0), ctab = c.arg(1);
	int n = entries(pal);
	if (!ctab || n <= 0)
		return;
	heap::setHandleSize(ctab, 8 + 8 * (uint32_t)n);
	gaddr t = heap::deref(ctab);
	emu::w32(t, seedCounter++);
	emu::w16(t + 4, 0);
	emu::w16(t + 6, (uint16_t)(n - 1));
	for (int i = 0; i < n; i++) {
		gaddr e = t + 8 + 8 * (uint32_t)i;
		emu::w16(e, (uint16_t)i);
		qd::writeRGB(e + 2, qd::readRGB(entryAddr(pal, i)));
	}
	qd::colorsChanged();
}
IMPORT(CTabChanged) {
	if (c.arg(0) && heap::deref(c.arg(0)))
		emu::w32(heap::deref(c.arg(0)), seedCounter++);
	qd::colorsChanged();
}
IMPORT(GetCTSeed) { c.ret(seedCounter++); }
