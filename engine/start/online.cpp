// SPDX-License-Identifier: GPL-3.0-or-later
#include "online.h"
#include "online_rules.h"

#include <nlohmann/json.hpp>

namespace start {
namespace {

const char* const kGameStatusOrigin = "https://ss4.scej-network.jp:20443";
const char* const kHuntersDream = "https://thehuntersdream.com";
const char* const kPlayLogHosts[] = {"bb-playlog-prod.s3.amazonaws.com", "bb-playlog-test.s3.amazonaws.com"};
constexpr int kGameApiPort = 18671;
const char* const kShadnetHost = "srv.shadps4.net";
constexpr int kShadnetPort = 31313;
constexpr int kDefaultP2p = 3658;
constexpr int kDefaultSignTimeout = 30;
const char* const kThdEnv[] = {"BB_NET_BACKEND", "BB_THD_SERVER", "BB_THD_AUTH", "BB_THD_TOKEN", "BB_THD_ONLINE_ID",
                               "BB_THD_RULESET"};
const char* const kSeamlessServer = "https://185-244-28-175.sslip.io";
const char* const kDirectPort = "38765";
const char* const kRoomEnv[] = {"BB_ROOM_SERVER", "BB_ROOM_NAME", "BB_ROOM_PLAYER", "BB_ROOM_HOST", "BB_ROOM_HTTP_PORT", "BB_ROOM_LOCAL_PORT", "BB_ROOM_VISITOR"};
const char* const kOnlineEnv[] = {"BB_ONLINE", "BB_SHADNET_SERVER", "BB_SHADNET_WEBAPI", "BB_SHADNET_NPID",
                                  "BB_SHADNET_PASSWORD", "BB_SHADNET_TOKEN", "BB_UPNP", "BB_P2P_PORT",
                                  "SHADPS4_HTTP_HOST_OVERRIDES_JSON", "BB_SIGN_TIMEOUT"};

struct Url {
    std::string scheme, host, path, query, fragment;
    std::optional<int> port;
    bool credentials = false;
};

/// urllib's urlsplit, for the origins typed here.
Url Parse(const std::string& text) {
    Url url;
    std::string rest = text;
    const auto colon = rest.find("://");
    if (colon != std::string::npos) {
        url.scheme = Lower(rest.substr(0, colon));
        rest = rest.substr(colon + 3);
    }
    if (const auto hash = rest.find('#'); hash != std::string::npos) url.fragment = rest.substr(hash + 1), rest.resize(hash);
    if (const auto q = rest.find('?'); q != std::string::npos) url.query = rest.substr(q + 1), rest.resize(q);
    const auto slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    url.path = slash == std::string::npos ? "" : rest.substr(slash);
    if (const auto at = authority.rfind('@'); at != std::string::npos) {
        url.credentials = true;
        authority = authority.substr(at + 1);
    }
    std::string host = authority;
    if (!host.empty() && host[0] == '[') {
        const auto close = host.find(']');
        url.host = Lower(host.substr(1, close - 1));
        host = close == std::string::npos ? "" : host.substr(close + 1);
        if (!host.empty() && host[0] == ':') {
            const std::string port = host.substr(1);
            if (!port.empty()) url.port = std::stoi(port);
        }
    } else {
        const auto c = host.rfind(':');
        url.host = Lower(host.substr(0, c));
        if (c != std::string::npos) {
            const std::string port = host.substr(c + 1);
            if (!port.empty()) {
                if (port.find_first_not_of("0123456789") != std::string::npos || port.size() > 6)
                    throw Error("Invalid online API port");
                url.port = std::stoi(port);
            }
        }
    }
    return url;
}

std::string GameServer(std::string text) {
    text = Trim(text);
    if (text.empty()) return kHuntersDream;
    if (text.find("://") == std::string::npos) text = "https://" + text;
    if (Parse(text).host == kShadnetHost) return kHuntersDream;
    return text;
}

std::string HttpBase(const std::string& text) {
    if (text.empty() || std::any_of(text.begin(), text.end(), [](char c) { return std::uint8_t(c) < 32; }))
        throw Error("Online requires an explicit WebAPI / game API URL");
    const Url value = Parse(text);
    if ((value.scheme != "http" && value.scheme != "https") || value.host.empty() || value.credentials ||
        !value.query.empty() || !value.fragment.empty())
        throw Error("Online API must be an HTTP(S) URL without credentials, query or fragment");
    if (value.port && (*value.port < 1 || *value.port > 65535)) throw Error("Invalid online API port");
    std::string out = text;
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

fs::path WriteOverrides(const fs::path& data, const nlohmann::ordered_json& overrides) {
    const fs::path destination = data / "network" / "host_overrides.json";
    WriteText(destination, overrides.dump(2));
    return fs::absolute(destination);
}

std::string P2pPort(const std::map<std::string, std::string>& settings) {
    const std::string p2p = Trim(Setting(settings, "online_p2p_port").value_or(""));
    if (p2p.empty()) return std::to_string(kDefaultP2p);
    int port = 0;
    try {
        port = std::stoi(p2p);
    } catch (const std::exception&) {
        throw Error("Local P2P port must be between 1 and 65535, or empty for the default (3658)");
    }
    if (port < 1 || port > 65535) throw Error("Local P2P port must be between 1 and 65535, or empty for the default (3658)");
    return std::to_string(port);
}

void SetSignTimeout(const std::map<std::string, std::string>& settings) {
    const std::string timeout = Trim(Setting(settings, "online_sign_timeout").value_or(""));
    if (!timeout.empty() && timeout.find_first_not_of("0123456789") == std::string::npos && timeout.size() < 10)
        SetEnv("BB_SIGN_TIMEOUT", std::to_string(std::clamp(std::stoi(timeout), 0, 600)));
    else
        SetEnv("BB_SIGN_TIMEOUT", std::to_string(kDefaultSignTimeout));
}

bool ConfigureHuntersDream(const std::map<std::string, std::string>& settings, const fs::path& data) {
    const std::string game_api = HttpBase(GameServer(Setting(settings, "online_game_api").value_or("")));
    const Url origin = Parse(game_api);
    if (!origin.path.empty() && origin.path != "/") throw Error("The game server needs an origin without a path");
    if (Env("BB_THD_TOKEN", "").empty() || Env("BB_THD_ONLINE_ID", "").empty())
        throw Error("Sign in to The Hunter's Dream with Discord on the launcher's Online page");
    const std::string base = origin.scheme + "://" + origin.host;
    nlohmann::ordered_json overrides;
    overrides["*.scej-network.jp"] = base;
    for (const char* host : kPlayLogHosts) overrides[host] = base + ":" + std::to_string(kGameApiPort);
    const fs::path destination = WriteOverrides(data, overrides);
    for (const char* key : {"BB_SHADNET_SERVER", "BB_SHADNET_WEBAPI", "BB_SHADNET_NPID", "BB_SHADNET_PASSWORD",
                            "BB_SHADNET_TOKEN", "BB_UPNP"})
        UnsetEnv(key);
    SetEnv("BB_ONLINE", "1");
    SetEnv("BB_NET_BACKEND", "thd");
    if (Env("BB_THD_TRACE", "") == "1") SetEnv("BBHOST_NP_TRACE", "1");
    SetEnv("BB_THD_SERVER", base + ":" + std::to_string(kGameApiPort));
    SetEnv("BB_THD_AUTH", base);
    std::vector<std::string> enabled;
    try {
        const auto chosen = nlohmann::json::parse(ReadText(Path(Env("BB_PATCHES_CONFIG", Utf8(data / "patches.json")))));
        if (chosen.contains("enabled"))
            for (const auto& key : chosen.at("enabled"))
                if (key.is_string()) enabled.push_back(key.get<std::string>());
    } catch (const std::exception&) {
    }
    const std::string typed = Trim(Setting(settings, "online_thd_ruleset").value_or(""));
    const std::string ruleset = OnlineRules::Valid(typed) ? typed : OnlineRules::Ruleset(enabled, false);
    SetEnv("BB_THD_RULESET", ruleset);
    if (OnlineRules::Valid(typed)) Log("Online rules: %s (set by hand)", ruleset.c_str());
    else Log("Online rules: %s (X-BBHost-Ruleset: %s); other rules share fewer bloodstains and messages", OnlineRules::Describe(enabled, false).c_str(), ruleset.c_str());
    SetEnv("BB_P2P_PORT", P2pPort(settings));
    SetEnv("SHADPS4_HTTP_HOST_OVERRIDES_JSON", Utf8(destination));
    SetSignTimeout(settings);
    return true;
}

bool ConfigureSeamless(const std::map<std::string, std::string>& settings, const fs::path& data) {
    const auto get = [&](const char* key) { return Trim(Setting(settings, key).value_or("")); };
    std::string server = get("seamless_server");
    if (server.empty()) server = kSeamlessServer;
    const bool host = get("seamless_role") == "host";
    const std::string room = get(host ? "seamless_host_room" : "seamless_room");
    std::string player;
    for (const char c : get("seamless_player")) {
        const auto u = std::uint8_t(c);
        if (u >= 0x20 && u < 0x7f && c != '"' && c != '\\' &&!(c == ' ' && (player.empty() || player.back() == ' ')))
            player.push_back(c);
        if (player.size() == 16) break;
    }
    while (!player.empty() && player.back() == ' ') player.pop_back();
    if (player.empty()) player = "Hunter";
    if (room.empty())
        throw Error(host ? "Open the launcher's Online page once to make your room" : "Paste the host's invite code on the launcher's Online page");

    const int p2p = std::stoi(P2pPort(settings));
    const int http = p2p < 65535 ? p2p + 1 : p2p - 1;
    const std::string local = "http://127.0.0.1:" + std::to_string(http);
    nlohmann::ordered_json overrides;
    overrides["*.scej-network.jp"] = local;
    overrides[kGameStatusOrigin] = local;
    for (const char* host : kPlayLogHosts) overrides[host] = local;
    const fs::path destination = WriteOverrides(data, overrides);
    for (const char* key : kOnlineEnv) UnsetEnv(key);
    for (const char* key : kThdEnv) UnsetEnv(key);
    SetEnv("BB_ONLINE", "1");
    SetEnv("BB_NET_BACKEND", "room");
    const std::string direct_port = get("seamless_direct_port").empty() ? std::string(kDirectPort) : get("seamless_direct_port");
    const bool direct = get("seamless_direct") == "1";
    if (direct && !host) {
        std::string address = get("seamless_direct_host");
        if (address.empty()) throw Error("Paste the host's invite code, or type the host's address on the launcher's Online page");
        if (address.find(':') == std::string::npos) address += ":" + direct_port;
        server = "ws://" + address;
        UnsetEnv("BB_ROOM_LOCAL_PORT");
    } else if (direct) {
        const int port = std::atoi(direct_port.c_str());
        if (port < 1024 || port > 65535) throw Error("The direct room's port must be between 1024 and 65535");
        server = "ws://127.0.0.1:" + std::to_string(port);
        SetEnv("BB_ROOM_LOCAL_PORT", std::to_string(port));
    } else {
        UnsetEnv("BB_ROOM_LOCAL_PORT");
        if (server.rfind("ws://", 0) == 0) server = kSeamlessServer;
    }
    SetEnv("BB_ROOM_SERVER", server);
    SetEnv("BB_ROOM_NAME", room);
    SetEnv("BB_ROOM_PLAYER", player);
    SetEnv("BB_ROOM_HOST", host ? "1" : "0");
    if (!host && !direct && get("seamless_visitor") == "1") SetEnv("BB_ROOM_VISITOR", "1");
    else UnsetEnv("BB_ROOM_VISITOR");
    if (host) SetEnv("BB_ROOM_PASSWORD", Env("BB_ROOM_HOST_PASSWORD", ""));
    SetEnv("BB_ROOM_HTTP_PORT", std::to_string(http));
    SetEnv("BB_P2P_PORT", std::to_string(p2p));
    SetEnv("SHADPS4_HTTP_HOST_OVERRIDES_JSON", Utf8(destination));
    SetSignTimeout(settings);
    return true;
}

} // namespace

bool ConfigureOnline(const std::map<std::string, std::string>& settings, const fs::path& data) {
    const auto get = [&](const char* key, const char* fallback = "") { return Setting(settings, key).value_or(fallback); };
    if (get("seamless_enabled") == "1" && get("online_enabled") == "1") return ConfigureSeamless(settings, data);
    for (const char* key : kRoomEnv) UnsetEnv(key);
    if (get("online_pool") == "huntersdream" && get("online_enabled") == "1" && get("online_connected", "1") == "1")
        return ConfigureHuntersDream(settings, data);
    const bool paused = Env("BB_ONLINE_PAUSED", "0") == "1";
    if (paused && get("online_enabled") == "1")
        Log("Online: paused in this version while we rework the connection together with the shadNet and The Hunter's "
            "Dream teams; the game starts offline");
    if (paused || get("online_enabled") != "1" || get("online_connected", "1") != "1") {
        for (const char* key : kOnlineEnv) UnsetEnv(key);
        for (const char* key : kThdEnv) UnsetEnv(key);
        SetEnv("BB_ONLINE", "0");
        return false;
    }
    for (const char* key : kThdEnv) UnsetEnv(key);
    std::string host = Trim(get("online_host"));
    while (!host.empty() && (host.front() == '[' || host.front() == ']')) host.erase(0, 1);
    while (!host.empty() && (host.back() == '[' || host.back() == ']')) host.pop_back();
    if (host.empty()) host = kShadnetHost;
    if (std::any_of(host.begin(), host.end(),
                    [](char c) { return std::isspace(std::uint8_t(c)) || std::uint8_t(c) < 32 || std::strchr("/@?#:", c); }))
        throw Error("Enter a shadNet server name without a scheme, path or port");
    int port = kShadnetPort;
    if (const std::string p = get("online_port"); !p.empty()) port = std::stoi(p);
    if (port < 1 || port > 65535) throw Error("shadNet TCP port must be between 1 and 65535");
    const std::string default_webapi = "http://" + host + ":31315";
    const std::string typed_webapi = Trim(get("online_webapi"));
    const std::string webapi = HttpBase(typed_webapi.empty() ? default_webapi : typed_webapi);
    const std::string game_api = HttpBase(GameServer(get("online_game_api")));
    const std::string webapi_path = Parse(webapi).path, game_path = Parse(game_api).path;
    if ((!webapi_path.empty() && webapi_path != "/") || (!game_path.empty() && game_path != "/"))
        throw Error("WebAPI and game API require a server origin without an extra path");
    const std::string username = get("online_username");
    if (username.empty() || username.size() > 16 ||
        std::any_of(username.begin(), username.end(), [](char c) { return std::isspace(std::uint8_t(c)) || std::uint8_t(c) < 32; }))
        throw Error("Enter the shadNet account name (up to 16 characters)");
    if (Env("BB_SHADNET_PASSWORD", "").empty()) throw Error("Sign in with the account password in the launcher");
    const std::string p2p = P2pPort(settings);
    const Url api = Parse(game_api);
    nlohmann::ordered_json overrides;
    overrides[kGameStatusOrigin] = game_api;
    for (const char* h : kPlayLogHosts) overrides[h] = api.scheme + "://" + api.host + ":" + std::to_string(kGameApiPort);
    const fs::path destination = WriteOverrides(data, overrides);
    SetEnv("BB_ONLINE", "1");
    SetEnv("BB_SHADNET_SERVER", host + ":" + std::to_string(port));
    SetEnv("BB_SHADNET_WEBAPI", webapi);
    SetEnv("BB_SHADNET_NPID", username);
    SetEnv("BB_UPNP", get("online_upnp") == "0" ? "0" : "1");
    SetEnv("BB_P2P_PORT", p2p);
    SetEnv("SHADPS4_HTTP_HOST_OVERRIDES_JSON", Utf8(destination));
    SetSignTimeout(settings);
    return true;
}

} // namespace start
