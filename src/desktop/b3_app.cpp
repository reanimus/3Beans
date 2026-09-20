/*
    Copyright 2023-2026 Hydr8gon

    This file is part of 3Beans.

    3Beans is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    3Beans is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with 3Beans. If not, see <https://www.gnu.org/licenses/>.
*/

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include "b3_app.h"
#include "../scripting/cli.h"

enum AppEvent {
    UPDATE = 1
};

wxBEGIN_EVENT_TABLE(b3App, wxApp)
EVT_TIMER(UPDATE, b3App::update)
wxEND_EVENT_TABLE()

int b3App::audBufSize = 1024;
int b3App::keyBinds[] = {
    'L', 'K', 'G', 'H', WXK_RIGHT, WXK_LEFT, WXK_UP, WXK_DOWN, // A, B, Select, Start, D-pad
    'P', 'Q', 'O', 'I', 'D', 'A', 'W', 'S', WXK_SHIFT, WXK_SPACE // R, L, X, Y, L-stick, Home
};

bool b3App::OnInit() {
    // Define and add platform-specific settings
    std::vector<Setting> platSettings = {
        Setting("audBufSize", &audBufSize, false),
        Setting("keyA", &keyBinds[0], false),
        Setting("keyB", &keyBinds[1], false),
        Setting("keySelect", &keyBinds[2], false),
        Setting("keyStart", &keyBinds[3], false),
        Setting("keyRight", &keyBinds[4], false),
        Setting("keyLeft", &keyBinds[5], false),
        Setting("keyUp", &keyBinds[6], false),
        Setting("keyDown", &keyBinds[7], false),
        Setting("keyR", &keyBinds[8], false),
        Setting("keyL", &keyBinds[9], false),
        Setting("keyX", &keyBinds[10], false),
        Setting("keyY", &keyBinds[11], false),
        Setting("keyLRight", &keyBinds[12], false),
        Setting("keyLLeft", &keyBinds[13], false),
        Setting("keyLUp", &keyBinds[14], false),
        Setting("keyLDown", &keyBinds[15], false),
        Setting("keyLMod", &keyBinds[16], false),
        Setting("keyHome", &keyBinds[17], false)
    };
    Settings::add(platSettings);

    SetAppName("3Beans");
    Settings::load(launchOptions.configDir.empty() ? defaultConfigDir() : launchOptions.configDir);

    // Create the program's frame
    frame = new b3Frame();

    // Set up the update timer
    timer = new wxTimer(this, UPDATE);
    timer->Start(6);

    // Initialize the audio output stream
    audioInitialized = Pa_Initialize() == paNoError;
    if (audioInitialized && Pa_OpenDefaultStream(&stream, 0, 2, paInt16, 48000, audBufSize, audioCallback, frame) == paNoError)
        Pa_StartStream(stream);
    return true;
}

int b3App::OnExit() {
    // Clean up the audio output stream
    stopAudio();
    return wxApp::OnExit();
}

void b3App::stopAudio() {
    if (timer) { timer->Stop(); delete timer; timer = nullptr; }
    if (stream) { Pa_StopStream(stream); Pa_CloseStream(stream); stream = nullptr; }
    if (audioInitialized) { Pa_Terminate(); audioInitialized = false; }
}

void b3App::update(wxTimerEvent &event) {
    // Continuously refresh the frame
    frame->Refresh();
}

int b3App::audioCallback(const void *in, void *out, unsigned long count,
        const PaStreamCallbackTimeInfo *info, PaStreamCallbackFlags flags, void *data) {
    // Get samples from the core if available
    b3Frame *frame = (b3Frame*)data;
    memset(out, 0, count * sizeof(uint32_t));
    if (frame->session.consumerMutex.try_lock()) {
        if (frame->session.core) {
            uint32_t *samples = frame->session.core->csnd.getSamples(48000, count);
            if (samples) memcpy(out, samples, count * sizeof(uint32_t));
        }
        frame->session.consumerMutex.unlock();
    }
    return paContinue;
}
