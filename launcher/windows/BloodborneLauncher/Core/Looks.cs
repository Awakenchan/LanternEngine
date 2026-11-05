using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    public static class Looks
    {
        public const string CodePrefix = "LOOK1:";
        public const int Version = 1;
        public const int MaxBytes = 16 * 1024;
        private const int MaxSaved = 200;
        private const int MaxName = 48;
        private const int MaxAuthor = 40;

        public sealed record Knob(string Key, double Min, double Max, string Default, bool Whole);

        public sealed record Look(string Name, string Author, IReadOnlyDictionary<string, string> Values,
            string Description = "", bool BuiltIn = false);

        private static Knob Toggle(string key, bool on) => new(key, 0, 1, on ? "1" : "0", true);
        private static Knob Amount(string key, double min, double max, double value) =>
            new(key, min, max, value.ToString("0.##", CultureInfo.InvariantCulture), false);
        private static Knob Level(string key, double max) => new(key, 0, max, "0", true);

        public static readonly Knob[] Knobs =
        {
            Toggle("graphics_controls", false),
            Toggle("graphics_vignette", true),
            Amount("graphics_brightness", 0, 5, 1),
            Amount("graphics_contrast", 0.5, 1.5, 1),
            Amount("graphics_saturation", 0, 5, 1),
            Amount("graphics_hue", -180, 180, 0),
            Amount("graphics_bloom", 0, 10, 1),
            Amount("graphics_fog", 0, 5, 1),
            Amount("graphics_ao_strength", 0, 5, 1),
            Amount("graphics_shadow_scale", 0.25, 8, 1),
            Toggle("effect_chromatic_aberration", false),
            Toggle("effect_dof", false),
            Toggle("effect_motion_blur", false),
            Toggle("effect_ssao", true),
            Toggle("effect_game_aa", false),
            Toggle("effect_dynamic_shadows", true),
            Toggle("effect_ssr", false),
            Toggle("effect_gtao", false),
            Toggle("effect_modern_ssr", false),
            Toggle("effect_pom_cavity", false),
            Toggle("effect_contact_shadows", false),
            Toggle("effect_pcss", false),
            Toggle("effect_gi", false),
            Toggle("effect_volumetric_light", false),
            Toggle("effect_point_fog", false),
            Toggle("effect_point_shadows", false),
            Toggle("effect_ray_tracing", false),
            Amount("graphics_gi_strength", 0, 12, 6),
            Amount("graphics_ssr_intensity", 0, 3, 1),
            Amount("graphics_volumetric_intensity", 0, 3, 1),
            Amount("graphics_point_fog_intensity", 0, 3, 1),
            Level("graphics_shadow_resolution", 2),
            Level("graphics_local_shadow_resolution", 2),
            Level("graphics_motion_blur_quality", 1),
            Level("graphics_ao_quality", 1),
        };

        private static readonly Dictionary<string, Knob> ByKey = Knobs.ToDictionary(k => k.Key, StringComparer.Ordinal);

        private static Dictionary<string, string> Set(params string[] pairs)
        {
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            for (var i = 0; i + 1 < pairs.Length; i += 2) values[pairs[i]] = pairs[i + 1];
            return values;
        }

        public static IReadOnlyList<Look> Presets { get; } = new[]
        {
            new Look(L.T("Original look"), "Lantern Engine", Set(),
                L.T("The game's own lighting and picture, with none of the extra effects. Your baseline, costs nothing extra."), true),
            new Look(L.T("Performance"), "Lantern Engine",
                Set("graphics_controls", "1", "graphics_shadow_scale", "0.75", "effect_dynamic_shadows", "0", "effect_motion_blur", "0", "effect_ssr", "0"),
                L.T("Turns off shadows from dynamic lights and motion blur and shortens shadow distance, so lamps stop casting moving shadows. How much it saves has not been measured."), true),
            new Look(L.T("Better shadows"), "Lantern Engine",
                Set("effect_contact_shadows", "1", "effect_pcss", "1"),
                L.T("Better ambient occlusion, contact shadows and softer sun shadows, without the extra light. Its cost has not been measured."), true),
            new Look(L.T("BB Remastered"), "Lantern Engine",
                Set("effect_ssr", "1", "effect_modern_ssr", "1", "effect_pom_cavity", "1", "effect_contact_shadows", "1",
                    "effect_pcss", "1", "effect_gi", "1", "effect_volumetric_light", "1", "effect_point_fog", "1",
                    "graphics_shadow_resolution", "1", "graphics_local_shadow_resolution", "1"),
                L.T("Nearly everything on: indirect light, volumetrics, sharper reflections and soft shadows. The heaviest choice; its cost has not been measured, so expect a large drop on older GPUs. An effect your GPU cannot run turns itself off."), true),
        };

        public static string Folder => Path.Combine(Paths.ConfigDir, "looks");
        private static string PreviousFile => Path.Combine(Path.GetDirectoryName(Paths.IniPath)!, "looks-previous.json");

        public static string Current(Knob knob, IniFile ini) => ini.Get(knob.Key, knob.Default);

        public static string ValueIn(Look look, Knob knob) =>
            look.Values.TryGetValue(knob.Key, out var value) ? value : knob.Default;

        public static bool IsCurrent(Look look, IniFile ini) =>
            Knobs.All(knob => Same(knob, ValueIn(look, knob), Current(knob, ini)));

        private static bool Same(Knob knob, string a, string b) =>
            Normal(knob, a) is { } x && Normal(knob, b) is { } y && x == y;

        public static string? Normal(Knob knob, string text)
        {
            if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) || !double.IsFinite(number))
                return null;
            number = Math.Clamp(number, knob.Min, knob.Max);
            return knob.Whole
                ? ((long)Math.Round(number)).ToString(CultureInfo.InvariantCulture)
                : number.ToString("0.##", CultureInfo.InvariantCulture);
        }

        private static string Clean(string? text, int max)
        {
            var kept = new string((text ?? "").Where(c =>
                !char.IsControl(c) && char.GetUnicodeCategory(c) != UnicodeCategory.Format).ToArray()).Trim();
            return kept.Length <= max ? kept : kept[..max].TrimEnd();
        }

        public static Look FromCurrent(IniFile ini, string name, string author)
        {
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var knob in Knobs)
                if (Normal(knob, Current(knob, ini)) is { } value && !Same(knob, value, knob.Default))
                    values[knob.Key] = value;
            return new Look(Clean(name, MaxName), Clean(author, MaxAuthor), values);
        }

        public static string ToJson(Look look, bool indented = true)
        {
            var values = new JsonObject();
            foreach (var knob in Knobs)
                if (look.Values.TryGetValue(knob.Key, out var value))
                    values[knob.Key] = value;
            var root = new JsonObject { ["lantern_look"] = Version, ["name"] = look.Name, ["author"] = look.Author, ["values"] = values };
            return root.ToJsonString(new JsonSerializerOptions { WriteIndented = indented });
        }

        public static string ToCode(Look look)
        {
            using var packed = new MemoryStream();
            using (var deflate = new DeflateStream(packed, CompressionLevel.SmallestSize))
                deflate.Write(Encoding.UTF8.GetBytes(ToJson(look, indented: false)));
            return CodePrefix + Convert.ToBase64String(packed.ToArray()).TrimEnd('=').Replace('+', '-').Replace('/', '_');
        }

        public static Look Parse(string text)
        {
            if (text.Length > MaxBytes * 2)
                throw new FormatException(L.T("That is far too large to be a look."));
            var code = Regex.Match(text, Regex.Escape(CodePrefix) + @"\s*([A-Za-z0-9_\-]+)", RegexOptions.IgnoreCase);
            var json = code.Success ? Unpack(code.Groups[1].Value) : text.Trim();
            if (json.Length > MaxBytes)
                throw new FormatException(L.T("That is far too large to be a look."));
            if (!json.StartsWith('{'))
                throw new FormatException(L.F($"That is not a look. Codes start with {CodePrefix}"));

            JsonObject root;
            try
            {
                root = JsonNode.Parse(json, null, new JsonDocumentOptions { MaxDepth = 4 }) as JsonObject ?? throw new FormatException("");
            }
            catch (Exception e) when (e is JsonException or FormatException)
            {
                throw new FormatException(L.T("The look is damaged (copy the whole code)."));
            }
            if (root["lantern_look"] is not JsonValue version || !version.TryGetValue<int>(out var v))
                throw new FormatException(L.T("That is not a look."));
            if (v != Version)
                throw new FormatException(L.T("This look comes from a newer launcher."));
            foreach (var (key, _) in root)
                if (key is not ("lantern_look" or "name" or "author" or "values"))
                    throw new FormatException(L.T("The look holds something a look may not hold, so it was refused."));
            if (root["values"] is not JsonObject values)
                throw new FormatException(L.T("The look is damaged (copy the whole code)."));

            var kept = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var (key, node) in values)
            {
                if (!ByKey.TryGetValue(key, out var knob))
                    throw new FormatException(L.T("The look holds something a look may not hold, so it was refused."));
                string? raw = null;
                if (node is JsonValue value)
                {
                    if (value.TryGetValue<string>(out var s)) raw = s;
                    else if (value.TryGetValue<double>(out var d)) raw = d.ToString("R", CultureInfo.InvariantCulture);
                    else if (value.TryGetValue<bool>(out var b)) raw = b ? "1" : "0";
                }
                if (raw == null || raw.Length > 32 || Normal(knob, raw) is not { } normal)
                    throw new FormatException(L.T("The look is damaged (copy the whole code)."));
                kept[key] = normal;
            }
            var name = root["name"] is JsonValue n && n.TryGetValue<string>(out var named) ? Clean(named, MaxName) : "";
            var author = root["author"] is JsonValue a && a.TryGetValue<string>(out var by) ? Clean(by, MaxAuthor) : "";
            return new Look(name.Length > 0 ? name : L.T("Unnamed look"), author, kept);
        }

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
                throw new FormatException(L.T("The look is damaged (copy the whole code)."));
            }
            try
            {
                using var deflate = new DeflateStream(new MemoryStream(packed), CompressionMode.Decompress);
                var buffer = new byte[MaxBytes + 1];
                var read = 0;
                int n;
                while (read < buffer.Length && (n = deflate.Read(buffer, read, buffer.Length - read)) > 0)
                    read += n;
                if (read > MaxBytes)
                    throw new FormatException(L.T("That is far too large to be a look."));
                return Encoding.UTF8.GetString(buffer, 0, read);
            }
            catch (InvalidDataException)
            {
                throw new FormatException(L.T("The look is damaged (copy the whole code)."));
            }
        }

        public static Look ReadFile(string file)
        {
            if (new FileInfo(file).Length > MaxBytes * 2)
                throw new FormatException(L.T("That is far too large to be a look."));
            return Parse(File.ReadAllText(file));
        }

        private static string FileOf(Look look)
        {
            var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(look.Name)))[..12].ToLowerInvariant();
            return Path.Combine(Folder, $"{hash}.look.json");
        }

        public static IReadOnlyList<Look> Saved()
        {
            var looks = new List<Look>();
            if (!Directory.Exists(Folder)) return looks;
            foreach (var file in Directory.GetFiles(Folder, "*.look.json").Take(MaxSaved))
            {
                try { looks.Add(ReadFile(file)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException) { }
            }
            return looks.OrderBy(l => l.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        public static bool IsSaved(Look look) => File.Exists(FileOf(look));

        public static void Save(Look look)
        {
            Directory.CreateDirectory(Folder);
            var file = FileOf(look);
            var staging = file + ".saving";
            File.WriteAllText(staging, ToJson(look));
            File.Move(staging, file, overwrite: true);
        }

        public static void Delete(Look look)
        {
            var file = FileOf(look);
            if (File.Exists(file)) File.Delete(file);
        }

        public static bool HasPrevious => File.Exists(PreviousFile);

        private static void KeepPrevious(IniFile ini)
        {
            if (HasPrevious) return;
            var values = new JsonObject();
            foreach (var knob in Knobs)
                values[knob.Key] = ini.Raw(knob.Key);
            var staging = PreviousFile + ".saving";
            File.WriteAllText(staging, new JsonObject { ["values"] = values }.ToJsonString());
            File.Move(staging, PreviousFile, overwrite: true);
        }

        public static void Apply(Look look, IniFile ini)
        {
            KeepPrevious(ini);
            foreach (var knob in Knobs)
                ini.Set(knob.Key, Normal(knob, ValueIn(look, knob)) ?? knob.Default);
        }

        public static void GoBack(IniFile ini)
        {
            if (!HasPrevious) return;
            JsonObject? values;
            try
            {
                values = JsonNode.Parse(File.ReadAllText(PreviousFile))?["values"] as JsonObject;
            }
            catch (JsonException)
            {
                values = null;
            }
            if (values == null)
                throw new InvalidDataException(L.T("The saved settings are damaged."));
            foreach (var knob in Knobs)
            {
                var kept = values[knob.Key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;
                ini.Set(knob.Key, kept == null ? null : Normal(knob, kept) ?? knob.Default);
            }
            File.Delete(PreviousFile);
        }
    }
}
