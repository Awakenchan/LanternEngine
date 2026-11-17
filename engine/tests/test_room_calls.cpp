// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "gpu/bbnet/room/room_calls.h"

namespace {
namespace C = BbRoom::Calls;
int g_failed = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
#ifndef LANTERN_ROOM_CALLS_MUTANTS
        std::printf("FAIL: %s\n", what);
#else
        (void)what;
#endif
    }
}

std::vector<std::uint8_t> Frame(std::uint32_t id, const std::string& path) {
    std::vector<std::uint8_t> f{1};
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    const std::string body = R"({"p":")" + path + R"("})";
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

std::vector<std::uint32_t> Ids(const std::vector<C::Owed>& owed) {
    std::vector<std::uint32_t> ids;
    for (const C::Owed& o : owed) ids.push_back(o.id);
    return ids;
}

void TestResend() {
    C::Pending p(10);
    const std::uint32_t a = p.NextId(), b = p.NextId(), c = p.NextId();
    Expect(a == 10 && b == 11 && c == 12, "ids are handed out in order");
    p.Add(a, 1, Frame(a, "/mp/matching2/join_room"), true);
    p.Add(b, 1, Frame(b, "/channel/search"), true);
    p.Add(c, 1, Frame(c, "/summon_messenger/get"), true);
    Expect(p.Back(1).empty(), "nothing is resent before the link drops");
    p.Lost();
    const auto first = p.Back(1);
    Expect(Ids(first) == std::vector<std::uint32_t>{a, b, c}, "every pending request is resent once, in order");
    Expect(!first.empty() && first[0].frame == Frame(a, "/mp/matching2/join_room") && first[0].host == 1,
           "a resend carries the same id and the same bytes to the same host");
    Expect(p.Back(1).empty(), "a second host notice after one reconnect resends nothing");
    p.Lost();
    Expect(Ids(p.Back(1)) == std::vector<std::uint32_t>{a, b, c}, "the next reconnect resends them once more");

    Expect(p.Answer(b), "a reply for a pending id is taken");
    Expect(!p.Answer(b), "a second reply for the same id is dropped");
    Expect(!p.Answer(999), "a reply for an unknown id is dropped");
    p.Lost();
    Expect(Ids(p.Back(1)) == std::vector<std::uint32_t>{a, c}, "an answered request is not resent");

    p.Drop(a);
    Expect(!p.Answer(a), "a request that timed out takes no late reply");
    p.Lost();
    Expect(Ids(p.Back(1)) == std::vector<std::uint32_t>{c}, "a request that timed out is not resent");
    p.Drop(c);
    Expect(p.Size() == 0, "nothing stays pending after replies and timeouts");
}

void TestResendRules() {
    C::Pending p(1);
    p.Add(1, 1, Frame(1, "/mp/matching2/create_room"), C::Resendable("/mp/matching2/create_room"));
    p.Add(2, 2, Frame(2, "/mp/matching2/join_room"), true);
    p.Add(3, 1, Frame(3, "/mp/matching2/join_room"), true);
    p.Lost();
    Expect(Ids(p.Back(1)) == std::vector<std::uint32_t>{3}, "only resendable requests to this host are resent");
    Expect(Ids(p.Back(2)) == std::vector<std::uint32_t>{2}, "a request to another host waits for that host");

    C::Pending q(1);
    q.Add(1, 1, Frame(1, "/mp/matching2/join_room"), true);
    q.Add(2, 1, Frame(2, "/mp/matching2/leave_room"), true);
    q.Unsent(2);
    Expect(Ids(q.Back(1)) == std::vector<std::uint32_t>{2}, "a request whose send failed goes on the next host notice");
    Expect(q.Back(1).empty(), "and only once");

    C::Pending w(0xffffffffu);
    Expect(w.NextId() == 0xffffffffu && w.NextId() == 1, "ids wrap past 0, which means no reply");

    for (const char* path : {"/mp/matching2/context_start", "/np/signaling/resolve", "/mp/matching2/join_room",
                             "/mp/matching2/leave_room", "/mp/matching2/heartbeat", "/lantern/leave_party",
                             "/summon_messenger/create", "/summon_messenger/get", "/summon_messenger/delete",
                             "/channel/upload", "/channel/search", "/channel/share"})
        Expect(C::Resendable(path), "a request that is safe to run twice on a 0.9.42 host is resent");
    for (const char* path : {"/mp/matching2/create_room", "/mp/matching2/kick_member", "/summon_messenger/request",
                             "/mp/matching2/something_new", ""})
        Expect(!C::Resendable(path), "a request that is not safe to run twice on a 0.9.42 host is never resent");
}

void TestRecent() {
    C::Recent r;
    int runs = 0;
    const auto handle = [&](int slot, std::uint32_t id, const std::vector<std::uint8_t>& req) {
        if (const std::string* kept = r.Find(slot, id, req.data(), req.size())) return *kept;
        ++runs;
        std::string reply = "reply " + std::to_string(runs);
        r.Keep(slot, id, req.data(), req.size(), reply);
        return reply;
    };
    const auto join = Frame(5, "/mp/matching2/join_room");
    const std::string one = handle(2, 5, join);
    const std::string two = handle(2, 5, join);
    Expect(runs == 1 && one == two, "a duplicate id gets the cached reply and the handler runs once");
    handle(3, 5, join);
    Expect(runs == 2, "the same id from another slot is a new request");
    handle(2, 5, Frame(5, "/mp/matching2/leave_room"));
    Expect(runs == 3, "the same id with other bytes is a new request");
    Expect(handle(2, 5, Frame(5, "/mp/matching2/leave_room")) == "reply 3", "the newer request replaces the older one");
    handle(2, 0, Frame(0, "/mp/matching2/context_start"));
    handle(2, 0, Frame(0, "/mp/matching2/context_start"));
    Expect(runs == 5, "id 0 is never deduped");
    r.Forget(2);
    handle(2, 5, Frame(5, "/mp/matching2/leave_room"));
    Expect(runs == 6, "a slot that left forgets its requests");

    C::Recent small(64, 1 << 20);
    for (std::uint32_t id = 1; id <= 200; ++id) {
        const auto f = Frame(id, "/channel/search");
        small.Keep(1, id, f.data(), f.size(), "x");
    }
    const auto old = Frame(1, "/channel/search");
    const auto last = Frame(200, "/channel/search");
    Expect(small.Size() == 64, "the window holds at most 64 requests");
    Expect(!small.Find(1, 1, old.data(), old.size()) && small.Find(1, 200, last.data(), last.size()),
           "the oldest requests leave the window first");

    C::Recent bytes(64, 1000);
    for (std::uint32_t id = 1; id <= 10; ++id) {
        const auto f = Frame(id, "/channel/search");
        bytes.Keep(1, id, f.data(), f.size(), std::string(300, 'r'));
    }
    Expect(bytes.Bytes() <= 1000 && bytes.Size() == 3, "the window holds at most its byte budget of replies");
    const auto big = Frame(11, "/channel/search");
    bytes.Keep(1, 11, big.data(), big.size(), std::string(2000, 'r'));
    Expect(!bytes.Find(1, 11, big.data(), big.size()) && bytes.Size() == 3, "a reply larger than the budget is not kept");
}

struct Host {
    bool dedupe;
    C::Recent recent;
    std::map<std::string, int> runs;
    std::string Handle(int slot, const std::vector<std::uint8_t>& frame) {
        std::uint32_t id = 0;
        for (int i = 0; i < 4; ++i) id |= static_cast<std::uint32_t>(frame[1 + i]) << (8 * i);
        if (dedupe)
            if (const std::string* kept = recent.Find(slot, id, frame.data(), frame.size())) return *kept;
        const std::string path(frame.begin() + 11, frame.end() - 2);
        const std::string reply = path + " " + std::to_string(++runs[path]);
        if (dedupe) recent.Keep(slot, id, frame.data(), frame.size(), reply);
        return reply;
    }
};

void TestMixedVersions() {
    for (const bool new_host : {true, false}) {
        Host host{new_host, {}, {}};
        C::Pending guest(77);
        std::vector<std::uint32_t> ids;
        for (const char* path : {"/mp/matching2/join_room", "/summon_messenger/request", "/mp/matching2/create_room",
                                 "/channel/upload"}) {
            const std::uint32_t id = guest.NextId();
            ids.push_back(id);
            const auto frame = Frame(id, path);
            guest.Add(id, 1, frame, C::Resendable(path));
            host.Handle(2, frame);
        }
        guest.Lost();
        std::map<std::uint32_t, std::string> replies;
        for (const C::Owed& o : guest.Back(1)) {
            const std::string reply = host.Handle(2, o.frame);
            if (guest.Answer(o.id)) replies[o.id] = reply;
        }
        Expect(replies.size() == 2 && replies.count(ids[0]) && replies.count(ids[3]),
               "after a reconnect with lost replies only the resendable requests are asked again");
        Expect(host.runs["/summon_messenger/request"] == 1 && host.runs["/mp/matching2/create_room"] == 1,
               "a request that is unsafe to repeat runs once on any host");
        if (new_host) {
            Expect(host.runs["/mp/matching2/join_room"] == 1 && host.runs["/channel/upload"] == 1,
                   "a new host runs each resent request once");
            Expect(replies[ids[0]] == "/mp/matching2/join_room 1", "and answers a resend with the first reply");
        } else {
            Expect(host.runs["/mp/matching2/join_room"] == 2 && host.runs["/channel/upload"] == 2,
                   "a 0.9.42 host runs a resent request again, so only requests safe to repeat are resent");
        }
    }

    Host host{true, {}, {}};
    host.Handle(3, Frame(1, "/mp/matching2/join_room"));
    host.Handle(3, Frame(2, "/mp/matching2/join_room"));
    Expect(host.runs["/mp/matching2/join_room"] == 2, "a 0.9.42 guest's new ids always run on a new host");
    host.recent.Forget(3);
    host.Handle(3, Frame(1, "/mp/matching2/join_room"));
    Expect(host.runs["/mp/matching2/join_room"] == 3, "a 0.9.42 guest that restarted in the slot runs its id 1 again");
}

void Run() {
    TestResend();
    TestResendRules();
    TestRecent();
    TestMixedVersions();
}

}

int main() {
#ifdef LANTERN_ROOM_CALLS_MUTANTS
    for (int m = 0; m <= C::kMutants; ++m) {
        C::g_mutant = m;
        g_failed = 0;
        Run();
        if (m == 0 && g_failed) {
            std::printf("room calls mutants: the unmutated run failed %d checks\n", g_failed);
            return 1;
        }
        if (m > 0 && !g_failed) {
            std::printf("room calls mutants: mutant %d survived\n", m);
            return 1;
        }
    }
    std::printf("room calls mutants: every mutant caught\n");
    return 0;
#else
    Run();
    if (g_failed) {
        std::printf("room-calls-test: %d failed\n", g_failed);
        return 1;
    }
    std::printf("room-calls-test: ok\n");
    return 0;
#endif
}
