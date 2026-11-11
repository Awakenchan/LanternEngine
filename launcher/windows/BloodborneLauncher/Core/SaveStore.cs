// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Globalization;
using System.IO;

namespace BloodborneLauncher.Core
{
    public sealed record SaveCharacter(string Profile, int Slot, string Name, int Level, string File, DateTime Saved, int BossesDefeated, double PlayHours);

    public sealed record SaveEditBackup(string Dir, DateTime When);

    public sealed record SavePlace(string Profile, string SaveData, string Folder, string Backups);

    public static class SaveStore
    {
        public const int KeepBackups = 20;
        private const string Stamp = "yyyy-MM-dd_HH-mm-ss";
        private const string WorkSuffix = ".lantern-work";
        private const string SlotFileName = "userdata{0:D4}";
        private const string SummaryFile = "userdata0010";
        private const int Slots = 10;
        private static readonly string[] GameProcesses = { "bb-start", "bb-probe" };

        public static bool SavesInUse(bool launcherGame) => SavesFolder.InUse(launcherGame, GameProcessRunning);

        public static bool GameProcessRunning()
        {
            foreach (var name in GameProcesses)
            {
                var found = Process.GetProcessesByName(name);
                var running = found.Length > 0;
                foreach (var process in found) process.Dispose();
                if (running) return true;
            }
            return false;
        }

        public static List<SaveCharacter> Characters(SavePlace place)
        {
            var list = new List<SaveCharacter>();
            var errors = new List<string>();
            for (var slot = 0; slot < Slots; slot++)
            {
                var path = Path.Combine(place.Folder, string.Format(CultureInfo.InvariantCulture, SlotFileName, slot));
                if (!File.Exists(path)) continue;
                try
                {
                    var bytes = File.ReadAllBytes(path);
                    if (SaveSlot.IsEmptySlot(bytes)) continue;
                    var parsed = SaveSlot.Parse(bytes);
                    list.Add(new SaveCharacter(place.Profile, slot, parsed.Name, parsed.Level, path, File.GetLastWriteTime(path),
                        SaveWorld.Bosses.Count(b => SaveWorld.State(parsed, b) == BossState.Defeated), parsed.PlayMilliseconds / 3600000.0));
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or SaveFormatException or ArgumentException)
                {
                    errors.Add(string.Format(CultureInfo.InvariantCulture, "slot {0}: {1}", slot, e.Message));
                }
            }
            if (list.Count == 0 && errors.Count > 0) throw new IOException(string.Join("; ", errors));
            return list;
        }

        public static string Title(string name, string profile, bool showProfile, string? detail = null)
        {
            var label = name.Length > 0 ? name : "?";
            var parts = new List<string>();
            if (showProfile && profile.Length > 0) parts.Add(profile);
            if (!string.IsNullOrEmpty(detail)) parts.Add(detail);
            return parts.Count > 0 ? $"{label} ({string.Join(", ", parts)})" : label;
        }

        public static List<string> Titles(IReadOnlyList<SaveCharacter> hunters, bool showProfile)
        {
            var plain = hunters.Select(h => Title(h.Name, h.Profile, showProfile)).ToList();
            var leveled = hunters.Select((h, i) => Count(plain, plain[i]) > 1 ? Title(h.Name, h.Profile, showProfile, L.F($"level {h.Level}")) : plain[i]).ToList();
            return hunters.Select((h, i) => Count(leveled, leveled[i]) > 1
                ? Title(h.Name, h.Profile, showProfile, L.F($"level {h.Level}, slot {h.Slot + 1}"))
                : leveled[i]).ToList();
        }

        private static int Count(List<string> titles, string title) =>
            titles.Count(t => string.Equals(t, title, StringComparison.OrdinalIgnoreCase));

        public static int DefaultCharacter(IReadOnlyList<SaveCharacter> characters, string currentProfile)
        {
            var best = -1;
            for (var i = 0; i < characters.Count; i++)
            {
                if (best < 0) { best = i; continue; }
                var current = string.Equals(characters[i].Profile, currentProfile, StringComparison.OrdinalIgnoreCase);
                var bestCurrent = string.Equals(characters[best].Profile, currentProfile, StringComparison.OrdinalIgnoreCase);
                if (current != bestCurrent ? current : characters[i].Saved > characters[best].Saved) best = i;
            }
            return best;
        }

        public static string Commit(SavePlace place, SaveCharacter character, byte[] original, SaveSlot edited)
        {
            if (GameProcessRunning()) throw new InvalidOperationException(L.T("Close the game first: saves can't be changed while it runs."));
            var onDisk = File.ReadAllBytes(character.File);
            if (!onDisk.AsSpan().SequenceEqual(original))
                throw new InvalidOperationException(L.T("This save changed on disk since it was opened. Open it again and redo the edit."));
            var bytes = edited.ToBytes();
            var check = SaveSlot.Parse(bytes);
            if (check.Name != character.Name) throw new InvalidOperationException(L.T("The edit failed its check, so the save was left as it was."));

            var backup = MakeBackup(place);
            var summaryPath = Path.Combine(place.Folder, SummaryFile);
            byte[]? summaryBefore = null, summaryAfter = null;
            if (check.Level != SaveSlot.Parse(original).Level && File.Exists(summaryPath))
            {
                summaryBefore = File.ReadAllBytes(summaryPath);
                summaryAfter = SaveSlotSummary.WithLevel(summaryBefore, character.Slot, check.Name, check.Level);
            }
            try
            {
                WriteChecked(character.File, bytes);
                if (summaryAfter != null) WriteChecked(summaryPath, summaryAfter);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                try { WriteChecked(character.File, original); } catch (Exception f) when (f is IOException or UnauthorizedAccessException or InvalidDataException) { }
                if (summaryBefore != null)
                    try { WriteChecked(summaryPath, summaryBefore); } catch (Exception f) when (f is IOException or UnauthorizedAccessException or InvalidDataException) { }
                throw new InvalidOperationException(L.F($"The save could not be written, so it was put back as it was: {e.Message}"));
            }
            return backup;
        }

        public static List<SaveEditBackup> Backups(SavePlace place)
        {
            if (!Directory.Exists(place.Backups)) return new();
            var list = new List<SaveEditBackup>();
            foreach (var dir in Directory.GetDirectories(place.Backups))
                if (DateTime.TryParseExact(Path.GetFileName(dir), Stamp, CultureInfo.InvariantCulture, DateTimeStyles.None, out var when) && HasFiles(Path.Combine(dir, "savedata")))
                    list.Add(new SaveEditBackup(dir, when));
            return list.OrderByDescending(b => b.When).ToList();
        }

        public static void Revert(SavePlace place, SaveEditBackup backup)
        {
            if (GameProcessRunning()) throw new InvalidOperationException(L.T("Close the game first: saves can't be changed while it runs."));
            var from = Path.Combine(backup.Dir, "savedata");
            if (!HasFiles(from)) throw new IOException(L.T("This backup is empty, so your saves were left as they are."));
            MakeBackup(place);
            var staging = place.SaveData + ".restoring";
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            SaveBackups.Copy(from, staging);
            if (Directory.Exists(place.SaveData)) Directory.Delete(place.SaveData, recursive: true);
            Directory.Move(staging, place.SaveData);
        }

        public static string MakeBackup(SavePlace place)
        {
            if (!HasFiles(place.SaveData)) throw new IOException(L.T("There are no saves to back up."));
            Directory.CreateDirectory(place.Backups);
            var now = DateTime.Now;
            var target = Path.Combine(place.Backups, now.ToString(Stamp, CultureInfo.InvariantCulture));
            while (Directory.Exists(target))
            {
                now = now.AddSeconds(1);
                target = Path.Combine(place.Backups, now.ToString(Stamp, CultureInfo.InvariantCulture));
            }
            var staging = target + WorkSuffix;
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            SaveBackups.Copy(place.SaveData, Path.Combine(staging, "savedata"));
            foreach (var file in Directory.GetFiles(place.SaveData, "*", SearchOption.AllDirectories))
            {
                if (file.EndsWith(WorkSuffix, StringComparison.OrdinalIgnoreCase)) continue;
                var copy = Path.Combine(staging, "savedata", Path.GetRelativePath(place.SaveData, file));
                if (!File.Exists(copy) || new FileInfo(copy).Length != new FileInfo(file).Length)
                    throw new IOException(L.T("The backup could not be made, so the save was not changed."));
            }
            File.WriteAllText(Path.Combine(staging, "fingerprint.txt"), SaveBackups.Fingerprint(Path.Combine(staging, "savedata")));
            Directory.Move(staging, target);
            foreach (var old in Backups(place).Skip(KeepBackups))
                try { Directory.Delete(old.Dir, recursive: true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            return target;
        }

        private static void WriteChecked(string path, byte[] bytes)
        {
            var work = path + WorkSuffix;
            File.WriteAllBytes(work, bytes);
            if (!File.ReadAllBytes(work).AsSpan().SequenceEqual(bytes))
            {
                File.Delete(work);
                throw new InvalidDataException(L.T("The written file did not read back the same."));
            }
            File.Move(work, path, overwrite: true);
        }

        private static bool HasFiles(string dir) =>
            Directory.Exists(dir) && Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Any();
    }
}
