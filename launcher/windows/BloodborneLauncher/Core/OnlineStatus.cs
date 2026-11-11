// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    public static class OnlineStatus
    {
        public const string Lamp2Lamp = "Bloodborne.xml/Lamp2Lamp (warp between lamps)";
        public const string Lamp2LampBosses = "Bloodborne.xml/Lamp2Lamp: warp to bosses";

        private static readonly Regex RoomFullLine = new(@"This room is full \((\d+) of (\d+) hunters\)", RegexOptions.Compiled);

        public static string? RoomFull(string line) =>
            RoomFullLine.Match(line) is { Success: true } m
                ? L.F($"This room is full ({m.Groups[1].Value} of {m.Groups[2].Value} hunters)")
                : null;

        public static string? RoomFull(IEnumerable<string> lines) => lines.Select(RoomFull).LastOrDefault(r => r != null);

        private static readonly (string Key, string Title)[] Rules =
        {
            (Lamp2Lamp, "Lamp2Lamp warps"),
            (Lamp2LampBosses, "Lamp2Lamp warps to bosses"),
        };

        private static readonly string[] CheatLikePatches =
        {
            "Bloodborne.xml/Player No Dead (Read note)", "Bloodborne.xml/Player Stealth (Read note)",
            "Bloodborne.xml/Player Silent (Read note)", "Bloodborne.xml/Enemy Control",
            "Bloodborne.xml/Restore Debug Menu (READ NOTES)",
        };

        public static bool CheatLike(string key) => CheatLikePatches.Contains(key, StringComparer.Ordinal);

        public static string CheatLikeNote => L.T("Off while Play online or seamless co-op is on");

        public static List<string> RuleTitles(IEnumerable<string> enabled)
        {
            var titles = new List<string>();
            foreach (var key in enabled)
                foreach (var rule in Rules)
                    if (rule.Key == key && !titles.Contains(rule.Title))
                        titles.Add(rule.Title);
            return titles;
        }

        public static string RulesLine(IEnumerable<string> enabled)
        {
            var titles = RuleTitles(enabled);
            return titles.Count == 0
                ? L.T("Online rules: vanilla")
                : L.T("Online rules: ") + string.Join(", ", titles.Select(L.T));
        }

        public static List<string> EnabledPatches(string profileJson)
        {
            try
            {
                return JsonNode.Parse(profileJson)?["enabled"] is JsonArray list
                    ? list.Select(n => n is JsonValue v && v.TryGetValue<string>(out var s) ? s : "").Where(s => s.Length > 0).ToList()
                    : new List<string>();
            }
            catch (Exception) { return new List<string>(); }
        }

        public static string RulesLine()
        {
            try
            {
                return RulesLine(System.IO.File.Exists(Patches.ProfilePath)
                    ? EnabledPatches(System.IO.File.ReadAllText(Patches.ProfilePath))
                    : new List<string>());
            }
            catch (Exception) { return RulesLine(Array.Empty<string>()); }
        }

        public static string RulesNote =>
            L.T("Players on other rules than vanilla, such as Lamp2Lamp warps, see fewer bloodstains and messages from vanilla players. Cheats are always off online.");

        public enum SignIn { Valid, Expired, Unknown }

        public sealed record SignInState(SignIn State, string Detail);

        public static SignInState ClassifyWhoAmI(string? reply)
        {
            JsonObject? json = null;
            try { json = reply is null ? null : JsonNode.Parse(reply) as JsonObject; } catch (Exception) { }
            if (json?["ResKind"] is not JsonValue kindNode || !kindNode.TryGetValue<int>(out var kind))
                return new SignInState(SignIn.Unknown, "");
            if (kind != 0)
                return new SignInState(SignIn.Expired, Text(json["Message"]));
            var id = Text(json["OnlineId"]);
            return id.Length > 0 ? new SignInState(SignIn.Valid, id) : new SignInState(SignIn.Unknown, "");

            static string Text(JsonNode? node) =>
                node is JsonValue v && v.TryGetValue<string>(out var s) ? s.Trim() : "";
        }

        public static string SignInLine(SignInState? state) => state switch
        {
            null => L.T("checking…"),
            { State: SignIn.Valid } => L.F($"signed in as {state.Detail}"),
            { State: SignIn.Expired, Detail.Length: > 0 } => L.F($"sign-in expired, sign in again ({state.Detail})"),
            { State: SignIn.Expired } => L.T("sign-in expired, sign in again"),
            _ => L.T("could not be checked"),
        };
    }
}
