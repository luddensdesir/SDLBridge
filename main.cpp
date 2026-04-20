/*
 * SDLBridge — SDL2 joystick-to-browser input bridge
 *
 * Runs a local WebSocket SERVER on port 9091.  The browser game client
 * connects and receives raw axes / button state each poll cycle.  The
 * client injects the data into its existing `devices[]` map so the
 * normal input pipeline picks it up.
 *
 * Uses the SDL2 Joystick API (not GameController) so HOTAS devices,
 * rudder pedals, and other non-Xbox controllers are supported.
 *
 * Dependencies: SDL2, libwebsockets (lws)
 */

#include <SDL2/SDL.h>
#include <libwebsockets.h>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

// ── Configuration ──────────────────────────────────────────────────────────
static const int   LISTEN_PORT = 9091;       // local WS server port
static const int   SEND_HZ    = 60;          // input broadcast rate
static const float DEADZONE   = 0.08f;       // stick deadzone (0–1)

// ── State ──────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};

// Track all connected browser clients. Modified from the lws thread
// (CALLBACK_ESTABLISHED / CALLBACK_CLOSED) and iterated from the main
// thread (requestBroadcast) — all access must hold g_clientsMutex.
static std::vector<struct lws*> g_clients;
static std::mutex g_clientsMutex;

// Pending outbound frame — updated at SEND_HZ by the main loop, consumed
// by each client's LWS_CALLBACK_SERVER_WRITEABLE on the lws thread. All
// access must hold g_pendingMsgMutex.
static std::string g_pendingMsg;
static std::mutex g_pendingMsgMutex;

// ── Joystick tracking ──────────────────────────────────────────────────────
struct JoystickInfo {
    SDL_Joystick* joystick = nullptr;
    SDL_JoystickID instanceID = -1;
    std::string name;
    int numAxes = 0;
    int numButtons = 0;
    int numHats = 0;
    int deviceIndex = 0;   // browser device index (200, 201, 202, ...)
};

static std::map<SDL_JoystickID, JoystickInfo> g_joysticks;
static int g_nextDeviceIndex = 200;  // matches SDL_BRIDGE_DEVICE_INDEX in gamepad.js

// ── Helpers ────────────────────────────────────────────────────────────────

static float applyDeadzone(float raw) {
    if (std::fabs(raw) < DEADZONE) return 0.0f;
    float sign = raw > 0.0f ? 1.0f : -1.0f;
    return sign * (std::fabs(raw) - DEADZONE) / (1.0f - DEADZONE);
}

// Request every client become writable. The actual lws_write happens
// inside LWS_CALLBACK_SERVER_WRITEABLE using the latest g_pendingMsg.
// lws_callback_on_writable is documented as thread-safe; we only lock
// to iterate g_clients safely while the lws thread may be pushing/
// erasing entries.
static void requestBroadcast() {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    for (auto* wsi : g_clients) {
        lws_callback_on_writable(wsi);
    }
}

// ── Joystick management ────────────────────────────────────────────────────

static void openJoystick(int sdlIndex) {
    SDL_Joystick* js = SDL_JoystickOpen(sdlIndex);
    if (!js) {
        fprintf(stderr, "[SDLBridge] Failed to open joystick %d: %s\n", sdlIndex, SDL_GetError());
        return;
    }

    JoystickInfo info;
    info.joystick = js;
    info.instanceID = SDL_JoystickInstanceID(js);
    info.name = SDL_JoystickName(js) ? SDL_JoystickName(js) : "Unknown";
    info.numAxes = SDL_JoystickNumAxes(js);
    info.numButtons = SDL_JoystickNumButtons(js);
    info.numHats = SDL_JoystickNumHats(js);
    info.deviceIndex = g_nextDeviceIndex++;

    printf("[SDLBridge] Opened joystick %d: \"%s\" (id=%d, axes=%d, buttons=%d, hats=%d) -> device %d\n",
           sdlIndex, info.name.c_str(), info.instanceID,
           info.numAxes, info.numButtons, info.numHats, info.deviceIndex);

    g_joysticks[info.instanceID] = info;
}

static void closeJoystick(SDL_JoystickID instanceID) {
    auto it = g_joysticks.find(instanceID);
    if (it != g_joysticks.end()) {
        printf("[SDLBridge] Closed joystick: \"%s\" (device %d)\n",
               it->second.name.c_str(), it->second.deviceIndex);
        SDL_JoystickClose(it->second.joystick);
        g_joysticks.erase(it);
    }
}

static void openAllJoysticks() {
    int n = SDL_NumJoysticks();
    printf("[SDLBridge] Found %d joystick(s)\n", n);
    for (int i = 0; i < n; i++) {
        openJoystick(i);
    }
}

// ── Build device-state JSON ────────────────────────────────────────────────
// Sends one message per joystick, each with:
//   { type: "sdlbridge", device: <index>, id: <name>, axes: [...], buttons: {...}, hats: [...] }

static std::string buildDeviceMessages() {
    // Force joystick state refresh before reading axes. With JOYSTICK_THREAD
    // enabled this is usually a no-op, but it guarantees fresh values even
    // in edge cases where the thread hasn't ticked yet.
    SDL_JoystickUpdate();

    // Wrap all devices in a single JSON array
    std::string out = "[";
    bool first = true;

    for (auto& [id, info] : g_joysticks) {
        if (!info.joystick) continue;

        if (!first) out += ",";
        first = false;

        char buf[4096];
        int off = snprintf(buf, sizeof(buf),
            "{\"type\":\"sdlbridge\",\"device\":%d,\"id\":\"%s\",\"axes\":[",
            info.deviceIndex, info.name.c_str());

        for (int i = 0; i < info.numAxes; i++) {
            float val = applyDeadzone(SDL_JoystickGetAxis(info.joystick, i) / 32767.0f);
            off += snprintf(buf + off, sizeof(buf) - off, "%s%.4f", i ? "," : "", val);
        }

        off += snprintf(buf + off, sizeof(buf) - off, "],\"buttons\":{");
        for (int i = 0; i < info.numButtons; i++) {
            bool p = SDL_JoystickGetButton(info.joystick, i);
            off += snprintf(buf + off, sizeof(buf) - off,
                "%s\"%d\":{\"value\":%s,\"pressed\":%s,\"touched\":false}",
                i ? "," : "", i,
                p ? "1.0" : "0.0", p ? "true" : "false");
        }

        off += snprintf(buf + off, sizeof(buf) - off, "},\"hats\":[");
        for (int i = 0; i < info.numHats; i++) {
            Uint8 hat = SDL_JoystickGetHat(info.joystick, i);
            off += snprintf(buf + off, sizeof(buf) - off, "%s%d", i ? "," : "", hat);
        }

        off += snprintf(buf + off, sizeof(buf) - off, "]}");
        out.append(buf, off);
    }

    out += "]";
    return out;
}

// ── LWS server callback ───────────────────────────────────────────────────

static int wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                      void* /*user*/, void* /*in*/, size_t /*len*/) {
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            printf("[SDLBridge] Browser client connected\n");
            fflush(stdout);
            {
                std::lock_guard<std::mutex> lock(g_clientsMutex);
                g_clients.push_back(wsi);
            }
            // Ask lws to give us a writable slot for the first frame.
            lws_callback_on_writable(wsi);
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            printf("[SDLBridge] Browser client disconnected\n");
            fflush(stdout);
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            g_clients.erase(
                std::remove(g_clients.begin(), g_clients.end(), wsi),
                g_clients.end());
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            // Copy the latest snapshot out under the lock, then release
            // before the (potentially slower) lws_write call so the main
            // thread isn't blocked building the next frame.
            std::string snapshot;
            {
                std::lock_guard<std::mutex> lock(g_pendingMsgMutex);
                if (g_pendingMsg.empty()) break;
                snapshot = g_pendingMsg;
            }
            size_t len = snapshot.size();
            // LWS requires LWS_PRE bytes of leading padding in the buffer
            // we hand to lws_write.
            std::vector<unsigned char> buf(LWS_PRE + len);
            memcpy(buf.data() + LWS_PRE, snapshot.data(), len);
            int written = lws_write(wsi, buf.data() + LWS_PRE, len, LWS_WRITE_TEXT);
            if (written < (int)len) {
                fprintf(stderr, "[SDLBridge] short write: %d of %zu bytes\n", written, len);
            }
            break;
        }

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "sdlbridge", wsCallback, 0, 8192 },
    { nullptr, nullptr, 0, 0 }
};

// ── Main ───────────────────────────────────────────────────────────────────

int main(int /*argc*/, char** /*argv*/) {
    printf("[SDLBridge] Starting SDL2 input bridge server on port %d\n", LISTEN_PORT);

#ifdef _WIN32
    // Windows defaults the system timer slice to ~15.6 ms, which would
    // clamp std::this_thread::sleep_for(1ms) to ~15.6 ms and cap the
    // main loop well below the 60 Hz broadcast rate. Requesting 1 ms
    // resolution for the process lifetime lets the loop actually run
    // at ~1 kHz, so the 60 Hz sendInterval check fires on schedule.
    timeBeginPeriod(1);
#endif

    // Hints MUST be set before SDL_Init.
    //
    // JOYSTICK_THREAD: on Windows, without a visible SDL window SDL has no
    //   message pump to drive DirectInput polling. This spawns a dedicated
    //   joystick thread so axes update in real time instead of only when
    //   a Windows message happens to be processed.
    // ALLOW_BACKGROUND_EVENTS: poll joystick state even when the bridge
    //   process doesn't have OS focus (it never will — it's a console app).
    // HIDAPI disabled: on Windows, SDL's HIDAPI backend can open Thrustmaster
    //   devices but then fail to report axis deltas. Forcing it off makes
    //   SDL fall back to DirectInput, which drives these devices correctly.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");

    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[SDLBridge] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Enable joystick events. SDL fires SDL_JOYDEVICEADDED for every
    // already-connected joystick right after SDL_Init, so the event-driven
    // path below opens them — no need for an explicit openAllJoysticks()
    // call here (which would otherwise double-open every device and leak
    // the first handle).
    SDL_JoystickEventState(SDL_ENABLE);

    // Create LWS server context
    struct lws_context_creation_info ctxInfo = {};
    ctxInfo.port = LISTEN_PORT;
    ctxInfo.protocols = protocols;
    ctxInfo.gid = -1;
    ctxInfo.uid = -1;

    struct lws_context* ctx = lws_create_context(&ctxInfo);
    if (!ctx) {
        fprintf(stderr, "[SDLBridge] Failed to create LWS context\n");
        SDL_Quit();
        return 1;
    }

    printf("[SDLBridge] Listening on ws://127.0.0.1:%d\n", LISTEN_PORT);

    // Run lws_service on its own thread so a Windows-side poll stall
    // can't freeze the main SDL/broadcast loop. A 50 ms blocking timeout
    // is fine here since nothing else depends on this thread's cadence.
    std::thread lwsThread([ctx]{
        while (g_running) {
            lws_service(ctx, 50);
        }
    });

    auto lastSend = std::chrono::steady_clock::now();
    const auto sendInterval = std::chrono::microseconds(1000000 / SEND_HZ); //1 second / SEND_HZ

    printf("[SDLBridge] About to enter main loop, g_running=%s\n",
           g_running.load() ? "true" : "false");
    fflush(stdout);

    while (g_running) {
        // printf("[SDLBridge] Main loop is running (first iteration, g_running=true)\n");
        // fflush(stdout);
        
        // Pump SDL events
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
                // printf("[SDLBridge] SDL event logging is enabled (first event)\n");
                // fflush(stdout);
            switch (ev.type) {
                case SDL_QUIT:
                    g_running = false;
                    break;
                case SDL_JOYDEVICEADDED:
                    openJoystick(ev.jdevice.which);
                    break;
                case SDL_JOYDEVICEREMOVED:
                    closeJoystick(ev.jdevice.which);
                    break;
                case SDL_JOYAXISMOTION:
                    // Raw-event diagnostic: prints only when SDL itself
                    // receives an axis change from the driver. If the main
                    // column log goes silent while these fire, the column
                    // log has a bug. If these go silent while a device is
                    // moving, SDL is not receiving events from the driver.
                    printf("[EVT] axis: which=%d axis=%d value=%d\n",
                           ev.jaxis.which, ev.jaxis.axis, ev.jaxis.value);
                    fflush(stdout);
                    break;
                case SDL_JOYBUTTONDOWN:
                case SDL_JOYBUTTONUP:
                    printf("[EVT] btn:  which=%d button=%d state=%d\n",
                           ev.jbutton.which, ev.jbutton.button, ev.jbutton.state);
                    fflush(stdout);
                    break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                        g_running = false;
                    break;
                default:
                    break;
            }
        }

        // 1 Hz heartbeat — confirms the loop is alive and reaching the
        // polling block, separately from whether SDL has any input to
        // report. If this stops printing, lws_service is blocking.
        {
            static auto lastBeat = std::chrono::steady_clock::now();
            auto beatNow = std::chrono::steady_clock::now();
            if (beatNow - lastBeat >= std::chrono::seconds(1)) {
                lastBeat = beatNow;
                printf("[HB] loop alive, devices=%zu clients=%zu\n",
                       g_joysticks.size(), g_clients.size());
                fflush(stdout);
            }
        }

        // Build a fresh snapshot at the configured rate. We always refresh
        // g_pendingMsg even with no clients so new connections see current
        // state immediately on their first writable callback.
        auto now = std::chrono::steady_clock::now();
        if (now - lastSend >= sendInterval) {
            lastSend = now;
            std::string msg = buildDeviceMessages();
            {
                std::lock_guard<std::mutex> lock(g_pendingMsgMutex);
                g_pendingMsg = std::move(msg);
            }
            // requestBroadcast locks g_clientsMutex internally; an empty
            // vector is a cheap no-op so no pre-check needed.
            requestBroadcast();
            // Wake the lws service thread so the writable callback fires
            // immediately instead of waiting out its poll timeout. Required
            // for cross-thread lws_callback_on_writable() delivery on
            // Windows.
            lws_cancel_service(ctx);

            // Two-column axes log: T.16000M on the left, T-Rudder on the
            // right. Only print when a value actually changes (change-delta
            // detection against the last-logged snapshot). Gives a flood of
            // lines during stick movement and zero lines when idle — which
            // is what the user wants for diagnosing frozen input state.
            JoystickInfo* stick = nullptr;
            JoystickInfo* rudder = nullptr;
            for (auto& [id, info] : g_joysticks) {
                if (!info.joystick) continue;
                if (!stick && info.name.find("16000") != std::string::npos) {
                    stick = &info;
                } else if (!rudder && (info.name.find("Rudder") != std::string::npos ||
                                       info.name.find("rudder") != std::string::npos ||
                                       info.name.find("Pedal") != std::string::npos)) {
                    rudder = &info;
                }
            }

            auto formatAxes = [](JoystickInfo* info) -> std::string {
                if (!info) return std::string("(no device)");
                char buf[256];
                int off = snprintf(buf, sizeof(buf), "%s:", info->name.c_str());
                for (int i = 0; i < info->numAxes; i++) {
                    float v = applyDeadzone(SDL_JoystickGetAxis(info->joystick, i) / 32767.0f);
                    off += snprintf(buf + off, sizeof(buf) - off, " [%d]%+.2f", i, v);
                }
                return std::string(buf, off);
            };

            std::string left = formatAxes(stick);
            std::string right = formatAxes(rudder);
            const size_t leftWidth = 70;
            if (left.size() < leftWidth) left.append(leftWidth - left.size(), ' ');
            std::string line = left + " | " + right;
            // Change-detection: only print when something actually moved,
            // so the console shows a live burst during input and stays
            // silent when idle. This both answers "are axes updating?" and
            // avoids conhost choking on 60 Hz printf.
            static std::string lastLine;
            if (line != lastLine) {
                lastLine = line;
                printf("%s\n", line.c_str());
                fflush(stdout);
            }
        }

        // Tiny yield so this thread doesn't spin at 100% CPU. lws_service
        // is on its own thread now, so the main loop only needs enough
        // cadence to keep SDL polling responsive.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Wake the lws service thread out of its poll so it can observe
    // g_running=false and exit promptly.
    lws_cancel_service(ctx);
    lwsThread.join();

    // Close all joysticks
    for (auto& [id, info] : g_joysticks) {
        SDL_JoystickClose(info.joystick);
    }
    g_joysticks.clear();

    lws_context_destroy(ctx);
    SDL_Quit();

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    printf("[SDLBridge] Shutdown complete\n");
    return 0;
}
