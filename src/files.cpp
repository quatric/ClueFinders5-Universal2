// File Manager. An FSRef holds an index into a table of host paths; an
// FSSpec names a file by its parent directory ID and leaf name, with
// directory IDs handed out on demand.
#include "files.h"
#include "cf.h"
#include "heap.h"
#include "host.h"

#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

const uint32_t kRefMagic = 0x46524546; // 'FREF'
const int16_t kVRefNum = -100;
const int16_t fnfErr = -43, dupFNErr = -48, paramErr = -50, rfNumErr = -51, ioErr = -36;

std::vector<std::string> refPaths;
std::map<std::string, uint32_t> dirIDs;
std::vector<std::string> dirPaths = {"", "", "/"}; // ID 2 is the root
std::map<int16_t, int> openFiles;
int16_t nextRefNum = 100;

uint32_t dirID(const std::string &dir) {
	if (dir.empty() || dir == "/")
		return 2;
	auto it = dirIDs.find(dir);
	if (it != dirIDs.end())
		return it->second;
	uint32_t id = (uint32_t)dirPaths.size() + 100;
	dirPaths.resize(id + 1);
	dirPaths[id] = dir;
	dirIDs[dir] = id;
	return id;
}

std::string dirPath(uint32_t id) {
	return id < dirPaths.size() ? dirPaths[id] : std::string();
}

void splitPath(const std::string &path, std::string &dir, std::string &leaf) {
	size_t slash = path.find_last_of('/');
	dir = slash == std::string::npos ? "" : path.substr(0, slash);
	leaf = slash == std::string::npos ? path : path.substr(slash + 1);
}

// FSCatalogInfo (144 bytes): nodeFlags, volume, parentDirID, nodeID, ...,
// dataLogicalSize at 104, dataPhysicalSize at 112
void writeCatInfo(gaddr info, const std::string &p, const struct stat &st) {
	std::string dir, leaf;
	splitPath(p, dir, leaf);
	memset(emu::ptr(info), 0, 144);
	emu::w16(info, S_ISDIR(st.st_mode) ? 0x10 : 0);
	emu::w16(info + 2, (uint16_t)kVRefNum);
	emu::w32(info + 4, dirID(dir));
	emu::w32(info + 8, S_ISDIR(st.st_mode) ? dirID(p) : (uint32_t)st.st_ino);
	uint64_t size = (uint64_t)st.st_size;
	memcpy(emu::ptr(info + 104), &size, 8);
	memcpy(emu::ptr(info + 112), &size, 8);
}

void writeHFSUniName(gaddr name, const std::string &leaf) {
	std::u16string u = cf::utf16(leaf);
	if (u.size() > 255)
		u.resize(255);
	emu::w16(name, (uint16_t)u.size());
	memcpy(emu::ptr(name + 2), u.data(), u.size() * 2);
}

// A Mac name or partial path ("folder:file", ":folder:file") relative to a
// directory, or a full path ("Volume:folder:file"), as a host path
std::string resolve(uint32_t dir, const std::string &macName) {
	std::string name = cf::macRomanToUtf8(macName);
	if (name.empty())
		return dirPath(dir);
	std::string base = dirPath(dir);
	size_t colon = name.find(':');
	if (colon != std::string::npos && colon > 0) {
		// Full path: the volume is the host root
		base = "";
		name = name.substr(colon);
	}
	std::string out = base;
	size_t i = name[0] == ':' ? 1 : 0;
	while (i <= name.size()) {
		size_t j = name.find(':', i);
		std::string part = name.substr(i, j == std::string::npos ? std::string::npos : j - i);
		if (part.empty() && j != std::string::npos) {
			// "::" goes up a level
			size_t slash = out.find_last_of('/');
			out = slash == std::string::npos ? "" : out.substr(0, slash);
		} else if (!part.empty()) {
			for (char &ch : part)
				if (ch == '/')
					ch = ':';
			out += "/" + part;
		}
		if (j == std::string::npos)
			break;
		i = j + 1;
	}
	return out.empty() ? "/" : out;
}

struct Iterator {
	std::vector<std::string> paths;
	size_t next = 0;
};
std::map<gaddr, Iterator> iterators;

const int16_t errFSNoMoreItems = -1417, fBsyErr = -47;

} // namespace

namespace files {

void writeFSRef(gaddr ref, const std::string &path) {
	uint32_t idx = 0;
	for (; idx < refPaths.size(); idx++) {
		if (refPaths[idx] == path)
			break;
	}
	if (idx == refPaths.size())
		refPaths.push_back(path);
	memset(emu::ptr(ref), 0, 80);
	emu::w32(ref, kRefMagic);
	emu::w32(ref + 4, idx);
}

std::string pathOfFSRef(gaddr ref) {
	if (!ref || emu::r32(ref) != kRefMagic || emu::r32(ref + 4) >= refPaths.size())
		return std::string();
	return refPaths[emu::r32(ref + 4)];
}

std::string pathOfFSSpec(gaddr spec) {
	std::string dir = dirPath(emu::r32(spec + 2));
	std::string name = cf::macRomanToUtf8(emu::pstr(spec + 6));
	for (char &ch : name) {
		if (ch == '/')
			ch = ':';
	}
	return dir + "/" + name;
}

void writeFSSpec(gaddr spec, const std::string &path) {
	std::string dir, leaf;
	splitPath(path, dir, leaf);
	for (char &ch : leaf) {
		if (ch == ':')
			ch = '/';
	}
	emu::w16(spec, (uint16_t)kVRefNum);
	emu::w32(spec + 2, dirID(dir));
	emu::wpstr(spec + 6, cf::utf8ToMacRoman(leaf), 63);
}

} // namespace files

IMPORT(FSpMakeFSRef) {
	std::string p = files::pathOfFSSpec(c.arg(0));
	struct stat st;
	if (::stat(p.c_str(), &st) != 0) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	files::writeFSRef(c.arg(1), p);
	c.ret(0);
}

IMPORT(FSGetCatalogInfo) {
	// (ref, whichInfo, catalogInfo, outName, fsSpec, parentRef)
	std::string p = files::pathOfFSRef(c.arg(0));
	struct stat st;
	if (p.empty() || ::stat(p.c_str(), &st) != 0) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	std::string dir, leaf;
	splitPath(p, dir, leaf);
	if (gaddr info = c.arg(2))
		writeCatInfo(info, p, st);
	if (gaddr name = c.arg(3))
		writeHFSUniName(name, leaf);
	if (c.arg(4))
		files::writeFSSpec(c.arg(4), p);
	if (c.arg(5))
		files::writeFSRef(c.arg(5), dir);
	c.ret(0);
}

IMPORT(FSpCreate) {
	std::string p = files::pathOfFSSpec(c.arg(0));
	int fd = open(p.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (fd < 0) {
		c.ret((uint32_t)(int32_t)(errno == EEXIST ? dupFNErr : ioErr));
		return;
	}
	close(fd);
	c.ret(0);
}

IMPORT(FSpOpenDF) {
	std::string p = files::pathOfFSSpec(c.arg(0));
	int8_t perm = (int8_t)c.arg(1);
	int fd = open(p.c_str(), perm == 1 ? O_RDONLY : O_RDWR);
	if (fd < 0)
		fd = open(p.c_str(), O_RDONLY);
	if (fd < 0) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	int16_t ref = nextRefNum++;
	openFiles[ref] = fd;
	emu::w16(c.arg(2), (uint16_t)ref);
	c.ret(0);
}

IMPORT(FSClose) {
	auto it = openFiles.find((int16_t)c.arg(0));
	if (it == openFiles.end()) {
		c.ret((uint32_t)(int32_t)rfNumErr);
		return;
	}
	close(it->second);
	openFiles.erase(it);
	c.ret(0);
}

IMPORT(LSSetExtensionHiddenForRef) { c.ret(0); }

int16_t fileParamErr() { return paramErr; }

// Standard folders all live in the game's support folder, so nothing is
// written outside it
static std::string folderPath(uint32_t type) {
	std::string name;
	switch (type) {
	case 0x70726566: name = "Preferences"; break;          // 'pref'
	case 0x61737570: name = "Application Support"; break;  // 'asup'
	case 0x646f6373: name = "Documents"; break;            // 'docs'
	case 0x74656d70: name = "Temporary Items"; break; // 'temp'
	case 0x73646174: name = "Shared"; break;               // 'sdat'
	case 0x63757372: name = "Home"; break;                 // 'cusr'
	default: {
		char s[5] = {(char)(type >> 24), (char)(type >> 16), (char)(type >> 8), (char)type, 0};
		name = s;
	}
	}
	std::string p = host::supportDir + "/" + name;
	mkdir(p.c_str(), 0755);
	logf("FindFolder '%s' -> %s", name.c_str(), p.c_str());
	return p;
}
IMPORT(FSFindFolder) {
	// (vRefNum, folderType, createFolder, FSRef *)
	files::writeFSRef(c.arg(3), folderPath(c.arg(1)));
	c.ret(0);
}
IMPORT(FindFolder) {
	// (vRefNum, folderType, createFolder, short *foundVRefNum, long *foundDirID)
	std::string p = folderPath(c.arg(1));
	emu::w16(c.arg(3), (uint16_t)kVRefNum);
	emu::w32(c.arg(4), dirID(p));
	c.ret(0);
}

// ---------------------------------------------------------------------------
// More of the File Manager

IMPORT(FSMakeFSSpec) {
	// (vRefNum, dirID, fileName, FSSpec *)
	std::string p = resolve(c.arg(1), emu::pstr(c.arg(2)));
	files::writeFSSpec(c.arg(3), p);
	struct stat st;
	c.ret(::stat(p.c_str(), &st) == 0 ? 0 : (uint32_t)(int32_t)fnfErr);
}
IMPORT(FSPathMakeRef) {
	// (const UInt8 *path, FSRef *, Boolean *isDirectory)
	std::string p = emu::cstr(c.arg(0));
	struct stat st;
	if (::stat(p.c_str(), &st) != 0) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	files::writeFSRef(c.arg(1), p);
	if (c.arg(2))
		emu::w8(c.arg(2), S_ISDIR(st.st_mode) ? 1 : 0);
	c.ret(0);
}
IMPORT(FSOpenIterator) {
	// (const FSRef *container, options, FSIterator *)
	std::string p = files::pathOfFSRef(c.arg(0));
	DIR *d = p.empty() ? nullptr : opendir(p.c_str());
	if (!d) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	Iterator it;
	while (dirent *e = readdir(d)) {
		if (e->d_name[0] != '.')
			it.paths.push_back(p + "/" + e->d_name);
	}
	closedir(d);
	std::sort(it.paths.begin(), it.paths.end());
	gaddr ref = heap::alloc(16, true);
	iterators[ref] = it;
	emu::w32(c.arg(2), ref);
	c.ret(0);
}
IMPORT(FSCloseIterator) {
	iterators.erase(c.arg(0));
	heap::free(c.arg(0));
	c.ret(0);
}
IMPORT(FSGetCatalogInfoBulk) {
	// (iterator, maximumObjects, ItemCount *actual, Boolean *containerChanged,
	//  whichInfo, FSCatalogInfo *, FSRef *, FSSpec *, HFSUniStr255 *)
	auto found = iterators.find(c.arg(0));
	if (found == iterators.end()) {
		c.ret((uint32_t)(int32_t)paramErr);
		return;
	}
	Iterator &it = found->second;
	uint32_t n = 0;
	while (n < c.arg(1) && it.next < it.paths.size()) {
		const std::string &p = it.paths[it.next++];
		struct stat st;
		if (::stat(p.c_str(), &st) != 0)
			continue;
		std::string dir, leaf;
		splitPath(p, dir, leaf);
		if (c.arg(5))
			writeCatInfo(c.arg(5) + 144 * n, p, st);
		if (c.arg(6))
			files::writeFSRef(c.arg(6) + 80 * n, p);
		if (c.arg(7))
			files::writeFSSpec(c.arg(7) + 70 * n, p);
		if (c.arg(8))
			writeHFSUniName(c.arg(8) + 512 * n, leaf);
		n++;
	}
	if (c.arg(2))
		emu::w32(c.arg(2), n);
	if (c.arg(3))
		emu::w8(c.arg(3), 0);
	c.ret(n ? 0 : (uint32_t)(int32_t)errFSNoMoreItems);
}
IMPORT(FSSetCatalogInfo) { c.ret(0); }
IMPORT(FSDeleteObject) {
	std::string p = files::pathOfFSRef(c.arg(0));
	struct stat st;
	if (p.empty() || ::stat(p.c_str(), &st) != 0) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	int r = S_ISDIR(st.st_mode) ? rmdir(p.c_str()) : unlink(p.c_str());
	c.ret(r == 0 ? 0 : (uint32_t)(int32_t)fBsyErr);
}
IMPORT(FSpDelete) {
	std::string p = files::pathOfFSSpec(c.arg(0));
	struct stat st;
	if (::stat(p.c_str(), &st) != 0) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	int r = S_ISDIR(st.st_mode) ? rmdir(p.c_str()) : unlink(p.c_str());
	c.ret(r == 0 ? 0 : (uint32_t)(int32_t)fBsyErr);
}
IMPORT(HOpenDF) {
	// (vRefNum, dirID, fileName, permission, short *refNum)
	std::string p = resolve(c.arg(1), emu::pstr(c.arg(2)));
	int8_t perm = (int8_t)c.arg(3);
	int fd = open(p.c_str(), perm == 1 ? O_RDONLY : O_RDWR);
	if (fd < 0)
		fd = open(p.c_str(), O_RDONLY);
	if (fd < 0) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	int16_t ref = nextRefNum++;
	openFiles[ref] = fd;
	emu::w16(c.arg(4), (uint16_t)ref);
	c.ret(0);
}
IMPORT(PBGetCatInfoSync) {
	// CInfoPBRec: ioResult 16, ioNamePtr 18, ioVRefNum 22, ioFDirIndex 28,
	// ioFlAttrib 30, ioDirID / ioDrDirID 48, ioFlLgLen 54, ioFlParID 100
	gaddr pb = c.arg(0);
	int16_t index = (int16_t)emu::r16(pb + 28);
	uint32_t dir = emu::r32(pb + 48);
	gaddr namePtr = emu::r32(pb + 18);
	std::string p;
	if (index > 0) {
		std::vector<std::string> items;
		if (DIR *d = opendir(dirPath(dir).c_str())) {
			while (dirent *e = readdir(d))
				if (e->d_name[0] != '.')
					items.push_back(dirPath(dir) + "/" + e->d_name);
			closedir(d);
		}
		std::sort(items.begin(), items.end());
		if (index > (int)items.size()) {
			emu::w16(pb + 16, (uint16_t)fnfErr);
			c.ret((uint32_t)(int32_t)fnfErr);
			return;
		}
		p = items[index - 1];
	} else if (index < 0) {
		p = dirPath(dir);
	} else {
		p = resolve(dir, namePtr ? emu::pstr(namePtr) : std::string());
	}
	struct stat st;
	if (p.empty() || ::stat(p.c_str(), &st) != 0) {
		emu::w16(pb + 16, (uint16_t)fnfErr);
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	std::string parent, leaf;
	splitPath(p, parent, leaf);
	if (namePtr && index != 0)
		emu::wpstr(namePtr, cf::utf8ToMacRoman(leaf), 31);
	emu::w16(pb + 22, (uint16_t)kVRefNum);
	bool isDir = S_ISDIR(st.st_mode);
	emu::w8(pb + 30, isDir ? 0x10 : 0);
	memset(emu::ptr(pb + 32), 0, 16);
	emu::w32(pb + 48, isDir ? dirID(p) : (uint32_t)st.st_ino);
	if (isDir) {
		uint16_t count = 0;
		if (DIR *d = opendir(p.c_str())) {
			while (dirent *e = readdir(d))
				if (e->d_name[0] != '.')
					count++;
			closedir(d);
		}
		emu::w16(pb + 52, count);
	} else {
		emu::w32(pb + 54, (uint32_t)st.st_size);
		emu::w32(pb + 58, (uint32_t)st.st_size);
	}
	emu::w32(pb + 100, dirID(parent));
	emu::w16(pb + 16, 0);
	c.ret(0);
}
IMPORT(PBHDeleteSync) {
	gaddr pb = c.arg(0);
	gaddr namePtr = emu::r32(pb + 18);
	std::string p = resolve(emu::r32(pb + 48), namePtr ? emu::pstr(namePtr) : std::string());
	struct stat st;
	int16_t err = 0;
	if (::stat(p.c_str(), &st) != 0)
		err = fnfErr;
	else if ((S_ISDIR(st.st_mode) ? rmdir(p.c_str()) : unlink(p.c_str())) != 0)
		err = fBsyErr;
	emu::w16(pb + 16, (uint16_t)err);
	c.ret((uint32_t)(int32_t)err);
}
IMPORT(FNNotify) { c.ret(0); }

// ---------------------------------------------------------------------------
// Classic File Manager calls used by the Assessment apps

namespace files {
// Path of a file named by (vRefNum, dirID, name); the volume is ignored
std::string resolvePath(uint32_t dir, const std::string &macName) {
	return resolve(dir, macName);
}
} // namespace files

IMPORT(HCreate) {
	// (vRefNum, dirID, fileName, creator, fileType)
	std::string p = resolve(c.arg(1), emu::pstr(c.arg(2)));
	int fd = open(p.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (fd < 0) {
		c.ret((uint32_t)(int32_t)(errno == EEXIST ? dupFNErr : fnfErr));
		return;
	}
	close(fd);
	c.ret(0);
}
IMPORT(FSpGetFInfo) {
	// (const FSSpec *, FInfo *): type, creator, flags, location, folder
	std::string p = files::pathOfFSSpec(c.arg(0));
	struct stat st;
	if (::stat(p.c_str(), &st) != 0) {
		c.ret((uint32_t)(int32_t)fnfErr);
		return;
	}
	memset(emu::ptr(c.arg(1)), 0, 16);
	if (S_ISDIR(st.st_mode) && p.size() > 4 && p.compare(p.size() - 4, 4, ".app") == 0)
		emu::w32(c.arg(1), 0x4150504C); // 'APPL'
	c.ret(0);
}
IMPORT(HGetVol) {
	// (StringPtr volName, short *vRefNum, long *dirID): the support folder
	if (c.arg(0))
		emu::wpstr(c.arg(0), "Macintosh HD");
	emu::w16(c.arg(1), (uint16_t)kVRefNum);
	emu::w32(c.arg(2), dirID(host::supportDir));
	c.ret(0);
}
IMPORT(FlushVol) { c.ret(0); }
IMPORT(UnmountVol) { c.ret((uint32_t)(int32_t)fBsyErr); } // the "disc" stays put
IMPORT(PBDirCreateSync) {
	// HFileParam: ioNamePtr 18, ioDirID 48 (in: parent, out: new directory)
	gaddr pb = c.arg(0);
	gaddr namePtr = emu::r32(pb + 18);
	std::string p = resolve(emu::r32(pb + 48), namePtr ? emu::pstr(namePtr) : std::string());
	int16_t err = 0;
	if (mkdir(p.c_str(), 0755) != 0)
		err = errno == EEXIST ? dupFNErr : fnfErr;
	else
		emu::w32(pb + 48, dirID(p));
	emu::w16(pb + 16, (uint16_t)err);
	c.ret((uint32_t)(int32_t)err);
}
IMPORT(PBHGetVInfoSync) {
	// HVolumeParam: ioNamePtr 18, ioVRefNum 22, ioVolIndex 28 ... (122 bytes)
	gaddr pb = c.arg(0);
	memset(emu::ptr(pb + 24), 0, 122 - 24);
	if (gaddr namePtr = emu::r32(pb + 18))
		emu::wpstr(namePtr, "Macintosh HD");
	emu::w16(pb + 22, (uint16_t)kVRefNum);
	emu::w16(pb + 16, 0);
	c.ret(0);
}
// No desktop database and no catalog search: the Assessment then looks for
// the game next to itself
IMPORT(PBHGetVolParmsSync) {
	emu::w16(c.arg(0) + 16, (uint16_t)paramErr);
	c.ret((uint32_t)(int32_t)paramErr);
}
IMPORT(PBCatSearchSync) {
	// CSParam: ioResult 16, ioActMatchCount 34
	emu::w16(c.arg(0) + 16, (uint16_t)-39); // eofErr
	emu::w32(c.arg(0) + 34, 0);
	c.ret((uint32_t)-39);
}
IMPORT(PBDTGetPath) {
	emu::w16(c.arg(0) + 16, (uint16_t)paramErr);
	c.ret((uint32_t)(int32_t)paramErr);
}
IMPORT(PBDTGetAPPLSync) {
	emu::w16(c.arg(0) + 16, (uint16_t)fnfErr);
	c.ret((uint32_t)(int32_t)fnfErr);
}
// Only reached after a code fragment loads, which never happens
IMPORT(PBHGetFInfoAsync) {
	emu::w16(c.arg(0) + 16, (uint16_t)paramErr);
	c.ret((uint32_t)(int32_t)paramErr);
}
