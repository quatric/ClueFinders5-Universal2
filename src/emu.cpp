#include "emu.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <sys/mman.h>

namespace emu {

uc_engine *uc = nullptr;
bool traceImports = false;

struct Region {
	gaddr base;
	uint32_t size;
	uint8_t *host;
};
static Region regions[4];
static int numRegions = 0;
static gaddr sysNext = kSysDataBase;

static void mapRegion(gaddr base, uint32_t size) {
	void *host = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (host == MAP_FAILED)
		fatal("mmap of %u bytes failed", size);
	if (uc_mem_map_ptr(uc, base, size, UC_PROT_ALL, host) != UC_ERR_OK)
		fatal("uc_mem_map_ptr 0x%08x failed", base);
	regions[numRegions++] = {base, size, (uint8_t *)host};
}

uint8_t *ptr(gaddr a) {
	for (int i = 0; i < numRegions; i++) {
		if (a - regions[i].base < regions[i].size)
			return regions[i].host + (a - regions[i].base);
	}
	fatal("access to unmapped guest address 0x%08x", a);
}

bool valid(gaddr a, uint32_t len) {
	for (int i = 0; i < numRegions; i++) {
		if (a - regions[i].base < regions[i].size && len <= regions[i].size - (a - regions[i].base))
			return true;
	}
	return false;
}

std::string cstr(gaddr a) {
	std::string s;
	if (!a)
		return s;
	for (;; a++) {
		char ch = (char)r8(a);
		if (!ch)
			break;
		s += ch;
	}
	return s;
}

std::string pstr(gaddr a) {
	if (!a)
		return std::string();
	uint8_t len = r8(a);
	return std::string((const char *)ptr(a + 1), len);
}

void wpstr(gaddr a, const std::string &s, int maxLen) {
	int len = (int)s.size() < maxLen ? (int)s.size() : maxLen;
	w8(a, (uint8_t)len);
	memcpy(ptr(a + 1), s.data(), len);
}

uint32_t reg(int r) {
	uint32_t v = 0;
	uc_reg_read(uc, r, &v);
	return v;
}

void setReg(int r, uint32_t v) {
	uc_reg_write(uc, r, &v);
}

gaddr sysAlloc(uint32_t size, uint32_t align) {
	sysNext = (sysNext + align - 1) & ~(align - 1);
	gaddr a = sysNext;
	sysNext += size;
	if (sysNext > kSysBase + kSysSize)
		fatal("system region exhausted");
	memset(ptr(a), 0, size);
	return a;
}

gaddr sysString(const std::string &s) {
	static std::map<std::string, gaddr> cache;
	auto it = cache.find(s);
	if (it != cache.end())
		return it->second;
	gaddr a = sysAlloc((uint32_t)s.size() + 1, 1);
	memcpy(ptr(a), s.c_str(), s.size() + 1);
	cache[s] = a;
	return a;
}

static int callDepth = 0;

uint32_t call(gaddr fn, const std::vector<uint32_t> &args) {
	static const int saved[] = {UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
		UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EFLAGS};
	uint32_t regs[10];
	for (int i = 0; i < 10; i++)
		regs[i] = reg(saved[i]);

	// Darwin i386 wants the stack 16-byte aligned at the call
	uint32_t esp = regs[7] - 64;
	esp -= 4 * (uint32_t)args.size();
	esp &= ~15u;
	for (size_t i = 0; i < args.size(); i++)
		w32(esp + 4 * (uint32_t)i, args[i]);
	esp -= 4;
	w32(esp, kReturnTrap);
	setReg(UC_X86_REG_ESP, esp);
	setReg(UC_X86_REG_EIP, fn);

	callDepth++;
	uc_err err = uc_emu_start(uc, fn, kReturnTrap, 0, 0);
	callDepth--;
	if (err != UC_ERR_OK)
		fatal("guest call to 0x%08x failed: %s", fn, uc_strerror(err));
	if (reg(UC_X86_REG_EIP) != kReturnTrap)
		fatal("guest call to 0x%08x stopped at 0x%08x", fn, reg(UC_X86_REG_EIP));
	uint32_t result = reg(UC_X86_REG_EAX);

	for (int i = 0; i < 10; i++) {
		if (saved[i] != UC_X86_REG_EAX)
			setReg(saved[i], regs[i]);
	}
	setReg(UC_X86_REG_EAX, regs[0]);
	return result;
}

// Block profile (porting aid, "prof on|off" control command)
static std::map<uint32_t, uint64_t> profile;
static uc_hook profHook;
static void hookBlock(uc_engine *, uint64_t addr, uint32_t, void *) {
	profile[(uint32_t)addr]++;
}
void setProfiling(bool on) {
	if (on) {
		profile.clear();
		uc_hook_add(uc, &profHook, UC_HOOK_BLOCK, (void *)hookBlock, nullptr, 1, 0);
		uc_ctl_flush_tb(uc); // blocks already translated lack the hook
		return;
	}
	uc_hook_del(uc, profHook);
	std::vector<std::pair<uint64_t, uint32_t>> top;
	for (auto &p : profile)
		top.push_back({p.second, p.first});
	std::sort(top.rbegin(), top.rend());
	for (size_t i = 0; i < top.size() && i < 60; i++)
		logf("prof %08x %llu", top[i].second, (unsigned long long)top[i].first);
}

static void hookBreak(uc_engine *, uint64_t addr, uint32_t, void *) {
	uint32_t esp = reg(UC_X86_REG_ESP);
	logf("break 0x%08llx from 0x%08x args %08x %08x %08x", (unsigned long long)addr, r32(esp), r32(esp + 4), r32(esp + 8),
		r32(esp + 12));
}

void dumpState() {
	uint32_t eip = reg(UC_X86_REG_EIP), esp = reg(UC_X86_REG_ESP), ebp = reg(UC_X86_REG_EBP);
	fprintf(stderr, "  eip=%08x esp=%08x ebp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n",
		eip, esp, ebp, reg(UC_X86_REG_EAX), reg(UC_X86_REG_EBX), reg(UC_X86_REG_ECX),
		reg(UC_X86_REG_EDX), reg(UC_X86_REG_ESI), reg(UC_X86_REG_EDI));
	// Walk the frame pointer chain for a rough backtrace
	fprintf(stderr, "  backtrace:");
	for (int i = 0; i < 16 && valid(ebp, 8); i++) {
		fprintf(stderr, " %08x", r32(ebp + 4));
		ebp = r32(ebp);
	}
	fprintf(stderr, "\n");
}

void fatal(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "fatal: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	if (uc)
		dumpState();
	fflush(stderr);
	_exit(1);
}

static void hookInvalid(uc_engine *, uc_mem_type type, uint64_t address, int size, int64_t, void *) {
	fatal("invalid memory %s at 0x%08llx (%d bytes)",
		type == UC_MEM_WRITE_UNMAPPED ? "write" : type == UC_MEM_FETCH_UNMAPPED ? "fetch" : "read",
		(unsigned long long)address, size);
}

void init() {
	if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc) != UC_ERR_OK)
		fatal("uc_open failed");
	mapRegion(kImageBase, kImageEnd - kImageBase);
	mapRegion(kHeapBase, kHeapSize);
	mapRegion(kStackBase, kStackSize);
	mapRegion(kSysBase, kSysSize);

	// Enable SSE: the i386 ABI on Mac OS X uses it for floating point
	uint64_t cr0 = 0, cr4 = 0;
	uc_reg_read(uc, UC_X86_REG_CR0, &cr0);
	uc_reg_read(uc, UC_X86_REG_CR4, &cr4);
	cr0 = (cr0 & ~4ull) | 2ull;  // clear EM, set MP
	cr4 |= (1ull << 9) | (1ull << 10); // OSFXSR, OSXMMEXCPT
	uc_reg_write(uc, UC_X86_REG_CR0, &cr0);
	uc_reg_write(uc, UC_X86_REG_CR4, &cr4);

	uc_hook h;
	uc_hook_add(uc, &h, UC_HOOK_MEM_INVALID, (void *)hookInvalid, nullptr, 1, 0);

	// TS_BREAK=addr,addr,... (porting aid): logs each time the guest executes one
	if (const char *b = getenv("TS_BREAK")) {
		for (const char *p = b; *p;) {
			char *end;
			uint64_t a = strtoull(p, &end, 16);
			if (end == p)
				break;
			uc_hook bh;
			uc_hook_add(uc, &bh, UC_HOOK_CODE, (void *)hookBreak, nullptr, a, a);
			p = *end ? end + 1 : end;
		}
	}

	// The return trap is never executed: emulation stops on reaching it
	w8(kReturnTrap, 0xF4);
	setReg(UC_X86_REG_ESP, kStackBase + kStackSize - 0x1000);
	traceImports = getenv("TS_TRACE") != nullptr;
}

} // namespace emu

static bool logEnabled = getenv("TS_LOG") != nullptr;

void logf(const char *fmt, ...) {
	if (!logEnabled)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

// ---------------------------------------------------------------------------
// Imports

namespace {

struct ImportEntry {
	std::string name;
	ImportFn fn;
	int kind;
	bool called;
};

std::map<std::string, std::pair<ImportFn, int>> &registry() {
	static std::map<std::string, std::pair<ImportFn, int>> r;
	return r;
}

std::map<std::string, gaddr (*)()> &dataRegistry() {
	static std::map<std::string, gaddr (*)()> r;
	return r;
}

std::vector<ImportEntry> thunks;
std::map<std::string, gaddr> thunkByName;

void hookIntr(uc_engine *, uint32_t intno, void *) {
	if (intno != 0x81)
		emu::fatal("unexpected interrupt 0x%x", intno);
	uint32_t eip = emu::reg(UC_X86_REG_EIP);
	uint32_t idx = (eip - 2 - kThunkBase) / kThunkSize;
	if (idx >= thunks.size())
		emu::fatal("interrupt from outside the thunks at 0x%08x", eip);
	ImportEntry &e = thunks[idx];
	Ctx c;
	c.esp = emu::reg(UC_X86_REG_ESP);
	if (!e.fn) {
		// TS_LENIENT=1 (porting aid): log and return 0 instead of stopping
		static bool lenient = getenv("TS_LENIENT") != nullptr;
		if (!lenient)
			emu::fatal("unimplemented import %s, called from 0x%08x", e.name.c_str(), emu::r32(c.esp));
		if (!e.called)
			logf("MISSING %s from 0x%08x", e.name.c_str(), emu::r32(c.esp));
		e.called = true;
		c.ret(0);
		return;
	}
	if (emu::traceImports || !e.called)
		logf("%s %s from 0x%08x", e.called ? "call" : "first call", e.name.c_str(), emu::r32(c.esp));
	e.called = true;
	e.fn(c);
}

} // namespace

ImportReg::ImportReg(const char *name, ImportFn fn, int kind) {
	registry()[name] = {fn, kind};
}

DataReg::DataReg(const char *name, gaddr (*make)()) {
	dataRegistry()[name] = make;
}

namespace imports {

gaddr thunk(const std::string &name) {
	auto it = thunkByName.find(name);
	if (it != thunkByName.end())
		return it->second;
	if (thunks.size() >= kMaxThunks)
		emu::fatal("too many imports");
	auto r = registry().find(name);
	ImportEntry e{name, nullptr, 0, false};
	if (r != registry().end()) {
		e.fn = r->second.first;
		e.kind = r->second.second;
	}
	gaddr a = kThunkBase + (gaddr)thunks.size() * kThunkSize;
	uint8_t *p = emu::ptr(a);
	int n = 0;
	p[n++] = 0xCD; // int 0x81
	p[n++] = 0x81;
	if (e.kind == 1) {
		p[n++] = 0xDD; // fld qword [kFloatScratch]
		p[n++] = 0x05;
		uint32_t s = kFloatScratch;
		memcpy(p + n, &s, 4);
		n += 4;
	}
	if (e.kind == 2) {
		p[n++] = 0xC2; // ret 4
		p[n++] = 4;
		p[n++] = 0;
	} else {
		p[n++] = 0xC3; // ret
	}
	thunks.push_back(e);
	thunkByName[name] = a;
	return a;
}

gaddr data(const std::string &name, bool *found) {
	static std::map<std::string, gaddr> made;
	auto m = made.find(name);
	if (m != made.end()) {
		*found = true;
		return m->second;
	}
	auto it = dataRegistry().find(name);
	if (it == dataRegistry().end()) {
		*found = false;
		return 0;
	}
	*found = true;
	gaddr a = it->second();
	made[name] = a;
	return a;
}

void installHooks() {
	uc_hook h;
	uc_hook_add(emu::uc, &h, UC_HOOK_INTR, (void *)hookIntr, nullptr, 1, 0);
}

} // namespace imports
