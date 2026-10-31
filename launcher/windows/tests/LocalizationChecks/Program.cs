using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows.Input;
using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

// No App/Workspace instance, settings.Load/Save, game process or network access.
Require(Localization.Language == "en" && Localization.T("Settings") == "Settings", "English must be the default");
Localization.Initialize("unknown");
Require(Localization.Language == "en" && Localization.T("Settings") == "Settings", "Unknown languages must fall back to English");
var language = args.FirstOrDefault(a => !a.StartsWith("--")) ?? "en";
Require(Localization.Languages.Any(l => l.Code == language), "Not a launcher language: " + language);
Localization.Initialize(language);
var selected = Localization.Catalog(language);
Require(Localization.Language == language, "Language selection failed");
Require(Localization.T("Settings") == (selected?.GetValueOrDefault("Settings") ?? "Settings"), "Language selection failed");
Require(Localization.T("untranslated fallback") == "untranslated fallback", "Missing-key fallback failed");

static void CheckGameLanguages()
{
    Require(Choices.GameLanguages.Select(c => c.Value).SequenceEqual(new[] { "1", "8", "0", "2", "3", "4", "5", "17" }), "Game language values changed");
    var brazilian = Choices.GameLanguages.Single(c => c.Value == "17");
    Require(brazilian.Label == "Português (Brasil)", "Brazilian Portuguese must retain its native name");
    var settings = new LauncherSettings();
    settings.Set("language", "1");
    Require(settings.Str("language") == "1", "English game language settings changed");
    settings.Set("language", brazilian.Value);
    Require(settings.Str("language") == "17", "Game language settings lost the PS4 language code");
}

if (args.Contains("--game-languages-only"))
{
    CheckGameLanguages();
    Console.WriteLine($"PASS {language}: game language choices, Brazilian Portuguese label and settings value; no game started.");
    return;
}

// Every language file, whichever language this run selected: the list and the embedded files agree,
// each parses, holds only strings zh-CN (the reference for the code's strings) has, and keeps their
// placeholders and file-dialog patterns.
var assembly = typeof(Localization).Assembly;
const string prefix = "BloodborneLauncher.Localization.";
var embedded = assembly.GetManifestResourceNames().Where(n => n.StartsWith(prefix) && n.EndsWith(".json"))
    .Select(n => n[prefix.Length..^".json".Length]).Order(StringComparer.Ordinal).ToArray();
var listed = Localization.Languages.Select(l => l.Code).Where(c => c != "en").Order(StringComparer.Ordinal).ToArray();
Require(embedded.SequenceEqual(listed), $"Language list ({string.Join(", ", listed)}) and embedded files ({string.Join(", ", embedded)}) differ");
Require(Localization.Languages.Select(l => l.Code).Distinct().Count() == Localization.Languages.Count, "Duplicate language code");
var reference = Localization.Catalog("zh-CN")!;
// Format slots ({0}, {0:HH:mm:ss}) and the named tokens the code replaces itself ({version}).
static string[] Slots(string text) => Regex.Matches(text, @"(?<!\{)\{(?:\d+(?:,[^}:]+)?(?::[^}]+)?|[A-Za-z_]\w*)\}(?!\})")
    .Select(m => m.Value).Order(StringComparer.Ordinal).ToArray();
var coverage = new List<string>();
foreach (var code in listed)
{
    IReadOnlyDictionary<string, string> catalog;
    try { catalog = Localization.Catalog(code)!; }
    catch (JsonException e) { throw new InvalidOperationException($"{code}.json does not parse: {e.Message}"); }
    foreach (var (english, translation) in catalog)
    {
        Require(reference.ContainsKey(english), $"{code}: string not in zh-CN (renamed or removed from the code?): {english}");
        Require(Slots(english).SequenceEqual(Slots(translation)), $"{code}: changed format placeholders: {english}");
        Require(!string.IsNullOrWhiteSpace(translation), $"{code}: empty translation: {english}");
        if (english.Contains('|'))
        {
            // File-dialog filters must retain their patterns, regardless of their display names.
            var original = english.Split('|');
            var localized = translation.Split('|');
            Require(original.Length == localized.Length, $"{code}: changed filter structure: {english}");
            for (var i = 1; i < original.Length; i += 2)
                Require(original[i] == localized[i], $"{code}: changed filter pattern: {english}");
        }
    }
    coverage.Add($"{code} {catalog.Count}/{reference.Count}");
}
CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("de-DE");
var value = 12.5;
Require(Localization.F($"Unknown format {value:0.0}") == "Unknown format 12,5", "Formatting must retain the current culture");
Require(Localization.F($"There is already a profile called {"测试玩家"}.").Contains("测试玩家"), "Dynamic Unicode text was lost");

Require(Choices.MenuLanguages.Count(c => c.Value == "zh_CN") == 1, "Chinese menu code missing or duplicated");
CheckGameLanguages();
Require(InputMap.SdlKeyName(Key.Left) == "Left" && InputMap.SdlKeyName(Key.Return) == "Return", "Keyboard protocol names were translated");
Require(InputMap.Controls.Single(c => c.Name == "switch_right").Default == "Right", "Default keyboard binding was translated");
Require(PadMap.InputFor(InputMap.Controls.Single(c => c.Name == "switch_item")) == "down", "Controller lookup labels changed");
foreach (var profile in ControlProfiles.BuiltIns)
{
    var roundtrip = ControlProfiles.Parse(ControlProfiles.ToJson(profile)).Profile;
    Require(roundtrip.Name == profile.Name, "Profile name changed on round-trip");
    Require(roundtrip.Bindings.OrderBy(x => x.Key).SequenceEqual(profile.Bindings.OrderBy(x => x.Key)), "Bindings changed on round-trip");
}
Require(PlayerProfiles.Main == "Main", "Saved main-profile identifier changed");
// Segmented labels drop their bracketed detail however a language writes the brackets.
Require(BloodborneLauncher.Views.Segmented.Short("Quality (x1.5)") == "Quality" &&
        BloodborneLauncher.Views.Segmented.Short("品質（x1.5）") == "品質" &&
        BloodborneLauncher.Views.Segmented.Short("울트라 성능(x3)") == "울트라 성능" &&
        BloodborneLauncher.Views.Segmented.Short("Off") == "Off", "Segmented labels keep their bracketed detail");
Console.WriteLine($"PASS {language}: translations ({string.Join(", ", coverage)}), placeholders, filters, language codes and input/profile round-trips; no game started.");
