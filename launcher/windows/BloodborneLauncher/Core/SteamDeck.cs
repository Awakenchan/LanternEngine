// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.IO;

namespace BloodborneLauncher.Core
{
    public static class SteamDeck
    {
        private static readonly string[] Models = { "Jupiter", "Galileo" };

        public static bool Detected { get; } = Detect();

        public static bool GameMode { get; } = !OperatingSystem.IsWindows() &&
            (Environment.GetEnvironmentVariable("SteamGamepadUI") == "1" ||
             string.Equals(Environment.GetEnvironmentVariable("XDG_CURRENT_DESKTOP"), "gamescope", StringComparison.OrdinalIgnoreCase) ||
             !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("GAMESCOPE_WAYLAND_DISPLAY")));

        private static bool Detect()
        {
            if (Environment.GetEnvironmentVariable("SteamDeck") == "1") return true;
            try
            {
                if (OperatingSystem.IsWindows())
                {
                    using var bios = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\BIOS");
                    return bios?.GetValue("SystemManufacturer") is string maker && maker.Trim() == "Valve" &&
                           bios.GetValue("SystemProductName") is string product && Models.Contains(product.Trim());
                }
                string Read(string name)
                {
                    var path = Path.Combine("/sys/devices/virtual/dmi/id", name);
                    return File.Exists(path) ? File.ReadAllText(path).Trim() : "";
                }
                return Read("sys_vendor") == "Valve" && (Models.Contains(Read("product_name")) || Models.Contains(Read("board_name")));
            }
            catch (Exception)
            {
                return false;
            }
        }

        public static void OpenKeyboard()
        {
            if (!GameMode) return;
            foreach (var (program, argument) in new[] { ("steam", "steam://open/keyboard"), ("xdg-open", "steam://open/keyboard") })
            {
                try
                {
                    var start = new ProcessStartInfo(program) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
                    start.ArgumentList.Add(argument);
                    Process.Start(start)?.Dispose();
                    return;
                }
                catch (Exception)
                {
                }
            }
        }
    }
}
