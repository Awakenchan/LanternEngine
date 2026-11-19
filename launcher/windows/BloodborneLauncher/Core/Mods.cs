using System.Globalization;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Xml.Linq;

namespace BloodborneLauncher.Core
{
    /// <summary>Mod folders and their load order (scripts/mods.py; profile in mods.json).</summary>
    public static class Mods
    {
        private static readonly HashSet<string> GameFolders = new(StringComparer.OrdinalIgnoreCase)
        {
            "action", "adhoc", "chr", "event", "facegen", "font", "map", "menu", "movie", "msg", "mtd",
            "obj", "other", "param", "paramdef", "parts", "remo", "script", "sfx", "shader", "sound",
        };

        private static readonly string[] Serials =
        {
            "CUSA03173", "CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
            "CUSA03179", "CUSA03014", "CUSA03023",
        };

        private static readonly string[] LooseFileTypes = { ".txt", ".md", ".jpg", ".png", ".ini", ".toml" };

        public static string ProfilePath => Path.Combine(Paths.DataDir, "mods.json");

        /// <summary>Folders of `root` with an accepted layout, alphabetically.</summary>
        public static List<string> Discover(string root)
        {
            if (!Directory.Exists(root))
                return new List<string>();
            return Directory.GetDirectories(root)
                .Where(HasContent)
                .Select(Path.GetFileName)
                .OfType<string>()
                .OrderBy(n => n, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }

        private static bool HasContent(string folder)
        {
            try
            {
                return ContentRoot(folder);
            }
            catch
            {
                return false;
            }
        }

        private static string Child(string folder, string name) =>
            Directory.Exists(folder)
                ? Directory.EnumerateFileSystemEntries(folder)
                      .FirstOrDefault(e => string.Equals(Path.GetFileName(e), name, StringComparison.OrdinalIgnoreCase))
                  ?? Path.Combine(folder, name)
                : Path.Combine(folder, name);

        private static bool ContentRoot(string folder)
        {
            if (!Directory.Exists(folder))
                return false;
            foreach (var wrapper in new[] { "", "app0" }.Concat(Serials))
            {
                var baseDir = wrapper.Length > 0 ? Child(folder, wrapper) : folder;
                if (Directory.Exists(Child(baseDir, "dvdroot_ps4")))
                    return true;
            }

            var entries = Directory.EnumerateFileSystemEntries(folder)
                .Where(e => !Path.GetFileName(e).StartsWith('.')).ToList();
            var folders = entries.Where(Directory.Exists).ToList();
            if (folders.Count > 0 && folders.All(f => GameFolders.Contains(Path.GetFileName(f))))
                return true;
            if (folders.Count == 1 && !entries.Any(e => File.Exists(e) &&
                    !LooseFileTypes.Contains(Path.GetExtension(e), StringComparer.OrdinalIgnoreCase)))
                return ContentRoot(folders[0]);
            return false;
        }

        public sealed record Entry(string Name, bool Enabled);

        /// <summary>The mods in load order: the saved order first, new folders after, enabled.</summary>
        public static List<Entry> Load(string root)
        {
            var available = Discover(root);
            var order = new List<string>();
            var disabled = new HashSet<string>();
            var read = SettingsText.Read(ProfilePath);
            var (profile, writable) = Parse(read, ProfilePath);
            Volatile.Write(ref _loadedUnreadable, read.State == TextState.Unreadable || !writable);
            if (profile != null)
            {
                order = Names(profile["order"]).ToList();
                disabled = Names(profile["disabled"]).ToHashSet();
            }

            return order.Concat(available).Distinct().Where(available.Contains)
                .Select(n => new Entry(n, !disabled.Contains(n))).ToList();
        }

        public static SaveOutcome Save(IEnumerable<Entry> mods)
        {
            var list = mods.ToList();
            var profile = new JsonObject
            {
                ["order"] = new JsonArray(list.Select(m => (JsonNode?)m.Name).ToArray()),
                ["disabled"] = new JsonArray(list.Where(m => !m.Enabled).Select(m => (JsonNode?)m.Name).ToArray()),
            };
            if (Volatile.Read(ref _loadedUnreadable)) throw new SettingsUnreadableException(ProfilePath);
            return SettingsText.Update(ProfilePath, _ => profile.ToJsonString(Indented) + "\n");
        }

        private static bool _loadedUnreadable;

        internal static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

        internal static (JsonObject? Profile, bool Writable) Parse(TextRead read, string path)
        {
            if (read.State != TextState.Read) return (null, true);
            string why;
            try
            {
                if (JsonNode.Parse(read.Text) is JsonObject profile) return (profile, true);
                why = "not a JSON object";
            }
            catch (JsonException e)
            {
                why = e.Message;
            }
            return (null, KeepBroken(path, read.Text, why));
        }

        public static string BrokenOf(string path) => path + ".broken";

        private static bool KeepBroken(string path, string text, string why)
        {
            try
            {
                SettingsText.Replace(BrokenOf(path), text);
                Trace.TraceWarning($"{Path.GetFileName(path)} could not be parsed ({why}); kept as {Path.GetFileName(BrokenOf(path))}");
                return true;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Trace.TraceWarning($"{Path.GetFileName(path)} could not be parsed ({why}) and its copy failed ({e.Message}); it is not written over");
                return false;
            }
        }

        internal static IEnumerable<string> Names(JsonNode? node) =>
            node is JsonArray array
                ? array.Select(n => n is JsonValue value && value.TryGetValue<string>(out var name) ? name : "")
                : Enumerable.Empty<string>();
    }

    /// <summary>Third-party shadPS4 XML patches for 01.09 (scripts/patches.py; profile in patches.json).</summary>
    public static class Patches
    {
        public sealed record Patch(string Key, string Name, string File, string Author, string Note, bool DefaultOn,
            bool ChangesRegion = false, bool SetsResolution = false);

        // The game's region function (scripts/patches.py REGION_FUNCTION, as patch addresses).
        // "Unlock Game Region" makes it return a region with no Bloodborne server.
        private const long RegionStart = 0x23b9190, RegionEnd = 0x23b91e3;

        // The bytes a line writes (patch addresses), or null when it can't be read.
        private static (long Start, long End)? Range(XElement line)
        {
            var address = ((string?)line.Attribute("Address") ?? "").Trim();
            if (address.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                address = address[2..];
            if (!long.TryParse(address, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var at))
                return null;
            var value = ((string?)line.Attribute("Value") ?? "").Trim();
            var length = ((string?)line.Attribute("Type") ?? "bytes") switch
            {
                "bytes16" => 2,
                "bytes32" or "float32" => 4,
                "bytes64" or "float64" => 8,
                _ => Math.Max(1, value.Replace(" ", "").Length / 2),
            };
            return (at, at + length);
        }

        private static bool WritesRegion(XElement line) =>
            Range(line) is { } r && r.Start < RegionEnd && RegionStart < r.End;

        // The game's resolution and UI layout: the sites every community "Resolution Patch"
        // writes, read from the built-in one the engine uses as its template (scripts/patches.py
        // RESOLUTION_TEMPLATE).
        private const string ResolutionTemplate = "Resolution Patch 1280x720 (16:9)";

        private static List<(long Start, long End)> ResolutionFootprint()
        {
            try
            {
                var xml = XDocument.Load(Path.Combine(Paths.EngineDir, "patches", "Bloodborne.xml"));
                var template = xml.Descendants("Metadata").FirstOrDefault(m => (string?)m.Attribute("Name") == ResolutionTemplate);
                return template?.Descendants("Line").Select(Range).Where(r => r.HasValue).Select(r => r!.Value).ToList() ?? new();
            }
            catch
            {
                return new();
            }
        }

        /// <summary>The enabled patches that set the game's resolution and UI layout: while the
        /// output resolution isn't 1920 x 1080 it does that itself, and the patch would keep its
        /// own shape on screen (black bars) and stop the widescreen hooks.</summary>
        public static List<Patch> ResolutionSetting(string directory)
        {
            var profile = LoadProfile();
            return Discover(directory).Where(p => p.SetsResolution && IsOn(p, profile)).ToList();
        }

        /// <summary>The enabled patches that change the game's region: with one on, Play online
        /// fails ("Failure to acquire information from the Bloodborne server").</summary>
        public static List<Patch> RegionChanging(string directory)
        {
            var profile = LoadProfile();
            return Discover(directory).Where(p => p.ChangesRegion && IsOn(p, profile)).ToList();
        }

        public static string ProfilePath => Path.Combine(Paths.DataDir, "patches.json");

        /// <summary>"1.09" is "01.09" (patch files write either; scripts/patches.py same_app_version).</summary>
        private static bool IsSupportedVersion(string? value)
        {
            var parts = (value ?? "").Trim().Split('.');
            return parts.Length == 2 && int.TryParse(parts[0], out var major) && int.TryParse(parts[1], out var minor) &&
                   major == 1 && minor == 9;
        }

        /// <summary>Built-in patches the port sets from its own settings or no longer needs: not
        /// offered (scripts/patches.py managed_builtin, the same list).</summary>
        public static bool ManagedBuiltIn(string name) =>
            name.Contains("FPS++", StringComparison.Ordinal) || name.StartsWith("Resolution Patch", StringComparison.Ordinal) ||
            name.StartsWith("Model LOD", StringComparison.Ordinal) || ManagedNames.Contains(name);

        private static readonly HashSet<string> ManagedNames = new(StringComparer.Ordinal)
        {
            "Sprint Fix (High FPS)", "60FPS (no deltatime)", "Intel Black Tonemap Fix",
            "Intel 12th Gen+ SFX workaround", "Disable Chromatic Aberration", "Disable DoF", "Disable Motion Blur (perf increase)",
            "Disable SSAO", "Disable AA", "Disable Dynamic Light Shadows (perf increase)", "Enable Screen Space Reflections (READ NOTE)",
            "Skip Intro", "Restore Debug Camera", "Restore Debug Menu (READ NOTES)", "Increased Graphics Heap Sizes",
        };

        public static List<Patch> Discover(string directory)
        {
            var found = new List<Patch>();
            // The player's patches folder (removed by 0.9.30's update cleanup, GitHub issue 44): made
            // again; the built-in patches are listed either way.
            try
            {
                Directory.CreateDirectory(directory);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
            }

            var builtIn = Path.GetFullPath(Path.Combine(Paths.EngineDir, "patches", "Bloodborne.xml"));
            var footprint = ResolutionFootprint();
            // The built-in file first (it lives in the package's patches folder; updates replace it):
            // its optional patches, off until switched on here.
            // A copy of Bloodborne.xml in another patches folder (an older package's) is never listed: it would
            // apply its frame-rate patches over the port's own (scripts/patches.py does the same).
            var files = (Directory.Exists(directory) ? Directory.GetFiles(directory, "*.xml").Concat(Directory.GetFiles(directory, "*.toml")) : Array.Empty<string>()).Where(p => !string.Equals(Path.GetFileName(p), Path.GetFileName(builtIn), StringComparison.OrdinalIgnoreCase));
            if (File.Exists(builtIn)) files = files.Prepend(builtIn);
            foreach (var path in files.Distinct().ToList())
            {
                var isBuiltIn = string.Equals(Path.GetFullPath(path), builtIn, StringComparison.OrdinalIgnoreCase);
                XElement root;
                try
                {
                    root = path.EndsWith(".toml", StringComparison.OrdinalIgnoreCase)
                        ? TomlPatch.AsXml(File.ReadAllText(path), Path.GetFileNameWithoutExtension(path))
                        : XDocument.Load(path).Root!;
                }
                catch
                {
                    continue;
                }

                var ids = root.Descendants("ID").Select(e => e.Value.Trim()).Where(v => v.Length > 0).ToHashSet();
                if (ids.Count > 0 && !ids.Overlaps(GameCheck.SupportedTitles))
                    continue;

                foreach (var meta in root.Descendants("Metadata"))
                {
                    if (!IsSupportedVersion((string?)meta.Attribute("AppVer")) ||
                        ((string?)meta.Attribute("AppElf") ?? "eboot.bin") != "eboot.bin")
                        continue;
                    var name = (string?)meta.Attribute("Name") ?? "";
                    if (isBuiltIn && ManagedBuiltIn(name))
                        continue;
                    var file = Path.GetFileName(path);
                    found.Add(new Patch($"{file}/{name}", name.Length > 0 ? name : $"{file}/{name}", file,
                        (string?)meta.Attribute("Author") ?? "",
                        ((string?)meta.Attribute("Note") ?? "").Replace("\\n", "\n"),
                        !isBuiltIn && string.Equals((string?)meta.Attribute("isEnabled"), "true", StringComparison.OrdinalIgnoreCase),
                        meta.Descendants("Line").Any(WritesRegion),
                        meta.Descendants("Line").Select(Range).Any(r => r is { } w &&
                            footprint.Any(f => f.Start < w.End && w.Start < f.End))));
                }
            }
            return found;
        }

        private static readonly Dictionary<string, string> RenamedBuiltIns = new(StringComparer.Ordinal)
        {
            ["Bloodborne.xml/All languages (region unlock)"] = "Bloodborne.xml/Unlock Game Region",
            ["Bloodborne.xml/Camera: no auto-turn"] = "Bloodborne.xml/Disable Camera Auto Rotation via Movement",
            ["Bloodborne.xml/Can't die (HP stops at 1)"] = "Bloodborne.xml/Player No Dead (Read note)",
        };

        private static bool Rename(HashSet<string> keys)
        {
            var old = keys.Where(RenamedBuiltIns.ContainsKey).ToList();
            foreach (var key in old)
            {
                keys.Remove(key);
                keys.Add(RenamedBuiltIns[key]);
            }
            return old.Count > 0;
        }

        public static (HashSet<string> Enabled, HashSet<string> Disabled) LoadProfile()
        {
            var read = SettingsText.Read(ProfilePath);
            var (enabled, disabled, writable) = Profile(read);
            Volatile.Write(ref _loadedUnreadable, read.State == TextState.Unreadable || !writable);
            if (writable && read.State != TextState.Unreadable && (Rename(enabled) | Rename(disabled)))
            {
                try
                {
                    SettingsText.Update(ProfilePath, before =>
                    {
                        var (savedEnabled, savedDisabled, _) = Writable(Profile(before));
                        Rename(savedEnabled);
                        Rename(savedDisabled);
                        return Text(savedEnabled, savedDisabled);
                    });
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    Trace.TraceWarning($"{Path.GetFileName(ProfilePath)}: renamed patches not written ({e.Message}); the next save tries again");
                }
            }
            return (enabled, disabled);
        }

        private static (HashSet<string> Enabled, HashSet<string> Disabled, bool Writable) Profile(TextRead read)
        {
            var (profile, writable) = Mods.Parse(read, ProfilePath);
            return profile != null
                ? (Mods.Names(profile["enabled"]).ToHashSet(), Mods.Names(profile["disabled"]).ToHashSet(), writable)
                : (new HashSet<string>(), new HashSet<string>(), writable);
        }

        private static (HashSet<string> Enabled, HashSet<string> Disabled, bool Writable) Writable(
            (HashSet<string> Enabled, HashSet<string> Disabled, bool Writable) profile) =>
            profile.Writable ? profile : throw new SettingsUnreadableException(ProfilePath);

        public static bool IsOn(Patch patch, (HashSet<string> Enabled, HashSet<string> Disabled) profile) =>
            profile.Enabled.Contains(patch.Key) || (patch.DefaultOn && !profile.Disabled.Contains(patch.Key));

        /// <summary>Records the switches that differ from each file's isEnabled; patches of other
        /// folders keep their saved choice.</summary>
        private static bool _loadedUnreadable;

        public static SaveOutcome Save(IEnumerable<(Patch Patch, bool On)> shown)
        {
            var list = shown.ToList();
            var keys = list.Select(p => p.Patch.Key).ToHashSet();
            if (Volatile.Read(ref _loadedUnreadable)) throw new SettingsUnreadableException(ProfilePath);
            return SettingsText.Update(ProfilePath, before =>
            {
                var (enabled, disabled, _) = Writable(Profile(before));
                Rename(enabled);
                Rename(disabled);
                enabled.RemoveWhere(keys.Contains);
                disabled.RemoveWhere(keys.Contains);
                foreach (var (patch, on) in list)
                {
                    if (on == patch.DefaultOn) continue;
                    (on ? enabled : disabled).Add(patch.Key);
                }
                return Text(enabled, disabled);
            });
        }

        private static string Text(HashSet<string> enabled, HashSet<string> disabled)
        {
            var profile = new JsonObject
            {
                ["enabled"] = new JsonArray(enabled.OrderBy(k => k, StringComparer.Ordinal).Select(k => (JsonNode?)k).ToArray()),
                ["disabled"] = new JsonArray(disabled.OrderBy(k => k, StringComparer.Ordinal).Select(k => (JsonNode?)k).ToArray()),
            };
            return profile.ToJsonString(Mods.Indented) + "\n";
        }
    }
}
