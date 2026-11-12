// SPDX-License-Identifier: GPL-3.0-or-later
// Unofficial standalone deployment of Lantern Engine by Awaken's room host.
#include "room_host.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
void Stop(int) { stopping = 1; }
}

int main() {
    const char* configured = std::getenv("ROOM_ID");
    const std::string id = configured ? configured : "";
    if (id.size() != 64 || !std::all_of(id.begin(), id.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        std::fprintf(stderr, "ROOM_ID must contain 64 lowercase hexadecimal characters; run configure.py first.\n");
        return 2;
    }
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
#ifndef _WIN32
    // A disconnected client must not terminate this standalone process.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    BbRoom::LocalRoom room;
    std::string error;
    if (!room.Start(38765, id, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("Unofficial private co-op relay, protocol %d, up to %d players, TCP 38765\n"
                "Based on Lantern Engine by Awaken (https://github.com/Awakenchan/LanternEngine)\n",
                BbRoom::kRoomProtocol, BbRoom::kMaxMembers);
    std::fflush(stdout);
    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    room.Stop();
    return 0;
}
