using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Keeps a player's saves from going missing. Save backups (SaveBackups) live in the saves
    /// folder and follow the settings; this does not:
    /// - a safety copy of every profile's savedata, at each start and when the game exits, in
    ///   %LOCALAPPDATA%\Lantern Engine\save-vault (the newest few per profile, whatever the Save
    ///   backups switch says): it outlives a deleted package folder (the default saves folder is
    ///   its user\), lost launcher settings and a moved saves folder;
    /// - at Play, when the current profile has no saves but a saves folder used before (or the
    ///   vault) has some, the launcher asks before the game starts a new one: it lost track of
    ///   them (settings reset, another launcher's settings folder, a new package folder).
    /// The vault keeps one folder per saves folder: place.txt (its path and profiles), then
    /// &lt;profile&gt;\&lt;time&gt;\savedata with a fingerprint, as SaveBackups does.
    /// </summary>
    public static class SaveGuard
    {
        public const int Keep = 5;
        private const string Stamp = "yyyy-MM-dd_HH-mm-ss";
        private const string PlaceFile = "place.txt";
        private const string FingerprintFile = "fingerprint.txt";
        private static readonly object Lock = new();

        public static string VaultDir =>
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Lantern Engine", "save-vault");

        /// <summary>Saves the launcher can point at instead: `Root` is a saves folder with
        /// `Profiles` holding saves; or, when that folder is gone, `Copy` is the vault's newest copy
        /// of the profile `Profiles` names.</summary>
        public sealed record Found(string Root, List<string> Profiles, DateTime Saved, string? Copy);

        /// <summary>A profile's savedata in the saves folder `root` (as PlayerProfiles.Folder).</summary>
        public static string SaveData(string root, string profile) =>
            Path.Combine(PlayerProfiles.IsMain(profile) ? root : Path.Combine(root, "profiles", PlayerProfiles.SafeName(profile)), "savedata");

        public static bool HasFiles(string dir)
        {
            try { return Directory.Exists(dir) && Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Any(); }
            catch { return false; }
        }

        public static bool HasHunters(string dir)
        {
            try
            {
                if (!Directory.Exists(dir)) return false;
                foreach (var file in Directory.EnumerateFiles(dir, "userdata000?", SearchOption.AllDirectories))
                {
                    var name = Path.GetFileName(file);
                    if (name.Length != 12 || !char.IsAsciiDigit(name[^1])) continue;
                    try
                    {
                        using var stream = File.OpenRead(file);
                        if (stream.Length != SaveSlot.FileSize) return true;
                        var head = new byte[64];
                        stream.ReadExactly(head);
                        if (Array.Exists(head, b => b != 0)) return true;
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                    {
                        return true;
                    }
                }
                return false;
            }
            catch
            {
                return HasFiles(dir);
            }
        }

        /// <summary>The profiles in `root` with saves (Main first) and the newest save time.</summary>
        public static (List<string> Profiles, DateTime Saved) ProfilesWithSaves(string root)
        {
            var names = new List<string> { PlayerProfiles.Main };
            try
            {
                var profiles = Path.Combine(root, "profiles");
                if (Directory.Exists(profiles))
                    names.AddRange(Directory.GetDirectories(profiles).Select(d => Path.GetFileName(d)!));
            }
            catch { }
            var found = new List<string>();
            var saved = DateTime.MinValue;
            foreach (var name in names)
            {
                var data = SaveData(root, name);
                if (!HasHunters(data)) continue;
                found.Add(name);
                try
                {
                    var newest = Directory.EnumerateFiles(data, "*", SearchOption.AllDirectories).Max(File.GetLastWriteTime);
                    if (newest > saved) saved = newest;
                }
                catch { }
            }
            return (found, saved);
        }

        private static string PlaceId(string root)
        {
            var full = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            return Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(full.ToLowerInvariant())))[..12];
        }

        private static bool SameDir(string a, string b)
        {
            try
            {
                return string.Equals(Path.GetFullPath(a).TrimEnd('\\', '/'), Path.GetFullPath(b).TrimEnd('\\', '/'),
                    OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
            }
            catch { return false; }
        }

        /// <summary>Copies each profile's savedata in `root` into the vault when it changed since its
        /// newest copy (the newest Keep are kept). On a worker thread; never throws. Returns how
        /// many profiles were copied.</summary>
        public static int Secure(string root)
        {
            lock (Lock)
            {
                try
                {
                    var (profiles, _) = ProfilesWithSaves(root);
                    if (profiles.Count == 0) return 0;
                    var place = Path.Combine(VaultDir, PlaceId(root));
                    Directory.CreateDirectory(place);
                    File.WriteAllText(Path.Combine(place, PlaceFile), Path.GetFullPath(root) + "\n" + string.Join("|", profiles) + "\n");
                    var copied = 0;
                    foreach (var profile in profiles)
                    {
                        var source = SaveData(root, profile);
                        var folder = Path.Combine(place, PlayerProfiles.SafeName(profile));
                        Directory.CreateDirectory(folder);
                        using var vaultGuard = new FileStream(Path.Combine(folder, ".vault.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
                        var print = SaveBackups.Fingerprint(source);
                        var copies = Copies(folder);
                        if (copies.FirstOrDefault() is { } newest && Read(Path.Combine(newest, FingerprintFile)) == print) continue;
                        var target = Path.Combine(folder, DateTime.Now.ToString(Stamp, CultureInfo.InvariantCulture));
                        if (Directory.Exists(target)) continue;
                        // Copied beside it first: a half-written copy never counts as the newest.
                        var staging = target + ".copying";
                        if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
                        SaveBackups.Copy(source, Path.Combine(staging, "savedata"));
                        File.WriteAllText(Path.Combine(staging, FingerprintFile), print);
                        Directory.Move(staging, target);
                        copied++;
                        foreach (var old in Copies(folder).Skip(Keep))
                        {
                            try { Directory.Delete(old, recursive: true); } catch { /* in use: next time */ }
                        }
                    }
                    return copied;
                }
                catch
                {
                    return 0;
                }
            }
        }

        public static string ProfileCopies(string root, string profile) =>
            Path.Combine(VaultDir, PlaceId(root), PlayerProfiles.SafeName(profile));

        /// <summary>A profile's copies in the vault, newest first.</summary>
        private static List<string> Copies(string folder)
        {
            if (!Directory.Exists(folder)) return new();
            return Directory.GetDirectories(folder)
                .Where(d => DateTime.TryParseExact(Path.GetFileName(d), Stamp, CultureInfo.InvariantCulture, DateTimeStyles.None, out _))
                .OrderByDescending(d => Path.GetFileName(d), StringComparer.Ordinal).ToList();
        }

        private static string? Read(string file)
        {
            try { return File.ReadAllText(file).Trim(); }
            catch { return null; }
        }

        /// <summary>When the current profile (`profile` in the saves folder `root`) has no saves:
        /// the saves the launcher knows elsewhere, newest first. Looks at the saves folders the vault
        /// has seen and those named by launcher settings files (this launcher's and the other
        /// settings folder's: before and after 0.9.37 beta 2). Empty when the profile has saves.</summary>
        public static List<Found> Elsewhere(string root, string profile, IEnumerable<string>? moreRoots = null)
        {
            var found = new List<Found>();
            try
            {
                if (HasHunters(SaveData(root, profile))) return found;
                var roots = new List<string>(moreRoots ?? Enumerable.Empty<string>());
                roots.AddRange(SettingsRoots());
                var vaultPlaces = new List<(string Root, string Dir)>();
                if (Directory.Exists(VaultDir))
                {
                    foreach (var place in Directory.GetDirectories(VaultDir))
                    {
                        var lines = (Read(Path.Combine(place, PlaceFile)) ?? "").Split('\n');
                        if (lines[0].Trim() is { Length: > 0 } path)
                        {
                            roots.Add(path);
                            vaultPlaces.Add((path, place));
                        }
                    }
                }
                var seen = new List<string>();
                foreach (var candidate in roots)
                {
                    if (candidate.Length == 0 || seen.Any(s => SameDir(s, candidate))) continue;
                    seen.Add(candidate);
                    if (SameDir(candidate, root)) continue;
                    var (profiles, saved) = ProfilesWithSaves(candidate);
                    if (profiles.Count > 0) found.Add(new Found(Path.GetFullPath(candidate), profiles, saved, null));
                }
                // Saves folders that are gone (a deleted package folder), and this one's own copies
                // (its savedata deleted): the vault's newest copy, of this profile or Main.
                foreach (var (path, dir) in vaultPlaces)
                {
                    if (!SameDir(path, root) && ProfilesWithSaves(path).Profiles.Count > 0) continue;
                    foreach (var name in new[] { profile, PlayerProfiles.Main }.Distinct(StringComparer.OrdinalIgnoreCase))
                    {
                        if (Copies(Path.Combine(dir, PlayerProfiles.SafeName(name))).FirstOrDefault(c => HasHunters(Path.Combine(c, "savedata"))) is not { } copy) continue;
                        var when = DateTime.TryParseExact(Path.GetFileName(copy), Stamp, CultureInfo.InvariantCulture, DateTimeStyles.None, out var t) ? t : DateTime.MinValue;
                        found.Add(new Found(path, new List<string> { name }, when, Path.Combine(copy, "savedata")));
                        break;
                    }
                }
            }
            catch
            {
                // Never stop the game from starting.
            }
            return found.OrderByDescending(f => f.Saved).ToList();
        }

        /// <summary>The saves folders named by launcher settings files: user_dir, else port_dir's user.</summary>
        private static IEnumerable<string> SettingsRoots()
        {
            var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            var files = new List<string>();
            foreach (var folder in new[] { "Lantern Engine", "bbport-launcher" })
            {
                var dir = Path.Combine(appData, folder);
                if (!Directory.Exists(dir)) continue;
                try { files.AddRange(Directory.GetFiles(dir, "settings*.json")); } catch { }
            }
            foreach (var file in files)
            {
                string? user = null, port = null;
                try
                {
                    using var doc = JsonDocument.Parse(File.ReadAllText(file));
                    if (doc.RootElement.TryGetProperty("user_dir", out var u) && u.ValueKind == JsonValueKind.String) user = u.GetString();
                    if (doc.RootElement.TryGetProperty("port_dir", out var p) && p.ValueKind == JsonValueKind.String) port = p.GetString();
                }
                catch { continue; }
                if (!string.IsNullOrWhiteSpace(user)) yield return Paths.Expand(user);
                else if (!string.IsNullOrWhiteSpace(port)) yield return Path.Combine(Paths.Expand(port), "user");
            }
        }

        /// <summary>Points the settings at the saves folder `found.Root` and lists its profiles; the
        /// current profile stays when it has saves there, else Main.</summary>
        public static void Use(LauncherSettings s, Found found)
        {
            s.Set("user_dir", found.Root);
            var names = PlayerProfiles.All(s).Concat(found.Profiles)
                .Where(p => !PlayerProfiles.IsMain(p)).Distinct(StringComparer.OrdinalIgnoreCase);
            s.Set("player_profiles", string.Join("|", names));
            if (!found.Profiles.Contains(PlayerProfiles.Current(s), StringComparer.OrdinalIgnoreCase))
                s.Set("player_profile", "");
        }

        /// <summary>At Play, with saves found elsewhere: asks (with the launcher's Prompt) whether to
        /// use them. Returns whether to start, and whether the settings or saves changed (the pages
        /// reload then).</summary>
        public static (bool Start, bool Changed) Ask(LauncherSettings s, Found found,
            Func<string, string, string[], int> choose, Action<string, string> tell)
        {
            var profile = PlayerProfiles.Current(s);
            var here = GameEnvironment.UserDir(s);
            if (found.Copy == null)
            {
                var choice = choose(L.T("Your saves are in another folder"),
                    L.F($"The profile {profile} has no saves in {here}.\n\n") +
                    L.F($"Bloodborne saves were found in {found.Root} (profiles: {string.Join(", ", found.Profiles)}), last saved {found.Saved:d MMM yyyy, HH:mm}.\n\n") +
                    L.T("\"Use that folder\" plays with them. Playing here starts a new game; the saves there stay as they are."),
                    new[] { L.T("Cancel"), L.T("Play here anyway"), L.T("Use that folder") });
                if (choice <= 0) return (false, false);
                if (choice == 1) return (true, false);
                Use(s, found);
                return (true, true);
            }
            var restore = choose(L.T("Your saves have a safety copy"),
                L.F($"The profile {profile} has no saves in {here}.\n\n") +
                L.F($"A safety copy of the {found.Profiles[0]} profile's saves from {found.Saved:d MMM yyyy, HH:mm} is kept in {VaultDir} ") +
                L.F($"(they were in {found.Root}).\n\n") +
                L.T("\"Restore it here\" copies them in. Playing here without them starts a new game; the copy stays either way."),
                new[] { L.T("Cancel"), L.T("Play here anyway"), L.T("Restore it here") });
            if (restore <= 0) return (false, false);
            if (restore == 1) return (true, false);
            try
            {
                Restore(s, found);
                return (true, true);
            }
            catch (Exception e)
            {
                tell(L.T("Your saves have a safety copy"), L.T("Could not restore it: ") + e.Message);
                return (false, false);
            }
        }

        /// <summary>Copies a vault copy's savedata in as the current profile's (which has none).</summary>
        public static void Restore(LauncherSettings s, Found found)
        {
            if (found.Copy == null) return;
            var target = SaveBackups.SaveData(s);
            if (HasHunters(target)) throw new IOException(L.T("This profile has saves now; nothing was replaced."));
            if (HasFiles(target)) SaveBackups.Create(s, beforeRestore: true);
            var staging = target + ".restoring";
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            SaveBackups.Copy(found.Copy, staging);
            if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
            Directory.Move(staging, target);
        }
    }
}
