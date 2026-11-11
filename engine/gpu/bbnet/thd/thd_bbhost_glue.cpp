// SPDX-License-Identifier: GPL-3.0-or-later
#include "thd_internal.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "core/libraries/libs.h"
#include "../vendor/bbhost_shim/hle/common.h"
#include "../vendor/bbhost_shim/hle/net_p2p.h"
#include "../vendor/bbhost_shim/hle/np.h"
#include "../vendor/bbhost_shim/log.h"
#include "../vendor/bbhost_shim/net/session.h"

namespace {
std::mutex g_fn_mu;
std::map<std::string, void*>& Functions() {
    static std::map<std::string, void*> functions;
    return functions;
}

net::Peer ToNet(const BbThd::Peer& p) {
    net::Peer out;
    out.member_id = p.member_id;
    out.online_id = p.online_id;
    out.addr = p.addr;
    out.port = p.port;
    return out;
}
}


void bbhost_np_log(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::string out;
    for (const char* at = text; *at;) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        int used = 0;
        const bool starts = (at == text || !std::isdigit(static_cast<unsigned char>(at[-1]))) &&
                            std::isdigit(static_cast<unsigned char>(*at));
        if (starts && std::sscanf(at, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &used) == 4 && a < 256 && b < 256 &&
            c < 256 && d < 256) {
            out += BbNet::MaskIpv4Host((a << 24) | (b << 16) | (c << 8) | d);
            at += used;
            continue;
        }
        out += *at++;
    }
    BbThd::Log("%s", out.c_str());
}

void register_hle_fn(const char* name, void* fn) {
    std::lock_guard lk(g_fn_mu);
    Functions()[name] = fn;
}

std::string hle_np_npid_text(const void* npid) { return BbThd::NpIdText(npid); }

void hle_net_p2p_signal(std::uint32_t addr, std::uint16_t port, const void* data, std::size_t len) {
    BbThd::P2pSignalSend(addr, port, data, len);
}

namespace net {
void dispatch_after(int delay_ms, Prio prio, std::function<void()> fn) {
    BbThd::DispatchAfter(delay_ms, static_cast<BbThd::Prio>(prio), std::move(fn));
}
bool peers_get(std::uint16_t member_id, Peer* out) {
    BbThd::Peer p;
    if (!BbThd::PeersGet(member_id, &p)) return false;
    *out = ToNet(p);
    return true;
}
bool peers_find_online(const std::string& id, Peer* out) {
    BbThd::Peer p;
    if (!BbThd::PeersFindOnline(id, &p)) return false;
    *out = ToNet(p);
    return true;
}
void peers_set_conn(std::uint16_t member_id, unsigned conn_id, int status) { BbThd::PeersSetConn(member_id, conn_id, status); }
Peer peers_provisional(const std::string& id, std::uint32_t addr, std::uint16_t port) {
    return ToNet(BbThd::PeersProvisional(id, addr, port));
}
bool server_signaling_resolve(const std::string& id, std::uint32_t* addr, std::uint16_t* port, std::string& error) {
    return BbThd::ServerSignalingResolve(id, addr, port, error);
}
std::string online_id() { return BbThd::OnlineId(); }
}

namespace {
template <typename F> F Vendored(const char* name) {
    std::lock_guard lk(g_fn_mu);
    const auto it = Functions().find(name);
    if (it == Functions().end()) {
        BbThd::Log("np: the vendored NpSignaling has no %s", name);
        return nullptr;
    }
    return reinterpret_cast<F>(it->second);
}

using InitFn = int(PS4_SYSV_ABI*)(u64, int, int, u64);
using TermFn = int(PS4_SYSV_ABI*)();
using CreateFn = int(PS4_SYSV_ABI*)(const void*, void*, void*, unsigned*);
using DeleteFn = int(PS4_SYSV_ABI*)(unsigned);
using ActivateFn = int(PS4_SYSV_ABI*)(unsigned, const void*, unsigned*);
using DeactivateFn = int(PS4_SYSV_ABI*)(unsigned, unsigned);
using StatusFn = int(PS4_SYSV_ABI*)(unsigned, unsigned, int*, std::uint32_t*, std::uint16_t*);
InitFn g_init = nullptr;
TermFn g_term = nullptr;
CreateFn g_create = nullptr;
DeleteFn g_delete = nullptr;
ActivateFn g_activate = nullptr;
DeactivateFn g_deactivate = nullptr;
StatusFn g_status = nullptr;
constexpr int kNotLoaded = static_cast<int>(0x80552715);

s32 PS4_SYSV_ABI SigInit(u64 a, int b, int c, u64 d) { return g_init ? g_init(a, b, c, d) : kNotLoaded; }
s32 PS4_SYSV_ABI SigTerm() { return g_term ? g_term() : kNotLoaded; }
s32 PS4_SYSV_ABI SigCreate(const void* npid, void* cb, void* arg, unsigned* ctx) {
    return g_create ? g_create(npid, cb, arg, ctx) : kNotLoaded;
}
s32 PS4_SYSV_ABI SigDelete(unsigned ctx) { return g_delete ? g_delete(ctx) : kNotLoaded; }
s32 PS4_SYSV_ABI SigActivate(unsigned ctx, const void* npid, unsigned* conn) {
    return g_activate ? g_activate(ctx, npid, conn) : kNotLoaded;
}
s32 PS4_SYSV_ABI SigDeactivate(unsigned ctx, unsigned conn) { return g_deactivate ? g_deactivate(ctx, conn) : kNotLoaded; }
s32 PS4_SYSV_ABI SigStatus(unsigned ctx, unsigned conn, int* st, std::uint32_t* addr, std::uint16_t* port) {
    return g_status ? g_status(ctx, conn, st, addr, port) : kNotLoaded;
}
}

namespace BbThd {
void RegisterVendoredSignaling(Core::Loader::SymbolsResolver* sym) {
    if (const char* trace = std::getenv("BB_THD_TRACE"); trace && trace[0] == '1') {
        Log("np: BB_THD_TRACE also turns on the signaling trace (BBHOST_NP_TRACE)");
    }
    hle_register_np_signaling();
    g_init = Vendored<InitFn>("sceNpSignalingInitialize");
    g_term = Vendored<TermFn>("sceNpSignalingTerminate");
    g_create = Vendored<CreateFn>("sceNpSignalingCreateContext");
    g_delete = Vendored<DeleteFn>("sceNpSignalingDeleteContext");
    g_activate = Vendored<ActivateFn>("sceNpSignalingActivateConnection");
    g_deactivate = Vendored<DeactivateFn>("sceNpSignalingDeactivateConnection");
    g_status = Vendored<StatusFn>("sceNpSignalingGetConnectionStatus");
#define SIG(nid, fn) LIB_FUNCTION(nid, "libSceNpSignaling", 1, "libSceNpSignaling", fn)
    SIG("3KOuC4RmZZU", SigInit);
    SIG("NPhw0UXaNrk", SigTerm);
    SIG("5yYjEdd4t8Y", SigCreate);
    SIG("hx+LIg-1koI", SigDelete);
    SIG("0UvTFeomAUM", SigActivate);
    SIG("6UEembipgrM", SigDeactivate);
    SIG("bD-JizUb3JM", SigStatus);
#undef SIG
}
}
