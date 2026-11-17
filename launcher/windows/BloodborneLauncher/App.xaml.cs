using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Threading;

namespace BloodborneLauncher
{
    public partial class App : Application
    {
        private static readonly string CrashLog = Path.Combine(Core.Paths.ConfigDir, "crash.log");

        /// <summary>When this launcher runs outside a package and the settings name one with its own
        /// launcher, that launcher is started with the same arguments (once: BB_LAUNCHER_HANDOVER).
        /// A build from the source checkout keeps running itself.</summary>
        private static bool HandOverToPackage(string[] args)
        {
            try
            {
                if (Environment.GetEnvironmentVariable("BB_LAUNCHER_HANDOVER") == "1" ||
                    Core.Paths.DetectPortDir() != null || Core.Paths.SourceRoot != null ||
                    Environment.ProcessPath is not { } self || !File.Exists(Core.Paths.ConfigFile))
                    return false;
                using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Core.Paths.ConfigFile));
                if (!doc.RootElement.TryGetProperty("port_dir", out var value) || value.ValueKind != System.Text.Json.JsonValueKind.String)
                    return false;
                var package = Core.Paths.Expand(value.GetString() ?? "");
                var launcher = Path.Combine(package, Core.Updater.ExeName);
                if (!Core.Paths.IsPortDir(package) || !File.Exists(launcher) ||
                    string.Equals(Path.GetFullPath(launcher), Path.GetFullPath(self), StringComparison.OrdinalIgnoreCase))
                    return false;
                var start = new System.Diagnostics.ProcessStartInfo(launcher) { UseShellExecute = false, WorkingDirectory = package };
                foreach (var arg in args) start.ArgumentList.Add(arg);
                start.Environment["BB_LAUNCHER_HANDOVER"] = "1";
                System.Diagnostics.Process.Start(start)?.Dispose();
                return true;
            }
            catch (Exception)
            {
                return false; // runs itself, as before
            }
        }

        protected override void OnStartup(StartupEventArgs e)
        {
            L.Initialize();
            base.OnStartup(e);

            // A UI-thread fault is recoverable; the other two are only logged, so a hard exit
            // still leaves evidence behind.
            DispatcherUnhandledException += OnDispatcherException;
            AppDomain.CurrentDomain.UnhandledException += (_, args) =>
                Record("AppDomain", args.ExceptionObject as Exception);
            TaskScheduler.UnobservedTaskException += (_, args) =>
            {
                Record("UnobservedTask", args.Exception);
                args.SetObserved();
            };

            // The launcher's new name (0.9.34 beta 8): once, before anything else.
            if (Core.LanternRename.Run(e.Args))
            {
                Shutdown();
                return;
            }

            // A copy outside the package (dragged to the desktop) is never updated and drifted from
            // the package's own launcher (other pages, settings missing): it starts that one.
            if (HandOverToPackage(e.Args))
            {
                Shutdown();
                return;
            }

            // An update to the engine\ layout: the old top-level engine folders go first.
            if (Core.Paths.DetectPortDir() is { } package)
            {
                FixShippedMouseBinds(package);
                var failed = Core.LayoutCleanup.Run(package);
                if (failed.Count > 0)
                    MessageBox.Show("These old files could not be removed (in use?) and can be deleted by hand:\n\n" +
                                    string.Join("\n", failed), "Lantern Engine", MessageBoxButton.OK, MessageBoxImage.Information);
            }
        }

        /// <summary>Packages before 0.9.34 beta 9 shipped lock on as Mouse3 and strong attack as Mouse2,
        /// meant as middle and right click; the engine counts buttons as SDL does (2 middle, 3 right),
        /// so they were swapped. Fixed once, and only while both are still those shipped values.</summary>
        private static void FixShippedMouseBinds(string package)
        {
            var ini = new Core.IniFile(Path.Combine(package, "lantern.ini"));
            ini.Load();
            if (ini.LastRead != Core.TextState.Read) return;
            if (ini.Raw("bind.lock_on") != "Mouse3" || ini.Raw("bind.strong_attack") != "Mouse2") return;
            ini.Set("bind.lock_on", "Mouse2");
            ini.Set("bind.strong_attack", "Mouse3");
            try
            {
                ini.Save();
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                System.Diagnostics.Trace.TraceWarning($"lantern.ini: the shipped mouse binds were not swapped ({e.Message}); the next start tries again");
            }
        }

        private void OnDispatcherException(object sender, DispatcherUnhandledExceptionEventArgs args)
        {
            // WPF's window frame (WindowChrome) throws when Windows switches desktop composition
            // off for a moment (a display mode change or a driver reset as a game exits): nothing
            // is wrong with the launcher, and the frame is redrawn when composition returns.
            if (args.Exception is System.Runtime.InteropServices.COMException { HResult: unchecked((int)0x80263001) })
            {
                args.Handled = true;
                return;
            }
            Record("Dispatcher", args.Exception);
            MessageBox.Show(L.F($"{args.Exception.Message}\n\nLogged to:\n{CrashLog}"),
                L.T("Unexpected error"), MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        }

        private static void Record(string source, Exception? exception)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(CrashLog)!);
                var entry = new StringBuilder()
                    .AppendLine(new string('=', 70))
                    .AppendLine($"{DateTime.Now:yyyy-MM-dd HH:mm:ss}  [{source}]")
                    .AppendLine(exception?.ToString() ?? L.T("(no exception object)"))
                    .AppendLine();
                File.AppendAllText(CrashLog, entry.ToString());
            }
            catch
            {
                // Logging must never be the thing that takes the app down.
            }
        }
    }
}
