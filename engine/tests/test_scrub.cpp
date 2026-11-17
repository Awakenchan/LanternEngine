// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>

#include "gpu/shim/lantern_scrub.h"

namespace {
std::atomic<long> g_news{0};
std::atomic<bool> g_count{false};
}

void* operator new(std::size_t size) {
    if (g_count.load(std::memory_order_relaxed)) g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {
using namespace Lantern;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::string Clean(std::string_view in) {
    static char out[8192];
    g_count = true;
    const std::size_t n = Scrub::Text(in, out, sizeof out);
    g_count = false;
    return std::string(out, n);
}

bool Has(const std::string& s, std::string_view part) {
    return s.find(part) != std::string::npos;
}
}

int main() {
    Scrub::ClearSecretsForTest();
    Expect(Scrub::AddSecret("C:\\Users\\Jane Doe", Scrub::Kind::Home), "the home folder is registered");
    Expect(Scrub::AddSecret("Jane Doe", Scrub::Kind::User), "the user name is registered");
    Expect(Scrub::AddSecret("C:\\Users\\jane\\OneDrive - Contoso", Scrub::Kind::OneDrive), "OneDrive is registered");
    Expect(Scrub::AddSecret("Lamplight Hunters", Scrub::Kind::Room), "a room name is registered");
    Expect(Scrub::AddSecret("hunter2pass", Scrub::Kind::Secret), "a room password is registered");
    Expect(!Scrub::AddSecret("ab", Scrub::Kind::Secret), "a value too short to scrub safely is refused");

    const std::string text =
        "log C:\\Users\\Jane Doe\\AppData\\Local\\Lantern\\logs\\last_session.log; game C:\\Users\\jane\\OneDrive - "
        "Contoso\\Games\\Bloodborne; steam /home/deck/.local/share/lantern; mac /Users/sam/Library; room Lamplight "
        "Hunters password hunter2pass; token=abc123def; invite: XK2-99Q; code=4711; server wss://185-244-28-175.sslip.io "
        "and http://example.org/path; peer 192.168.1.20 and [fe80::1:2]; save SPRJ0005; JANE DOE again; "
        "vulkan 1.3.280; key 9f13c0a2e4b1d377; LANTERN_STEPS_OFF=group:gx; BB_GPU_ID=1; BB_ROOM_PASSWORD=<set>";
    const std::string out = Clean(text);
    Expect(g_news.load() == 0, "the scrubber allocates nothing");
    for (std::string_view secret : {"Jane", "jane", "JANE", "Contoso", "deck", "sam", "Lamplight", "hunter2pass",
                                    "abc123def", "XK2-99Q", "4711", "185-244-28-175", "sslip", "example.org",
                                    "192.168.1.20", "fe80", "SPRJ0005"}) {
        if (Has(out, secret)) {
            std::printf("left in: %.*s\n", int(secret.size()), secret.data());
            Expect(false, "nothing personal survives the scrubber");
        }
    }
    for (std::string_view placeholder : {"<home>", "<onedrive>", "/home/<user>", "/Users/<user>", "<room>", "<secret>",
                                         "<server>", "<address>", "<save>", "<user>"}) {
        if (!Has(out, placeholder)) {
            std::printf("missing: %.*s\n", int(placeholder.size()), placeholder.data());
            Expect(false, "each placeholder appears");
        }
    }
    Expect(Has(out, "vulkan 1.3.280") && Has(out, "key 9f13c0a2e4b1d377"),
           "versions and pipeline keys are not taken for addresses or secrets");
    Expect(Has(out, "LANTERN_STEPS_OFF=group:gx") && Has(out, "BB_GPU_ID=1"), "switch lines stay readable");
    Expect(Has(out, "token=<secret>") && Has(out, "code=<secret>") && Has(out, "invite: <secret>"),
           "the keyword stays, the value goes");

    char small[16];
    const std::size_t n = Scrub::Text(text, small, sizeof small);
    Expect(n == sizeof small - 1 && small[n] == 0, "output is bounded and terminated");

    Scrub::ClearSecretsForTest();
    Expect(Clean("C:\\Users\\Someone Else\\x").find("Someone") == std::string::npos,
           "a profile folder is scrubbed by its pattern even without a registered secret");

    if (failures) {
        std::printf("scrub-test: %d failed\n", failures);
        std::printf("%s\n", out.c_str());
        return 1;
    }
    std::printf("scrub-test: ok\n");
    return 0;
}
