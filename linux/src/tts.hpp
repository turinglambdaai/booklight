// Text-to-speech: read one page of text aloud, toggle to stop (mac
// toggleTTS semantics).
//
// Transport: the `spd-say` command from speech-dispatcher, spawned with
// -w so the child's exit doubles as the end-of-speech notification (a
// child-watch callback clears the speaking state; killing the process stops
// playback). Chosen over dlopen'ing libspeechd because the end-of-speech
// callback surface differs across speech-dispatcher versions (0.8 vs 0.9+),
// while spd-say has been stable for a decade and needs no build-time or
// runtime library coupling. When the binary is missing the host surfaces a
// readable message instead of failing.
#pragma once

#include <string>

namespace pdfgist::tts {

bool speaking();

// Starts speaking `text` (mac caps at 20000 chars — caller trims), or stops
// when already speaking. Returns false only when the speech engine is
// unavailable (spd-say not found); stopping returns true.
bool toggle(std::string const& text);

// Playback rate in the 0.5-3.0 band (settings slider); 1.0 = default.
void set_rate(double rate);

// Stops playback if active; safe when idle.
void stop();

}  // namespace pdfgist::tts
