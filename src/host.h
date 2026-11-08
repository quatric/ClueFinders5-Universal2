// Host side: the SDL window, input, audio output and file locations
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace host {

extern std::string gameApp;   // path of the original game's .app bundle
extern std::string gameName;  // its CFBundleExecutable, e.g. "ClueFinders 3rd Grade"
extern std::string supportDir; // where preferences and player files go
extern std::string supportName; // its folder name in ~/Library/Application Support
extern std::string wrapperApp; // the Universal 2 .app running the game, if any

// The emulated screen. Pixels are 32-bit ARGB, stored big-endian (A,R,G,B
// bytes), the layout of a k32ARGBPixelFormat PixMap.
extern int screenW, screenH;

// Carbon modifier bits (cmdKey, shiftKey, optionKey, controlKey)
enum : uint32_t { kCmd = 0x100, kShift = 0x200, kCapsLock = 0x400, kOption = 0x800, kControl = 0x1000 };

struct Input {
	enum Kind { MouseDown, MouseUp, MouseMove, KeyDown, KeyRepeat, KeyUp, Wheel, Quit } kind;
	int x = 0, y = 0;     // screen coordinates
	uint16_t macKey = 0;  // virtual key code
	uint8_t ch = 0;       // Mac Roman character
	uint32_t mods = 0;
	int clicks = 1;
	int wheel = 0;
	double time = 0;      // seconds since startup
};

extern int mouseX, mouseY;
extern bool mouseDown;
extern uint32_t modifiers;

void init();
void openWindow();
// Shows a finished frame of the emulated screen
void present(const uint8_t *pixels);
// Next pending input event, without waiting
bool poll(Input &in);
// Sleeps until input arrives or the time runs out
void wait(double seconds);
double now();
// A native message box; returns the index of the button pressed
int messageBox(const std::string &title, const std::string &text, const std::vector<std::string> &buttons);

// Audio: the Sound Manager mixes into this callback on the audio thread
void startAudio(void (*mix)(float *out, int frames, int rate));

void saveScreenshot(const std::string &path);

} // namespace host
