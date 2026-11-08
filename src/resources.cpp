// Resource Manager over resource maps stored in data forks. The game's own
// "<executable name>.rsrc" is opened at launch, as CFBundle does for Carbon
// apps; FSOpenResourceFile adds the activity data files on top of it.
#include "cf.h"
#include "emu.h"
#include "files.h"
#include "heap.h"
#include "host.h"
#include "qd.h"
#include "res.h"

#include <fcntl.h>
#include <map>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

const int16_t resNotFound = -192, resFNotFound = -193, fnfErr = -43;

struct Entry {
	uint32_t offset; // of the data, past its length word
	uint32_t size;
	std::string name;
};

struct ResFile {
	int16_t refNum;
	std::string path;
	const uint8_t *data = nullptr;
	size_t size = 0;
	std::vector<uint8_t> written; // the data after a save, instead of the mapping
	bool dirty = false;
	std::map<std::pair<uint32_t, int16_t>, Entry> entries;
	std::map<std::pair<uint32_t, int16_t>, gaddr> loaded;
};

std::vector<ResFile *> chain; // searched from the back
ResFile *curFile = nullptr;   // UseResFile; searches start here
std::map<gaddr, std::pair<ResFile *, std::pair<uint32_t, int16_t>>> handleOwner;
int16_t resErr = 0;
int16_t nextRefNum = 2000;

inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

std::string fourccStr(uint32_t v) {
	char s[5] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, 0};
	return s;
}

bool readMap(ResFile *f);

ResFile *openFile(const std::string &path) {
	for (ResFile *f : chain) {
		if (f->path == path)
			return f;
	}
	int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return nullptr;
	struct stat st;
	fstat(fd, &st);
	void *m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (m == MAP_FAILED || st.st_size < 16)
		return nullptr;
	ResFile *f = new ResFile;
	f->path = path;
	f->data = (const uint8_t *)m;
	f->size = st.st_size;
	if (!readMap(f)) {
		logf("resources: %s is not a resource file", path.c_str());
		munmap(m, st.st_size);
		delete f;
		return nullptr;
	}
	f->refNum = nextRefNum++;
	chain.push_back(f);
	curFile = f; // as OpenResFile does
	logf("resources: opened %s (%zu resources) as %d", path.c_str(), f->entries.size(), f->refNum);
	return f;
}

// Reads the resource map of f->data into f->entries
bool readMap(ResFile *f) {
	const uint8_t *d = f->data;
	uint32_t doff = be32(d), moff = be32(d + 4), mlen = be32(d + 12);
	if (moff + mlen > f->size || doff > f->size)
		return false;
	f->entries.clear();
	const uint8_t *map = d + moff;
	uint16_t toff = be16(map + 24), noff = be16(map + 26);
	int ntypes = be16(map + toff) + 1;
	if (be16(map + toff) == 0xFFFF)
		ntypes = 0;
	for (int i = 0; i < ntypes; i++) {
		const uint8_t *t = map + toff + 2 + i * 8;
		uint32_t type = be32(t);
		int n = be16(t + 4) + 1;
		uint16_t ref = be16(t + 6);
		for (int j = 0; j < n; j++) {
			const uint8_t *r = map + toff + ref + j * 12;
			int16_t id = (int16_t)be16(r);
			uint16_t nameOff = be16(r + 2);
			uint32_t dataOff = be32(r + 4) & 0xFFFFFF;
			Entry e;
			e.offset = doff + dataOff + 4;
			e.size = be32(d + doff + dataOff);
			if (nameOff != 0xFFFF) {
				const uint8_t *nm = map + noff + nameOff;
				e.name.assign((const char *)nm + 1, nm[0]);
			}
			f->entries[{type, id}] = e;
		}
	}
	return true;
}

void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
	v[at] = x >> 24; v[at + 1] = x >> 16; v[at + 2] = x >> 8; v[at + 3] = x;
}
void put16(std::vector<uint8_t> &v, size_t at, uint16_t x) {
	v[at] = x >> 8; v[at + 1] = x;
}

// Writes a file's resources, with the current contents of the handles the
// game changed or added, in the standard resource fork layout. Only the
// game's own (unflipped) types are expected here.
void save(ResFile *f) {
	std::map<std::pair<uint32_t, int16_t>, std::vector<uint8_t>> contents;
	for (auto &e : f->entries) {
		auto l = f->loaded.find(e.first);
		const uint8_t *p = f->data + e.second.offset;
		uint32_t n = e.second.size;
		if (l != f->loaded.end() && heap::isHandle(l->second)) {
			p = emu::ptr(heap::deref(l->second));
			n = heap::handleSize(l->second);
		}
		contents[e.first].assign(p, p + n);
	}
	std::vector<uint8_t> out(256, 0);
	std::map<std::pair<uint32_t, int16_t>, uint32_t> dataOff;
	for (auto &c : contents) {
		dataOff[c.first] = (uint32_t)out.size() - 256;
		size_t at = out.size();
		out.resize(at + 4);
		put32(out, at, (uint32_t)c.second.size());
		out.insert(out.end(), c.second.begin(), c.second.end());
	}
	uint32_t dataLen = (uint32_t)out.size() - 256, mapOff = (uint32_t)out.size();
	std::map<uint32_t, std::vector<int16_t>> types;
	for (auto &c : contents)
		types[c.first.first].push_back(c.first.second);
	// Map: header copy (16), next map (4), file ref (2), attributes (2),
	// type list offset (2), name list offset (2), then the type list
	uint32_t typeList = 28;
	uint32_t refLists = typeList + 2 + 8 * (uint32_t)types.size();
	uint32_t nameList = refLists + 12 * (uint32_t)contents.size();
	std::vector<uint8_t> map(nameList, 0), names;
	put16(map, 24, (uint16_t)typeList);
	put16(map, 26, (uint16_t)nameList);
	put16(map, typeList, (uint16_t)(types.size() - 1));
	uint32_t ti = typeList + 2, ri = refLists;
	for (auto &t : types) {
		put32(map, ti, t.first);
		put16(map, ti + 4, (uint16_t)(t.second.size() - 1));
		put16(map, ti + 6, (uint16_t)(ri - typeList));
		ti += 8;
		for (int16_t id : t.second) {
			const Entry &e = f->entries[{t.first, id}];
			put16(map, ri, (uint16_t)id);
			if (e.name.empty()) {
				put16(map, ri + 2, 0xFFFF);
			} else {
				put16(map, ri + 2, (uint16_t)names.size());
				names.push_back((uint8_t)e.name.size());
				names.insert(names.end(), e.name.begin(), e.name.end());
			}
			put32(map, ri + 4, dataOff[{t.first, id}]);
			ri += 12;
		}
	}
	map.insert(map.end(), names.begin(), names.end());
	out.insert(out.end(), map.begin(), map.end());
	put32(out, 0, 256);
	put32(out, 4, mapOff);
	put32(out, 8, dataLen);
	put32(out, 12, (uint32_t)map.size());
	memcpy(out.data() + mapOff, out.data(), 16);

	std::string tmp = f->path + ".tmp";
	FILE *fp = fopen(tmp.c_str(), "wb");
	if (!fp || fwrite(out.data(), 1, out.size(), fp) != out.size()) {
		logf("resources: can't write %s", f->path.c_str());
		if (fp)
			fclose(fp);
		resErr = -36; // ioErr
		return;
	}
	fclose(fp);
	rename(tmp.c_str(), f->path.c_str());
	if (f->written.empty())
		munmap((void *)f->data, f->size);
	f->written = std::move(out);
	f->data = f->written.data();
	f->size = f->written.size();
	readMap(f);
	f->dirty = false;
	logf("resources: saved %s (%zu resources)", f->path.c_str(), f->entries.size());
}

ResFile *fileOf(int16_t ref) {
	for (ResFile *f : chain)
		if (f->refNum == ref)
			return f;
	return nullptr;
}

// The Intel Resource Manager byte-swaps standard resource types as they are
// loaded ("flippers"); the game's own types stay big-endian
void flip16(uint8_t *p) { std::swap(p[0], p[1]); }
void flip32(uint8_t *p) {
	std::swap(p[0], p[3]);
	std::swap(p[1], p[2]);
}

void flipSoundHeader(uint8_t *h, uint8_t *end) {
	if (h + 22 > end)
		return;
	uint8_t encode = h[20];
	for (int off : {0, 4, 8, 12, 16})
		flip32(h + off);
	if (encode == 0xFF && h + 64 <= end) { // extSH
		flip32(h + 22);
		for (int off : {36, 40, 44})
			flip32(h + off);
		flip16(h + 48);
		flip16(h + 50);
		for (int off : {52, 56, 60})
			flip32(h + off);
	} else if (encode == 0xFE && h + 64 <= end) { // cmpSH
		flip32(h + 22);
		for (int off : {36, 40, 44, 48, 52})
			flip32(h + off);
		for (int off : {56, 58, 60, 62})
			flip16(h + off);
	}
}

void flipResource(uint32_t type, uint8_t *p, uint32_t size) {
	uint8_t *end = p + size;
	switch (type) {
	case 0x53545223: // 'STR#'
		if (size >= 2)
			flip16(p);
		break;
	case 0x636C7574: // 'clut'
		if (size < 8)
			break;
		flip32(p);
		flip16(p + 4);
		flip16(p + 6);
		for (uint8_t *e = p + 8; e + 8 <= end; e += 8)
			for (int k = 0; k < 8; k += 2)
				flip16(e + k);
		break;
	case 0x7374796C: { // 'styl'
		if (size < 2)
			break;
		int n = be16(p);
		flip16(p);
		for (int i = 0; i < n && p + 2 + 20 * (i + 1) <= end; i++) {
			uint8_t *e = p + 2 + 20 * i;
			flip32(e);
			for (int off : {4, 6, 8, 12, 14, 16, 18})
				flip16(e + off);
		}
		break;
	}
	case 0x736E6420: { // 'snd '
		if (size < 6)
			break;
		int format = be16(p);
		uint8_t *q = p + 2;
		flip16(p);
		if (format == 1) {
			int mods = be16(q);
			flip16(q);
			q += 2;
			for (int i = 0; i < mods && q + 6 <= end; i++, q += 6) {
				flip16(q);
				flip32(q + 2);
			}
		} else {
			flip16(q); // refCount
			q += 2;
		}
		if (q + 2 > end)
			break;
		int cmds = be16(q);
		flip16(q);
		q += 2;
		for (int i = 0; i < cmds && q + 8 <= end; i++, q += 8) {
			uint16_t cmd = be16(q);
			uint32_t param2 = be32(q + 4);
			flip16(q);
			flip16(q + 2);
			flip32(q + 4);
			if ((cmd & 0x8000) && ((cmd & 0x7FFF) == 80 || (cmd & 0x7FFF) == 81) && param2 < size)
				flipSoundHeader(p + param2, end);
		}
		break;
	}
	default:
		break;
	}
}

gaddr getResource(uint32_t type, int16_t id, bool onlyCurrent = false) {
	auto start = chain.rbegin();
	while (curFile && start != chain.rend() && *start != curFile)
		++start;
	if (start == chain.rend())
		start = chain.rbegin();
	for (auto it = start; it != chain.rend(); ++it) {
		ResFile *f = *it;
		if (onlyCurrent && it != start)
			break;
		auto e = f->entries.find({type, id});
		if (e == f->entries.end())
			continue;
		auto l = f->loaded.find({type, id});
		if (l != f->loaded.end() && heap::isHandle(l->second)) {
			resErr = 0;
			return l->second;
		}
		gaddr h = heap::handleFromData(f->data + e->second.offset, e->second.size);
		flipResource(type, emu::ptr(heap::deref(h)), e->second.size);
		f->loaded[{type, id}] = h;
		handleOwner[h] = {f, {type, id}};
		resErr = 0;
		return h;
	}
	resErr = resNotFound;
	return 0;
}

} // namespace

namespace res {

void init() {
	std::string exe = host::gameApp + "/Contents/Resources/" + host::gameName + ".rsrc";
	if (!openFile(exe))
		logf("resources: no application resource file");
}

gaddr get(uint32_t type, int16_t id) {
	return getResource(type, id);
}

} // namespace res

IMPORT(FSOpenResourceFile) {
	// (const FSRef *, forkNameLength, const UniChar *forkName, permissions, ResFileRefNum *)
	std::string path = files::pathOfFSRef(c.arg(0));
	ResFile *f = path.empty() ? nullptr : openFile(path);
	if (!f) {
		logf("FSOpenResourceFile %s failed", path.c_str());
		c.ret((uint32_t)(int32_t)(path.empty() ? fnfErr : resFNotFound));
		return;
	}
	emu::w16(c.arg(4), (uint16_t)f->refNum);
	c.ret(0);
}

IMPORT(CloseResFile) {
	int16_t ref = (int16_t)c.arg(0);
	for (auto it = chain.begin(); it != chain.end(); ++it) {
		ResFile *f = *it;
		if (f->refNum != ref)
			continue;
		if (f->dirty)
			save(f);
		for (auto &l : f->loaded) {
			if (heap::isHandle(l.second)) {
				handleOwner.erase(l.second);
				heap::disposeHandle(l.second);
			}
		}
		if (f->written.empty())
			munmap((void *)f->data, f->size);
		chain.erase(it);
		if (curFile == f)
			curFile = chain.empty() ? nullptr : chain.back();
		delete f;
		break;
	}
	resErr = 0;
}

IMPORT(GetResource) {
	uint32_t type = c.arg(0);
	int16_t id = c.sarg16(1);
	gaddr h = getResource(type, id);
	if (!h || emu::traceImports)
		logf("GetResource('%s', %d) -> 0x%08x", fourccStr(type).c_str(), id, h);
	c.ret(h);
}

IMPORT(GetPicture) { c.ret(getResource(0x50494354, c.sarg16(0))); }

IMPORT(ReleaseResource) {
	auto it = handleOwner.find(c.arg(0));
	if (it == handleOwner.end()) {
		resErr = -192;
		return;
	}
	if (it->second.first->dirty)
		save(it->second.first); // keep what the game changed
	it->second.first->loaded.erase(it->second.second);
	handleOwner.erase(it);
	heap::disposeHandle(c.arg(0));
	resErr = 0;
}

IMPORT(DetachResource) {
	auto it = handleOwner.find(c.arg(0));
	if (it != handleOwner.end()) {
		it->second.first->loaded.erase(it->second.second);
		handleOwner.erase(it);
	}
	resErr = 0;
}

IMPORT(SetResLoad) {}
IMPORT(ResError) { c.ret((uint32_t)(int32_t)resErr); }

IMPORT(GetIndString) {
	// (Str255 out, strListID, index)
	gaddr out = c.arg(0);
	emu::w8(out, 0);
	gaddr h = getResource(0x53545223, c.sarg16(1)); // 'STR#'
	int16_t index = c.sarg16(2);
	if (!h || index < 1)
		return;
	const uint8_t *p = emu::ptr(heap::deref(h));
	uint32_t size = heap::handleSize(h);
	int count = p[0] | p[1] << 8;
	if (index > count)
		return;
	uint32_t off = 2;
	for (int i = 1; i < index && off < size; i++)
		off += 1 + p[off];
	if (off < size)
		memcpy(emu::ptr(out), p + off, std::min<uint32_t>(p[off] + 1, size - off));
	if (emu::traceImports)
		logf("GetIndString(%d, %d) = \"%s\"", c.sarg16(1), index, emu::pstr(out).c_str());
}

IMPORT(GetCTable) {
	// Color tables come from 'clut' resources, in native byte order
	int16_t id = c.sarg16(0);
	gaddr src = getResource(0x636C7574, id); // 'clut'
	if (!src) {
		int depth = id == 8 ? 8 : id == 4 ? 4 : id == 2 ? 2 : id == 1 ? 1 : id == 40 ? 8 : 8;
		const auto &pal = qd::palette(0, depth);
		gaddr h = heap::newHandle(8 + 8 * (uint32_t)pal.size(), true);
		gaddr t = heap::deref(h);
		emu::w32(t, (uint32_t)id);
		emu::w16(t + 6, (uint16_t)(pal.size() - 1));
		for (size_t i = 0; i < pal.size(); i++) {
			gaddr e = t + 8 + 8 * (uint32_t)i;
			emu::w16(e, (uint16_t)i);
			uint32_t col = pal[i];
			qd::writeRGB(e + 2, {(uint16_t)((col >> 16 & 0xFF) * 0x101), (uint16_t)((col >> 8 & 0xFF) * 0x101), (uint16_t)((col & 0xFF) * 0x101)});
		}
		c.ret(h);
		return;
	}
	// Already in native order: copy it
	uint32_t size = heap::handleSize(src);
	c.ret(heap::handleFromData(emu::ptr(heap::deref(src)), size));
}

IMPORT(GetCursor) {
	gaddr h = getResource(0x43555253, c.sarg16(0)); // 'CURS'
	if (!h)
		h = heap::newHandle(68, true);
	c.ret(h);
}

// ---------------------------------------------------------------------------
// Classic calls

IMPORT(CurResFile) {
	ResFile *f = curFile ? curFile : chain.empty() ? nullptr : chain.back();
	c.ret(f ? (uint32_t)(uint16_t)f->refNum : 0);
}
IMPORT(UseResFile) {
	int16_t ref = (int16_t)c.arg(0);
	for (ResFile *f : chain) {
		if (f->refNum == ref) {
			curFile = f;
			resErr = 0;
			return;
		}
	}
	resErr = -193;
}
IMPORT(Get1Resource) {
	gaddr h = getResource(c.arg(0), c.sarg16(1), true);
	if (!h)
		logf("Get1Resource('%s', %d) -> 0", fourccStr(c.arg(0)).c_str(), c.sarg16(1));
	c.ret(h);
}
IMPORT(HOpenResFile) {
	// (vRefNum, dirID, fileName, permission)
	gaddr spec = heap::alloc(70, true);
	emu::w16(spec, (uint16_t)c.arg(0));
	emu::w32(spec + 2, c.arg(1));
	emu::wpstr(spec + 6, emu::pstr(c.arg(2)), 63);
	std::string path = files::pathOfFSSpec(spec);
	heap::free(spec);
	ResFile *f = openFile(path);
	if (!f) {
		logf("HOpenResFile %s failed", path.c_str());
		resErr = fnfErr;
		c.ret((uint32_t)-1);
		return;
	}
	curFile = f;
	resErr = 0;
	c.ret((uint32_t)(uint16_t)f->refNum);
}
IMPORT(GetResourceSizeOnDisk) {
	auto it = handleOwner.find(c.arg(0));
	if (it == handleOwner.end()) {
		c.ret((uint32_t)-1);
		return;
	}
	c.ret(it->second.first->entries[it->second.second].size);
}
IMPORT(ReadPartialResource) {
	// (handle, offset, buffer, count): reads the bytes as stored
	auto it = handleOwner.find(c.arg(0));
	if (it == handleOwner.end()) {
		resErr = -192;
		return;
	}
	const Entry &e = it->second.first->entries[it->second.second];
	uint32_t off = c.arg(1), n = c.arg(3);
	if (off > e.size)
		off = e.size;
	if (n > e.size - off)
		n = e.size - off;
	memcpy(emu::ptr(c.arg(2)), it->second.first->data + e.offset + off, n);
	resErr = 0;
}

// ---------------------------------------------------------------------------
// Writing (the Assessment apps keep their records in a preferences file)

IMPORT(HCreateResFile) {
	// (vRefNum, dirID, fileName): an empty resource map in the data fork
	std::string path = files::resolvePath(c.arg(1), emu::pstr(c.arg(2)));
	struct stat st;
	if (::stat(path.c_str(), &st) == 0 && st.st_size >= 16) {
		resErr = -48; // dupFNErr
		return;
	}
	static const uint8_t empty[] = {
		0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 30,
	};
	std::vector<uint8_t> out(256 + 30, 0);
	memcpy(out.data(), empty, 16);
	memcpy(out.data() + 256, empty, 16);
	out[256 + 25] = 28; // type list offset
	out[256 + 27] = 30; // name list offset
	out[256 + 28] = out[256 + 29] = 0xFF; // no types
	FILE *fp = fopen(path.c_str(), "wb");
	if (!fp) {
		resErr = -43; // fnfErr
		return;
	}
	fwrite(out.data(), 1, out.size(), fp);
	fclose(fp);
	resErr = 0;
}

IMPORT(AddResource) {
	// (handle, type, id, name)
	ResFile *f = curFile ? curFile : chain.empty() ? nullptr : chain.back();
	gaddr h = c.arg(0);
	if (!f || !heap::isHandle(h) || handleOwner.count(h)) {
		resErr = -194; // addResFailed
		return;
	}
	std::pair<uint32_t, int16_t> key{c.arg(1), c.sarg16(2)};
	Entry e{0, 0, c.arg(3) ? emu::pstr(c.arg(3)) : std::string()};
	f->entries[key] = e;
	auto old = f->loaded.find(key);
	if (old != f->loaded.end())
		handleOwner.erase(old->second);
	f->loaded[key] = h;
	handleOwner[h] = {f, key};
	f->dirty = true;
	resErr = 0;
}

IMPORT(ChangedResource) {
	auto it = handleOwner.find(c.arg(0));
	if (it == handleOwner.end()) {
		resErr = -192;
		return;
	}
	it->second.first->dirty = true;
	resErr = 0;
}

IMPORT(WriteResource) {
	auto it = handleOwner.find(c.arg(0));
	if (it == handleOwner.end()) {
		resErr = -192;
		return;
	}
	resErr = 0;
	if (it->second.first->dirty)
		save(it->second.first);
}

IMPORT(UpdateResFile) {
	ResFile *f = fileOf((int16_t)c.arg(0));
	resErr = f ? 0 : -193;
	if (f && f->dirty)
		save(f);
}
