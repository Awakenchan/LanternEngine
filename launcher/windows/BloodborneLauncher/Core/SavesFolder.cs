// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;

namespace BloodborneLauncher.Core
{
    public static class SavesFolder
    {
        public static string Full(string path) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

        public static bool Same(string a, string b) =>
            string.Equals(Full(a), Full(b), OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

        public static bool HasSaves(string root)
        {
            try
            {
                if (root.Length == 0 || !Directory.Exists(root)) return false;
                var places = new List<string> { Path.Combine(root, "savedata") };
                var profiles = Path.Combine(root, "profiles");
                if (Directory.Exists(profiles))
                    places.AddRange(Directory.GetDirectories(profiles).Select(p => Path.Combine(p, "savedata")));
                return places.Any(p => Directory.Exists(p) &&
                                       Directory.EnumerateFiles(p, "userdata*", SearchOption.AllDirectories).Any());
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
        }

        public static bool InUse(bool launcherGame, Func<bool> processRunning) => launcherGame || processRunning();

        public const string LastRootKey = "last_saves_root";

        public static string? Moved(string userDir, string lastRoot, string root)
        {
            try
            {
                return userDir.Length == 0 && lastRoot.Length > 0 && root.Length > 0 && !Same(lastRoot, root) && Directory.Exists(lastRoot)
                    ? Full(lastRoot) : null;
            }
            catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException) { return null; }
        }

        public static string Absolute(string path)
        {
            try { return path.Length > 0 ? Path.GetFullPath(path) : path; }
            catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException) { return path; }
        }

        public static string ForLog(string path)
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return home.Length > 3 ? path.Replace(home, OperatingSystem.IsWindows() ? "%USERPROFILE%" : "~", StringComparison.OrdinalIgnoreCase) : path;
        }
    }
}
