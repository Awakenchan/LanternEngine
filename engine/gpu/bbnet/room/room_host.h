// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "room_ws.h"

namespace BbRoom {

class LocalRoom {
public:
    LocalRoom();
    ~LocalRoom();
    LocalRoom(const LocalRoom&) = delete;
    LocalRoom& operator=(const LocalRoom&) = delete;

    bool Start(int port, const std::string& room_id, std::string& error, int most = kMaxMembers);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace BbRoom
