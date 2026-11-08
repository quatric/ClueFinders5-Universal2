// Core of the i386 compatibility layer: guest memory, the CPU, imports and
// calls back into guest code.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unicorn/unicorn.h>

typedef uint32_t gaddr;

// Guest memory map
enum : gaddr {
	kImageBase = 0x00001000,
	kImageEnd = 0x00200000,
	kHeapBase = 0x10000000,
	kHeapSize = 0x40000000,
	kStackBase = 0xB0000000,
	kStackSize = 0x01000000,
	kSysBase = 0xE0000000,
	kSysSize = 0x01000000,

	kThunkBase = kSysBase,                 // one 16-byte thunk per import
	kThunkSize = 16,
	kMaxThunks = 0x4000,
	kReturnTrap = kSysBase + 0x00080000,   // native -> guest calls return here
	kFloatScratch = kSysBase + 0x00080010, // double results of imports
	kSysDataBase = kSysBase + 0x00100000,  // imported data symbols, constants
};

namespace emu {

extern uc_engine *uc;

// Host pointer for a guest address. Valid for the whole mapped region.
uint8_t *ptr(gaddr a);
inline uint32_t r32(gaddr a) { uint32_t v; memcpy(&v, ptr(a), 4); return v; }
inline uint16_t r16(gaddr a) { uint16_t v; memcpy(&v, ptr(a), 2); return v; }
inline uint8_t r8(gaddr a) { return *ptr(a); }
inline int16_t rs16(gaddr a) { return (int16_t)r16(a); }
inline double rdouble(gaddr a) { double v; memcpy(&v, ptr(a), 8); return v; }
inline float rfloat(gaddr a) { float v; memcpy(&v, ptr(a), 4); return v; }
inline void w32(gaddr a, uint32_t v) { memcpy(ptr(a), &v, 4); }
inline void w16(gaddr a, uint16_t v) { memcpy(ptr(a), &v, 2); }
inline void w8(gaddr a, uint8_t v) { *ptr(a) = v; }
inline void wdouble(gaddr a, double v) { memcpy(ptr(a), &v, 8); }
inline void wfloat(gaddr a, float v) { memcpy(ptr(a), &v, 4); }
bool valid(gaddr a, uint32_t len = 1);
std::string cstr(gaddr a);
std::string pstr(gaddr a); // Pascal string
void wpstr(gaddr a, const std::string &s, int maxLen = 255);

uint32_t reg(int r);
void setReg(int r, uint32_t v);

// Bump allocator for permanent data in the system region (data symbols,
// fake vtables, constant strings handed to the guest)
gaddr sysAlloc(uint32_t size, uint32_t align = 16);
gaddr sysString(const std::string &s);

// Calls a guest function (cdecl) and returns EAX
uint32_t call(gaddr fn, const std::vector<uint32_t> &args);

void init();
[[noreturn]] void fatal(const char *fmt, ...);
void dumpState();
void setProfiling(bool on);
extern bool traceImports;

} // namespace emu

// Arguments and results of an import call. Arguments are 32-bit words on the
// stack after the return address.
struct Ctx {
	uint32_t esp;
	uint32_t arg(int n) const { return emu::r32(esp + 4 + 4 * n); }
	int32_t sarg(int n) const { return (int32_t)arg(n); }
	int16_t sarg16(int n) const { return (int16_t)arg(n); }
	double argd(int n) const { return emu::rdouble(esp + 4 + 4 * n); }
	float argf(int n) const { return emu::rfloat(esp + 4 + 4 * n); }
	void ret(uint32_t v) const { emu::setReg(UC_X86_REG_EAX, v); }
	void ret64(uint64_t v) const {
		emu::setReg(UC_X86_REG_EAX, (uint32_t)v);
		emu::setReg(UC_X86_REG_EDX, (uint32_t)(v >> 32));
	}
	void retd(double v) const { emu::wdouble(kFloatScratch, v); }
};

typedef void (*ImportFn)(Ctx &c);

struct ImportReg {
	// kind: 0 plain, 1 double result in ST(0), 2 struct returned through a
	// hidden first argument, which the callee pops
	ImportReg(const char *name, ImportFn fn, int kind);
};

#define IMPORT(name) \
	static void imp_##name(Ctx &c); \
	static ImportReg reg_##name(#name, imp_##name, 0); \
	static void imp_##name(Ctx &c)
// For functions that return a float or double in ST(0)
#define IMPORT_F(name) \
	static void imp_##name(Ctx &c); \
	static ImportReg reg_##name(#name, imp_##name, 1); \
	static void imp_##name(Ctx &c)

// For functions returning a struct larger than 8 bytes: arg(0) is where it
// goes, and the result should be that address (c.ret(c.arg(0)))
#define IMPORT_SRET(name) \
	static void imp_##name(Ctx &c); \
	static ImportReg reg_##name(#name, imp_##name, 2); \
	static void imp_##name(Ctx &c)

// Imported data symbols: the loader points __pointers entries at the address
// this returns
struct DataReg {
	DataReg(const char *name, gaddr (*make)());
};
#define IMPORT_DATA(name) \
	static gaddr data_##name(); \
	static DataReg dreg_##name(#name, data_##name); \
	static gaddr data_##name()

namespace imports {
// Thunk address for an import by name (registers an unimplemented stub if
// there is no native implementation)
gaddr thunk(const std::string &name);
gaddr data(const std::string &name, bool *found);
void installHooks();
}

// Debug logging, enabled with TS_LOG=1
void logf(const char *fmt, ...);
