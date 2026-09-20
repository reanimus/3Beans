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

#include "b3_frame.h"
#include "b3_canvas_ogl.h"
#include "b3_canvas_soft.h"
#include "gpu_dialog.h"
#include "hardware_dialog.h"
#include "input_dialog.h"
#include "path_dialog.h"
#include "b3_app.h"
#include "../scripting/cli.h"

enum FrameEvent {
    INSERT_CART = 1,
    EJECT_CART,
    QUIT,
    PAUSE,
    RESTART,
    STOP,
    SET_HARDWARE,
    FPS_LIMITER,
    CART_AUTO_BOOT,
    DSP_INTERP,
    DSP_HLE,
    GPU_SETTINGS,
    PATH_SETTINGS,
    INPUT_BINDINGS,
    UPDATE_JOYSTICK,
    SCRIPTING
};

wxBEGIN_EVENT_TABLE(b3Frame, wxFrame)
EVT_MENU(INSERT_CART, b3Frame::insertCart)
EVT_MENU(EJECT_CART, b3Frame::ejectCart)
EVT_MENU(QUIT, b3Frame::quit)
EVT_MENU(PAUSE, b3Frame::pause)
EVT_MENU(RESTART, b3Frame::restart)
EVT_MENU(STOP, b3Frame::stop)
EVT_MENU(SET_HARDWARE, b3Frame::setHardware)
EVT_MENU(FPS_LIMITER, b3Frame::fpsLimiter)
EVT_MENU(CART_AUTO_BOOT, b3Frame::cartAutoBoot)
EVT_MENU(DSP_INTERP, b3Frame::dspBackend<0>)
EVT_MENU(DSP_HLE, b3Frame::dspBackend<1>)
EVT_MENU(GPU_SETTINGS, b3Frame::gpuSettings)
EVT_MENU(PATH_SETTINGS, b3Frame::pathSettings)
EVT_MENU(INPUT_BINDINGS, b3Frame::inputBindings)
EVT_TIMER(UPDATE_JOYSTICK, b3Frame::updateJoystick)
EVT_MENU(SCRIPTING, b3Frame::scripting)
EVT_CLOSE(b3Frame::close)
wxEND_EVENT_TABLE()

b3Frame::b3Frame(): wxFrame(nullptr, wxID_ANY, "3Beans"), session(false), mutex(session.mutex) {
    // Set up the file menu
    fileMenu = new wxMenu();
    fileMenu->Append(INSERT_CART, "&Insert Cart ROM");
    fileMenu->Append(EJECT_CART, "&Eject Cart ROM");
    fileMenu->AppendSeparator();
    fileMenu->Append(SCRIPTING, "&Scripting...");
    fileMenu->Append(QUIT, "&Quit");
    fileMenu->Enable(EJECT_CART, false);

    // Set up the system menu
    systemMenu = new wxMenu();
    systemMenu->Append(PAUSE, "&Pause");
    systemMenu->Append(RESTART, "&Restart");
    systemMenu->Append(STOP, "&Stop");
    systemMenu->AppendSeparator();
    systemMenu->Append(SET_HARDWARE, "&Set Hardware");

    // Set up the DSP backend submenu
    wxMenu *dspMenu = new wxMenu();
    dspMenu->AppendRadioItem(DSP_INTERP, "&Interpreter");
    dspMenu->AppendRadioItem(DSP_HLE, "&HLE");

    // Set up the settings menu
    wxMenu *settingsMenu = new wxMenu();
    settingsMenu->AppendCheckItem(FPS_LIMITER, "&FPS Limiter");
    settingsMenu->AppendCheckItem(CART_AUTO_BOOT, "&Cart Auto-Boot");
    settingsMenu->AppendSubMenu(dspMenu, "&DSP Backend");
    settingsMenu->AppendSeparator();
    settingsMenu->Append(GPU_SETTINGS, "&GPU Settings");
    settingsMenu->Append(PATH_SETTINGS, "&Path Settings");
    settingsMenu->Append(INPUT_BINDINGS, "&Input Bindings");

    // Set up the menu bar
    wxMenuBar *menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    menuBar->Append(systemMenu, "&System");
    menuBar->Append(settingsMenu, "&Settings");
    SetMenuBar(menuBar);

    // Set up and show the window
    systemMenu->SetLabel(RESTART, "&Start");
    systemMenu->Enable(PAUSE, false);
    systemMenu->Enable(STOP, false);
    SetClientSize(MIN_SIZE);
    SetBackgroundColour(*wxBLACK);
    Centre();
    Show(true);

    // Create a canvas for drawing the screens
    try {
        // Use an OpenGL canvas if supported
        canvas = new b3CanvasOgl(this);
    }
    catch (CanvasError e) {
        // Fall back to software and disable OpenGL
        canvas = new b3CanvasSoft(this);
        Settings::gpuRenderer = 0;
        glSupport = false;
        Settings::save();
    }

    // Add the canvas to the frame
    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(canvas, 1, wxEXPAND);
    SetSizer(sizer);

    // Set the initial setting states
    settingsMenu->Check(FPS_LIMITER, Settings::fpsLimiter);
    settingsMenu->Check(CART_AUTO_BOOT, Settings::cartAutoBoot);
    dspMenu->Check(DSP_INTERP + std::min(Settings::dspBackend, 1), true);

    // Prepare a joystick if one is connected
    joystick = new wxJoystick();
    if (joystick->IsOk()) {
        // Initialize data and start the joystick update timer
        axisBases.resize(joystick->GetNumberAxes());
        timer = new wxTimer(this, UPDATE_JOYSTICK);
        timer->Start(10);
    }
    else {
        // Don't use a joystick if one isn't connected
        delete joystick;
        joystick = nullptr;
        timer = nullptr;
    }
    session.context = glSupport ? &((b3CanvasOgl*)canvas)->contextFunc : nullptr;
    session.changed = [this] { uiCore = bool(session.core); uiFps = 0; };
    session.output = [this](const std::string &text) {
        auto *event = new wxThreadEvent(wxEVT_THREAD);
        event->SetString(wxString::FromUTF8(text)); event->SetInt(0); wxQueueEvent(this, event);
    };
    session.bufferOutput = [this](const std::string &name, const std::string &text) {
        auto *event = new wxThreadEvent(wxEVT_THREAD);
        event->SetString(wxString::FromUTF8(text)); event->SetInt(1);
        event->SetPayload(name); wxQueueEvent(this, event);
    };
    Bind(wxEVT_THREAD, &b3Frame::scriptMessage, this);
    thread = new std::thread(&b3Frame::runCore, this);
    enqueue([this] {
        launchOptions.apply(session);
        for (auto &path : launchOptions.scripts) if (!session.runFile(path)) break;
    });
}

void b3Frame::Refresh() {
    wxFrame::Refresh();
    SetMinClientSize(MIN_SIZE);
    running.store(session.autoRun.load());
    wxString label = "3Beans";
    if (uiCore.load() && running.load()) label += wxString::Format(" - %d FPS", uiFps.load());
    if (uiOverrides.load()) label += " - Temporary boot paths";
    systemMenu->SetLabel(PAUSE, running.load() ? "&Pause" : "&Resume");
    systemMenu->SetLabel(RESTART, uiCore.load() ? "&Restart" : "&Start");
    systemMenu->Enable(PAUSE, uiCore.load());
    systemMenu->Enable(STOP, uiCore.load());
    SetTitle(label);
}

void b3Frame::enqueue(std::function<void()> command) {
    { std::lock_guard<std::mutex> lock(queueMutex); commands.push_back(std::move(command)); }
    queueReady.notify_one();
}

void b3Frame::runCore() {
    while (!workerStop.load()) {
        std::function<void()> command;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueReady.wait(lock, [this] { return workerStop.load() || (!settingsOpen.load() && (!commands.empty() || session.autoRun.load())); });
            if (workerStop.load()) break;
            if (!commands.empty()) { command = std::move(commands.front()); commands.pop_front(); }
        }
        try {
            if (command) command();
            else if (session.autoRun.load()) {
                auto next = std::chrono::steady_clock::now() + std::chrono::microseconds(16667);
                session.advance();
                bool limit; { std::lock_guard<std::recursive_mutex> lock(mutex); limit = Settings::fpsLimiter; }
                if (limit) {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    queueReady.wait_until(lock, next, [this] { return workerStop.load() || !commands.empty(); });
                }
            }
        } catch (const std::exception &e) { session.pause(); session.output(std::string("ERROR: ") + e.what()); }
        catch (...) { session.pause(); session.output("ERROR: Emulator operation failed"); }
        uiFps = session.core ? session.core->fps : 0;
        uiOverrides = session.hasOverrides();
    }
    // Release Lua callbacks before shutdown so user code cannot prolong closing.
    session.resetScripts(); session.stop();
}

void b3Frame::startCore(bool full) {
    if (joystick) for (int i = 0; i < joystick->GetNumberAxes(); ++i) axisBases[i] = joystick->GetPosition(i);
    enqueue([this, full] { session.clearCancel(); session.start(full); session.resume(); });
}

void b3Frame::stopCore(bool full) {
    enqueue([this, full] { session.clearCancel(); full ? session.stop() : session.pause(); });
}

void b3Frame::runScript(const std::string &path) {
    uint64_t generation = session.cancelGeneration.load();
    enqueue([this, path, generation] {
        if (generation != session.cancelGeneration.load()) return;
        session.clearCancel();
        session.runFile(path);
    });
}

uint32_t *b3Frame::getFrame() {
    // Track refresh rate and update the swap interval every second
    refreshRate++;
    std::chrono::duration<double> rateTime = std::chrono::steady_clock::now() - lastRateTime;
    if (rateTime.count() >= 1.0f) {
        swapInterval = (refreshRate + 5) / 60; // Margin of 5
        refreshRate = 0;
        lastRateTime = std::chrono::steady_clock::now();
    }

    // Wait until the swap interval is reached
    if (++frameCount < swapInterval)
        return nullptr;

    // Get a new frame from the core, or make an empty one if inactive
    uint32_t *frame;
    // A long-running callback must not block painting or the Cancel button.
    if (!session.consumerMutex.try_lock()) return nullptr;
    if (session.core) {
        frame = session.core->pdc.getFrame();
        session.consumerMutex.unlock();
    }
    else {
        session.consumerMutex.unlock();
        frame = new uint32_t[400 * 480];
        memset(frame, 0, 400 * 480 * sizeof(uint32_t));
    }
    frameCount = 0;
    return frame;
}

void b3Frame::pressKey(int key) {
    if (key >= 12 && key < 17) { stickKeys[key - 12] = true; updateKeyStick(); return; }
    enqueue([this, key] { if (session.core) { if (key < 12) session.core->input.pressKey(key); else session.core->input.pressHome(); } });
}

void b3Frame::releaseKey(int key) {
    if (key >= 12 && key < 17) { stickKeys[key - 12] = false; updateKeyStick(); return; }
    enqueue([this, key] { if (session.core) { if (key < 12) session.core->input.releaseKey(key); else session.core->input.releaseHome(); } });
}

void b3Frame::updateKeyStick() {
    // Apply the base stick movement from pressed keys
    int stickX = 0, stickY = 0;
    if (stickKeys[0]) stickX -= 0x7FF;
    if (stickKeys[1]) stickX += 0x7FF;
    if (stickKeys[2]) stickY += 0x7FF;
    if (stickKeys[3]) stickY -= 0x7FF;

    // Scale diagonals to create a round boundary
    if (stickX && stickY) {
        stickX = stickX * 0x5FF / 0x7FF;
        stickY = stickY * 0x5FF / 0x7FF;
    }

    // Halve coordinates if the modifier is active
    if (stickKeys[4]) {
        stickX /= 2;
        stickY /= 2;
    }

    // Send key-stick coordinates to the core
    enqueue([this, stickX, stickY] { if (session.core) session.core->input.setLStick(stickX, stickY); });
}

void b3Frame::pressScreen(int x, int y) {
    enqueue([this, x, y] { if (session.core) session.core->input.pressScreen(x, y); });
}

void b3Frame::releaseScreen() {
    enqueue([this] { if (session.core) session.core->input.releaseScreen(); });
}

void b3Frame::insertCart(wxCommandEvent &event) {
    // Open a file browser for cartridge ROMs
    wxFileDialog romSelect(this, "Select Cart ROM", "", "",
        "3DS Cart ROMs (*.3ds, *.cci)|*.3ds;*.cci", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (romSelect.ShowModal() == wxID_CANCEL) return;

    // Set the cartridge path and start or restart the core
    cartPath = (const char*)romSelect.GetPath().mb_str(wxConvUTF8);
    std::string path = cartPath;
    enqueue([this, path] { session.cartPath = path; });
    startCore(true);
    fileMenu->Enable(EJECT_CART, true);
}

void b3Frame::ejectCart(wxCommandEvent &event) {
    // Clear the cartridge path and restart the core if started
    cartPath = "";
    enqueue([this] { session.cartPath.clear(); if (session.core) { session.start(true); session.resume(); } });
    fileMenu->Enable(EJECT_CART, false);
}

void b3Frame::quit(wxCommandEvent &event) {
    // Close the program
    Close(true);
}

void b3Frame::pause(wxCommandEvent &event) {
    // Pause or resume the core
    session.autoRun.load() ? stopCore(false) : startCore(false);
}

void b3Frame::restart(wxCommandEvent &event) {
    // Restart the core
    startCore(true);
}

void b3Frame::stop(wxCommandEvent &event) {
    // Stop the core
    stopCore(true);
}

void b3Frame::showSettings(std::function<void()> dialog) {
    if (settingsPending || workerStop.load()) return;
    settingsPending = true;
    // Reach a worker command boundary without ever blocking the UI on Lua.
    enqueue([this, dialog] {
        bool resume = session.autoRun.load();
        session.pause();
        settingsOpen = true;
        CallAfter([this, dialog, resume] {
            if (workerStop.load()) return;
            dialog();
            settingsPending = false;
            if (workerStop.load()) return;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                // Restore the old run state before handling subsequently queued work.
                commands.push_front([this, resume] {
                    if (resume && session.core && !session.cancelled.load()) session.resume();
                });
                settingsOpen = false;
            }
            queueReady.notify_one();
        });
    });
}

void b3Frame::setHardware(wxCommandEvent &event) {
    showSettings([] {
        HardwareDialog hardwareDialog;
        hardwareDialog.ShowModal();
    });
}

void b3Frame::fpsLimiter(wxCommandEvent &event) {
    // Toggle the FPS limiter setting
    enqueue([this] { std::lock_guard<std::recursive_mutex> lock(mutex); Settings::fpsLimiter = !Settings::fpsLimiter; Settings::save(); });
}

void b3Frame::cartAutoBoot(wxCommandEvent &event) {
    // Toggle the cart auto-boot setting
    enqueue([this] { std::lock_guard<std::recursive_mutex> lock(mutex); Settings::cartAutoBoot = !Settings::cartAutoBoot; Settings::save(); });
}

template <int i> void b3Frame::dspBackend(wxCommandEvent &event) {
    // Set the DSP backend to a specific value
    enqueue([this] { std::lock_guard<std::recursive_mutex> lock(mutex); Settings::dspBackend = i; Settings::save(); });
}

void b3Frame::gpuSettings(wxCommandEvent &event) {
    showSettings([this] {
        GpuDialog gpuDialog(glSupport);
        gpuDialog.ShowModal();
    });
}

void b3Frame::pathSettings(wxCommandEvent &event) {
    showSettings([this] {
        PathDialog pathDialog;
        if (session.hasOverrides()) pathDialog.SetTitle("Saved Path Settings (temporary overrides active)");
        pathDialog.ShowModal();
    });
}

void b3Frame::inputBindings(wxCommandEvent &event) {
    showSettings([this] {
        if (timer) timer->Stop();
        InputDialog inputDialog(joystick);
        inputDialog.ShowModal();
        if (timer) timer->Start(10);
    });
}

void b3Frame::updateJoystick(wxTimerEvent &event) {
    // Check the status of mapped joystick inputs
    int stickX = 0, stickY = 0;
    int size = abs(joystick->GetXMax() - joystick->GetXMin()) / 2;
    for (int i = 0; i < MAX_KEYS; i++) {
        if (b3App::keyBinds[i] >= 3000 && joystick->GetNumberAxes() > b3App::keyBinds[i] - 3000) { // Axis -
            int j = b3App::keyBinds[i] - 3000;
            switch (i) {
            case 12: // Stick Right
                // Scale the axis position and apply it to the stick in the right direction
                if (joystick->GetPosition(j) < axisBases[j])
                    stickX += (joystick->GetPosition(j) - axisBases[j]) * 0x7FF / size;
                continue;

            case 13: // Stick Left
                // Scale the axis position and apply it to the stick in the left direction
                if (joystick->GetPosition(j) < axisBases[j])
                    stickX -= (joystick->GetPosition(j) - axisBases[j]) * 0x7FF / size;
                continue;

            case 14: // Stick Up
                // Scale the axis position and apply it to the stick in the up direction
                if (joystick->GetPosition(j) < axisBases[j])
                    stickY -= (joystick->GetPosition(j) - axisBases[j]) * 0x7FF / size;
                continue;

            case 15: // Stick Down
                // Scale the axis position and apply it to the stick in the down direction
                if (joystick->GetPosition(j) < axisBases[j])
                    stickY += (joystick->GetPosition(j) - axisBases[j]) * 0x7FF / size;
                continue;

            default:
                // Trigger a key press or release based on the axis position
                if (joystick->GetPosition(j) - axisBases[j] < -size / 2)
                    pressKey(i);
                else
                    releaseKey(i);
                continue;
            }
        }
        else if (b3App::keyBinds[i] >= 2000 && joystick->GetNumberAxes() > b3App::keyBinds[i] - 2000) { // Axis +
            int j = b3App::keyBinds[i] - 2000;
            switch (i) {
            case 12: // Stick Right
                // Scale the axis position and apply it to the stick in the right direction
                if (joystick->GetPosition(j) > axisBases[j])
                    stickX -= (joystick->GetPosition(j) - axisBases[j]) * 0x7FF / size;
                continue;

            case 13: // Stick Left
                // Scale the axis position and apply it to the stick in the left direction
                if (joystick->GetPosition(j) > axisBases[j])
                    stickX += (joystick->GetPosition(j) - axisBases[j]) * 0x7FF / size;
                continue;

            case 14: // Stick Up
                // Scale the axis position and apply it to the stick in the up direction
                if (joystick->GetPosition(j) > axisBases[j])
                    stickY += (joystick->GetPosition(j) - axisBases[j]) * 0x7FF / size;
                continue;

            case 15: // Stick Down
                // Scale the axis position and apply it to the stick in the down direction
                if (joystick->GetPosition(j) > axisBases[j])
                    stickY -= (joystick->GetPosition(j) - axisBases[j]) * 0x7FF / size;
                continue;

            default:
                // Trigger a key press or release based on the axis position
                if (joystick->GetPosition(j) - axisBases[j] > size / 2)
                    pressKey(i);
                else
                    releaseKey(i);
                continue;
            }
        }
        else if (b3App::keyBinds[i] >= 1000 && joystick->GetNumberButtons() > b3App::keyBinds[i] - 1000) { // Button
            // Trigger a key press or release based on the button status
            if (joystick->GetButtonState(b3App::keyBinds[i] - 1000))
                pressKey(i);
            else
                releaseKey(i);
        }
    }

    // Halve stick coordinates if the modifier is active
    if (stickKeys[4]) {
        stickX /= 2;
        stickY /= 2;
    }

    // Send coordinates to the core if key-stick is inactive
    if (!stickKeys[0] && !stickKeys[1] && !stickKeys[2] && !stickKeys[3])
        enqueue([this, stickX, stickY] { if (session.core) session.core->input.setLStick(stickX, stickY); });
}

b3Frame::~b3Frame() {
    // Native application quit can destroy windows without a close event.
    shutdown();
}

void b3Frame::shutdown() {
    // Clean up the joystick if used
    if (joystick) {
        timer->Stop();
        delete joystick; joystick = nullptr;
    }

    // Audio must stop before this frame and its core pointers can be destroyed.
    static_cast<b3App*>(wxTheApp)->stopAudio();
    session.requestCancel(); workerStop = true; queueReady.notify_one();
    if (thread) { thread->join(); delete thread; thread = nullptr; }
}

void b3Frame::close(wxCloseEvent &event) {
    shutdown();
    event.Skip(true);
}

void b3Frame::scriptMessage(wxThreadEvent &event) {
    if (!event.GetInt()) {
        logHistory += event.GetString() + "\n";
        if (event.GetString().StartsWith("ERROR:") && !scriptWindow) {
            wxCommandEvent open; scripting(open);
        }
        if (logHistory.size() > 1048576) logHistory = logHistory.Right(524288);
        if (scriptLog) { scriptLog->ChangeValue(logHistory); scriptLog->ShowPosition(scriptLog->GetLastPosition()); }
    } else {
        std::string name = event.GetPayload<std::string>();
        if (event.GetString().empty()) {
            bufferTexts.erase(name);
            auto found = bufferViews.find(name);
            if (found != bufferViews.end()) {
                int page = scriptBuffers->FindPage(found->second);
                if (page != wxNOT_FOUND) scriptBuffers->DeletePage(page);
                bufferViews.erase(found);
            }
            return;
        }
        bufferTexts[name] = event.GetString().ToStdString(wxConvUTF8);
        if (scriptBuffers) {
            if (!bufferViews.count(name)) {
                auto *view = new wxTextCtrl(scriptBuffers, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
                view->SetFont(wxFont(wxFontInfo(11).Family(wxFONTFAMILY_TELETYPE)));
                bufferViews[name] = view; scriptBuffers->AddPage(view, wxString::FromUTF8(name));
            }
            bufferViews[name]->ChangeValue(event.GetString());
        }
    }
}

void b3Frame::scripting(wxCommandEvent &) {
    if (scriptWindow) { scriptWindow->Show(); scriptWindow->Raise(); return; }
    scriptWindow = new wxFrame(this, wxID_ANY, "3Beans Scripting", wxDefaultPosition, wxSize(800, 600));
    auto *panel = new wxPanel(scriptWindow);
    auto *layout = new wxBoxSizer(wxVERTICAL);
    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *load = new wxButton(panel, wxID_ANY, "Load script...");
    auto *cancel = new wxButton(panel, wxID_ANY, "Cancel execution");
    auto *reset = new wxButton(panel, wxID_ANY, "Reset scripting");
    buttons->Add(load, 0, wxALL, 4); buttons->Add(cancel, 0, wxALL, 4); buttons->Add(reset, 0, wxALL, 4);
    layout->Add(buttons, 0, wxEXPAND);
    scriptBuffers = new wxNotebook(panel, wxID_ANY);
    scriptLog = new wxTextCtrl(scriptBuffers, wxID_ANY, logHistory, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
    scriptLog->SetFont(wxFont(wxFontInfo(11).Family(wxFONTFAMILY_TELETYPE)));
    scriptBuffers->AddPage(scriptLog, "Console");
    for (auto &entry : bufferTexts) {
        auto *view = new wxTextCtrl(scriptBuffers, wxID_ANY, wxString::FromUTF8(entry.second), wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
        view->SetFont(wxFont(wxFontInfo(11).Family(wxFONTFAMILY_TELETYPE)));
        bufferViews[entry.first] = view; scriptBuffers->AddPage(view, wxString::FromUTF8(entry.first));
    }
    layout->Add(scriptBuffers, 1, wxEXPAND | wxALL, 4);
    scriptCommand = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    layout->Add(scriptCommand, 0, wxEXPAND | wxALL, 4); panel->SetSizer(layout);
    scriptCommand->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) {
        std::string code = scriptCommand->GetValue().ToStdString(wxConvUTF8); scriptCommand->Clear();
        uint64_t generation = session.cancelGeneration.load();
        enqueue([this, code, generation] {
            if (generation != session.cancelGeneration.load()) return;
            session.clearCancel();
            session.runString(code);
        });
    });
    load->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxFileDialog dialog(scriptWindow, "Load Lua script", "", "", "Lua scripts (*.lua)|*.lua", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) runScript(dialog.GetPath().ToStdString(wxConvUTF8));
    });
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { session.requestCancel(); });
    reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        session.requestCancel();
        enqueue([this] { session.clearCancel(); session.resetScripts(); session.output("Scripting reset; temporary boot paths retained."); });
    });
    scriptWindow->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &event) {
        if (event.CanVeto()) { scriptWindow->Hide(); event.Veto(); } else event.Skip();
    });
    scriptWindow->Show();
}
