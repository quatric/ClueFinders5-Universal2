// Guest heap: malloc, C++ new/delete and the Memory Manager share it
#include "heap.h"

#include <map>
#include <unordered_map>
#include <vector>

namespace heap {

int16_t memErr = 0;

namespace {

const uint32_t kHeader = 16;
const uint32_t kMagic = 0x48454150; // 'HEAP'
const uint32_t kSmallMax = 4096;

gaddr top = kHeapBase;
std::vector<gaddr> smallBins[kSmallMax / 16 + 1];
std::map<gaddr, uint32_t> largeFree; // block address (incl. header) -> total size
std::unordered_map<gaddr, uint32_t> handles; // handle -> logical size

uint32_t roundSize(uint32_t size) {
	return (size + 15) & ~15u;
}

gaddr carve(uint32_t total) {
	if (top + total > kHeapBase + kHeapSize || top + total < top)
		emu::fatal("guest heap exhausted (%u bytes requested)", total);
	gaddr b = top;
	top += total;
	return b;
}

} // namespace

gaddr alloc(uint32_t size, bool clear) {
	uint32_t payload = roundSize(size ? size : 1);
	uint32_t total = payload + kHeader;
	gaddr block = 0;
	if (payload <= kSmallMax) {
		auto &bin = smallBins[payload / 16];
		if (!bin.empty()) {
			block = bin.back();
			bin.pop_back();
		}
	} else {
		for (auto it = largeFree.begin(); it != largeFree.end(); ++it) {
			if (it->second >= total) {
				block = it->first;
				uint32_t rest = it->second - total;
				largeFree.erase(it);
				if (rest >= kSmallMax + kHeader + 16)
					largeFree[block + total] = rest;
				else
					total += rest;
				break;
			}
		}
	}
	if (!block)
		block = carve(total);
	emu::w32(block, payload <= kSmallMax ? payload : total - kHeader);
	emu::w32(block + 4, kMagic);
	emu::w32(block + 8, size);
	gaddr p = block + kHeader;
	if (clear)
		memset(emu::ptr(p), 0, emu::r32(block));
	return p;
}

void free(gaddr p) {
	if (!p)
		return;
	gaddr block = p - kHeader;
	if (!emu::valid(block, kHeader) || emu::r32(block + 4) != kMagic) {
		logf("heap: free of bad pointer 0x%08x", p);
		return;
	}
	emu::w32(block + 4, 0);
	uint32_t payload = emu::r32(block);
	if (payload <= kSmallMax) {
		smallBins[payload / 16].push_back(block);
		return;
	}
	uint32_t total = payload + kHeader;
	auto next = largeFree.find(block + total);
	if (next != largeFree.end()) {
		total += next->second;
		largeFree.erase(next);
	}
	auto it = largeFree.lower_bound(block);
	if (it != largeFree.begin()) {
		auto prev = std::prev(it);
		if (prev->first + prev->second == block) {
			prev->second += total;
			return;
		}
	}
	largeFree[block] = total;
}

uint32_t size(gaddr p) {
	if (!p)
		return 0;
	return emu::r32(p - kHeader + 8);
}

gaddr realloc(gaddr p, uint32_t newSize) {
	if (!p)
		return alloc(newSize);
	uint32_t cap = emu::r32(p - kHeader);
	if (newSize <= cap) {
		emu::w32(p - kHeader + 8, newSize);
		return p;
	}
	gaddr q = alloc(newSize);
	memcpy(emu::ptr(q), emu::ptr(p), size(p));
	free(p);
	return q;
}

gaddr newHandle(uint32_t size, bool clear) {
	gaddr h = alloc(4);
	gaddr d = alloc(size, clear);
	emu::w32(h, d);
	handles[h] = size;
	return h;
}

gaddr handleFromData(const void *data, uint32_t size) {
	gaddr h = newHandle(size);
	if (size)
		memcpy(emu::ptr(deref(h)), data, size);
	return h;
}

void disposeHandle(gaddr h) {
	auto it = handles.find(h);
	if (it == handles.end()) {
		logf("heap: DisposeHandle of non-handle 0x%08x", h);
		return;
	}
	free(deref(h));
	free(h);
	handles.erase(it);
}

uint32_t handleSize(gaddr h) {
	auto it = handles.find(h);
	return it == handles.end() ? 0 : it->second;
}

bool setHandleSize(gaddr h, uint32_t size) {
	auto it = handles.find(h);
	if (it == handles.end())
		return false;
	emu::w32(h, realloc(deref(h), size));
	it->second = size;
	return true;
}

bool isHandle(gaddr h) {
	return handles.count(h) != 0;
}

} // namespace heap

// ---------------------------------------------------------------------------
// Memory Manager

IMPORT(NewPtr) { heap::memErr = 0; c.ret(heap::alloc(c.arg(0))); }
IMPORT(NewPtrClear) { heap::memErr = 0; c.ret(heap::alloc(c.arg(0), true)); }
IMPORT(DisposePtr) { heap::free(c.arg(0)); heap::memErr = 0; }
IMPORT(NewHandle) { heap::memErr = 0; c.ret(heap::newHandle(c.arg(0))); }
IMPORT(NewHandleClear) { heap::memErr = 0; c.ret(heap::newHandle(c.arg(0), true)); }
IMPORT(DisposeHandle) { heap::disposeHandle(c.arg(0)); heap::memErr = 0; }
IMPORT(GetHandleSize) { c.ret(heap::handleSize(c.arg(0))); }
IMPORT(SetHandleSize) { heap::memErr = heap::setHandleSize(c.arg(0), c.arg(1)) ? 0 : -109; }
IMPORT(HLock) { heap::memErr = 0; }
IMPORT(HUnlock) { heap::memErr = 0; }
IMPORT(HNoPurge) { heap::memErr = 0; }
IMPORT(MemError) { c.ret((uint32_t)(int32_t)heap::memErr); }

// ---------------------------------------------------------------------------
// malloc and C++ operators

IMPORT(malloc) { c.ret(heap::alloc(c.arg(0))); }
IMPORT(calloc) { c.ret(heap::alloc(c.arg(0) * c.arg(1), true)); }
IMPORT(free) { heap::free(c.arg(0)); }
IMPORT(_Znwm) { c.ret(heap::alloc(c.arg(0))); }
IMPORT(_Znam) { c.ret(heap::alloc(c.arg(0))); }
IMPORT(_ZdlPv) { heap::free(c.arg(0)); }
IMPORT(_ZdaPv) { heap::free(c.arg(0)); }
IMPORT(MoreMasters) {}
IMPORT(MaxMem) {
	if (c.arg(0))
		emu::w32(c.arg(0), 0);
	c.ret(64 << 20);
}
IMPORT(TempMaxMem) {
	if (c.arg(0))
		emu::w32(c.arg(0), 64 << 20);
	c.ret(64 << 20);
}
IMPORT(MoveHHi) {}
IMPORT(GetPtrSize) { c.ret(heap::size(c.arg(0))); }
