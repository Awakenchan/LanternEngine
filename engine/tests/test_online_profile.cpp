// SPDX-License-Identifier: GPL-3.0-or-later
#include "start/common.h"
#include "start/online.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::stringstream text;
    text << file.rdbuf();
    std::string out;
    for (const char c : text.str())
        if (c != '\r') out.push_back(c);
    return out;
}

std::string Normalised(std::string value, const std::string& data) {
    if (!data.empty() && value.compare(0, data.size(), data) == 0) {
        std::string rest = value.substr(data.size());
        for (char& c : rest)
            if (c == '\\') c = '/';
        value = "<data>" + rest;
    }
    return value;
}

std::string RunCase(const nlohmann::json& cases, const nlohmann::json& one, const fs::path& root) {
    const fs::path data = root / one.at("name").get<std::string>();
    std::error_code error;
    fs::remove_all(data, error);
    fs::create_directories(data);
    for (const auto& key : cases.at("tracked")) start::UnsetEnv(key.get<std::string>().c_str());
    for (const auto& [key, value] : cases.at("inherited").items()) start::SetEnv(key.c_str(), value.get<std::string>());
    for (const auto& [key, value] : one.at("env").items()) start::SetEnv(key.c_str(), value.get<std::string>());
    if (one.contains("patches")) {
        const nlohmann::json chosen = {{"enabled", one.at("patches")}};
        start::WriteText(data / "patches.json", chosen.dump());
    }
    std::map<std::string, std::string> settings;
    for (const auto& [key, value] : one.at("settings").items()) settings[key] = value.get<std::string>();

    std::string out = "[" + one.at("name").get<std::string>() + "]\n";
    try {
        out += start::ConfigureOnline(settings, data) ? "result: online\n" : "result: offline\n";
    } catch (const std::exception& e) {
        out += std::string("result: error: ") + e.what() + "\n";
    }
    const std::string data_text = start::Utf8(fs::absolute(data));
    out += "env:\n";
    for (const auto& key : cases.at("tracked")) {
        const std::string name = key.get<std::string>();
        const auto value = start::Env(name.c_str());
        out += value ? name + "=" + Normalised(*value, data_text) + "\n" : name + " unset\n";
    }
    const fs::path overrides = data / "network" / "host_overrides.json";
    out += "host_overrides.json:\n";
    out += fs::exists(overrides) ? ReadAll(overrides) + "\n" : "(none)\n";
    return out;
}

}

int main(int argc, char** argv) {
    const std::string cases_path = argc > 1 ? argv[1] : "tests/golden/online-profile-cases.json";
    const std::string golden = argc > 2 ? argv[2] : "tests/golden/online-profile-start.txt";
    const bool record = argc > 3 && std::strcmp(argv[3], "--record") == 0;
    const std::string cases_text = ReadAll(cases_path);
    const nlohmann::json cases = nlohmann::json::parse(cases_text, nullptr, false);
    if (cases.is_discarded()) {
        std::printf("FAIL cannot read %s\n", cases_path.c_str());
        return 1;
    }
    const fs::path root = fs::temp_directory_path() / "lantern-online-profile-start";
    std::string text;
    for (const auto& one : cases.at("cases")) text += RunCase(cases, one, root);
    std::error_code error;
    fs::remove_all(root, error);

    if (record) {
        start::WriteText(start::Path(golden), text);
        std::printf("online-profile-test: recorded %zu cases\n", cases.at("cases").size());
        return 0;
    }
    const std::string expected = ReadAll(start::Path(golden));
    if (expected != text) {
        std::printf("FAIL the online profile differs from %s\n", golden.c_str());
        std::size_t at = 0;
        while (at < expected.size() && at < text.size() && expected[at] == text[at]) ++at;
        const std::size_t line_start = text.rfind('\n', at == 0 ? 0 : at - 1);
        const std::size_t from = line_start == std::string::npos ? 0 : line_start + 1;
        std::printf("first difference near: %s\n", text.substr(from, 160).c_str());
        return 1;
    }
    std::printf("online-profile-test: ok (%zu cases)\n", cases.at("cases").size());
    return 0;
}
