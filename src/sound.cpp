// Sound Manager. Sample data is converted to float when a bufferCmd is
// queued, so the audio thread never touches guest memory. Sound headers may
// come straight from 'snd ' resources (big-endian) or be built by the game
// (native), so each one is read in whichever byte order gives a sane rate.
#include "sound.h"
#include "emu.h"
#include "heap.h"
#include "host.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace {

enum {
	quietCmd = 3, flushCmd = 4, reInitCmd = 5, waitCmd = 10, pauseCmd = 11, resumeCmd = 12, callBackCmd = 13,
	syncCmd = 14, ampCmd = 43, getAmpCmd = 45, volumeCmd = 46, getVolumeCmd = 47, soundCmd = 80, bufferCmd = 81,
	rateCmd = 82, getRateCmd = 85, rateMultiplierCmd = 86, getRateMultiplierCmd = 87,
};

struct Sample {
	std::vector<float> data; // interleaved
	int channels = 1;
	double rate = 22050;
	size_t frames() const { return data.size() / channels; }
};

struct Cmd {
	uint16_t cmd;
	uint16_t param1;
	uint32_t param2;
	std::shared_ptr<Sample> sample; // bufferCmd
};

struct Channel {
	gaddr addr;
	gaddr callback;
	std::deque<Cmd> queue;
	std::shared_ptr<Sample> playing;
	double pos = 0;
	double rateMul = 1.0;
	float volL = 1, volR = 1, amp = 1;
	bool paused = false;
	double waitUntil = 0; // seconds of audio time still to wait
};

std::mutex lock;
std::map<gaddr, Channel *> channels_;
std::deque<std::pair<gaddr, Cmd>> pendingCallbacks; // filled by the audio thread
gaddr cmdScratch = 0;

inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

double extendedToDouble(const uint8_t *p) {
	int exp = ((p[0] & 0x7F) << 8) | p[1];
	uint64_t mant = 0;
	for (int i = 0; i < 8; i++)
		mant = mant << 8 | p[2 + i];
	if (!exp && !mant)
		return 0;
	double v = (double)mant * pow(2.0, exp - 16383 - 63);
	return (p[0] & 0x80) ? -v : v;
}

int16_t imaStep(int &predictor, int &index, int nibble) {
	static const int steps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
		60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
		544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
		3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
		16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
	static const int adjust[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
	int step = steps[index];
	int diff = step >> 3;
	if (nibble & 4) diff += step;
	if (nibble & 2) diff += step >> 1;
	if (nibble & 1) diff += step >> 2;
	predictor += (nibble & 8) ? -diff : diff;
	predictor = std::max(-32768, std::min(32767, predictor));
	index = std::max(0, std::min(88, index + adjust[nibble]));
	return (int16_t)predictor;
}

// Reads a SoundHeader at `h` into float samples
std::shared_ptr<Sample> readHeader(gaddr h) {
	const uint8_t *p = emu::ptr(h);
	// Byte order: native for game-built and flipped resource headers, but
	// big-endian is accepted too. The rate must be audible and the length sane.
	auto sane = [&](bool be) {
		double rate = (be ? be32(p + 8) : le32(p + 8)) / 65536.0;
		uint32_t len = be ? be32(p + (p[20] == 0 ? 4 : 22)) : le32(p + (p[20] == 0 ? 4 : 22));
		return rate >= 1000 && rate <= 100000 && len < (16u << 20);
	};
	bool big = !sane(false);
	if (big && !sane(true)) {
		logf("snd: implausible sound header at 0x%08x", h);
		return nullptr;
	}
	double rateBE = be32(p + 8) / 65536.0, rateLE = le32(p + 8) / 65536.0;
	auto u32 = [&](int off) { return big ? be32(p + off) : le32(p + off); };
	auto u16 = [&](int off) { return big ? be16(p + off) : (uint16_t)(p[off] | p[off + 1] << 8); };
	auto s = std::make_shared<Sample>();
	s->rate = big ? rateBE : rateLE;
	uint8_t encode = p[20];
	gaddr samplePtr = u32(0);
	if (encode == 0) { // stdSH: 8-bit offset binary, mono
		uint32_t n = u32(4);
		const uint8_t *d = samplePtr ? emu::ptr(samplePtr) : p + 22;
		s->data.resize(n);
		for (uint32_t i = 0; i < n; i++)
			s->data[i] = (d[i] - 128) / 128.0f;
	} else if (encode == 0xFF) { // extSH
		int ch = (int)u32(4);
		uint32_t frames = u32(22);
		int bits = u16(48);
		const uint8_t *d = samplePtr ? emu::ptr(samplePtr) : p + 64;
		s->channels = std::max(1, std::min(ch, 2));
		s->data.resize((size_t)frames * s->channels);
		for (uint32_t f = 0; f < frames; f++) {
			for (int c = 0; c < s->channels; c++) {
				size_t i = (size_t)f * ch + c;
				float v = bits == 16 ? (int16_t)(d[i * 2] << 8 | d[i * 2 + 1]) / 32768.0f : (d[i] - 128) / 128.0f;
				s->data[(size_t)f * s->channels + c] = v;
			}
		}
	} else if (encode == 0xFE) { // cmpSH
		int ch = (int)u32(4);
		uint32_t frames = u32(22);
		uint32_t format = u32(40);
		int bits = u16(62);
		const uint8_t *d = samplePtr ? emu::ptr(samplePtr) : p + 64;
		s->channels = std::max(1, std::min(ch, 2));
		if (format == 0x696D6134) { // 'ima4': 34-byte packets of 64 samples per channel
			s->data.resize((size_t)frames * 64 * s->channels);
			for (uint32_t pk = 0; pk < frames; pk++) {
				for (int c = 0; c < ch; c++) {
					const uint8_t *q = d + ((size_t)pk * ch + c) * 34;
					int pred = (int16_t)(be16(q) & 0xFF80), idx = std::min(88, q[1] & 0x7F);
					for (int i = 0; i < 32; i++) {
						int16_t a = imaStep(pred, idx, q[2 + i] & 15);
						int16_t b = imaStep(pred, idx, q[2 + i] >> 4);
						if (c < s->channels) {
							s->data[((size_t)pk * 64 + i * 2) * s->channels + c] = a / 32768.0f;
							s->data[((size_t)pk * 64 + i * 2 + 1) * s->channels + c] = b / 32768.0f;
						}
					}
				}
			}
		} else {
			bool le = format == 0x736F7774; // 'sowt'
			if (bits != 8 && bits != 16)
				bits = 16;
			s->data.resize((size_t)frames * s->channels);
			for (uint32_t f = 0; f < frames; f++) {
				for (int c = 0; c < s->channels; c++) {
					size_t i = (size_t)f * ch + c;
					float v = bits == 16 ? (int16_t)(le ? (d[i * 2] | d[i * 2 + 1] << 8) : (d[i * 2] << 8 | d[i * 2 + 1])) / 32768.0f
						: (format == 0x74776F73 ? (int8_t)d[i] / 128.0f : (d[i] - 128) / 128.0f);
					s->data[(size_t)f * s->channels + c] = v;
				}
			}
		}
	} else {
		logf("snd: unknown encode 0x%02x", encode);
		return nullptr;
	}
	return s;
}

// Audio thread: advance a channel's queue until something plays
void advance(Channel *c, double secondsPerFrame) {
	(void)secondsPerFrame;
	while (!c->playing && c->waitUntil <= 0 && !c->queue.empty() && !c->paused) {
		Cmd cmd = c->queue.front();
		c->queue.pop_front();
		switch (cmd.cmd & 0x7FFF) {
		case bufferCmd:
		case soundCmd:
			if (cmd.sample) {
				c->playing = cmd.sample;
				c->pos = 0;
			}
			break;
		case callBackCmd:
			pendingCallbacks.push_back({c->addr, cmd});
			break;
		case waitCmd:
			c->waitUntil = cmd.param1 / 2000.0;
			break;
		case volumeCmd:
			c->volL = (cmd.param2 & 0xFFFF) / 256.0f;
			c->volR = (cmd.param2 >> 16) / 256.0f;
			break;
		case ampCmd:
			c->amp = cmd.param1 / 255.0f;
			break;
		case rateMultiplierCmd:
			c->rateMul = cmd.param2 / 65536.0;
			break;
		default:
			break;
		}
	}
}

void mix(float *out, int frames, int rate) {
	std::lock_guard<std::mutex> g(lock);
	for (auto &it : channels_) {
		Channel *c = it.second;
		int f = 0;
		while (f < frames) {
			advance(c, 1.0 / rate);
			if (c->paused)
				break;
			if (c->waitUntil > 0) {
				int n = std::min(frames - f, (int)(c->waitUntil * rate) + 1);
				c->waitUntil -= (double)n / rate;
				f += n;
				continue;
			}
			if (!c->playing)
				break;
			Sample &s = *c->playing;
			double step = s.rate / rate * c->rateMul;
			size_t total = s.frames();
			float gl = c->volL * c->amp, gr = c->volR * c->amp;
			for (; f < frames; f++) {
				size_t i = (size_t)c->pos;
				if (i >= total) {
					c->playing.reset();
					break;
				}
				double frac = c->pos - i;
				size_t j = i + 1 < total ? i + 1 : i;
				float l, r;
				if (s.channels == 2) {
					l = (float)(s.data[i * 2] * (1 - frac) + s.data[j * 2] * frac);
					r = (float)(s.data[i * 2 + 1] * (1 - frac) + s.data[j * 2 + 1] * frac);
				} else {
					l = r = (float)(s.data[i] * (1 - frac) + s.data[j] * frac);
				}
				out[f * 2] += l * gl;
				out[f * 2 + 1] += r * gr;
				c->pos += step;
			}
		}
		// Let commands after the last buffer (callbacks) run promptly
		advance(c, 1.0 / rate);
	}
	for (int i = 0; i < frames * 2; i++)
		out[i] = std::max(-1.0f, std::min(1.0f, out[i]));
}

Channel *find(gaddr chan) {
	auto it = channels_.find(chan);
	return it == channels_.end() ? nullptr : it->second;
}

Cmd readCmd(gaddr a) {
	Cmd c;
	c.cmd = emu::r16(a);
	c.param1 = emu::r16(a + 2);
	c.param2 = emu::r32(a + 4);
	if ((c.cmd & 0x7FFF) == bufferCmd || (c.cmd & 0x7FFF) == soundCmd)
		c.sample = readHeader(c.param2);
	return c;
}

} // namespace

namespace snd {

void init() {
	cmdScratch = heap::alloc(16, true);
	host::startAudio(mix);
}

int play(std::vector<float> &&samples, int channels, double rate) {
	static int nextID = 1;
	auto smp = std::make_shared<Sample>();
	smp->data = std::move(samples);
	smp->channels = channels;
	smp->rate = rate;
	Channel *ch = new Channel;
	int id = nextID++;
	ch->addr = 0xF0000000u + (gaddr)id; // not a guest address: no callbacks
	ch->callback = 0;
	ch->playing = smp;
	std::lock_guard<std::mutex> g(lock);
	channels_[ch->addr] = ch;
	return id;
}

void stop(int id) {
	std::lock_guard<std::mutex> g(lock);
	auto it = channels_.find(0xF0000000u + (gaddr)id);
	if (it != channels_.end()) {
		delete it->second;
		channels_.erase(it);
	}
}

void serviceCallbacks() {
	for (;;) {
		std::pair<gaddr, Cmd> cb;
		gaddr proc = 0;
		{
			std::lock_guard<std::mutex> g(lock);
			if (pendingCallbacks.empty())
				return;
			cb = pendingCallbacks.front();
			pendingCallbacks.pop_front();
			if (Channel *c = find(cb.first))
				proc = c->callback;
		}
		if (!proc)
			continue;
		emu::w16(cmdScratch, cb.second.cmd);
		emu::w16(cmdScratch + 2, cb.second.param1);
		emu::w32(cmdScratch + 4, cb.second.param2);
		if (emu::traceImports)
			logf("snd callback 0x%08x chan 0x%08x p1 %u p2 0x%08x", proc, cb.first, cb.second.param1, cb.second.param2);
		emu::call(proc, {cb.first, cmdScratch});
	}
}

} // namespace snd

IMPORT(NewSndCallBackUPP) { c.ret(c.arg(0)); }
IMPORT(DisposeSndCallBackUPP) {}

IMPORT(SndNewChannel) {
	// (SndChannelPtr *chan, synth, init, userRoutine)
	gaddr chan = emu::r32(c.arg(0));
	if (!chan) {
		chan = heap::alloc(36 + 128 * 8, true);
		emu::w32(c.arg(0), chan);
	}
	emu::w32(chan + 8, c.arg(3));
	emu::w16(chan + 30, 128); // qLength
	Channel *ch = new Channel;
	ch->addr = chan;
	ch->callback = c.arg(3);
	std::lock_guard<std::mutex> g(lock);
	channels_[chan] = ch;
	c.ret(0);
}

IMPORT(SndDisposeChannel) {
	std::lock_guard<std::mutex> g(lock);
	if (Channel *ch = find(c.arg(0))) {
		channels_.erase(c.arg(0));
		delete ch;
	}
	c.ret(0);
}

IMPORT(SndDoCommand) {
	Cmd cmd = readCmd(c.arg(1));
	if (emu::traceImports)
		logf("SndDoCommand chan 0x%08x cmd %u p1 %u p2 0x%08x%s", c.arg(0), cmd.cmd, cmd.param1, cmd.param2,
			cmd.sample ? " (sound)" : "");
	std::lock_guard<std::mutex> g(lock);
	Channel *ch = find(c.arg(0));
	if (!ch) {
		c.ret((uint32_t)-205); // badChannel
		return;
	}
	if (ch->queue.size() >= 128) {
		c.ret((uint32_t)-28); // queueFull
		return;
	}
	ch->queue.push_back(cmd);
	c.ret(0);
}

IMPORT(SndDoImmediate) {
	Cmd cmd = readCmd(c.arg(1));
	gaddr cmdAddr = c.arg(1);
	std::unique_lock<std::mutex> g(lock);
	Channel *ch = find(c.arg(0));
	if (!ch) {
		c.ret((uint32_t)-205);
		return;
	}
	switch (cmd.cmd & 0x7FFF) {
	case quietCmd:
		ch->playing.reset();
		ch->waitUntil = 0;
		break;
	case flushCmd:
		ch->queue.clear();
		break;
	case pauseCmd:
		ch->paused = true;
		break;
	case resumeCmd:
		ch->paused = false;
		break;
	case volumeCmd:
		ch->volL = (cmd.param2 & 0xFFFF) / 256.0f;
		ch->volR = (cmd.param2 >> 16) / 256.0f;
		break;
	case getVolumeCmd:
		emu::w32(cmd.param2, (uint32_t)(ch->volR * 256) << 16 | (uint32_t)(ch->volL * 256));
		break;
	case ampCmd:
		ch->amp = cmd.param1 / 255.0f;
		break;
	case getAmpCmd:
		emu::w16(cmd.param2, (uint16_t)(ch->amp * 255));
		break;
	case rateMultiplierCmd:
		ch->rateMul = cmd.param2 / 65536.0;
		break;
	case getRateMultiplierCmd:
		emu::w32(cmd.param2, (uint32_t)(ch->rateMul * 65536));
		break;
	case bufferCmd:
	case soundCmd:
		if (cmd.sample) {
			ch->playing = cmd.sample;
			ch->pos = 0;
		}
		break;
	case callBackCmd: {
		gaddr proc = ch->callback;
		g.unlock();
		if (proc)
			emu::call(proc, {c.arg(0), cmdAddr});
		break;
	}
	default:
		logf("SndDoImmediate: command %u", cmd.cmd);
		break;
	}
	c.ret(0);
}

IMPORT(SndChannelStatus) {
	// (chan, length, SCStatus *): startTime, endTime, currentTime, busy, disposed, paused
	std::lock_guard<std::mutex> g(lock);
	Channel *ch = find(c.arg(0));
	gaddr st = c.arg(2);
	memset(emu::ptr(st), 0, std::min<uint32_t>(c.arg(1), 24));
	if (!ch) {
		c.ret((uint32_t)-205);
		return;
	}
	bool busy = ch->playing || ch->waitUntil > 0;
	for (auto &q : ch->queue)
		busy = busy || q.sample;
	emu::w8(st + 12, busy);
	emu::w8(st + 14, ch->paused);
	c.ret(0);
}
