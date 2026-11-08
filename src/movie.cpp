// QuickTime movies: enough of the Movie Toolbox to play the game's movies.
// The .mov sample tables are read here; video is decoded by wall-clock time
// into the movie's port (Animation 'rle ' here, H.264 through VideoToolbox)
// and 8/16-bit PCM plays through the mixer. Playback runs from the event
// loop, as QuickTime's own tasking would. Sorenson Video movies are converted
// to H.264 (without B-frames) when the app is built.
#include "files.h"
#include "heap.h"
#include "host.h"
#include "qd.h"
#include "sound.h"

#include <VideoToolbox/VideoToolbox.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>

namespace {

struct Track {
	uint32_t codec = 0;
	uint32_t timescale = 1, duration = 0;
	int width = 0, height = 0, depth = 0;
	int channels = 1, sampleBits = 8;
	std::vector<std::pair<uint32_t, uint32_t>> samples; // offset, size (video)
	std::vector<double> times;                          // start of each video sample, seconds
	std::vector<std::pair<uint32_t, uint32_t>> chunks;  // offset, samples (sound)
	std::vector<uint8_t> avcC;                          // H.264 decoder configuration
	std::vector<bool> sync;                             // keyframes (all if no stss)
};

struct Movie {
	std::vector<uint8_t> file;
	Track video, sound;
	bool hasVideo = false, hasSound = false;
	uint32_t timescale = 600, duration = 0;
	gaddr port = 0;
	qd::Rect box;
	bool playing = false;
	double start = 0, pausedAt = 0;
	int decoded = -1;
	std::vector<uint32_t> frame;
	int soundID = 0;
	VTDecompressionSessionRef vt = nullptr;
	CMVideoFormatDescriptionRef format = nullptr;
	~Movie();
};

std::map<gaddr, Movie *> movies;
std::map<int16_t, std::string> movieFiles;
int16_t nextFileRef = 3000;

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

uint32_t fourcc(const char *s) { return be32((const uint8_t *)s); }

// Calls fn(type, offset of contents, size of contents) for each atom
template <typename F> void atoms(const std::vector<uint8_t> &d, size_t off, size_t end, F fn) {
	while (off + 8 <= end) {
		uint32_t size = be32(&d[off]), type = be32(&d[off + 4]);
		if (size < 8 || off + size > end)
			break;
		fn(type, off + 8, (size_t)size - 8);
		off += size;
	}
}

void readTrack(Movie *m, size_t off, size_t len) {
	const std::vector<uint8_t> &d = m->file;
	Track t;
	uint32_t handler = 0;
	std::vector<std::pair<uint32_t, uint32_t>> stts, stsc; // count/samplesPerChunk pairs
	std::vector<uint32_t> stsz, stco, stss;
	uint32_t uniformSize = 0, sampleCount = 0;
	std::function<void(uint32_t, size_t, size_t)> visit = [&](uint32_t type, size_t o, size_t n) {
		if (type == fourcc("mdia") || type == fourcc("minf") || type == fourcc("stbl")) {
			atoms(d, o, o + n, visit);
		} else if (type == fourcc("mdhd")) {
			t.timescale = be32(&d[o + 12]);
			t.duration = be32(&d[o + 16]);
		} else if (type == fourcc("hdlr")) {
			if (be32(&d[o + 4]) == fourcc("mhlr") || !handler) // not the data handler
				handler = be32(&d[o + 8]);
		} else if (type == fourcc("stsd")) {
			size_t e = o + 8;
			t.codec = be32(&d[e + 4]);
			if (handler == fourcc("vide")) {
				t.width = be16(&d[e + 32]);
				t.height = be16(&d[e + 34]);
				t.depth = be16(&d[e + 82]);
				size_t entryEnd = e + be32(&d[e]);
				atoms(d, e + 86, std::min(entryEnd, o + n), [&](uint32_t ext, size_t eo, size_t en) {
					if (ext == fourcc("avcC"))
						t.avcC.assign(d.begin() + eo, d.begin() + eo + en);
				});
			} else {
				t.channels = be16(&d[e + 24]);
				t.sampleBits = be16(&d[e + 26]);
			}
		} else if (type == fourcc("stts")) {
			for (uint32_t i = 0, n2 = be32(&d[o + 4]); i < n2; i++)
				stts.push_back({be32(&d[o + 8 + 8 * i]), be32(&d[o + 12 + 8 * i])});
		} else if (type == fourcc("stsc")) {
			for (uint32_t i = 0, n2 = be32(&d[o + 4]); i < n2; i++)
				stsc.push_back({be32(&d[o + 8 + 12 * i]), be32(&d[o + 12 + 12 * i])});
		} else if (type == fourcc("stsz")) {
			uniformSize = be32(&d[o + 4]);
			sampleCount = be32(&d[o + 8]);
			if (!uniformSize)
				for (uint32_t i = 0; i < sampleCount; i++)
					stsz.push_back(be32(&d[o + 12 + 4 * i]));
		} else if (type == fourcc("stss")) {
			for (uint32_t i = 0, n2 = be32(&d[o + 4]); i < n2; i++)
				stss.push_back(be32(&d[o + 8 + 4 * i]));
		} else if (type == fourcc("stco")) {
			for (uint32_t i = 0, n2 = be32(&d[o + 4]); i < n2; i++)
				stco.push_back(be32(&d[o + 8 + 4 * i]));
		}
	};
	atoms(d, off, off + len, [&](uint32_t type, size_t o, size_t n) {
		if (type == fourcc("mdia"))
			atoms(d, o, o + n, visit);
	});
	// Samples per chunk for each chunk
	std::vector<uint32_t> perChunk(stco.size(), 0);
	for (size_t i = 0; i < stsc.size(); i++) {
		uint32_t first = stsc[i].first, last = i + 1 < stsc.size() ? stsc[i + 1].first : (uint32_t)stco.size() + 1;
		for (uint32_t c = first; c < last && c - 1 < perChunk.size(); c++)
			perChunk[c - 1] = stsc[i].second;
	}
	if (handler == fourcc("vide")) {
		uint32_t s = 0;
		for (size_t c = 0; c < stco.size(); c++) {
			uint32_t pos = stco[c];
			for (uint32_t k = 0; k < perChunk[c] && s < sampleCount; k++, s++) {
				uint32_t size = uniformSize ? uniformSize : stsz[s];
				t.samples.push_back({pos, size});
				pos += size;
			}
		}
		double time = 0;
		for (auto &e : stts)
			for (uint32_t k = 0; k < e.first; k++) {
				t.times.push_back(time / t.timescale);
				time += e.second;
			}
		t.sync.assign(t.samples.size(), stss.empty());
		for (uint32_t k : stss)
			if (k >= 1 && k <= t.sync.size())
				t.sync[k - 1] = true;
		m->video = t;
		m->hasVideo = !t.samples.empty();
	} else if (handler == fourcc("soun")) {
		for (size_t c = 0; c < stco.size(); c++)
			t.chunks.push_back({stco[c], perChunk[c]});
		m->sound = t;
		m->hasSound = !t.chunks.empty();
	}
}

Movie *load(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return nullptr;
	Movie *m = new Movie;
	m->file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	const std::vector<uint8_t> &d = m->file;
	atoms(d, 0, d.size(), [&](uint32_t type, size_t o, size_t n) {
		if (type != fourcc("moov"))
			return;
		atoms(d, o, o + n, [&](uint32_t t2, size_t o2, size_t n2) {
			if (t2 == fourcc("mvhd")) {
				m->timescale = be32(&d[o2 + 12]);
				m->duration = be32(&d[o2 + 16]);
			} else if (t2 == fourcc("trak")) {
				readTrack(m, o2, n2);
			}
		});
	});
	m->box.right = (int16_t)m->video.width;
	m->box.bottom = (int16_t)m->video.height;
	return m;
}

std::vector<float> soundSamples(Movie *m) {
	std::vector<float> out;
	const Track &t = m->sound;
	int bytes = t.sampleBits == 16 ? 2 : 1;
	int ch = std::max(1, t.channels);
	bool signed16 = t.codec != fourcc("sowt");
	for (auto &c : t.chunks) {
		size_t n = (size_t)c.second * ch;
		if (c.first + n * bytes > m->file.size())
			break;
		const uint8_t *p = &m->file[c.first];
		for (size_t i = 0; i < n; i++) {
			if (bytes == 2)
				out.push_back((int16_t)(signed16 ? (p[2 * i] << 8 | p[2 * i + 1]) : (p[2 * i + 1] << 8 | p[2 * i])) / 32768.0f);
			else
				out.push_back(t.codec == fourcc("twos") ? (int8_t)p[i] / 128.0f : (p[i] - 128) / 128.0f);
		}
	}
	return out;
}

// --- H.264 through VideoToolbox

struct DecodeTarget {
	std::vector<uint32_t> *frame;
	int width, height;
};

void decoded(void *, void *ctx, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
	if (status != noErr || !image)
		return;
	auto *t = (DecodeTarget *)ctx;
	CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddress(image);
	size_t rowBytes = CVPixelBufferGetBytesPerRow(image);
	int w = std::min<int>(t->width, (int)CVPixelBufferGetWidth(image));
	int h = std::min<int>(t->height, (int)CVPixelBufferGetHeight(image));
	t->frame->assign((size_t)t->width * t->height, 0xFF000000u);
	for (int y = 0; y < h; y++) {
		const uint8_t *row = base + y * rowBytes;
		uint32_t *out = t->frame->data() + (size_t)y * t->width;
		for (int x = 0; x < w; x++) // 32ARGB: A, R, G, B bytes
			out[x] = 0xFF000000u | row[4 * x + 1] << 16 | row[4 * x + 2] << 8 | row[4 * x + 3];
	}
	CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
}

bool openDecoder(Movie *m) {
	const std::vector<uint8_t> &a = m->video.avcC;
	if (a.size() < 7)
		return false;
	// avcC: version, profile, compat, level, lengthSize-1, SPS count, SPSs, PPS count, PPSs
	std::vector<const uint8_t *> sets;
	std::vector<size_t> sizes;
	size_t p = 5;
	for (int kind = 0; kind < 2; kind++) {
		if (p >= a.size())
			return false;
		int count = kind == 0 ? a[p] & 0x1F : a[p];
		p++;
		for (int i = 0; i < count && p + 2 <= a.size(); i++) {
			size_t len = be16(&a[p]);
			p += 2;
			if (p + len > a.size())
				return false;
			sets.push_back(&a[p]);
			sizes.push_back(len);
			p += len;
		}
	}
	int nalLength = (a[4] & 3) + 1;
	if (CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, sets.size(), sets.data(), sizes.data(), nalLength,
			&m->format) != noErr)
		return false;
	int32_t fmt = kCVPixelFormatType_32ARGB;
	CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fmt);
	const void *keys[] = {kCVPixelBufferPixelFormatTypeKey};
	const void *vals[] = {n};
	CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(n);
	VTDecompressionOutputCallbackRecord cb = {decoded, nullptr};
	OSStatus st = VTDecompressionSessionCreate(nullptr, m->format, nullptr, attrs, &cb, &m->vt);
	CFRelease(attrs);
	return st == noErr;
}

void decodeH264(Movie *m, int index, bool show) {
	if (!m->vt && !openDecoder(m)) {
		logf("movie: can't decode H.264");
		m->video.codec = 0;
		return;
	}
	auto &s = m->video.samples[index];
	if (s.first + s.second > m->file.size())
		return;
	CMBlockBufferRef block = nullptr;
	if (CMBlockBufferCreateWithMemoryBlock(nullptr, &m->file[s.first], s.second, kCFAllocatorNull, nullptr, 0, s.second,
			0, &block) != noErr)
		return;
	CMSampleBufferRef sample = nullptr;
	size_t size = s.second;
	if (CMSampleBufferCreateReady(nullptr, block, m->format, 1, 0, nullptr, 1, &size, &sample) == noErr) {
		DecodeTarget t{&m->frame, m->video.width, m->video.height};
		std::vector<uint32_t> scratch;
		if (!show)
			t.frame = &scratch;
		VTDecompressionSessionDecodeFrame(m->vt, sample, 0, &t, nullptr);
		CFRelease(sample);
	}
	CFRelease(block);
}

Movie::~Movie() {
	if (vt) {
		VTDecompressionSessionInvalidate(vt);
		CFRelease(vt);
	}
	if (format)
		CFRelease(format);
}

double elapsed(Movie *m) {
	return m->playing ? host::now() - m->start : m->pausedAt;
}

void task(Movie *m) {
	if (!m->hasVideo || !m->port)
		return;
	qd::Port *p = qd::port(m->port);
	if (!p)
		return;
	double t = elapsed(m);
	int target = 0;
	while (target + 1 < (int)m->video.times.size() && m->video.times[target + 1] <= t)
		target++;
	if (target == m->decoded)
		return;
	const Track &v = m->video;
	if (v.codec == fourcc("avc1")) {
		// Start from the last keyframe at or before the target when going
		// backwards or jumping ahead past one
		int from = m->decoded + 1;
		int key = target;
		while (key > 0 && !v.sync[key])
			key--;
		if (target < m->decoded || key > m->decoded)
			from = key;
		for (int i = from; i <= target; i++)
			decodeH264(m, i, i == target);
	} else {
		if (target < m->decoded)
			m->decoded = -1;
		for (int i = m->decoded + 1; i <= target; i++) {
			auto &s = v.samples[i];
			if (s.first + s.second <= m->file.size())
				qd::decodeAnimation(&m->file[s.first], s.second, v.width, v.height, v.depth, m->frame);
		}
	}
	m->decoded = target;
	if (target == 0 || emu::traceImports)
		logf("movie frame %d -> port 0x%08x box (%d,%d)-(%d,%d)", target, m->port, m->box.left, m->box.top, m->box.right,
			m->box.bottom);
	if (m->frame.size() < (size_t)v.width * v.height)
		return;
	std::vector<uint32_t> img = m->frame;
	for (uint32_t &c : img)
		c |= 0xFF000000u;
	qd::drawImage(p, img.data(), v.width, v.height, m->box);
}

Movie *argMovie(Ctx &c) {
	auto it = movies.find(c.arg(0));
	return it == movies.end() ? nullptr : it->second;
}

} // namespace

namespace movie {

// Called from the event loop
void taskAll() {
	for (auto &it : movies)
		task(it.second);
}

} // namespace movie

IMPORT(OpenMovieFile) {
	// (const FSSpec *, short *resRefNum, SInt8 permission)
	std::string path = files::pathOfFSSpec(c.arg(0));
	int16_t ref = nextFileRef++;
	movieFiles[ref] = path;
	emu::w16(c.arg(1), (uint16_t)ref);
	c.ret(0);
}
IMPORT(NewMovieFromFile) {
	// (Movie *out, resRefNum, short *resId, StringPtr name, flags, Boolean *changed)
	auto it = movieFiles.find((int16_t)c.arg(1));
	Movie *m = it == movieFiles.end() ? nullptr : load(it->second);
	if (!m) {
		emu::w32(c.arg(0), 0);
		c.ret((uint32_t)-2048); // noMovieFound
		return;
	}
	gaddr ref = heap::alloc(16, true);
	movies[ref] = m;
	logf("movie %s: %zu frames %dx%d depth %d, %.2fs", it->second.c_str(), m->video.samples.size(), m->video.width,
		m->video.height, m->video.depth, m->duration / (double)m->timescale);
	emu::w32(c.arg(0), ref);
	if (c.arg(5))
		emu::w8(c.arg(5), 0);
	c.ret(0);
}
IMPORT(DisposeMovie) {
	auto it = movies.find(c.arg(0));
	if (it != movies.end()) {
		if (it->second->soundID)
			snd::stop(it->second->soundID);
		delete it->second;
		movies.erase(it);
	}
}
IMPORT(GetMovieBox) {
	if (Movie *m = argMovie(c))
		qd::writeRect(c.arg(1), m->box);
}
IMPORT(SetMovieBox) {
	if (Movie *m = argMovie(c))
		m->box = qd::readRect(c.arg(1));
}
IMPORT(SetMovieGWorld) {
	if (Movie *m = argMovie(c))
		m->port = c.arg(1) ? c.arg(1) : (qd::current() ? qd::current()->addr : 0);
}
IMPORT(GoToBeginningOfMovie) {
	if (Movie *m = argMovie(c)) {
		m->start = host::now();
		m->pausedAt = 0;
		m->decoded = -1;
	}
}
IMPORT(StartMovie) {
	Movie *m = argMovie(c);
	if (!m || m->playing)
		return;
	m->playing = true;
	m->start = host::now() - m->pausedAt;
	if (m->hasSound && m->pausedAt == 0)
		m->soundID = snd::play(soundSamples(m), std::max(1, m->sound.channels), m->sound.timescale);
	task(m);
}
IMPORT(StopMovie) {
	Movie *m = argMovie(c);
	if (!m || !m->playing)
		return;
	m->pausedAt = elapsed(m);
	m->playing = false;
	if (m->soundID)
		snd::stop(m->soundID);
	m->soundID = 0;
}
IMPORT(MoviesTask) {
	if (c.arg(0)) {
		if (Movie *m = argMovie(c))
			task(m);
	} else {
		movie::taskAll();
	}
}
IMPORT(IsMovieDone) {
	Movie *m = argMovie(c);
	c.ret(!m || elapsed(m) >= m->duration / (double)m->timescale);
}
IMPORT(GetMovieDuration) {
	Movie *m = argMovie(c);
	c.ret(m ? m->duration : 0);
}
IMPORT(CloseMovieFile) {
	movieFiles.erase((int16_t)c.arg(0));
	c.ret(0);
}
IMPORT(ExitMovies) {}
IMPORT(GetMovieTime) {
	// (movie, TimeRecord *): the value in the movie's time scale
	Movie *m = argMovie(c);
	uint64_t v = m ? (uint64_t)(elapsed(m) * m->timescale) : 0;
	if (gaddr tr = c.arg(1)) {
		emu::w32(tr, (uint32_t)(v >> 32)); // TimeRecord: value {hi, lo}, scale, base
		emu::w32(tr + 4, (uint32_t)v);
		emu::w32(tr + 8, m ? m->timescale : 600);
		emu::w32(tr + 12, 0);
	}
	c.ret((uint32_t)v);
}
IMPORT(SetMovieTimeValue) {
	Movie *m = argMovie(c);
	if (!m)
		return;
	double t = c.arg(1) / (double)m->timescale;
	if (m->playing)
		m->start = host::now() - t;
	else
		m->pausedAt = t;
}
