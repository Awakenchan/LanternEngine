using System.IO;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Settings players share: the game and engine options from lantern.ini and the launcher's
    /// game options, as a code to paste in a chat (deflated JSON in base64) or a .json file. What
    /// belongs to this PC or this player stays out: display and HDR, the online account and
    /// servers, folders, languages. Keys are allowlisted both ways, so an imported code can set
    /// nothing else (no command, folder or environment variable).
    /// </summary>
    public static class SharedSettings
    {
        public const string CodePrefix = "BBPC1:";
        private const int MaxChars = 64 * 1024;

        private static readonly string[] IniKeys =
        {
            "upscaler", "preset", "sharpen", "sharpness", "show_fps", "show_fps_rate", "show_fps_frame_time", "show_fps_upscaler", "live_resolution", "model_lod", "object_motion",
            "frame_gen", "frame_gen_even_pacing",
            "jitter", "reactive", "widescreen", "pc_controls", "camera_controls", "graphics_controls",
            "change_appearance", "rebirth", "skip_intro",
        };
        private static readonly string[] IniPrefixes =
            { "graphics_", "camera_", "effect_", "reactive_", "fsr4_", "cheat_", "bind.", "mouse_", "pad_" };
        private static readonly string[] LauncherKeys = { "present_mode", "fps_mode", "draw_pipe", "readbacks", "fsr4_balanced" };
        private static readonly string[] LauncherPrefixes = { "exp_", "cheat_" };

        /// <summary>What a setting is about: an import can leave the controls and cheats as they are.</summary>
        public enum Part { Settings, Controls, Cheats }

        public sealed record Snapshot(SortedDictionary<string, string> Ini, SortedDictionary<string, JsonNode> Launcher, string From);

        /// <summary>A setting an import changes (Launcher: the launcher's, else lantern.ini's); New is null
        /// for a binding that goes back to its default.</summary>
        public sealed record Change(string Key, string? Old, string? New, Part Part, bool Launcher = false);

        public static Part PartOf(string key) =>
            key.StartsWith("cheat_", StringComparison.Ordinal) ? Part.Cheats
            : key.StartsWith("bind.", StringComparison.Ordinal) || key.StartsWith("mouse_", StringComparison.Ordinal) ||
              key.StartsWith("pad_", StringComparison.Ordinal) || key == "pc_controls" ? Part.Controls
            : Part.Settings;

        private static bool Shared(string key, string[] keys, string[] prefixes) =>
            keys.Contains(key) || prefixes.Any(p => key.StartsWith(p, StringComparison.Ordinal));

        public static Snapshot Take(IniFile ini, LauncherSettings s)
        {
            var values = new SortedDictionary<string, string>(StringComparer.Ordinal);
            foreach (var (key, value) in ini.Values)
                if (Shared(key, IniKeys, IniPrefixes))
                    values[key] = value;
            var launcher = new SortedDictionary<string, JsonNode>(StringComparer.Ordinal);
            foreach (var (key, value) in s.All)
                if (value is JsonValue && Shared(key, LauncherKeys, LauncherPrefixes))
                    launcher[key] = value.DeepClone();
            return new Snapshot(values, launcher, Updater.CurrentLabel);
        }

        public static string ToJson(Snapshot snapshot, bool indented = true)
        {
            var ini = new JsonObject();
            foreach (var (key, value) in snapshot.Ini)
                ini[key] = value;
            var launcher = new JsonObject();
            foreach (var (key, value) in snapshot.Launcher)
                launcher[key] = value.DeepClone();
            var root = new JsonObject { ["bbpc_settings"] = 1, ["from"] = snapshot.From, ["ini"] = ini, ["launcher"] = launcher };
            return root.ToJsonString(new JsonSerializerOptions { WriteIndented = indented });
        }

        public static string ToCode(Snapshot snapshot)
        {
            using var packed = new MemoryStream();
            using (var deflate = new DeflateStream(packed, CompressionLevel.SmallestSize))
                deflate.Write(Encoding.UTF8.GetBytes(ToJson(snapshot, indented: false)));
            return CodePrefix + Convert.ToBase64String(packed.ToArray()).TrimEnd('=').Replace('+', '-').Replace('/', '_');
        }

        /// <summary>A code (also inside a longer message) or the JSON of a settings file. Throws
        /// FormatException with a message for the player.</summary>
        public static Snapshot Parse(string text)
        {
            if (text.Length > MaxChars * 2)
                throw new FormatException(L.T("That is far too long to be shared settings."));
            var code = Regex.Match(text, Regex.Escape(CodePrefix) + @"\s*([A-Za-z0-9_\-]+)", RegexOptions.IgnoreCase);
            var json = code.Success ? Unpack(code.Groups[1].Value) : text.Trim();
            if (!json.StartsWith('{'))
                throw new FormatException(L.F($"That is not a settings code. Codes start with {CodePrefix}"));

            JsonObject root;
            try
            {
                root = JsonNode.Parse(json) as JsonObject ?? throw new FormatException("");
            }
            catch (Exception e) when (e is JsonException or FormatException)
            {
                throw new FormatException(L.T("The settings are damaged (copy the whole code)."));
            }
            if (root["bbpc_settings"] is not JsonValue version || !version.TryGetValue<int>(out var v) || v != 1)
                throw new FormatException("These are not Lantern Engine settings, or they come from a newer launcher.");

            var ini = new SortedDictionary<string, string>(StringComparer.Ordinal);
            if (root["ini"] is JsonObject iniObject)
                foreach (var (key, value) in iniObject)
                    if (ValidKey(key) && Shared(key, IniKeys, IniPrefixes) && value is JsonValue jv &&
                        jv.TryGetValue<string>(out var s) && s.Length <= 256 && !s.Any(char.IsControl))
                        ini[key] = s.Trim();
            var launcher = new SortedDictionary<string, JsonNode>(StringComparer.Ordinal);
            if (root["launcher"] is JsonObject launcherObject)
                foreach (var (key, value) in launcherObject)
                    if (ValidKey(key) && Shared(key, LauncherKeys, LauncherPrefixes) && value is JsonValue lv &&
                        (lv.TryGetValue<bool>(out _) || lv.TryGetValue<double>(out _) ||
                         lv.TryGetValue<string>(out var ls) && ls.Length <= 256 && !ls.Any(char.IsControl)))
                        launcher[key] = lv.DeepClone();
            if (ini.Count == 0 && launcher.Count == 0)
                throw new FormatException(L.T("These settings are empty."));
            var from = root["from"] is JsonValue f && f.TryGetValue<string>(out var label) && label.Length <= 40 ? label : L.T("another launcher");
            return new Snapshot(ini, launcher, from);
        }

        private static bool ValidKey(string key) => key.Length is > 0 and <= 64 && key.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_' or '.');

        private static string Unpack(string code)
        {
            byte[] packed;
            try
            {
                var base64 = code.Replace('-', '+').Replace('_', '/');
                packed = Convert.FromBase64String(base64.PadRight(base64.Length + (4 - base64.Length % 4) % 4, '='));
            }
            catch (FormatException)
            {
                throw new FormatException(L.T("The code is damaged (copy the whole code)."));
            }
            try
            {
                using var deflate = new DeflateStream(new MemoryStream(packed), CompressionMode.Decompress);
                var buffer = new byte[MaxChars + 1];
                var read = 0;
                int n;
                while (read < buffer.Length && (n = deflate.Read(buffer, read, buffer.Length - read)) > 0)
                    read += n;
                if (read > MaxChars)
                    throw new FormatException(L.T("That code is far too large to be shared settings."));
                return Encoding.UTF8.GetString(buffer, 0, read);
            }
            catch (InvalidDataException)
            {
                throw new FormatException(L.T("The code is damaged (copy the whole code)."));
            }
        }

        /// <summary>What importing `parts` of `shared` would change here.</summary>
        public static List<Change> Changes(Snapshot shared, IniFile ini, LauncherSettings s, IReadOnlySet<Part> parts)
        {
            var changes = new List<Change>();
            foreach (var (key, value) in shared.Ini)
                if (parts.Contains(PartOf(key)) && ini.Raw(key) != value)
                    changes.Add(new Change(key, ini.Raw(key), value, PartOf(key)));
            // Bindings left at their default are absent: one set here goes back to it.
            if (parts.Contains(Part.Controls))
                foreach (var (key, value) in ini.Values)
                    if (key.StartsWith("bind.", StringComparison.Ordinal) && !shared.Ini.ContainsKey(key))
                        changes.Add(new Change(key, value, null, Part.Controls));
            var current = s.All.ToDictionary(kv => kv.Key, kv => kv.Value);
            foreach (var (key, value) in shared.Launcher)
            {
                var old = current.GetValueOrDefault(key);
                if (parts.Contains(PartOf(key)) && !JsonNode.DeepEquals(old, value))
                    changes.Add(new Change(key, Plain(old), Plain(value), PartOf(key), Launcher: true));
            }
            return changes;
        }

        public static void Apply(Snapshot shared, IniFile ini, LauncherSettings s, IReadOnlySet<Part> parts)
        {
            foreach (var change in Changes(shared, ini, s, parts))
            {
                if (change.Launcher)
                    s.SetNode(change.Key, shared.Launcher[change.Key]);
                else
                    ini.Set(change.Key, change.New);
            }
        }

        private static string? Plain(JsonNode? node) =>
            node is JsonValue v && v.TryGetValue<string>(out var s) ? s : node?.ToJsonString();

        /// <summary>One line for the import summary: "bind.roll: Space → Left Shift".</summary>
        public static string Describe(Change change) =>
            $"{change.Key}: {change.Old ?? L.T("default")} → {change.New ?? L.T("default")}";
    }
}
