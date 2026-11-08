// Host side on SDL2. TS_HEADLESS=1 runs without a window (frames are only
// kept for screenshots) and TS_SCRIPT=<file> feeds scripted input:
//   wait <secs> | click <x> <y> | down <x> <y> | up <x> <y> | move <x> <y>
//   key <char|return|escape|space|tab|left|right|up|down|delete> | shot <file.png> | trace on|off | quit
//   clickcolor <r> <g> <b> [tolerance] [x0 y0 x1 y1]: clicks the middle of the
//   largest patch of that color. TS_CONTROL=<fifo> takes the same commands live.
#include "host.h"
#include "emu.h"

#include <ApplicationServices/ApplicationServices.h>
#include <ImageIO/ImageIO.h>
#include <SDL.h>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace host {

std::string gameApp;
std::string gameName;
std::string supportDir;
std::string supportName;
std::string wrapperApp;
int screenW = 1280, screenH = 800;
int mouseX = 0, mouseY = 0;
bool mouseDown = false;
uint32_t modifiers = 0;

namespace {

bool headless = false;
SDL_Window *window = nullptr;
SDL_Renderer *renderer = nullptr;
SDL_Texture *texture = nullptr;
std::vector<uint8_t> lastFrame;
auto startTime = std::chrono::steady_clock::now();
void (*mixFn)(float *, int, int) = nullptr;
int audioRate = 44100;

// Scripted input
struct ScriptCmd {
	std::string op;
	std::vector<std::string> args;
};
std::deque<ScriptCmd> script;
double scriptTime = 0;
std::deque<Input> injected;

// SDL keycodes to Mac virtual key codes (US layout)
uint16_t macKeyCode(SDL_Keycode k) {
	static const char *letters = "asdfhgzxcv?bqweryt123465=97-8]ou[ip?lj'k;\\,/nm.";
	if (k < 128 && k > 32) {
		const char *p = strchr(letters, (int)k);
		if (p && *p != '?')
			return (uint16_t)(p - letters);
		if (k == '0')
			return 29;
		if (k == '`')
			return 50;
	}
	switch (k) {
	case SDLK_RETURN: return 36;
	case SDLK_TAB: return 48;
	case SDLK_SPACE: return 49;
	case SDLK_BACKSPACE: return 51;
	case SDLK_ESCAPE: return 53;
	case SDLK_KP_ENTER: return 76;
	case SDLK_DELETE: return 117;
	case SDLK_HOME: return 115;
	case SDLK_END: return 119;
	case SDLK_PAGEUP: return 116;
	case SDLK_PAGEDOWN: return 121;
	case SDLK_LEFT: return 123;
	case SDLK_RIGHT: return 124;
	case SDLK_DOWN: return 125;
	case SDLK_UP: return 126;
	case SDLK_F1: return 122;
	default: return 0x7F;
	}
}

uint8_t macChar(SDL_Keycode k, bool shift) {
	switch (k) {
	case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
	case SDLK_TAB: return 9;
	case SDLK_BACKSPACE: return 8;
	case SDLK_ESCAPE: return 27;
	case SDLK_DELETE: return 127;
	case SDLK_LEFT: return 28;
	case SDLK_RIGHT: return 29;
	case SDLK_UP: return 30;
	case SDLK_DOWN: return 31;
	case SDLK_HOME: return 1;
	case SDLK_END: return 4;
	case SDLK_PAGEUP: return 11;
	case SDLK_PAGEDOWN: return 12;
	}
	if (k < 32 || k >= 127)
		return 0;
	char ch = (char)k;
	if (!shift)
		return (uint8_t)ch;
	if (ch >= 'a' && ch <= 'z')
		return (uint8_t)(ch - 32);
	static const char from[] = "1234567890-=[]\\;',./`", to[] = "!@#$%^&*()_+{}|:\"<>?~";
	const char *p = strchr(from, ch);
	return (uint8_t)(p ? to[p - from] : ch);
}

uint32_t macMods(Uint16 m) {
	uint32_t r = 0;
	if (m & KMOD_GUI)
		r |= kCmd;
	if (m & KMOD_SHIFT)
		r |= kShift;
	if (m & KMOD_CAPS)
		r |= kCapsLock;
	if (m & KMOD_ALT)
		r |= kOption;
	if (m & KMOD_CTRL)
		r |= kControl;
	return r;
}

int controlFd = -1;
std::string controlBuf;

void readControl() {
	if (controlFd < 0)
		return;
	char buf[512];
	ssize_t n;
	while ((n = read(controlFd, buf, sizeof buf)) > 0)
		controlBuf.append(buf, n);
	size_t nl;
	while ((nl = controlBuf.find('\n')) != std::string::npos) {
		std::istringstream ss(controlBuf.substr(0, nl));
		controlBuf.erase(0, nl + 1);
		ScriptCmd c;
		if (!(ss >> c.op))
			continue;
		std::string a;
		while (ss >> a)
			c.args.push_back(a);
		script.push_back(c);
	}
}

void loadScript() {
	if (const char *ctl = getenv("TS_CONTROL"))
		controlFd = open(ctl, O_RDONLY | O_NONBLOCK);
	const char *path = getenv("TS_SCRIPT");
	if (!path)
		return;
	std::ifstream f(path);
	std::string line;
	while (std::getline(f, line)) {
		std::istringstream ss(line);
		ScriptCmd c;
		if (!(ss >> c.op) || c.op[0] == '#')
			continue;
		std::string a;
		while (ss >> a)
			c.args.push_back(a);
		script.push_back(c);
	}
}

void injectKey(const std::string &name) {
	struct Named {
		const char *name;
		SDL_Keycode key;
	} named[] = {{"return", SDLK_RETURN}, {"escape", SDLK_ESCAPE}, {"space", SDLK_SPACE}, {"tab", SDLK_TAB},
		{"left", SDLK_LEFT}, {"right", SDLK_RIGHT}, {"up", SDLK_UP}, {"down", SDLK_DOWN},
		{"delete", SDLK_BACKSPACE}};
	SDL_Keycode k = name.size() == 1 ? (SDL_Keycode)tolower(name[0]) : 0;
	for (auto &n : named) {
		if (name == n.name)
			k = n.key;
	}
	bool shift = name.size() == 1 && isupper((unsigned char)name[0]);
	Input in;
	in.kind = Input::KeyDown;
	in.macKey = macKeyCode(k);
	in.ch = macChar(k, shift);
	in.mods = shift ? kShift : 0;
	in.x = mouseX;
	in.y = mouseY;
	in.time = now();
	injected.push_back(in);
	in.kind = Input::KeyUp;
	injected.push_back(in);
}

void injectMouse(Input::Kind kind, int x, int y) {
	Input in;
	in.kind = kind;
	in.x = x;
	in.y = y;
	in.time = now();
	injected.push_back(in);
}

void clickColor(const std::vector<std::string> &a) {
	auto num = [&](size_t i, int def) { return i < a.size() ? atoi(a[i].c_str()) : def; };
	int r = num(0, 0), g = num(1, 0), b = num(2, 0), tol = num(3, 24);
	int x0 = std::max(num(4, 0), 0), y0 = std::max(num(5, 0), 0);
	int x1 = std::min(num(6, screenW), screenW), y1 = std::min(num(7, screenH), screenH);
	int w = x1 - x0, h = y1 - y0;
	if (w <= 0 || h <= 0 || lastFrame.empty())
		return;
	// Clicks the middle of the largest connected patch of that color
	std::vector<uint8_t> match((size_t)w * h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *p = &lastFrame[((size_t)(y + y0) * screenW + x + x0) * 4];
			match[(size_t)y * w + x] = abs(p[1] - r) <= tol && abs(p[2] - g) <= tol && abs(p[3] - b) <= tol;
		}
	}
	long best = 0, bx = 0, by = 0;
	std::vector<int> stack;
	for (int i = 0; i < w * h; i++) {
		if (match[i] != 1)
			continue;
		long n = 0, sx = 0, sy = 0;
		stack.push_back(i);
		match[i] = 2;
		while (!stack.empty()) {
			int j = stack.back();
			stack.pop_back();
			int x = j % w, y = j / w;
			n++;
			sx += x;
			sy += y;
			int next[4] = {x > 0 ? j - 1 : -1, x < w - 1 ? j + 1 : -1, y > 0 ? j - w : -1, y < h - 1 ? j + w : -1};
			for (int k : next) {
				if (k >= 0 && match[k] == 1) {
					match[k] = 2;
					stack.push_back(k);
				}
			}
		}
		if (n > best) {
			best = n;
			bx = sx / n;
			by = sy / n;
		}
	}
	if (!best) {
		fprintf(stderr, "clickcolor: no pixels match\n");
		return;
	}
	int x = (int)bx + x0, y = (int)by + y0;
	fprintf(stderr, "clickcolor: %ld pixels, clicking %d,%d\n", best, x, y);
	script.push_front({"click", {std::to_string(x), std::to_string(y)}});
}

void runScript() {
	readControl();
	while (!script.empty() && now() >= scriptTime) {
		ScriptCmd c = script.front();
		script.pop_front();
		auto num = [&](size_t i) { return i < c.args.size() ? atoi(c.args[i].c_str()) : 0; };
		if (c.op == "wait") {
			scriptTime = now() + (c.args.empty() ? 1.0 : atof(c.args[0].c_str()));
		} else if (c.op == "click") {
			// Held briefly: some buttons poll the button state rather than
			// waiting for the mouse-up event
			injectMouse(Input::MouseMove, num(0), num(1));
			injectMouse(Input::MouseDown, num(0), num(1));
			script.push_front({"up", c.args});
			script.push_front({"wait", {"0.15"}});
		} else if (c.op == "down") {
			injectMouse(Input::MouseDown, num(0), num(1));
		} else if (c.op == "up") {
			injectMouse(Input::MouseUp, num(0), num(1));
		} else if (c.op == "move") {
			injectMouse(Input::MouseMove, num(0), num(1));
		} else if (c.op == "key") {
			injectKey(c.args.empty() ? " " : c.args[0]);
		} else if (c.op == "shot") {
			saveScreenshot(c.args.empty() ? "shot.bmp" : c.args[0]);
		} else if (c.op == "clickcolor") {
			clickColor(c.args);
		} else if (c.op == "trace") {
			emu::traceImports = !c.args.empty() && c.args[0] == "on";
		} else if (c.op == "prof") {
			emu::setProfiling(!c.args.empty() && c.args[0] == "on");
		} else if (c.op == "quit") {
			Input in;
			in.kind = Input::Quit;
			injected.push_back(in);
		} else {
			fprintf(stderr, "script: unknown command %s\n", c.op.c_str());
		}
	}
}

} // namespace

double now() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
}

void init() {
	const char *home = getenv("HOME");
	supportDir = std::string(home ? home : "/tmp") + "/Library/Application Support/" + supportName;
	if (const char *dir = getenv("TS_SUPPORT"))
		supportDir = dir;
	mkdir(supportDir.c_str(), 0755);
	headless = getenv("TS_HEADLESS") != nullptr;

	if (SDL_Init((headless ? 0 : SDL_INIT_VIDEO) | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
		emu::fatal("SDL_Init: %s", SDL_GetError());

	// The emulated screen keeps the display's shape at 800 lines, which
	// leaves room around the game's 640x480 area for the background art
	if (const char *s = getenv("TS_SCREEN")) {
		sscanf(s, "%dx%d", &screenW, &screenH);
	} else if (!headless) {
		SDL_DisplayMode dm;
		if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.h > 0) {
			screenH = 800;
			screenW = (int)((double)dm.w * screenH / dm.h + 0.5) & ~7;
			if (screenW < 1024)
				screenW = 1024;
		}
	}
	lastFrame.assign((size_t)screenW * screenH * 4, 0);
	loadScript();
}

void openWindow() {
	if (headless || window)
		return;
	Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
	if (!getenv("TS_WINDOWED"))
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	window = SDL_CreateWindow(gameName.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		screenW, screenH, flags);
	if (!window)
		emu::fatal("SDL_CreateWindow: %s", SDL_GetError());
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!renderer)
		renderer = SDL_CreateRenderer(window, -1, 0);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	SDL_RenderSetLogicalSize(renderer, screenW, screenH);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB32, SDL_TEXTUREACCESS_STREAMING, screenW, screenH);
}

void present(const uint8_t *pixels) {
	memcpy(lastFrame.data(), pixels, lastFrame.size());
	if (!renderer)
		return;
	SDL_UpdateTexture(texture, nullptr, pixels, screenW * 4);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	SDL_RenderPresent(renderer);
}

bool poll(Input &in) {
	runScript();
	if (!injected.empty()) {
		in = injected.front();
		injected.pop_front();
	} else {
		SDL_Event e;
		bool got = false;
		while (!got && SDL_PollEvent(&e)) {
			in = Input();
			in.time = now();
			in.x = mouseX;
			in.y = mouseY;
			switch (e.type) {
			case SDL_QUIT:
				in.kind = Input::Quit;
				got = true;
				break;
			case SDL_MOUSEMOTION:
				in.kind = Input::MouseMove;
				in.x = e.motion.x;
				in.y = e.motion.y;
				got = true;
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				if (e.button.button != SDL_BUTTON_LEFT)
					break;
				in.kind = e.type == SDL_MOUSEBUTTONDOWN ? Input::MouseDown : Input::MouseUp;
				in.x = e.button.x;
				in.y = e.button.y;
				in.clicks = e.button.clicks;
				got = true;
				break;
			case SDL_MOUSEWHEEL:
				in.kind = Input::Wheel;
				in.wheel = e.wheel.y;
				got = true;
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP: {
				SDL_Keycode k = e.key.keysym.sym;
				// Cmd-Q and Cmd-F toggle are handled here, as the menu bar would
				if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_GUI) && k == SDLK_f) {
					bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
					SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
					break;
				}
				in.kind = e.type == SDL_KEYUP ? Input::KeyUp : e.key.repeat ? Input::KeyRepeat : Input::KeyDown;
				in.macKey = macKeyCode(k);
				in.mods = macMods(e.key.keysym.mod);
				in.ch = macChar(k, (e.key.keysym.mod & KMOD_SHIFT) != 0);
				modifiers = in.mods;
				got = true;
				break;
			}
			default:
				break;
			}
		}
		if (!got)
			return false;
	}
	if (in.kind == Input::MouseMove || in.kind == Input::MouseDown || in.kind == Input::MouseUp) {
		mouseX = in.x < 0 ? 0 : in.x >= screenW ? screenW - 1 : in.x;
		mouseY = in.y < 0 ? 0 : in.y >= screenH ? screenH - 1 : in.y;
		in.x = mouseX;
		in.y = mouseY;
	}
	if (in.kind == Input::MouseDown)
		mouseDown = true;
	else if (in.kind == Input::MouseUp)
		mouseDown = false;
	if (in.kind != Input::KeyDown && in.kind != Input::KeyUp && in.kind != Input::KeyRepeat)
		in.mods = modifiers;
	return true;
}

void wait(double seconds) {
	if (seconds <= 0)
		return;
	if (!script.empty()) {
		double until = scriptTime - now();
		if (until < seconds)
			seconds = until > 0 ? until : 0;
	}
	if (!injected.empty())
		return;
	if (headless) {
		SDL_Delay((Uint32)(seconds * 1000));
		return;
	}
	SDL_WaitEventTimeout(nullptr, (int)(seconds * 1000));
}

int messageBox(const std::string &title, const std::string &text, const std::vector<std::string> &buttons) {
	fprintf(stderr, "alert: %s: %s\n", title.c_str(), text.c_str());
	if (headless || buttons.empty())
		return 0;
	std::vector<SDL_MessageBoxButtonData> b;
	for (size_t i = 0; i < buttons.size(); i++) {
		Uint32 flags = i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
		if (i == buttons.size() - 1 && i > 0)
			flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
		b.push_back({flags, (int)i, buttons[i].c_str()});
	}
	SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION, window, title.c_str(), text.c_str(), (int)b.size(), b.data(), nullptr};
	int hit = 0;
	SDL_ShowMessageBox(&d, &hit);
	return hit;
}

static void audioCallback(void *, Uint8 *stream, int len) {
	float *out = (float *)stream;
	int frames = len / 8;
	memset(stream, 0, len);
	if (mixFn)
		mixFn(out, frames, audioRate);
}

void startAudio(void (*mix)(float *, int, int)) {
	mixFn = mix;
	SDL_AudioSpec want = {}, have = {};
	want.freq = 44100;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audioCallback;
	SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (!dev) {
		logf("audio: %s", SDL_GetError());
		return;
	}
	audioRate = have.freq;
	SDL_PauseAudioDevice(dev, 0);
}

void saveScreenshot(const std::string &path) {
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(lastFrame.data(), screenW, screenH, 8, screenW * 4, rgb,
		kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big);
	CGImageRef img = CGBitmapContextCreateImage(ctx);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), path.size(), false);
	CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
	if (dst) {
		CGImageDestinationAddImage(dst, img, nullptr);
		CGImageDestinationFinalize(dst);
		CFRelease(dst);
	}
	CFRelease(url);
	CGImageRelease(img);
	CGContextRelease(ctx);
	CGColorSpaceRelease(rgb);
	logf("screenshot %s", path.c_str());
}

} // namespace host
