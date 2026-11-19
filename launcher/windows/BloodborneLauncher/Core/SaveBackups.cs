using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Copies of the current profile's savedata, made before each start when it changed since the
    /// last copy; the newest ten are kept, in launcher-backups\&lt;profile&gt;\&lt;time&gt; in the saves
    /// folder. Restoring first backs up what is there now.
    /// </summary>
    public static class SaveBackups
    {
        public const int Keep = 10;
        public const int PlayKeep = 6;
        public static readonly TimeSpan PlayEvery = TimeSpan.FromMinutes(10);
        private const string DuringPlay = "_play";
        private const string Stamp = "yyyy-MM-dd_HH-mm-ss";
        private const string BeforeRestore = "_before-restore";
        private const string FingerprintFile = "fingerprint.txt";

        public sealed record Backup(string Dir, DateTime When, long Bytes, bool MadeBeforeRestore, bool MadeDuringPlay = false,
            string? Label = null, bool SafetyCopy = false);

        public static string SaveData(LauncherSettings s) => Path.Combine(GameEnvironment.UserDir(s), "savedata");

        public static string Folder(LauncherSettings s) => FolderOf(s, PlayerProfiles.Current(s));

        public static string FolderOf(LauncherSettings s, string profile) =>
            Path.Combine(GameEnvironment.SavesRoot(s), "launcher-backups", profile);

        public static List<Backup> ListWithSafetyCopies(string folder, string vaultFolder)
        {
            var backups = ListIn(folder);
            var byPrint = backups.Select(b => (Backup: b, Print: BackupFingerprint(b.Dir)))
                .Where(b => b.Print != null).ToLookup(b => b.Print!);
            var copies = ListIn(vaultFolder)
                .Where(c => BackupFingerprint(c.Dir) is not { } print ||
                    !byPrint[print].Any(b => SameContent(Path.Combine(b.Backup.Dir, "savedata"), Path.Combine(c.Dir, "savedata"))))
                .Select(c => c with { SafetyCopy = true });
            return backups.Concat(copies).OrderByDescending(b => b.When).ToList();
        }

        /// <summary>This profile's backups, newest first.</summary>
        public static List<Backup> List(LauncherSettings s) => ListIn(Folder(s));

        /// <summary>The backups in a profile's backup folder, newest first. Reads the disk only (each
        /// backup's size too), so it can run on a worker thread.</summary>
        public static List<Backup> ListIn(string folder)
        {
            if (!Directory.Exists(folder)) return new();
            var list = new List<Backup>();
            foreach (var dir in Directory.GetDirectories(folder))
            {
                var name = Path.GetFileName(dir);
                var before = name.EndsWith(BeforeRestore, StringComparison.Ordinal);
                if (before) name = name[..^BeforeRestore.Length];
                var play = name.EndsWith(DuringPlay, StringComparison.Ordinal);
                if (play) name = name[..^DuringPlay.Length];
                string? label = null;
                if (name.Length > Stamp.Length + 1 && name[^(Stamp.Length + 1)] == '_')
                {
                    label = name[..^(Stamp.Length + 1)];
                    name = name[^Stamp.Length..];
                }
                if (!DateTime.TryParseExact(name, Stamp, CultureInfo.InvariantCulture, DateTimeStyles.None, out var when))
                    continue;
                if (label != null && File.Exists(Path.Combine(dir, "label.txt")))
                    label = File.ReadAllText(Path.Combine(dir, "label.txt"));
                var saved = Size(Path.Combine(dir, "savedata"));
                if (saved > 0)
                    list.Add(new Backup(dir, when, saved, before, play, label));
            }
            return list.OrderByDescending(b => b.When).ToList();
        }

        /// <summary>Copies the savedata unless the newest backup already holds the same files.
        /// Returns the new backup's folder, or null when there was nothing new to copy.</summary>
        public static string? Create(LauncherSettings s, bool beforeRestore = false) =>
            Create(SaveData(s), Folder(s), beforeRestore);

        public static string? Create(string source, string folder, bool beforeRestore = false)
        {
            if (!Directory.Exists(source) || !Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories).Any())
                return null;
            return Snapshot(source, folder, beforeRestore: beforeRestore);
        }

        public static string? CreateDuringPlay(LauncherSettings s)
        {
            try { return Snapshot(SaveData(s), Folder(s), play: true); }
            catch (IOException) { return null; }
        }

        public static string CreateNamed(LauncherSettings s, string name) => CreateNamed(SaveData(s), Folder(s), name);

        public static string CreateNamed(string source, string folder, string name)
        {
            name = name.Trim();
            if (name.Length == 0 || name.Length > 80 || name.Any(c => !char.IsLetterOrDigit(c) && c is not (' ' or '-' or '_')))
                throw new ArgumentException(L.T("Use 1–80 characters: letters, numbers, spaces, - or _."));
            return Snapshot(source, folder, label: name)
                ?? throw new IOException(L.T("There are no saved files to copy yet."));
        }

        private static string? Snapshot(string source, string folder, bool beforeRestore = false, bool play = false, string? label = null)
        {
            if (!Directory.Exists(source)) return null;
            Directory.CreateDirectory(folder);
            using var guard = new FileStream(Path.Combine(folder, ".backup.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            if (Directory.EnumerateFileSystemEntries(source, "*", SearchOption.AllDirectories).Any(Writing))
                throw new IOException(L.T("The game is writing its saves. Try again in a moment."));
            if (!Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories).Any(f => !Staging(source, f))) return null;
            var print = Fingerprint(source);
            if (label == null && !beforeRestore && ListIn(folder).FirstOrDefault() is { } newest &&
                (ReadFingerprint(newest.Dir) == print || SameContent(source, Path.Combine(newest.Dir, "savedata"))) && Fingerprint(source) == print)
                return null;
            var stamp = DateTime.Now.ToString(Stamp, CultureInfo.InvariantCulture);
            var suffix = beforeRestore ? BeforeRestore : play ? DuringPlay : "";
            var target = Path.Combine(folder, (label == null ? "" : label + "_") + stamp + suffix);
            if (Directory.Exists(target))
            {
                if (label == null) return null;
                for (var n = 2; Directory.Exists(target); n++)
                    target = Path.Combine(folder, label + "--" + n + "_" + stamp);
            }
            var staging = target + ".copying";
            try
            {
                if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
                var copied = Path.Combine(staging, "savedata");
                Copy(source, copied);
                if (Fingerprint(source) != print || !SameContent(source, copied) || Fingerprint(source) != print ||
                    Directory.EnumerateFileSystemEntries(source, "*", SearchOption.AllDirectories).Any(Writing))
                    throw new IOException(L.T("The saves changed while copying. Try again in a moment."));
                File.WriteAllText(Path.Combine(staging, FingerprintFile), print);
                if (label != null) File.WriteAllText(Path.Combine(staging, "label.txt"), label);
                Publish(staging, target);
                Prune(folder);
                return target;
            }
            finally { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
        }

        private static bool SameContent(string source, string copy)
        {
            var files = Directory.GetFiles(source, "*", SearchOption.AllDirectories).Where(f => !Staging(source, f)).ToList();
            if (files.Count != Directory.GetFiles(copy, "*", SearchOption.AllDirectories).Length) return false;
            foreach (var file in files)
            {
                var copied = Path.Combine(copy, Path.GetRelativePath(source, file));
                if (!File.Exists(copied)) return false;
                using var original = ReadShared(file);
                using var snapshot = ReadShared(copied);
                if (!SHA256.HashData(original).AsSpan().SequenceEqual(SHA256.HashData(snapshot))) return false;
            }
            return true;
        }

        private static void Publish(string staging, string target)
        {
            // Windows scanners can briefly hold the freshly written folder without delete sharing.
            // Keep the profile lock throughout the retry, so the engine cannot begin another save.
            for (var attempt = 0; ; attempt++)
            {
                try { Directory.Move(staging, target); return; }
                catch (IOException e) when (OperatingSystem.IsWindows() && attempt < 10 && (e.HResult & 0xffff) is 5 or 32 or 33)
                { System.Threading.Thread.Sleep(50); }
                catch (UnauthorizedAccessException) when (OperatingSystem.IsWindows() && attempt < 10)
                { System.Threading.Thread.Sleep(50); }
            }
        }

        private static void Prune(string folder)
        {
            var all = ListIn(folder).Where(b => b.Label == null).ToList();
            foreach (var old in all.Where(b => !b.MadeDuringPlay).Skip(Keep).Concat(all.Where(b => b.MadeDuringPlay).Skip(PlayKeep)))
            {
                try { Directory.Delete(old.Dir, recursive: true); } catch { }
            }
        }

        private static bool Writing(string path)
        {
            var name = Path.GetFileName(path);
            return name.EndsWith(".lantern-work", StringComparison.OrdinalIgnoreCase) ||
                   name.EndsWith(".lantern-commit", StringComparison.OrdinalIgnoreCase);
        }

        public static void Delete(LauncherSettings s, Backup backup) => Delete(Folder(s), backup,
            SaveGuard.ProfileCopies(GameEnvironment.SavesRoot(s), PlayerProfiles.Current(s)));

        public static void Delete(string folder, Backup backup) => Delete(folder, backup, null);

        /// <summary>Explicitly deletes a selected snapshot and its hidden vault mirrors, never live saves or other local snapshots.</summary>
        public static void Delete(string folder, Backup backup, string? vaultFolder)
        {
            var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(folder));
            var vault = vaultFolder == null ? null : Path.TrimEndingDirectorySeparator(Path.GetFullPath(vaultFolder));
            var expectedRoot = backup.SafetyCopy ? vault : root;
            var target = Path.TrimEndingDirectorySeparator(Path.GetFullPath(backup.Dir));
            var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (expectedRoot == null || !string.Equals(Path.GetDirectoryName(target), expectedRoot, comparison))
                throw new ArgumentException(L.T("Only a backup in this profile's backup folder can be deleted."));
            Directory.CreateDirectory(root);
            using var guard = new FileStream(Path.Combine(root, ".backup.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            if (vault != null) Directory.CreateDirectory(vault);
            // SaveGuard.Secure takes the same vault lock before publishing or pruning copies.
            using var vaultGuard = vault == null ? null : new FileStream(Path.Combine(vault, ".vault.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            if (!Directory.Exists(target)) return; // already removed by cleanup or another action
            if ((File.GetAttributes(target) & FileAttributes.ReparsePoint) != 0 ||
                !ListIn(expectedRoot).Any(b => string.Equals(Path.GetFullPath(b.Dir), target, comparison)))
                throw new ArgumentException(L.T("This folder is not a recognized save backup."));
            var print = BackupFingerprint(target);
            if (vault != null)
            {
                foreach (var copy in ListIn(vault))
                {
                    if (string.Equals(Path.GetFullPath(copy.Dir), target, comparison)) continue;
                    if ((File.GetAttributes(copy.Dir) & FileAttributes.ReparsePoint) != 0) continue;
                    if (print != null && BackupFingerprint(copy.Dir) == print &&
                        SameContent(Path.Combine(target, "savedata"), Path.Combine(copy.Dir, "savedata")))
                        Directory.Delete(copy.Dir, recursive: true);
                }
            }
            Directory.Delete(target, recursive: true);
        }

        /// <summary>Puts a backup's savedata back, after backing up the current one.</summary>
        public static void Restore(LauncherSettings s, Backup backup)
        {
            Directory.CreateDirectory(Folder(s));
            using var session = IdleSession(Folder(s));
            var from = Path.Combine(backup.Dir, "savedata");
            if (!Directory.Exists(from)) throw new DirectoryNotFoundException(L.T("This backup has no savedata folder."));
            if (!Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories).Any())
                throw new IOException(L.T("This backup is empty, so your saves were left as they are."));
            Create(s, beforeRestore: true);

            var target = SaveData(s);
            // Copied next to it first, so a failed copy leaves the current saves in place.
            var staging = target + ".restoring";
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            Copy(from, staging);
            if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
            Publish(staging, target);
        }

        /// <summary>A save import's staging folder (SaveImport): never part of the saves. One left by
        /// an older import held a whole Downloads folder, copied into every backup.</summary>
        private static bool Staging(string from, string path) =>
            Path.GetRelativePath(from, path).Split(Path.DirectorySeparatorChar)
                .Any(part => part.EndsWith(".importing", StringComparison.OrdinalIgnoreCase) || Writing(part));

        internal static void Copy(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (var dir in Directory.GetDirectories(from, "*", SearchOption.AllDirectories).Where(d => !Staging(from, d)))
                Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, dir)));
            foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories).Where(f => !Staging(from, f)))
            {
                var target = Path.Combine(to, Path.GetRelativePath(from, file));
                var modified = File.GetLastWriteTimeUtc(file);
                using (var input = ReadShared(file))
                using (var output = File.Create(target)) input.CopyTo(output);
                File.SetLastWriteTimeUtc(target, modified);
            }
        }

        private static FileStream ReadShared(string path) =>
            new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);

        private static FileStream IdleSession(string folder)
        {
            try { return new FileStream(Path.Combine(folder, ".session.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException e) { throw new IOException(L.T("Close the game before restoring saves, including games started directly."), e); }
        }

        /// <summary>Names, sizes and times of the files: equal when nothing was saved since.</summary>
        internal static string Fingerprint(string dir)
        {
            var text = new StringBuilder();
            foreach (var file in Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Where(f => !Staging(dir, f)).OrderBy(f => f, StringComparer.Ordinal))
            {
                var info = new FileInfo(file);
                text.Append(Path.GetRelativePath(dir, file)).Append('|').Append(info.Length).Append('|')
                    .Append(info.LastWriteTimeUtc.Ticks).Append('\n');
            }
            return Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(text.ToString())));
        }

        private static string? BackupFingerprint(string backup)
        {
            try { return ReadFingerprint(backup) ?? Fingerprint(Path.Combine(backup, "savedata")); }
            catch (IOException) { return null; }
            catch (UnauthorizedAccessException) { return null; }
        }

        private static string? ReadFingerprint(string backup)
        {
            try { return File.ReadAllText(Path.Combine(backup, FingerprintFile)).Trim(); }
            catch { return null; }
        }

        private static long Size(string dir)
        {
            try { return Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length); }
            catch { return 0; }
        }
    }
}
