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
}
