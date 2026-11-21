// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_patterns.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gow3_settings.h"

#if !GOW3_DEV_TOOLS
namespace Gow3Patterns {
void Start() {
    const char* env = std::getenv("GOW3_PATTERN_SERVER");
    if (env && env[0] && env[0] != '0') {
        std::printf("Patterns: GOW3_PATTERN_SERVER needs the development build (launcher: Advanced > Diagnostics > "
                    "Development build); no server\n");
    }
}
} // namespace Gow3Patterns
#else

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static void CloseSocket(SocketHandle s) {
    closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr int INVALID_SOCKET = -1;
static void CloseSocket(SocketHandle s) {
    close(s);
}
#endif

namespace Gow3Patterns {

namespace {

std::mutex mutex;
Patch current;
std::atomic<int> mode{Off};
std::atomic<uint64_t> presents{0};
int template_w = 0, template_h = 0; // PGenerator SETCONF DIM (pixels); 0: 10% of the screen

void Set(const Patch& patch) {
    {
        std::scoped_lock lock{mutex};
        current = patch;
    }
    mode.store(patch.mode, std::memory_order_release);
}

int ScreenWidth() {
    const int w = Gow3Settings::Get().window_width.load(std::memory_order_relaxed);
    return w > 0 ? w : 3840;
}
int ScreenHeight() {
    const int h = Gow3Settings::Get().window_height.load(std::memory_order_relaxed);
    return h > 0 ? h : 2160;
}

/// Waits until a frame drawn after the change has been presented (two presents later), at most 2 s.
void WaitPresented() {
    const uint64_t target = presents.load(std::memory_order_acquire) + 2;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (presents.load(std::memory_order_acquire) < target && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::vector<float> Numbers(const std::string& text) {
    std::vector<float> out;
    const char* p = text.c_str();
    while (*p) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        out.push_back(v);
        p = end;
    }
    return out;
}

std::vector<std::string> Split(const std::string& text, char sep) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (true) {
        const size_t next = text.find(sep, at);
        parts.push_back(text.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) {
            return parts;
        }
        at = next + 1;
    }
}

/// One message: the reply ("" = none). Patch changes wait for the present.
std::string Handle(const std::string& message, bool& quit) {
    const auto starts = [&](const char* prefix) { return message.rfind(prefix, 0) == 0; };
    if (starts("CMD:GET_RESOLUTION")) {
        return "OK:" + std::to_string(ScreenWidth()) + "x" + std::to_string(ScreenHeight());
    }
    if (starts("CMD:GET_GPU_MEMORY")) {
        return "OK:128M";
    }
    if (starts("GOW3:INFO")) {
        return std::string("OK:hdr=") + (Gow3Settings::Get().hdr_active.load() ? "1" : "0") +
               ";w=" + std::to_string(ScreenWidth()) + ";h=" + std::to_string(ScreenHeight());
    }
    if (starts("SETCONF:")) {
        if (const auto at = message.find("DIM="); at != std::string::npos) {
            const auto v = Numbers(message.substr(at + 4, 32));
            if (v.size() >= 2) {
                template_w = int(v[0]);
                template_h = int(v[1]);
            }
        }
        return "OK";
    }
    if (starts("RGB=TEXT")) {
        return "OK";
    }
    if (starts("QUIT") || starts("GOW3:OFF") || starts("TESTTEMPLATE:PatternDynamic")) {
        Set(Patch{});
        WaitPresented();
        quit = starts("QUIT");
        return "OK";
    }
    const float screen = float(ScreenWidth()) * float(ScreenHeight());
    if (starts("RGB=RECTANGLE;")) { // w,h;100;r,g,b;bgR,bgG,bgB;-1,-1,x,y
        const auto parts = Split(message.substr(14), ';');
        Patch p;
        p.mode = Raw;
        if (parts.size() >= 4) {
            const auto dim = Numbers(parts[0]);
            const auto rgb = Numbers(parts[2]);
            const auto bg = Numbers(parts[3]);
            if (dim.size() >= 2 && dim[0] > 0 && dim[1] > 0) {
                p.area = std::clamp(dim[0] * dim[1] / screen, 0.001f, 1.0f);
            }
            for (int c = 0; c < 3 && c < int(rgb.size()); ++c) {
                p.value[c] = std::clamp(rgb[c] / 255.0f, 0.0f, 1.0f);
            }
            p.background = bg.size() >= 3;
            for (int c = 0; c < 3 && c < int(bg.size()); ++c) {
                p.bg[c] = std::clamp(bg[c] / 255.0f, 0.0f, 1.0f);
            }
        }
        Set(p);
        WaitPresented();
        return "OK";
    }
    if (starts("TESTTEMPLATERAMDISK:")) { // HCFR:r,g,b;bgR,bgG,bgB
        const auto colon = message.find(':', 20);
        const auto parts = Split(colon == std::string::npos ? "" : message.substr(colon + 1), ';');
        Patch p;
        p.mode = Raw;
        if (template_w > 0 && template_h > 0) {
            p.area = std::clamp(float(template_w) * float(template_h) / screen, 0.001f, 1.0f);
        }
        const auto rgb = parts.empty() ? std::vector<float>{} : Numbers(parts[0]);
        const auto bg = parts.size() > 1 ? Numbers(parts[1]) : std::vector<float>{};
        for (int c = 0; c < 3 && c < int(rgb.size()); ++c) {
            p.value[c] = std::clamp(rgb[c] / 255.0f, 0.0f, 1.0f);
        }
        p.background = bg.size() >= 3;
        for (int c = 0; c < 3 && c < int(bg.size()); ++c) {
            p.bg[c] = std::clamp(bg[c] / 255.0f, 0.0f, 1.0f);
        }
        Set(p);
        WaitPresented();
        return "OK";
    }
    if (starts("GOW3:RAW;") || starts("GOW3:GAME;")) {
        const bool raw = starts("GOW3:RAW;");
        const auto parts = Split(message.substr(raw ? 9 : 10), ';');
        Patch p;
        p.mode = raw ? Raw : Game;
        const auto v = parts.empty() ? std::vector<float>{} : Numbers(parts[0]);
        for (int c = 0; c < 3; ++c) {
            p.value[c] = v.empty() ? 0.0f : v[std::min<size_t>(c, v.size() - 1)];
            if (raw) {
                p.value[c] = std::clamp(p.value[c], 0.0f, 1.0f);
            }
        }
        if (parts.size() > 1) {
            const auto a = Numbers(parts[1]);
            if (!a.empty() && a[0] > 0.0f) {
                p.area = std::clamp(a[0], 0.001f, 1.0f);
            }
        }
        for (size_t i = 2; i < parts.size(); ++i) {
            p.background |= parts[i] == "black"; // raw: cleared; game: only the patch blitted
            p.neutral_brightness |= !raw && parts[i] == "neutral";
        }
        Set(p);
        WaitPresented();
        return "OK";
    }
    std::printf("Patterns: unknown message \"%s\"\n", message.substr(0, 80).c_str());
    return "ERR";
}

void Serve(SocketHandle client) {
    std::string pending;
    char buffer[1024];
    bool quit = false;
    while (!quit) {
        const int n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        pending.append(buffer, size_t(n));
        while (true) {
            // A message ends in "\x02\r" (PGenerator) or a newline.
            size_t end = pending.find("\x02\r");
            size_t skip = 2;
            const size_t newline = pending.find('\n');
            if (newline != std::string::npos && (end == std::string::npos || newline < end)) {
                end = newline;
                skip = 1;
            }
            if (end == std::string::npos) {
                break;
            }
            std::string message = pending.substr(0, end);
            pending.erase(0, end + skip);
            while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
                message.pop_back();
            }
            if (message.empty()) {
                continue;
            }
            const std::string reply = Handle(message, quit) + "\x02\r";
            send(client, reply.data(), int(reply.size()), 0);
        }
    }
    CloseSocket(client);
    Set(Patch{}); // a client that went away leaves no patch on screen
}

void Listen(int port, bool all_interfaces) {
    const SocketHandle server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        std::printf("Patterns: no socket\n");
        return;
    }
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u_short(port));
    addr.sin_addr.s_addr = htonl(all_interfaces ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(server, 1) != 0) {
        std::printf("Patterns: cannot listen on TCP %d\n", port);
        CloseSocket(server);
        return;
    }
    std::printf("Patterns: listening on TCP %s:%d\n", all_interfaces ? "0.0.0.0" : "127.0.0.1", port);
    std::fflush(stdout);
    while (true) {
        const SocketHandle client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        std::printf("Patterns: client connected on TCP %d\n", port);
        std::fflush(stdout);
        Serve(client);
    }
}

/// HCFR's PGenerator discovery: a UDP broadcast "Who is a PGenerator" to port 1977, answered from here.
void Discovery() {
    const SocketHandle udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET) {
        return;
    }
    int yes = 1;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1977);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(udp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::printf("Patterns: cannot listen on UDP 1977 (PGenerator discovery)\n");
        CloseSocket(udp);
        return;
    }
    char buffer[256];
    while (true) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(udp, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) {
            continue;
        }
        buffer[n] = 0;
        if (std::strstr(buffer, "PGenerator")) {
            static const char reply[] = "This is PGenerator MediEvil";
            sendto(udp, reply, int(sizeof(reply) - 1), 0, reinterpret_cast<sockaddr*>(&from), from_len);
        }
    }
}

} // namespace

void Start() {
    const char* env = std::getenv("GOW3_PATTERN_SERVER");
    if (!env || !env[0] || env[0] == '0') {
        return;
    }
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::printf("Patterns: WSAStartup failed\n");
        return;
    }
#endif
    const char* port_env = std::getenv("GOW3_PATTERN_PORT");
    const int port = port_env && port_env[0] ? std::atoi(port_env) : 21977;
    std::thread([port] { Listen(port, false); }).detach();
    if (std::strcmp(env, "pgen") == 0) {
        std::thread([] { Listen(85, true); }).detach();
        std::thread(Discovery).detach();
    }
}

bool Active() {
    return mode.load(std::memory_order_relaxed) != Off;
}

bool GameActive() {
    return mode.load(std::memory_order_relaxed) == Game;
}

Patch Current() {
    std::scoped_lock lock{mutex};
    return current;
}

void NotePresented() {
    presents.fetch_add(1, std::memory_order_release);
}

} // namespace Gow3Patterns
#endif // GOW3_DEV_TOOLS
