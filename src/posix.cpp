// More of libSystem: stdio streams, POSIX file descriptors, string and math
// functions, time. Guest FILE pointers are small blocks of guest memory that
// stand for host FILE streams; file descriptors are the host's own.
#include "emu.h"
#include "guestfmt.h"
#include "heap.h"

#include <runetype.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

gaddr errnoAddr(); // libc.cpp

namespace {

std::map<gaddr, FILE *> streams;

FILE *stream(gaddr f) {
	auto it = streams.find(f);
	return it == streams.end() ? nullptr : it->second;
}

void setErrno(int e) { emu::w32(errnoAddr(), (uint32_t)e); }

// A POSIX result for the guest, with errno set on failure (the host's errno
// values are the same as the 10.4 ABI's)
uint32_t result(long r) {
	if (r < 0)
		setErrno(errno);
	return (uint32_t)r;
}

// struct stat of the i386 Mac OS X 10.4 ABI (96 bytes)
void writeStat(gaddr b, const struct stat &st) {
	memset(emu::ptr(b), 0, 96);
	emu::w32(b + 0, (uint32_t)st.st_dev);
	emu::w32(b + 4, (uint32_t)st.st_ino);
	emu::w16(b + 8, st.st_mode);
	emu::w16(b + 10, (uint16_t)st.st_nlink);
	emu::w32(b + 12, st.st_uid);
	emu::w32(b + 16, st.st_gid);
	emu::w32(b + 24, (uint32_t)st.st_atimespec.tv_sec);
	emu::w32(b + 32, (uint32_t)st.st_mtimespec.tv_sec);
	emu::w32(b + 40, (uint32_t)st.st_ctimespec.tv_sec);
	emu::w32(b + 48, (uint32_t)st.st_size);
	emu::w32(b + 52, (uint32_t)((uint64_t)st.st_size >> 32));
	emu::w32(b + 56, (uint32_t)st.st_blocks);
	emu::w32(b + 64, (uint32_t)st.st_blksize);
}

// open() flags of the 10.4 ABI match the host's for the common bits
int hostOpenFlags(uint32_t f) {
	int r = f & 3;
	if (f & 0x8) r |= O_APPEND;
	if (f & 0x200) r |= O_CREAT;
	if (f & 0x400) r |= O_TRUNC;
	if (f & 0x800) r |= O_EXCL;
	return r;
}

} // namespace

// ---------------------------------------------------------------------------
// stdio

IMPORT(fopen) {
	std::string path = emu::cstr(c.arg(0)), mode = emu::cstr(c.arg(1));
	FILE *f = fopen(path.c_str(), mode.c_str());
	if (!f) {
		logf("fopen(%s, %s) failed", path.c_str(), mode.c_str());
		setErrno(errno);
		c.ret(0);
		return;
	}
	gaddr g = heap::alloc(88, true);
	streams[g] = f;
	c.ret(g);
}
IMPORT(fclose) {
	FILE *f = stream(c.arg(0));
	if (!f) {
		c.ret((uint32_t)EOF);
		return;
	}
	streams.erase(c.arg(0));
	heap::free(c.arg(0));
	c.ret((uint32_t)fclose(f));
}
IMPORT(fread) {
	FILE *f = stream(c.arg(3));
	uint32_t size = c.arg(1), count = c.arg(2);
	c.ret(f && size && count ? (uint32_t)fread(emu::ptr(c.arg(0)), size, count, f) : 0);
}
IMPORT(fwrite) {
	FILE *f = stream(c.arg(3));
	uint32_t size = c.arg(1), count = c.arg(2);
	c.ret(f && size && count ? (uint32_t)fwrite(emu::ptr(c.arg(0)), size, count, f) : 0);
}
IMPORT(fseek) {
	FILE *f = stream(c.arg(0));
	c.ret(f ? (uint32_t)fseek(f, (long)c.sarg(1), c.sarg(2)) : (uint32_t)-1);
}
IMPORT(ftell) {
	FILE *f = stream(c.arg(0));
	c.ret(f ? (uint32_t)ftell(f) : (uint32_t)-1);
}
IMPORT(feof) {
	FILE *f = stream(c.arg(0));
	c.ret(f ? (uint32_t)feof(f) : 1);
}
IMPORT(fgetc) {
	FILE *f = stream(c.arg(0));
	c.ret((uint32_t)(f ? fgetc(f) : EOF));
}
IMPORT(fputc) {
	FILE *f = stream(c.arg(1));
	c.ret((uint32_t)(f ? fputc(c.sarg(0), f) : EOF));
}
IMPORT(fputs) {
	FILE *f = stream(c.arg(1));
	c.ret((uint32_t)(f ? fputs(emu::cstr(c.arg(0)).c_str(), f) : EOF));
}
IMPORT(ferror) {
	FILE *f = stream(c.arg(0));
	c.ret(f ? (uint32_t)ferror(f) : 1);
}
IMPORT(fgets) {
	FILE *f = stream(c.arg(2));
	int n = c.sarg(1);
	if (!f || n <= 0) {
		c.ret(0);
		return;
	}
	std::vector<char> buf(n);
	if (!fgets(buf.data(), n, f)) {
		c.ret(0);
		return;
	}
	memcpy(emu::ptr(c.arg(0)), buf.data(), strlen(buf.data()) + 1);
	c.ret(c.arg(0));
}
IMPORT(remove) { c.ret(result(remove(emu::cstr(c.arg(0)).c_str()))); }
IMPORT(rename) { c.ret(result(rename(emu::cstr(c.arg(0)).c_str(), emu::cstr(c.arg(1)).c_str()))); }

IMPORT(vsprintf) {
	std::string s = guestFormat(emu::cstr(c.arg(1)), c.arg(2));
	memcpy(emu::ptr(c.arg(0)), s.c_str(), s.size() + 1);
	c.ret((uint32_t)s.size());
}

// Scans with the host's scanf (sscanf or fscanf, called by `scan` with the
// host format and twelve pointers) into host temporaries, then copies each
// assigned value to the guest pointer for it. `va` is the guest's first
// pointer argument.
template <typename Scan> static int guestScan(const std::string &fmt, gaddr va, Scan scan) {
	struct Out {
		char kind; // 'i' int, 'h' short, 'l' long long, 'f' float, 'd' double, 's' string, 'c' char, 'n' count
		gaddr dst;
		int width;
	};
	std::vector<Out> outs;
	for (size_t i = 0; i < fmt.size(); i++) {
		if (fmt[i] != '%')
			continue;
		i++;
		if (i < fmt.size() && fmt[i] == '%')
			continue;
		bool suppress = false;
		if (i < fmt.size() && fmt[i] == '*') {
			suppress = true;
			i++;
		}
		int width = 0;
		while (i < fmt.size() && isdigit((unsigned char)fmt[i]))
			width = width * 10 + (fmt[i++] - '0');
		int len = 0; // 1 h, 2 l, 3 ll
		while (i < fmt.size() && strchr("hlLqjzt", fmt[i])) {
			len += fmt[i] == 'h' ? 1 : fmt[i] == 'L' || fmt[i] == 'q' ? 3 : 2;
			i++;
		}
		if (i >= fmt.size())
			break;
		char conv = fmt[i];
		if (conv == '[') {
			while (i < fmt.size() && fmt[i] != ']')
				i++;
		}
		if (suppress)
			continue;
		Out o{'i', emu::r32(va), width};
		va += 4;
		if (strchr("diouxXp", conv))
			o.kind = len == 1 ? 'h' : len >= 3 ? 'l' : 'i';
		else if (strchr("eEfgGaA", conv))
			o.kind = len >= 2 ? 'd' : 'f';
		else if (conv == 's' || conv == '[')
			o.kind = 's';
		else if (conv == 'c')
			o.kind = 'c';
		else if (conv == 'n')
			o.kind = 'n';
		outs.push_back(o);
	}
	if (outs.size() > 12)
		emu::fatal("scanf with %zu conversions", outs.size());
	struct Slot {
		long long ll;
		double d;
		char s[1024];
	};
	std::vector<Slot> slots(12);
	void *p[12];
	for (size_t i = 0; i < 12; i++) {
		memset(&slots[i], 0, sizeof(Slot));
		char k = i < outs.size() ? outs[i].kind : 'i';
		p[i] = k == 'f' || k == 'd' ? (void *)&slots[i].d : k == 's' || k == 'c' ? (void *)slots[i].s : (void *)&slots[i].ll;
	}
	// The host formats need matching sizes: rewrite the length modifiers
	std::string hfmt;
	for (size_t i = 0; i < fmt.size(); i++) {
		hfmt += fmt[i];
		if (fmt[i] != '%')
			continue;
		size_t j = i + 1;
		if (j < fmt.size() && fmt[j] == '%') {
			hfmt += '%';
			i = j;
			continue;
		}
		while (j < fmt.size() && (fmt[j] == '*' || isdigit((unsigned char)fmt[j])))
			hfmt += fmt[j++];
		while (j < fmt.size() && strchr("hlLqjzt", fmt[j]))
			j++;
		if (j < fmt.size()) {
			char conv = fmt[j];
			if (strchr("diouxXn", conv))
				hfmt += "ll";
			else if (strchr("eEfgGaA", conv))
				hfmt += 'l';
		}
		i = j - 1;
	}
	int n = scan(hfmt.c_str(), p);
	for (size_t i = 0; i < outs.size(); i++) {
		const Out &o = outs[i];
		if ((int)i >= n && o.kind != 'n')
			break;
		switch (o.kind) {
		case 'h': emu::w16(o.dst, (uint16_t)slots[i].ll); break;
		case 'i':
		case 'n': emu::w32(o.dst, (uint32_t)slots[i].ll); break;
		case 'l': emu::w32(o.dst, (uint32_t)slots[i].ll); emu::w32(o.dst + 4, (uint32_t)(slots[i].ll >> 32)); break;
		case 'f': emu::wfloat(o.dst, (float)slots[i].d); break;
		case 'd': emu::wdouble(o.dst, slots[i].d); break;
		case 's': memcpy(emu::ptr(o.dst), slots[i].s, strlen(slots[i].s) + 1); break;
		case 'c': memcpy(emu::ptr(o.dst), slots[i].s, o.width ? o.width : 1); break;
		}
	}
	return n;
}
IMPORT(sscanf) {
	std::string in = emu::cstr(c.arg(0));
	int n = guestScan(emu::cstr(c.arg(1)), c.esp + 12, [&](const char *f, void **p) {
		return sscanf(in.c_str(), f, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
	});
	c.ret((uint32_t)n);
}
IMPORT(fscanf) {
	FILE *f = stream(c.arg(0));
	if (!f) {
		c.ret((uint32_t)EOF);
		return;
	}
	int n = guestScan(emu::cstr(c.arg(1)), c.esp + 12, [&](const char *fmt, void **p) {
		return fscanf(f, fmt, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
	});
	c.ret((uint32_t)n);
}

// ---------------------------------------------------------------------------
// File descriptors

IMPORT(open) {
	std::string path = emu::cstr(c.arg(0));
	int fd = open(path.c_str(), hostOpenFlags(c.arg(1)), (int)c.arg(2));
	if (fd < 0) {
		logf("open(%s) failed", path.c_str());
		setErrno(errno);
	}
	c.ret((uint32_t)fd);
}
IMPORT(close) { c.ret(result(close(c.sarg(0)))); }
IMPORT(read) { c.ret(result(read(c.sarg(0), emu::ptr(c.arg(1)), c.arg(2)))); }
IMPORT(write) { c.ret(result(write(c.sarg(0), emu::ptr(c.arg(1)), c.arg(2)))); }
IMPORT(lseek) {
	// off_t is 64-bit: (fd, offset lo, offset hi, whence); result in edx:eax
	int64_t off = (int64_t)((uint64_t)c.arg(1) | ((uint64_t)c.arg(2) << 32));
	off_t r = lseek(c.sarg(0), off, c.sarg(3));
	if (r < 0)
		setErrno(errno);
	c.ret64((uint64_t)r);
}
IMPORT(fstat) {
	struct stat st;
	if (fstat(c.sarg(0), &st) != 0) {
		setErrno(errno);
		c.ret((uint32_t)-1);
		return;
	}
	writeStat(c.arg(1), st);
	c.ret(0);
}
IMPORT(mkdir) { c.ret(result(mkdir(emu::cstr(c.arg(0)).c_str(), (mode_t)c.arg(1)))); }
IMPORT(chdir) { c.ret(result(chdir(emu::cstr(c.arg(0)).c_str()))); }

// ---------------------------------------------------------------------------
// Strings

IMPORT(strcmp) { c.ret((uint32_t)strcmp(emu::cstr(c.arg(0)).c_str(), emu::cstr(c.arg(1)).c_str())); }
IMPORT(strncmp) {
	std::string a = emu::cstr(c.arg(0)), b = emu::cstr(c.arg(1));
	c.ret((uint32_t)strncmp(a.c_str(), b.c_str(), c.arg(2)));
}
IMPORT(strcat) {
	std::string a = emu::cstr(c.arg(0)), b = emu::cstr(c.arg(1));
	memcpy(emu::ptr(c.arg(0) + a.size()), b.c_str(), b.size() + 1);
	c.ret(c.arg(0));
}
IMPORT(strncpy) {
	gaddr d = c.arg(0), s = c.arg(1);
	uint32_t n = c.arg(2), i = 0;
	for (; i < n; i++) {
		uint8_t ch = emu::r8(s + i);
		emu::w8(d + i, ch);
		if (!ch)
			break;
	}
	for (; i < n; i++)
		emu::w8(d + i, 0);
	c.ret(d);
}
IMPORT(strchr) {
	gaddr s = c.arg(0);
	uint8_t ch = (uint8_t)c.arg(1);
	for (;; s++) {
		uint8_t b = emu::r8(s);
		if (b == ch) {
			c.ret(s);
			return;
		}
		if (!b)
			break;
	}
	c.ret(0);
}
IMPORT(strrchr) {
	gaddr s = c.arg(0), found = 0;
	uint8_t ch = (uint8_t)c.arg(1);
	for (;; s++) {
		uint8_t b = emu::r8(s);
		if (b == ch)
			found = s;
		if (!b)
			break;
	}
	c.ret(found);
}
IMPORT(strstr) {
	std::string a = emu::cstr(c.arg(0)), b = emu::cstr(c.arg(1));
	size_t pos = a.find(b);
	c.ret(pos == std::string::npos ? 0 : c.arg(0) + (uint32_t)pos);
}
IMPORT(strpbrk) {
	std::string a = emu::cstr(c.arg(0)), set = emu::cstr(c.arg(1));
	size_t pos = a.find_first_of(set);
	c.ret(pos == std::string::npos ? 0 : c.arg(0) + (uint32_t)pos);
}
IMPORT(__toupper) { c.ret((uint32_t)toupper(c.sarg(0))); }
IMPORT(__tolower) { c.ret((uint32_t)tolower(c.sarg(0))); }
// ctype: the guest's isalpha() and friends read _DefaultRuneLocale's
// __runetype table directly for ASCII and call __maskrune for the rest
IMPORT_DATA(_DefaultRuneLocale) {
	// i386 _RuneLocale: magic[8], encoding[32], 3 words, then __runetype,
	// __maplower and __mapupper (256 32-bit entries each)
	gaddr a = emu::sysAlloc(52 + 3 * 1024 + 64);
	memcpy(emu::ptr(a), "RuneMagA", 8);
	strcpy((char *)emu::ptr(a + 8), "NONE");
	for (int i = 0; i < 256; i++) {
		emu::w32(a + 52 + 4 * i, _DefaultRuneLocale.__runetype[i]);
		emu::w32(a + 52 + 1024 + 4 * i, (uint32_t)_DefaultRuneLocale.__maplower[i]);
		emu::w32(a + 52 + 2048 + 4 * i, (uint32_t)_DefaultRuneLocale.__mapupper[i]);
	}
	return a;
}
IMPORT(__maskrune) {
	int ch = c.sarg(0);
	c.ret(ch >= 0 && ch < 256 ? _DefaultRuneLocale.__runetype[ch] & c.arg(1) : 0);
}
IMPORT(atol) { c.ret((uint32_t)atol(emu::cstr(c.arg(0)).c_str())); }
IMPORT_F(atof) { c.retd(atof(emu::cstr(c.arg(0)).c_str())); }
IMPORT(realloc) { c.ret(heap::realloc(c.arg(0), c.arg(1))); }

// ---------------------------------------------------------------------------
// Math and time

IMPORT(clock) { c.ret((uint32_t)clock()); } // only seeds srand()
IMPORT_F(ceil) { c.retd(ceil(c.argd(0))); }
IMPORT_F(pow) { c.retd(pow(c.argd(0), c.argd(2))); }
IMPORT_F(round) { c.retd(round(c.argd(0))); }

IMPORT(time) {
	uint32_t t = (uint32_t)::time(nullptr);
	if (c.arg(0))
		emu::w32(c.arg(0), t);
	c.ret(t);
}
IMPORT_F(difftime) { c.retd((double)(int32_t)c.arg(0) - (double)(int32_t)c.arg(1)); }

// struct tm on i386: eleven 32-bit fields (tm_zone is a char *)
static gaddr guestTm(const struct tm &t) {
	static gaddr g = 0, zone = 0;
	if (!g) {
		g = emu::sysAlloc(44);
		zone = emu::sysAlloc(16);
	}
	int32_t f[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
	for (int i = 0; i < 9; i++)
		emu::w32(g + 4 * i, (uint32_t)f[i]);
	emu::w32(g + 36, (uint32_t)t.tm_gmtoff);
	strlcpy((char *)emu::ptr(zone), t.tm_zone ? t.tm_zone : "", 16);
	emu::w32(g + 40, zone);
	return g;
}
static struct tm hostTm(gaddr g) {
	struct tm t = {};
	int *f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
	for (int i = 0; i < 9; i++)
		*f[i] = (int32_t)emu::r32(g + 4 * i);
	return t;
}
IMPORT(localtime) {
	time_t t = (int32_t)emu::r32(c.arg(0));
	struct tm tm;
	localtime_r(&t, &tm);
	c.ret(guestTm(tm));
}
IMPORT(asctime) {
	static gaddr buf = 0;
	if (!buf)
		buf = emu::sysAlloc(32);
	struct tm t = hostTm(c.arg(0));
	char s[32];
	asctime_r(&t, s);
	strlcpy((char *)emu::ptr(buf), s, 32);
	c.ret(buf);
}
IMPORT(strtok) {
	static gaddr last = 0;
	gaddr s = c.arg(0) ? c.arg(0) : last;
	std::string delim = emu::cstr(c.arg(1));
	auto isDelim = [&](uint8_t ch) { return ch && delim.find((char)ch) != std::string::npos; };
	if (!s) {
		c.ret(0);
		return;
	}
	while (isDelim(emu::r8(s)))
		s++;
	if (!emu::r8(s)) {
		last = 0;
		c.ret(0);
		return;
	}
	gaddr tok = s;
	while (emu::r8(s) && !isDelim(emu::r8(s)))
		s++;
	if (emu::r8(s)) {
		emu::w8(s, 0);
		last = s + 1;
	} else {
		last = 0;
	}
	c.ret(tok);
}
