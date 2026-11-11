// SPDX-License-Identifier: GPL-3.0-or-later
using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

Require(OnlineStatus.RoomFull("This room is full (5 of 5 hunters)") == "This room is full (5 of 5 hunters)", "Plain room full line");
Require(OnlineStatus.RoomFull("Seamless: refused: This room is full (5 of 5 hunters)") == "This room is full (5 of 5 hunters)", "Prefixed room full line");
Require(OnlineStatus.RoomFull("bb-start: This room is full (4 of 4 hunters).") == "This room is full (4 of 4 hunters)", "Start program reason");
Require(OnlineStatus.RoomFull("This room is full") is null, "Room full without counts");
Require(OnlineStatus.RoomFull("Seamless: joined the room (3 of 5 hunters)") is null, "Not a refusal");
Require(OnlineStatus.RoomFull(new[] { "Engine: start", "Seamless: This room is full (5 of 5 hunters)", "bb-probe exited with code 23" })
    == "This room is full (5 of 5 hunters)", "Room full in a log tail");
Require(OnlineStatus.RoomFull(new[] { "Engine: start", "bb-probe exited with code 0" }) is null, "No room full in a log tail");

var session = new SessionStats();
session.Observe("Seamless: This room is full (5 of 5 hunters)");
Require(session.RoomFull == "This room is full (5 of 5 hunters)", "Session sees the refusal");
Require(new SessionStats().RoomFull is null, "Fresh session has no refusal");

Require(OnlineStatus.RulesLine(Array.Empty<string>()) == "Online rules: vanilla", "Vanilla rules line");
Require(OnlineStatus.RulesLine(new[] { "Bloodborne.xml/Unlock Game Region", "Bloodborne.xml/Player No Dead (Read note)" }) == "Online rules: vanilla", "Other patches are not rules");
Require(OnlineStatus.RulesLine(new[] { "Bloodborne.xml/Lamp2Lamp (warp between lamps)" }) == "Online rules: Lamp2Lamp warps", "Lamp2Lamp rules line");
Require(OnlineStatus.RulesLine(new[] { "Bloodborne.xml/Lamp2Lamp (warp between lamps)", "Bloodborne.xml/Disable Rally (HP Regain)" })
    == "Online rules: Lamp2Lamp warps", "Rally patches stay vanilla, as in bbhost");
Require(OnlineStatus.RulesLine(new[] { "Bloodborne.xml/No Rally Decay", "Bloodborne.xml/DS1-like physics", "Bloodborne.xml/Lamp2Lamp: warp to bosses" })
    == "Online rules: Lamp2Lamp warps to bosses", "DS1-like physics and rally stay vanilla");
Require(OnlineStatus.RulesLine(new[] { "Bloodborne.xml/Lamp2Lamp (warp between lamps)", "Bloodborne.xml/Lamp2Lamp: warp to bosses", "Bloodborne.xml/Lamp2Lamp (warp between lamps)" })
    == "Online rules: Lamp2Lamp warps, Lamp2Lamp warps to bosses", "No duplicates");
Require(OnlineStatus.RulesLine(new[] { "Lamp2Lamp (warp between lamps)", "Other.xml/No Rally Decay" }) == "Online rules: vanilla", "Only Bloodborne.xml keys");
Require(OnlineStatus.RulesLine() == "Online rules: vanilla", "Missing patches.json");
Require(OnlineStatus.RulesNote.Contains("fewer bloodstains and messages from vanilla players"), "Rules note");
var profile = OnlineStatus.EnabledPatches("{\"enabled\":[\"Bloodborne.xml/No Rally Decay\",\"Bloodborne.xml/DS1-like physics\",5,null],\"disabled\":[]}");
Require(profile.SequenceEqual(new[] { "Bloodborne.xml/No Rally Decay", "Bloodborne.xml/DS1-like physics" }), "patches.json enabled list in order");
Require(OnlineStatus.EnabledPatches("not json").Count == 0 && OnlineStatus.EnabledPatches("{}").Count == 0, "Broken patches.json");

foreach (var name in new[] { "Player No Dead (Read note)", "Player Stealth (Read note)", "Player Silent (Read note)", "Enemy Control", "Restore Debug Menu (READ NOTES)" })
    Require(OnlineStatus.CheatLike("Bloodborne.xml/" + name), "Cheat-like: " + name);
Require(!OnlineStatus.CheatLike("Bloodborne.xml/Lamp2Lamp (warp between lamps)") && !OnlineStatus.CheatLike("Bloodborne.xml/No Rally Decay"), "Rules are not cheat-like");
Require(!OnlineStatus.CheatLike("Mine.xml/Enemy Control"), "Cheat-like only in Bloodborne.xml");
Require(OnlineStatus.CheatLikeNote == "Off while Play online or seamless co-op is on", "Cheat-like note");

var valid = OnlineStatus.ClassifyWhoAmI("{\"ResKind\":0,\"OnlineId\":\"Hunter_1\"}");
Require(valid.State == OnlineStatus.SignIn.Valid && valid.Detail == "Hunter_1", "Valid token");
Require(OnlineStatus.SignInLine(valid) == "signed in as Hunter_1", "Valid line");
var expired = OnlineStatus.ClassifyWhoAmI("{\"ResKind\":3,\"Message\":\"token expired\"}");
Require(expired.State == OnlineStatus.SignIn.Expired && expired.Detail == "token expired", "Expired token");
Require(OnlineStatus.SignInLine(expired) == "sign-in expired, sign in again (token expired)", "Expired line");
var bare = OnlineStatus.ClassifyWhoAmI("{\"ResKind\":1}");
Require(bare.State == OnlineStatus.SignIn.Expired && OnlineStatus.SignInLine(bare) == "sign-in expired, sign in again", "Expired without message");
Require(OnlineStatus.ClassifyWhoAmI("{\"ResKind\":0}").State == OnlineStatus.SignIn.Unknown, "Valid without id");
Require(OnlineStatus.ClassifyWhoAmI("<html>502 Bad Gateway</html>").State == OnlineStatus.SignIn.Unknown, "Not JSON");
Require(OnlineStatus.ClassifyWhoAmI("{\"error\":\"x\"}").State == OnlineStatus.SignIn.Unknown, "No ResKind");
Require(OnlineStatus.ClassifyWhoAmI("{\"ResKind\":\"0\"}").State == OnlineStatus.SignIn.Unknown, "ResKind as text");
Require(OnlineStatus.ClassifyWhoAmI(null).State == OnlineStatus.SignIn.Unknown, "No reply");
Require(OnlineStatus.SignInLine(new OnlineStatus.SignInState(OnlineStatus.SignIn.Unknown, "")) == "could not be checked", "Unknown line");
Require(OnlineStatus.SignInLine(null) == "checking…", "Checking line");

Console.WriteLine("PASS: room full refusal, online rules line, whoami classification");
