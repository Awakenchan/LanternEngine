// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Text;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    public static class OnlineSettings
    {
        public const string Pool = "huntersdream";
        public const int ReadAttempts = 5;
        public const int RetryPauseMs = 50;

        private static readonly string[] RetiredIniKeys =
        {
            "online_server", "online_host", "online_port", "online_webapi", "online_username",
            "online_upnp", "online_remember", "online_website", "online_thd_account",
        };

        private static readonly string[] RetiredGameHosts = { "game.supermedo.com", "srv.shadps4.net" };

        public const string RetiredSettingsKey = "online_password";

        public const string MigratedMarker = "online_shadnet_retired";

        public enum IniMigration { Absent, Unchanged, Migrated, Unreadable, WriteFailed, AlreadyDone }

        public readonly record struct IniMigrationResult(IniMigration State, string Detail)
        {
            public bool Settled => State is IniMigration.Absent or IniMigration.Unchanged or IniMigration.Migrated;
        }

        private static string PoolFor(LauncherSettings s) =>
            s.HuntersDreamToken.Length > 0 && s.Str("hd_name").Length > 0 ? Pool : "";

        public static void ChoosePool(IniFile ini, LauncherSettings s)
        {
            if (ini.Get("online_game_api").Length > 0) ini.Set("online_game_api", "");
            ini.Set("online_pool", PoolFor(s));
        }

        public static bool Migrated(JsonObject data) =>
            data[MigratedMarker] is JsonValue marker && marker.TryGetValue<bool>(out var done) && done;

        public static bool RetireSettings(JsonObject data) => !Migrated(data) && data.Remove(RetiredSettingsKey);

        public static IniMigrationResult MigrateIniOnce(LauncherSettings s, string path)
        {
            if (s.Bool(MigratedMarker)) return new(IniMigration.AlreadyDone, "");
            var result = MigrateIniFile(path);
            if (result.Settled) s.Set(MigratedMarker, true);
            return result;
        }

        public static string? MigrateIni(string text)
        {
            var kept = new StringBuilder(text.Length);
            var changed = false;
            for (var start = 0; start < text.Length;)
            {
                var end = text.IndexOf('\n', start);
                var next = end < 0 ? text.Length : end + 1;
                if (RetiredLine(text[start..next].Trim()))
                    changed = true;
                else
                    kept.Append(text, start, next - start);
                start = next;
            }
            return changed ? kept.ToString() : null;
        }

        private static bool RetiredLine(string line)
        {
            var at = line.IndexOf('=');
            return at > 0 && line[0] != '#' && line[0] != ';' && Retired(line[..at].Trim(), line[(at + 1)..].Trim());
        }

        public static IniMigrationResult MigrateIniFile(string path)
        {
            string text;
            for (var attempt = 1; ; attempt++)
            {
                try
                {
                    if (!File.Exists(path)) return new(IniMigration.Absent, "");
                    text = File.ReadAllText(path);
                    break;
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    if (attempt >= ReadAttempts) return new(IniMigration.Unreadable, e.Message);
                    Thread.Sleep(RetryPauseMs);
                }
            }
            if (MigrateIni(text) is not { } migrated) return new(IniMigration.Unchanged, "");
            return WriteReplacing(path, migrated) is { Length: > 0 } problem
                ? new(IniMigration.WriteFailed, problem)
                : new(IniMigration.Migrated, "");
        }

        private static bool Retired(string key, string value) =>
            RetiredIniKeys.Contains(key, StringComparer.Ordinal) || key == "online_game_api" && RetiredGameApi(value);

        private static bool RetiredGameApi(string value)
        {
            var text = value.Contains("://") ? value : "https://" + value;
            return Uri.TryCreate(text, UriKind.Absolute, out var uri) &&
                   RetiredGameHosts.Contains(uri.Host, StringComparer.OrdinalIgnoreCase);
        }

        private static string WriteReplacing(string path, string text)
        {
            var full = Path.GetFullPath(path);
            var temp = Path.Combine(Path.GetDirectoryName(full)!, $"{Path.GetFileName(full)}.{Environment.ProcessId}.tmp");
            try
            {
                using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
                {
                    stream.Write(new UTF8Encoding(false).GetBytes(text));
                    stream.Flush(flushToDisk: true);
                }
                File.Replace(temp, full, full + ".bak");
                return "";
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                try
                {
                    if (File.Exists(temp)) File.Delete(temp);
                    return e.Message;
                }
                catch (Exception left) when (left is IOException or UnauthorizedAccessException)
                {
                    return e.Message + "; " + temp + ": " + left.Message;
                }
            }
        }
    }
}
