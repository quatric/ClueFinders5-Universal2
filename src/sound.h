// Sound Manager channels mixed on the host audio thread
#pragma once

#include <vector>

namespace snd {

void init();
// Runs callBackCmd callbacks that the audio thread has reached; they call
// guest code, so this happens on the emulator thread
void serviceCallbacks();

// Plays interleaved float samples outside any Sound Manager channel (movie
// soundtracks); returns an id for stop()
int play(std::vector<float> &&samples, int channels, double rate);
void stop(int id);

} // namespace snd
