// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

std::size_t Count(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}
}

int main() {
    std::ifstream file("gpu/bbnet/thd/thd_session.cpp");
    std::stringstream text;
    text << file.rdbuf();
    const std::string code = text.str();
    const auto begin = code.find("HttpResult Perform(");
    const auto end = begin == std::string::npos ? std::string::npos : code.find("\n}\n", begin);
    Check(begin != std::string::npos && end != std::string::npos, "Perform found");
    const std::string perform = begin == std::string::npos ? std::string() : code.substr(begin, end - begin);
    Check(Count(perform, "= send()") == 1,
          "every request to The Hunter's Dream goes out once: no resend, also when a kept connection drops after part "
          "of an answer");
    Check(Count(perform, "->Post(") == 1 && Count(perform, "->Get(") == 1 && Count(perform, "post ?") == 1,
          "the one send is the single Post or Get");
    Check(perform.find("!PollerLive()") != std::string::npos, "a stopped poller's poll is not sent");
    Check(perform.find("PollAbandoned") != std::string::npos, "a stopped poller's new connection is refused");
    std::printf(failures ? "thd-resend-test: %d failed\n" : "thd-resend-test: ok\n", failures);
    return failures ? 1 : 0;
}
