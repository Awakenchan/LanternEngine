// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Lantern::Meter::Threads {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr u32 kUnknownCpu = ~0u;

u32 CurrentCpu();
u32 CurrentTid();
bool CurrentThreadName(char* out, std::size_t room);
std::vector<int> CoreOfCpu();

enum class Kind : std::uint8_t { Main, Render, Worker };

struct Times {
    bool main_found = false;
    u64 main_ns = 0;
    bool render_found = false;
    u64 render_ns = 0;
    u32 workers = 0;
    u64 worker_max_ns = 0;
    u64 worker_sum_ns = 0;
    u32 gone = 0;
};

bool Wanted(const char* name, Kind* kind);

class GuestThreads {
public:
    GuestThreads() = default;
    GuestThreads(const GuestThreads&) = delete;
    GuestThreads& operator=(const GuestThreads&) = delete;
    ~GuestThreads();
    [[nodiscard]] bool Refresh();
    Times Take();
    [[nodiscard]] bool SampleMain(u64* rip, u64* stopped_ns);
    std::size_t Count() const {
        return entries.size();
    }

private:
    struct Entry {
        u32 tid = 0;
        Kind kind = Kind::Worker;
        void* handle = nullptr;
        void* sample_handle = nullptr;
        u64 last_ns = 0;
        bool primed = false;
        bool seen = false;
    };
    std::vector<Entry> entries;
    std::vector<u32> rejected;
};
}
