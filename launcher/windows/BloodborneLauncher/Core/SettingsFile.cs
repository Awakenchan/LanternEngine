// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    public static class SettingsFile
    {
        public readonly record struct Stamp(DateTime Written, long Length);

        public static Stamp StampOf(string path)
        {
            try
            {
                var info = new FileInfo(path);
                return info.Exists ? new Stamp(info.LastWriteTimeUtc, info.Length) : default;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return default; }
        }

        public static (JsonObject? Data, Stamp Stamp) Read(string path)
        {
            using var gate = Lock(path);
            return (Best(path), StampOf(path));
        }

        public static (JsonObject Data, JsonObject Baseline) Layer(IEnumerable<KeyValuePair<string, JsonNode?>> defaults, JsonObject? saved)
        {
            var data = new JsonObject();
            foreach (var (key, value) in defaults)
                data[key] = value?.DeepClone();
            if (saved != null)
                foreach (var (key, value) in saved)
                    data[key] = value?.DeepClone();
            return (data, (JsonObject)data.DeepClone());
        }

        public static (JsonObject Data, Stamp Stamp) Save(string path, JsonObject baseline, JsonObject current)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
            using var gate = Lock(path);
            var merged = Merge(Best(path), baseline, current);
            var text = merged.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
            var temp = $"{path}.{Environment.ProcessId}.tmp";
            File.WriteAllText(temp, text);
            try
            {
                File.Move(temp, path, overwrite: true);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                File.WriteAllText(path, text);
                try { File.Delete(temp); } catch (Exception) { }
            }
            try { File.WriteAllText(BackupOf(path), text); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            return (merged, StampOf(path));
        }

        public static bool Changed(JsonObject baseline, JsonObject current, string key) =>
            baseline.ContainsKey(key) != current.ContainsKey(key) || !JsonNode.DeepEquals(baseline[key], current[key]);

        public static JsonObject Merge(JsonObject? disk, JsonObject baseline, JsonObject current)
        {
            var merged = disk?.DeepClone() as JsonObject ?? new JsonObject();
            foreach (var (key, value) in current)
                if (!merged.ContainsKey(key) || Changed(baseline, current, key))
                    merged[key] = value?.DeepClone();
            foreach (var (key, _) in baseline)
                if (!current.ContainsKey(key))
                    merged.Remove(key);
            return merged;
        }

        public static (JsonObject Current, JsonObject Baseline) Refresh(JsonObject disk, JsonObject baseline, JsonObject current)
        {
            var next = (JsonObject)current.DeepClone();
            var nextBaseline = (JsonObject)baseline.DeepClone();
            foreach (var (key, value) in disk)
            {
                if (!Changed(baseline, current, key))
                    next[key] = value?.DeepClone();
                nextBaseline[key] = value?.DeepClone();
            }
            return (next, nextBaseline);
        }

        public static string BackupOf(string path) => path + ".bak";

        public static string BrokenOf(string path) => path + ".broken";

        private enum State { Missing, Broken, Read }

        private static State Parse(string path, out JsonObject? data)
        {
            data = null;
            try
            {
                if (!File.Exists(path)) return State.Missing;
                if (JsonNode.Parse(File.ReadAllText(path)) is not JsonObject saved) return State.Broken;
                data = saved;
                return State.Read;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return State.Broken; }
        }

        private static JsonObject? Best(string path)
        {
            for (var tries = 0; tries < 6; tries++)
            {
                var state = Parse(path, out var data);
                if (state != State.Broken) return data;
                Thread.Sleep(40);
            }
            try { File.Copy(path, BrokenOf(path), overwrite: true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            return Parse(BackupOf(path), out var backup) == State.Read ? backup : null;
        }

        private static FileStream? Lock(string path)
        {
            var lockPath = path + ".lock";
            if (!Directory.Exists(Path.GetDirectoryName(Path.GetFullPath(lockPath)))) return null;
            for (var tries = 0; tries < 40; tries++)
            {
                try
                {
                    return new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.DeleteOnClose);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    Thread.Sleep(25);
                }
            }
            return null;
        }
    }
}
