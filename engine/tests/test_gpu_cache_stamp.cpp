// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "start/gpu_cache_stamp.h"

namespace {
int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}
}

int main() {
    using GpuCacheStamp::Action;
    using GpuCacheStamp::Decide;
    const std::string now = "gpu 8fafa7ab9e56bc23", legacy = "gpu 1111111111111111";
    std::ifstream file("gpu-cache.accepted.id");
    std::stringstream text;
    text << file.rdbuf();
    const auto accepted = GpuCacheStamp::Accepted(text.str());
    Check(accepted.size() == 1 && accepted[0] == "gpu 10318a912f823da3", "the shipped list holds exactly 0.9.41's stamp");
    Check(Decide(now, now, legacy, accepted) == Action::Keep, "same stamp: kept");
    Check(Decide("gpu 10318a912f823da3", now, legacy, accepted) == Action::Restamp, "0.9.41's cache: kept and stamped again");
    Check(Decide(legacy, now, legacy, accepted) == Action::Restamp, "legacy stamp: kept and stamped again");
    Check(Decide("gpu ec3f2f26b6dc390a", now, legacy, accepted) == Action::Recompile, "another engine's cache: recompiled");
    Check(Decide("", now, legacy, accepted) == Action::Recompile, "no stamp: recompiled");
    Check(Decide("x", "", legacy, accepted) == Action::Keep, "no identity: left alone");
    Check(GpuCacheStamp::Accepted("10318a912f823da3\r\nnot-an-id\n0123456789ABCDEF\n\n").size() == 1, "only 16 lower-case hex ids count");
    const std::string notice = GpuCacheStamp::RecompileNotice("CUSA03173");
    Check(notice.find("CUSA03173") != std::string::npos && notice.find("recompiling") != std::string::npos &&
              notice.find("window opens after") != std::string::npos,
          "the recompile says what it does before it starts");
    std::ifstream main_source("start/main.cpp");
    std::stringstream main_text;
    main_text << main_source.rdbuf();
    const std::string code = main_text.str();
    const auto notice_at = code.find("GpuCacheStamp::RecompileNotice");
    const auto run_at = code.find("--recompile-cache");
    Check(notice_at != std::string::npos && run_at != std::string::npos && notice_at < run_at,
          "the start program prints the notice before it runs the recompile");
    std::printf(failures ? "gpu-cache-stamp-test: %d failed\n" : "gpu-cache-stamp-test: ok\n", failures);
    return failures ? 1 : 0;
}
