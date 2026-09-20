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

#pragma once

#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <wx/notebook.h>
#include "../scripting/session.h"
#include <wx/wx.h>
#include <wx/joystick.h>

#include "../core/core.h"

#define MIN_SIZE wxSize(400, 480)

class b3Frame: public wxFrame {
public:
    ScriptSession session;
    std::atomic<bool> running{false};
    std::recursive_mutex &mutex;

    b3Frame();
    ~b3Frame() override;
    void Refresh();
    void enqueue(std::function<void()> command);
    void runScript(const std::string &path);

    uint32_t *getFrame();
    void pressKey(int key);
    void releaseKey(int key);
    void pressScreen(int x, int y);
    void releaseScreen();

private:
    wxMenu *fileMenu, *systemMenu;
    wxWindow *canvas;
    wxJoystick *joystick;
    wxTimer *timer;

    std::thread *thread = nullptr;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<std::function<void()>> commands;
    std::atomic<bool> workerStop{false}, settingsOpen{false};
    bool settingsPending = false;
    void showSettings(std::function<void()> dialog);
    std::atomic<bool> uiCore{false}, uiOverrides{false};
    std::atomic<int> uiFps{0};
    wxFrame *scriptWindow = nullptr;
    wxTextCtrl *scriptLog = nullptr, *scriptCommand = nullptr;
    wxNotebook *scriptBuffers = nullptr;
    std::map<std::string, wxTextCtrl*> bufferViews;
    std::map<std::string, std::string> bufferTexts;
    wxString logHistory;
    void scripting(wxCommandEvent &event);
    void scriptMessage(wxThreadEvent &event);
    std::string cartPath;
    std::vector<int> axisBases;
    bool stickKeys[5] = {};

    std::chrono::steady_clock::time_point lastRateTime;
    int frameCount = 0;
    int swapInterval = 0;
    int refreshRate = 0;
    bool glSupport = true;

    void shutdown();
    void runCore();
    void startCore(bool full);
    void stopCore(bool full);
    void updateKeyStick();

    void insertCart(wxCommandEvent &event);
    void ejectCart(wxCommandEvent &event);
    void quit(wxCommandEvent &event);
    void pause(wxCommandEvent &event);
    void restart(wxCommandEvent &event);
    void stop(wxCommandEvent &event);
    void setHardware(wxCommandEvent &event);
    void fpsLimiter(wxCommandEvent &event);
    void cartAutoBoot(wxCommandEvent &event);
    template <int i> void dspBackend(wxCommandEvent &event);
    void gpuSettings(wxCommandEvent &event);
    void pathSettings(wxCommandEvent &event);
    void inputBindings(wxCommandEvent &event);
    void updateJoystick(wxTimerEvent &event);
    void close(wxCloseEvent &event);
    wxDECLARE_EVENT_TABLE();
};
