using System.Diagnostics;
using System.IO;
#if !AVALONIA
using System.Windows;
using System.Windows.Controls;
#endif

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// 0.9.34 beta 8 renames the launcher from "Bloodborne Launcher.exe" to "Lantern Engine.exe".
    /// Packages still carry a small "Bloodborne Launcher.exe" forwarder (launcher\LanternForward):
    /// launchers before the rename update by that name and restart it, and it opens Lantern Engine.
    /// Once here: the launcher copied under its new name (when an old launcher updated only itself),
    /// shortcuts on the desktop, in the Start menu and on the taskbar pointed at the new name, and
    /// the old file deleted, so the folder holds only Lantern Engine.exe and Bloodborne.exe. A window
    /// says so while it works; the forwarder an update brings back is deleted quietly.
    /// </summary>
    public static class LanternRename
    {
        private const long ForwarderMaxBytes = 1_000_000; // the forwarder is ~170 KB, the launcher ~75 MB

        /// <summary>Returns true when this process should end now (it started the renamed launcher).</summary>
        public static bool Run(string[] args)
        {
#if AVALONIA
            // Only Windows packages ever had the old name.
            if (!OperatingSystem.IsWindows()) return false;
#endif
            try
            {
                var self = Environment.ProcessPath;
                if (self == null) return false;
                var folder = Path.GetDirectoryName(self)!;
                var renamed = Path.Combine(folder, Updater.ExeName);
                var old = Path.Combine(folder, Updater.OldExeName);
                var runningOld = string.Equals(Path.GetFileName(self), Updater.OldExeName, StringComparison.OrdinalIgnoreCase);
                if (runningOld)
                {
                    // The full launcher under the old name (an old launcher updated only itself): it
                    // becomes Lantern Engine.exe, which finishes the job (this file is in use meanwhile).
                    Show(new() { ("Renaming the launcher to Lantern Engine.exe", () =>
                    {
                        if (!File.Exists(renamed) || new FileInfo(renamed).Length < ForwarderMaxBytes ||
                            File.GetLastWriteTimeUtc(renamed) < File.GetLastWriteTimeUtc(self))
                            Retry(() => File.Copy(self, renamed, overwrite: true));
                    }) });
                    if (!File.Exists(renamed)) return false;
                    Process.Start(new ProcessStartInfo(renamed, args) { UseShellExecute = false, WorkingDirectory = folder });
                    return true;
                }
                if (!File.Exists(old)) return false;
                var oldIsFull = new FileInfo(old).Length > ForwarderMaxBytes;
                var shortcuts = Shortcuts.Pointing(old);
                var steps = new List<(string Text, Action Work)>();
#if AVALONIA
#pragma warning disable CA1416 // Windows only: Run returned at its start elsewhere
#endif
                if (shortcuts.Count > 0)
                    steps.Add(($"Pointing {shortcuts.Count} shortcut(s) at Lantern Engine.exe", () => Shortcuts.Retarget(shortcuts, renamed)));
#if AVALONIA
#pragma warning restore CA1416
#endif
                // The old launcher or the forwarder that started this one may still be closing.
                steps.Add(("Removing Bloodborne Launcher.exe", () => Retry(() => File.Delete(old), attempts: 40)));
                if (oldIsFull || shortcuts.Count > 0)
                    Show(steps);
                else
                    _ = Task.Run(() => { try { steps[^1].Work(); } catch (Exception) { } });
            }
            catch (Exception)
            {
                // Never stop the launcher from starting.
            }
            return false;
        }

        private static void Retry(Action action, int attempts = 10)
        {
            for (var attempt = 1; ; attempt++)
            {
                try { action(); return; }
                catch (Exception e) when (attempt < attempts && e is IOException or UnauthorizedAccessException)
                {
                    Thread.Sleep(250);
                }
            }
        }

        private static void Show(List<(string Text, Action Work)> steps)
        {
#if AVALONIA
            Views.WorkWindow.Run("Updating to Lantern Engine",
                "The launcher has a new name. Renaming files and fixing shortcuts; your game, saves, " +
                "settings and mods stay as they are. Don't close this window: it takes a few seconds.", steps, showSteps: true);
            return;
#else
            if (steps.Count == 0) return;
            var status = new TextBlock { Margin = new Thickness(0, 10, 0, 0), Opacity = 0.8 };
            var bar = new ProgressBar { Height = 6, Minimum = 0, Maximum = steps.Count, Margin = new Thickness(0, 12, 0, 0) };
            var window = new Window
            {
                Title = "Lantern Engine",
                Width = 440,
                SizeToContent = SizeToContent.Height,
                ResizeMode = ResizeMode.NoResize,
                WindowStartupLocation = WindowStartupLocation.CenterScreen,
                WindowStyle = WindowStyle.ToolWindow,
                Topmost = true,
                Content = new StackPanel
                {
                    Margin = new Thickness(22),
                    Children =
                    {
                        new TextBlock { Text = "Updating to Lantern Engine", FontSize = 16, FontWeight = FontWeights.SemiBold },
                        new TextBlock
                        {
                            Text = "The launcher has a new name. Renaming files and fixing shortcuts; your game, saves, " +
                                   "settings and mods stay as they are. Don't close this window: it takes a few seconds.",
                            TextWrapping = TextWrapping.Wrap,
                            Margin = new Thickness(0, 8, 0, 0),
                        },
                        status,
                        bar,
                    },
                },
            };
            window.Closing += (_, e) => e.Cancel = bar.Value < steps.Count; // only the work closes it
            window.Loaded += async (_, _) =>
            {
                for (var i = 0; i < steps.Count; ++i)
                {
                    status.Text = steps[i].Text + "…";
                    var work = steps[i].Work;
                    try { await Task.Run(work); } catch (Exception) { /* the next step still runs */ }
                    bar.Value = i + 1;
                }
                status.Text = "Done.";
                await Task.Delay(400);
                window.Close();
            };
            var mode = Application.Current.ShutdownMode;
            Application.Current.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            window.ShowDialog();
            Application.Current.ShutdownMode = mode;
#endif
        }

        /// <summary>Shortcuts (.lnk) on the desktop, in the Start menu and pinned to the taskbar.</summary>
#if AVALONIA
        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
#endif
        internal static class Shortcuts
        {
            private static IEnumerable<string> Folders()
            {
                yield return Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
                yield return Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory);
                yield return Environment.GetFolderPath(Environment.SpecialFolder.Programs);
                yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "Microsoft", "Internet Explorer", "Quick Launch", "User Pinned", "TaskBar");
            }

            public static List<string> Pointing(string target)
            {
                var found = new List<string>();
                var wanted = Path.GetFullPath(target);
                foreach (var folder in Folders().Where(Directory.Exists))
                {
                    IEnumerable<string> links;
                    try { links = Directory.EnumerateFiles(folder, "*.lnk", SearchOption.AllDirectories).ToList(); }
                    catch (Exception) { continue; }
                    foreach (var link in links)
                    {
                        if (string.Equals(ShellLinks.TargetOf(link), wanted, StringComparison.OrdinalIgnoreCase))
                            found.Add(link);
                    }
                }
                return found;
            }

            public static void Retarget(List<string> links, string target)
            {
                foreach (var link in links)
                {
                    try
                    {
                        ShellLinks.Change(link, shortcut =>
                        {
                            shortcut.SetPath(target);
                            shortcut.SetWorkingDirectory(Path.GetDirectoryName(target) ?? "");
                            shortcut.SetIconLocation(target, 0);
                        });
                    }
                    catch (Exception) { }
                }
            }

            public static void Restyle(IEnumerable<string> links, string icon)
            {
                foreach (var link in links)
                {
                    try
                    {
                        if (ShellLinks.Read(link) is { IconIndex: 0 } current &&
                            string.Equals(current.IconPath, icon, StringComparison.OrdinalIgnoreCase)) continue;
                        ShellLinks.Change(link, shortcut => shortcut.SetIconLocation(icon, 0));
                    }
                    catch (Exception) { }
                }
            }
        }
    }
}
