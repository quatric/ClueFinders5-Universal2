// CoreAudio HAL: the game only inspects the output device (format, volume),
// so it sees one stereo 44.1 kHz device. Sound itself goes through the
// Sound Manager.
#include "emu.h"

namespace {

const uint32_t kDevice = 1;
const uint32_t kUnknownProperty = 0x77686F3F; // 'who?'

uint32_t fourcc(const char *s) {
	return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint32_t)s[3];
}

std::string fourccStr(uint32_t v) {
	char s[5] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, 0};
	return s;
}

float gVolume = 1.0f;

// Writes a property value; returns the size, or 0 if unknown
uint32_t deviceProperty(uint32_t id, uint32_t channel, gaddr out, uint32_t room) {
	uint8_t buf[64];
	uint32_t size = 0;
	auto put32 = [&](uint32_t v) { memcpy(buf + size, &v, 4); size += 4; };
	if (id == fourcc("nsrt")) { // nominal sample rate
		double r = 44100.0;
		memcpy(buf, &r, 8);
		size = 8;
	} else if (id == fourcc("sfmt") || id == fourcc("pft ") || id == fourcc("sfma")) {
		double r = 44100.0;
		memcpy(buf, &r, 8);
		size = 8;
		put32(fourcc("lpcm"));
		put32(9); // float, packed
		put32(8);
		put32(1);
		put32(8);
		put32(2);
		put32(32);
		put32(0);
	} else if (id == fourcc("bsiz")) {
		put32(2048);
	} else if (id == fourcc("fsiz")) {
		put32(512);
	} else if (id == fourcc("volm")) { // scalar volume
		memcpy(buf, &gVolume, 4);
		size = 4;
	} else if (id == fourcc("mute")) {
		put32(0);
	} else if (id == fourcc("dch#")) { // preferred stereo channels
		put32(1);
		put32(2);
	} else if (id == fourcc("alve") || id == fourcc("goin")) { // alive, running
		put32(1);
	} else if (id == fourcc("uid ") || id == fourcc("name")) {
		size = 0;
	} else {
		return 0;
	}
	if (out && size <= room)
		memcpy(emu::ptr(out), buf, size);
	return size;
}

} // namespace

IMPORT(AudioHardwareGetProperty) {
	uint32_t id = c.arg(0);
	gaddr ioSize = c.arg(1), out = c.arg(2);
	if (id == fourcc("dOut") || id == fourcc("sOut")) {
		emu::w32(out, kDevice);
		emu::w32(ioSize, 4);
		c.ret(0);
	} else if (id == fourcc("dev#")) {
		emu::w32(out, kDevice);
		emu::w32(ioSize, 4);
		c.ret(0);
	} else {
		logf("AudioHardwareGetProperty('%s'): unknown", fourccStr(id).c_str());
		c.ret(kUnknownProperty);
	}
}

IMPORT(AudioDeviceGetPropertyInfo) {
	uint32_t id = c.arg(3);
	uint32_t size = deviceProperty(id, c.arg(1), 0, 0);
	if (!size) {
		logf("AudioDeviceGetPropertyInfo('%s'): unknown", fourccStr(id).c_str());
		c.ret(kUnknownProperty);
		return;
	}
	if (c.arg(4))
		emu::w32(c.arg(4), size);
	if (c.arg(5))
		emu::w8(c.arg(5), id == fourcc("volm") ? 1 : 0); // writable
	c.ret(0);
}

IMPORT(AudioDeviceGetProperty) {
	uint32_t id = c.arg(3);
	gaddr ioSize = c.arg(4), out = c.arg(5);
	uint32_t size = deviceProperty(id, c.arg(1), out, emu::r32(ioSize));
	if (!size) {
		logf("AudioDeviceGetProperty('%s'): unknown", fourccStr(id).c_str());
		c.ret(kUnknownProperty);
		return;
	}
	emu::w32(ioSize, size);
	c.ret(0);
}

IMPORT(AudioDeviceSetProperty) {
	uint32_t id = c.arg(4);
	if (id == fourcc("volm"))
		gVolume = emu::rfloat(c.arg(6));
	else
		logf("AudioDeviceSetProperty('%s') ignored", fourccStr(id).c_str());
	c.ret(0);
}

IMPORT(AudioDeviceAddPropertyListener) { c.ret(0); }
IMPORT(AudioDeviceRemovePropertyListener) { c.ret(0); }
IMPORT(AudioHardwareAddPropertyListener) { c.ret(0); }
IMPORT(AudioHardwareRemovePropertyListener) { c.ret(0); }
