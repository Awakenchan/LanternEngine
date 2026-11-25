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
        internal const string Previous = ".restore-previous";

        public sealed record Backup(string Dir, DateTime When, long Bytes, bool MadeBeforeRestore, bool MadeDuringPlay = false,
            string? Label = null, bool SafetyCopy = false);

        public static string SaveData(LauncherSettings s) => Path.Combine(GameEnvironment.UserDir(s), "savedata");

        public static string Folder(LauncherSettings s) => FolderOf(s, PlayerProfiles.Current(s));

        public static string FolderOf(LauncherSettings s, string profile) =>
            Path.Combine(GameEnvironment.SavesRoot(s), "launcher-backups", profile);

        public static List<Backup> ListWithSafetyCopies(string folder, string vaultFolder)
        {
            var backups = ListIn(folder);
            var bySize = backups.ToLookup(b => b.Bytes);
            var copies = ListIn(vaultFolder)
                .Where(c => !bySize[c.Bytes].Any(b => IsMirror(b, c)))
                .Select(c => c with { SafetyCopy = true });
            return backups.Concat(copies).OrderByDescending(b => b.When).ToList();
        }

        private static bool IsMirror(Backup backup, Backup copy)
        {
            try { return SameContent(Path.Combine(backup.Dir, "savedata"), Path.Combine(copy.Dir, "savedata")); }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
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
                if ((File.GetAttributes(dir) & FileAttributes.ReparsePoint) != 0) continue;
                var name = Path.GetFileName(dir);
                var series = name.IndexOf(BeforeRestore + "-", StringComparison.Ordinal);
                if (series == Stamp.Length && int.TryParse(name[(series + BeforeRestore.Length + 1)..], out var ordinal) && ordinal >= 2)
                    name = name[..(series + BeforeRestore.Length)];
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
            if (!Directory.Exists(Path.GetDirectoryName(Path.GetFullPath(source)))) return null;
            using var live = LockLive(source);
            Directory.CreateDirectory(folder);
            using var guard = LockFile(folder, ".backup.lock");
            return SnapshotLocked(source, folder, beforeRestore, play, label);
        }

        private static string? SnapshotLocked(string source, string folder, bool beforeRestore = false, bool play = false, string? label = null)
        {
            if (!Directory.Exists(source) || SavedFiles(source).Count == 0) return null;
            var print = Fingerprint(source);
            if (label == null && !beforeRestore && ListIn(folder).FirstOrDefault() is { } newest &&
                SameContent(source, Path.Combine(newest.Dir, "savedata")) && Fingerprint(source) == print)
                return null;
            var stamp = DateTime.Now.ToString(Stamp, CultureInfo.InvariantCulture);
            var suffix = beforeRestore ? BeforeRestore : play ? DuringPlay : "";
            var target = Path.Combine(folder, (label == null ? "" : label + "_") + stamp + suffix);
            for (var n = 2; Path.Exists(target); n++)
            {
                if (beforeRestore) target = Path.Combine(folder, stamp + suffix + "-" + n);
                else if (label != null) target = Path.Combine(folder, label + "--" + n + "_" + stamp);
                else return null;
            }
            var staging = target + ".copying";
            try
            {
                RemoveStaging(staging);
                var copied = Path.Combine(staging, "savedata");
                CopyVerified(source, copied);
                File.WriteAllText(Path.Combine(staging, FingerprintFile), print);
                if (label != null) File.WriteAllText(Path.Combine(staging, "label.txt"), label);
                Publish(staging, target);
                Prune(folder, target);
                return target;
            }
            finally { TryRemove(staging); }
        }

        internal static bool SameContent(string source, string copy)
        {
            var files = SavedFiles(source);
            if (files.Count != SavedFiles(copy).Count) return false;
            foreach (var file in files)
            {
                var copied = Path.Combine(copy, Path.GetRelativePath(source, file));
                if (!File.Exists(copied)) return false;
                using var original = ReadShared(file);
                using var snapshot = ReadShared(copied);
                if (original.Length != snapshot.Length || !SHA256.HashData(original).AsSpan().SequenceEqual(SHA256.HashData(snapshot))) return false;
            }
            return true;
        }

        internal static void CopyVerified(string source, string target)
        {
            var print = Fingerprint(source);
            if (SavedFiles(source).Count == 0)
                throw new IOException(L.T("This backup has no saved files to copy."));
            Copy(source, target, flush: true);
            if (Fingerprint(source) != print || !SameContent(source, target) || Fingerprint(source) != print)
                throw new IOException(L.T("The saves changed while copying. Try again in a moment."));
        }

        internal static void Publish(string staging, string target)
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

        private static void Prune(string folder, string published)
        {
            var all = ListIn(folder).Where(b => b.Label == null).OrderByDescending(b => b.Dir == published).ToList();
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
            using var guard = LockFile(root, ".backup.lock");
            if (vault != null) Directory.CreateDirectory(vault);
            // SaveGuard.Secure takes the same vault lock before publishing or pruning copies.
            using var vaultGuard = vault == null ? null : LockFile(vault, ".vault.lock");
            if (!Directory.Exists(target)) return; // already removed by cleanup or another action
            if ((File.GetAttributes(target) & FileAttributes.ReparsePoint) != 0 ||
                !ListIn(expectedRoot).Any(b => string.Equals(Path.GetFullPath(b.Dir), target, comparison)))
                throw new ArgumentException(L.T("This folder is not a recognized save backup."));
            var selected = ListIn(expectedRoot).Single(b => string.Equals(Path.GetFullPath(b.Dir), target, comparison));
            if (vault != null)
            {
                var mirrors = new List<string>();
                foreach (var copy in ListIn(vault))
                {
                    if (string.Equals(Path.GetFullPath(copy.Dir), target, comparison)) continue;
                    if (selected.Bytes == copy.Bytes &&
                        SameContent(Path.Combine(target, "savedata"), Path.Combine(copy.Dir, "savedata")))
                        mirrors.Add(copy.Dir);
                }
                foreach (var mirror in mirrors) Directory.Delete(mirror, recursive: true);
            }
            Directory.Delete(target, recursive: true);
        }

        /// <summary>Puts a backup's savedata back, after backing up the current one.</summary>
        public static void Restore(LauncherSettings s, Backup backup)
        {
            var folder = Folder(s);
            var vault = backup.SafetyCopy ? SaveGuard.ProfileCopies(GameEnvironment.SavesRoot(s), PlayerProfiles.Current(s)) : null;
            var backupPath = Path.GetFullPath(backup.Dir);
            var parent = Path.GetDirectoryName(backupPath);
            if (!PathEquals(parent, vault ?? folder))
                throw new IOException(L.T("This backup does not belong to the current profile."));
            RestoreFrom(SaveData(s), folder, Path.Combine(backupPath, "savedata"), vault);
        }

        // Order across all callers: live session, live data, backup folder, then vault.
        internal static void RestoreFrom(string target, string folder, string from, string? vault, Func<string, bool>? alreadyHasSaves = null)
        {
            var profile = Profile(target);
            Directory.CreateDirectory(profile);
            using var session = IdleSession(profile);
            using var live = LockLive(target);
            Directory.CreateDirectory(folder);
            using var backups = LockFile(folder, ".backup.lock");
            using var vaultCopy = vault == null ? null : LockFile(vault, ".vault.lock");
            var previous = target + Previous;
            if (Path.Exists(target) && !Directory.Exists(target))
                throw new IOException(L.T("The live save path is not a folder."));
            if (!Path.Exists(target) && Path.Exists(previous))
            {
                RejectLink(previous);
                Publish(previous, target);
            }
            if (alreadyHasSaves?.Invoke(target) == true)
                throw new IOException(L.T("This profile has saves now; nothing was replaced."));
            var fromBackup = Path.GetDirectoryName(from)!;
            if (!ListIn(vault ?? folder).Any(b => PathEquals(b.Dir, fromBackup)))
                throw new IOException(L.T("This backup is no longer available."));
            var hadLive = Directory.Exists(target);
            if (hadLive) RejectLink(target);
            var staging = target + ".restoring";
            RemoveStaging(staging);
            try
            {
                CopyVerified(from, staging);
                if (hadLive && SavedFiles(target).Count > 0 && SnapshotLocked(target, folder, beforeRestore: true) == null)
                    throw new IOException(L.T("Could not back up the current saves. The restore was stopped."));
                RemoveStaging(previous);
                if (hadLive) Publish(target, previous);
                try { Publish(staging, target); }
                catch
                {
                    if (hadLive && !Path.Exists(target)) Publish(previous, target);
                    throw;
                }
                TryRemove(previous);
            }
            finally { TryRemove(staging); }
        }

        internal static bool PathEquals(string? left, string right) => left != null &&
            string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(left)), Path.TrimEndingDirectorySeparator(Path.GetFullPath(right)),
                OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

        internal static FileStream LockLive(string savedata) => LockFile(Profile(savedata), ".save-write.lock");
        private static string Profile(string savedata) => Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(Path.GetFullPath(savedata)))!;

        internal static FileStream LockFile(string folder, string name)
        {
            var path = Path.Combine(folder, name);
            if (Path.Exists(path)) RejectLink(path);
            return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        }

        private static void RejectLink(string path)
        {
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new IOException(L.T("A save path is a symbolic link."));
        }

        private static List<string> SavedFiles(string root)
        {
            RejectLink(root);
            var files = new List<string>();
            void Scan(string folder)
            {
                foreach (var entry in Directory.EnumerateFileSystemEntries(folder))
                {
                    if (Writing(entry)) throw new IOException(L.T("The game is writing its saves. Try again in a moment."));
                    if (Staging(root, entry)) continue;
                    var attributes = File.GetAttributes(entry);
                    if ((attributes & FileAttributes.ReparsePoint) != 0) RejectLink(entry);
                    if ((attributes & FileAttributes.Directory) != 0) Scan(entry);
                    else files.Add(entry);
                }
            }
            Scan(root);
            return files;
        }

        internal static void RemoveStaging(string path)
        {
            if (!Path.Exists(path)) return;
            RejectLink(path);
            Directory.Delete(path, recursive: true);
        }

        internal static void TryRemove(string path)
        {
            try { RemoveStaging(path); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        /// <summary>A save import's staging folder (SaveImport): never part of the saves. One left by
        /// an older import held a whole Downloads folder, copied into every backup.</summary>
        private static bool Staging(string from, string path) =>
            Path.GetRelativePath(from, path).Split(Path.DirectorySeparatorChar)
                .Any(part => part.EndsWith(".importing", StringComparison.OrdinalIgnoreCase) || Writing(part));

        internal static void Copy(string from, string to, bool flush = false)
        {
            Directory.CreateDirectory(to);
            foreach (var dir in Directory.GetDirectories(from, "*", SearchOption.AllDirectories).Where(d => !Staging(from, d)))
                Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, dir)));
            foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories).Where(f => !Staging(from, f)))
            {
                var target = Path.Combine(to, Path.GetRelativePath(from, file));
                var modified = File.GetLastWriteTimeUtc(file);
                using (var input = ReadShared(file))
                using (var output = File.Create(target))
                {
                    input.CopyTo(output);
                    if (flush) output.Flush(flushToDisk: true);
                }
                File.SetLastWriteTimeUtc(target, modified);
            }
        }

        private static FileStream ReadShared(string path) =>
            new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);

        private static FileStream IdleSession(string folder)
        {
            try { return LockFile(folder, ".session.lock"); }
            catch (IOException e) { throw new IOException(L.T("Close the game before restoring saves, including games started directly."), e); }
        }

        // Used to detect changes during a read. Even an equal fingerprint requires a byte comparison.
        internal static string Fingerprint(string dir)
        {
            var text = new StringBuilder();
            foreach (var file in SavedFiles(dir).OrderBy(f => f, StringComparer.Ordinal))
            {
                var info = new FileInfo(file);
                text.Append(Path.GetRelativePath(dir, file)).Append('|').Append(info.Length).Append('|')
                    .Append(info.LastWriteTimeUtc.Ticks).Append('\n');
            }
            return Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(text.ToString())));
        }

        private static long Size(string dir)
        {
            try { return Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length); }
            catch { return 0; }
        }
    }
}
