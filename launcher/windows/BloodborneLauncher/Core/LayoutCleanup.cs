using System.IO;
#if !AVALONIA
using System.Windows;
using System.Windows.Controls;
#endif

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Packages since 0.9.30 keep the engine in engine\, so the top of the folder holds only the
    /// launcher, Bloodborne.exe and the player's files. An update copies the new layout over an
    /// older one, which leaves the engine's old top-level folders behind: they are removed once,
    /// at the next start, behind a window that says so. The player's files (lantern.ini, user, mods, patches,
    /// logs, network, engine-assets, fsr4_411) are never touched; the old out\ only held the engine
    /// and files each start makes again.
    /// </summary>
    public static class LayoutCleanup
    {
        private static readonly string[] OldEngine =
            { "out", "scripts", "tools", "fsr4_shaders", "fsr4vk", "licenses", "gamecontrollerdb.txt" };
        /// <summary>The engine's files in the old top-level patches\. The folder itself is the player's
        /// (GameEnvironment.PatchesDir: their own XML patches): it stays, with whatever else is in it.</summary>
        private static readonly string[] OldEnginePatches = { "Bloodborne.xml", "Bloodborne-1.09-sites.json", "redzone.json" };

        /// <summary>The old top-level engine entries of a package that has the new engine\.</summary>
        public static List<string> Leftovers(string? package)
        {
            var found = new List<string>();
            // Only an installed package: its package.json and a built engine in engine\ (a source
            // checkout also has engine\scripts, beside out\ and tools\ that are not leftovers).
            if (string.IsNullOrEmpty(package) || Paths.EngineDirOf(package) == package ||
                !File.Exists(Path.Combine(package, "package.json")) ||
                !File.Exists(Path.Combine(package, "engine", "out", "bb-probe.exe")))
                return found;
            foreach (var name in OldEngine)
            {
                var path = Path.Combine(package, name);
                if (Directory.Exists(path) || File.Exists(path)) found.Add(path);
            }
            foreach (var name in OldEnginePatches)
            {
                var path = Path.Combine(package, "patches", name);
                if (File.Exists(path)) found.Add(path);
            }
            // 0.9.32 starts the game natively (engine\out\bb-start.exe): the Python scripts and the
            // Python that ran them are no longer part of the engine.
            if (File.Exists(Path.Combine(package, "engine", "out", "bb-start.exe")))
            {
                foreach (var name in new[] { "scripts", "tools" })
                {
                    var path = Path.Combine(package, "engine", name);
                    if (Directory.Exists(path)) found.Add(path);
                }
            }
            return found;
        }

        /// <summary>Deletes a file or folder tree. OneDrive marks folders read-only, which a plain
        /// recursive delete refuses ("in use?" on every start): the attribute is cleared first. A link
        /// (junction or symbolic link) is removed itself, never what it points to.</summary>
        private static void Remove(string path)
        {
            // The player may have edited the old Bloodborne.xml: it is set aside (patches only reads
            // *.xml), not deleted.
            if (Path.GetFileName(path) == "Bloodborne.xml" && File.Exists(path))
            {
                File.Move(path, path + ".bak", overwrite: true);
                return;
            }
            if (File.Exists(path))
            {
                File.SetAttributes(path, FileAttributes.Normal);
                File.Delete(path);
                return;
            }
            var dir = new DirectoryInfo(path);
            if (!dir.Exists) return;
            dir.Attributes &= ~FileAttributes.ReadOnly;
            if (dir.LinkTarget == null)
            {
                foreach (var entry in dir.EnumerateFileSystemInfos())
                    Remove(entry.FullName);
            }
            dir.Delete(recursive: false);
        }

        /// <summary>Removes the leftovers behind a "please wait" window; returns what could not go.</summary>
        public static List<string> Run(string package)
        {
            var leftovers = Leftovers(package);
            var failed = new List<string>();
            if (leftovers.Count == 0) return failed;
#if AVALONIA
            var work = leftovers.Select(path => ("", (Action)(() =>
            {
                try { Remove(path); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { lock (failed) failed.Add(path); }
            }))).ToList();
            Views.WorkWindow.Run("New version: cleaning up",
                "Removing the old engine files from the Lantern Engine folder. Your saves, settings and mods stay. Please wait…", work, showSteps: false);
            return failed;
#else

            var bar = new ProgressBar { Height = 6, Minimum = 0, Maximum = leftovers.Count, Margin = new Thickness(0, 14, 0, 0) };
            var window = new Window
            {
                Title = "Lantern Engine",
                Width = 420,
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
                        new TextBlock { Text = "New version: cleaning up", FontSize = 16, FontWeight = FontWeights.SemiBold },
                        new TextBlock
                        {
                            Text = "Removing the old engine files from the Lantern Engine folder. Your saves, settings and mods stay. Please wait…",
                            TextWrapping = TextWrapping.Wrap,
                            Margin = new Thickness(0, 8, 0, 0),
                        },
                        bar,
                    },
                },
            };
            window.Loaded += async (_, _) =>
            {
                for (var i = 0; i < leftovers.Count; ++i)
                {
                    var path = leftovers[i];
                    try
                    {
                        await Task.Run(() => Remove(path));
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                    {
                        failed.Add(path);
                    }
                    bar.Value = i + 1;
                }
                window.Close();
            };
            // The main window comes after this one: closing it must not end the application.
            var mode = Application.Current.ShutdownMode;
            Application.Current.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            window.ShowDialog();
            Application.Current.ShutdownMode = mode;
            return failed;
#endif
        }
    }
}
