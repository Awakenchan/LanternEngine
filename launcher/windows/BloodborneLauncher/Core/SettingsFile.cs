// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Text;
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
            var (data, stamp, _) = ReadWithState(path);
            return (data, stamp);
        }

        public static (JsonObject? Data, Stamp Stamp, TextState State) ReadWithState(string path)
        {
            using var gate = Lock(path);
            var data = Best(path, out var unreadable);
            var state = unreadable ? TextState.Unreadable : data != null ? TextState.Read : File.Exists(path) ? TextState.Unreadable : TextState.Absent;
            return (data, StampOf(path), state);
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
            using var gate = Lock(path) ?? throw new SettingsBusyException(path, new IOException(path + ".lock is held"));
            var disk = Best(path, out var unreadable);
            if (unreadable) throw new SettingsUnreadableException(path);
            var merged = Merge(disk, baseline, current);
            var text = merged.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
            SettingsText.Replace(path, text);
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

        private enum State { Missing, Broken, Unreadable, Read }

        private const int ParseAttempts = 6;
        private const int ParsePauseMs = 40;

        private static State Parse(string path, out JsonObject? data)
        {
            data = null;
            var read = SettingsText.Read(path);
            if (read.State == TextState.Absent) return State.Missing;
            if (read.State == TextState.Unreadable) return State.Unreadable;
            try
            {
                if (JsonNode.Parse(read.Text) is not JsonObject saved) return State.Broken;
                data = saved;
                return State.Read;
            }
            catch (JsonException) { return State.Broken; }
        }

        private static JsonObject? Best(string path, out bool unreadable)
        {
            unreadable = false;
            var state = State.Missing;
            for (var attempt = 0; attempt < ParseAttempts; attempt++)
            {
                if (attempt > 0) Thread.Sleep(ParsePauseMs);
                state = Parse(path, out var data);
                if (state is State.Missing or State.Read) return data;
            }
            if (state == State.Unreadable)
            {
                unreadable = true;
                return null;
            }
            try
            {
                File.Copy(path, BrokenOf(path), overwrite: true);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                unreadable = true;
                return null;
            }
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
                    return new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    Thread.Sleep(25);
                }
            }
            return null;
        }
    }

    public enum TextState { Read, Absent, Unreadable }

    public enum SaveOutcome { Written, Unchanged }

    public readonly record struct TextRead(TextState State, string Text);

    public sealed class SettingsBusyException : IOException
    {
        public SettingsBusyException(string path, Exception inner)
            : base($"{Path.GetFileName(path)} is being written by another program, so it was not written. Your changes are kept and are written with the next save.", inner) { }
    }

    public sealed class SettingsUnreadableException : IOException
    {
        public SettingsUnreadableException(string path)
            : base($"{Path.GetFileName(path)} could not be read, so it was not written. Your changes are kept and are written once the file can be read.") { }
    }

    public static class SettingsText
    {
        public const int ReadAttempts = 20;
        public const int ReplaceAttempts = 20;
        public const int LockAttempts = 100;
        public const int RetryPauseMs = 10;
        private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);
        private static readonly object Gate = new();
        private static long _temporaries;
        private static long _temporariesLeft;

        public static long TemporariesLeft => Interlocked.Read(ref _temporariesLeft);

        public static string BackupOf(string path) => path + ".bak";

        public static TextRead Read(string path)
        {
            for (var attempt = 1; ; attempt++)
            {
                try
                {
                    using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    using var reader = new StreamReader(stream, Utf8, detectEncodingFromByteOrderMarks: true);
                    return new TextRead(TextState.Read, reader.ReadToEnd());
                }
                catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException)
                {
                    return new TextRead(TextState.Absent, "");
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    if (attempt >= ReadAttempts) return new TextRead(TextState.Unreadable, "");
                    Thread.Sleep(RetryPauseMs);
                }
            }
        }

        public static SaveOutcome Update(string path, Func<TextRead, string> build)
        {
            lock (Gate)
            {
                using var fileLock = LockFile(path);
                var before = Read(path);
                if (before.State == TextState.Unreadable) throw new SettingsUnreadableException(path);
                var text = build(before);
                if (before.State == TextState.Read && before.Text == text) return SaveOutcome.Unchanged;
                Replace(path, text);
                return SaveOutcome.Written;
            }
        }

        public static FileStream LockFile(string path)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
            for (var attempt = 1; ; attempt++)
            {
                try
                {
                    return new FileStream(path + ".lock", FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1);
                }
                catch (Exception e) when ((e is IOException or UnauthorizedAccessException) && attempt < LockAttempts)
                {
                    Thread.Sleep(RetryPauseMs);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    throw new SettingsBusyException(path, e);
                }
            }
        }

        public static void Replace(string target, string text)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(target))!);
            var before = Read(target);
            if (before.State == TextState.Unreadable) throw new SettingsUnreadableException(target);
            if (before.State == TextState.Read) Swap(BackupOf(target), before.Text);
            Swap(target, text);
        }

        private static void Swap(string target, string text)
        {
            var temp = $"{target}.{Environment.ProcessId}.{Interlocked.Increment(ref _temporaries)}.tmp";
            try
            {
                using (var stream = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    stream.Write(Utf8.GetBytes(text));
                    stream.Flush(flushToDisk: true);
                }
                for (var attempt = 1; ; attempt++)
                {
                    try
                    {
                        File.Move(temp, target, overwrite: true);
                        return;
                    }
                    catch (Exception e) when ((e is IOException or UnauthorizedAccessException) && attempt < ReplaceAttempts)
                    {
                        Thread.Sleep(RetryPauseMs);
                    }
                }
            }
            finally
            {
                RemoveTemporary(temp);
            }
        }

        private static void RemoveTemporary(string temp)
        {
            try
            {
                File.Delete(temp);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Interlocked.Increment(ref _temporariesLeft);
                System.Diagnostics.Trace.TraceWarning($"{Path.GetFileName(temp)} could not be removed: {e.Message}");
            }
        }
    }
}
