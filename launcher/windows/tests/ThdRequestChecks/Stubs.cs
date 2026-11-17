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
        public static string ProfilePath => Path.Combine(Path.GetTempPath(), "lantern-thd-request-checks-missing", "patches.json");
    }

    public static class LauncherSettings
    {
        public const string OnlineCommunity = "https://thehuntersdream.com";
    }
}
