// SPDX-License-Identifier: GPL-3.0-or-later
global using L = BloodborneLauncher.Core.Localization;
using System.Globalization;

namespace BloodborneLauncher.Core
{
    public static class Localization
    {
        public static string T(string text) => text;
        public static string F(FormattableString text) => text.ToString(CultureInfo.InvariantCulture);
    }

    public static class Patches
    {
        public static string ProfilePath => Path.Combine(Path.GetTempPath(), "lantern-online-checks-missing", "patches.json");
    }

    public sealed class LauncherSettings
    {
        private readonly Dictionary<string, int> _values = new();
        public int Int(string key) => _values.GetValueOrDefault(key);
        public void Set(string key, int value) => _values[key] = value;
    }
}
