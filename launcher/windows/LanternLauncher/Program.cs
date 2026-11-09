using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Styling;
using Avalonia.Threading;

namespace BloodborneLauncher;

internal static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        NativeRuntime.Prepare();
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args, ShutdownMode.OnMainWindowClose);
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<LanternApp>().UsePlatformDetect();
}

/// <summary>The application: theme, the checks made once at start, and the crash log (App.xaml.cs in WPF).</summary>
internal sealed class LanternApp : Application
{
    private static readonly string CrashLog = Path.Combine(Core.Paths.ConfigDir, "crash.log");
    private static bool _errorShown;
    public static int OldRuntimeRemoved { get; private set; }
    public static int OldExtractsRemoved { get; private set; }
    public static long OldExtractBytes { get; private set; }

    public override void Initialize()
    {
        RequestedThemeVariant = ThemeVariant.Dark;
        Styles.Add(new Avalonia.Themes.Fluent.FluentTheme());
        Styles.Add(new LanternStyles());
    }

    public override void OnFrameworkInitializationCompleted()
    {
        L.Initialize();
        // A UI-thread fault is recoverable; the other two are only logged, so a hard exit still
        // leaves evidence behind.
        Dispatcher.UIThread.UnhandledException += (_, args) =>
        {
            Record("Dispatcher", args.Exception);
            args.Handled = true;
            if (_errorShown) return;
            _errorShown = true;
            try
            {
                Views.Prompt.Tell(L.T("Unexpected error"), L.F($"{args.Exception.Message}\n\nLogged to:\n{CrashLog}"));
            }
            catch (Exception)
            {
                // The log has it.
            }
            finally
            {
                _errorShown = false;
            }
        };
        AppDomain.CurrentDomain.UnhandledException += (_, args) => Record("AppDomain", args.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            Record("UnobservedTask", args.Exception);
            args.SetObserved();
        };

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var args = desktop.Args ?? Array.Empty<string>();
            // The launcher's new name (0.9.34 beta 8): once, before anything else (Windows packages only).
            if (Core.LanternRename.Run(args))
            {
                desktop.Shutdown();
                return;
            }
            if (HandOverToPackage(args))
            {
                desktop.Shutdown();
                return;
            }
            string[] failed = Array.Empty<string>();
            RemoveOldLauncherExtracts();
            // An update to the engine\ layout: the old top-level engine folders go first.
            if (Core.Paths.DetectPortDir() is { } package)
            {
                RemoveOldLauncherRuntime();
                FixShippedMouseBinds(package);
                failed = Core.LayoutCleanup.Run(package).ToArray();
            }
            var window = new MainWindow();
            desktop.MainWindow = window;
            if (failed.Length > 0)
                window.Opened += (_, _) => Dispatcher.UIThread.Post(() =>
                    Views.Prompt.Tell("Lantern Engine", "These old files could not be removed (in use?) and can be deleted by hand:\n\n" +
                                                        string.Join("\n", failed)), DispatcherPriority.Background);
        }
        base.OnFrameworkInitializationCompleted();
    }

    private static void RemoveOldLauncherExtracts()
    {
        if (!OperatingSystem.IsWindows() || System.Runtime.CompilerServices.RuntimeFeature.IsDynamicCodeSupported) return;
        try
        {
            var bases = new[] { Environment.GetEnvironmentVariable("DOTNET_BUNDLE_EXTRACT_BASE_DIR"), Path.Combine(Path.GetTempPath(), ".net") }
                .Where(b => !string.IsNullOrEmpty(b)).Select(b => Path.GetFullPath(b!)).Distinct(StringComparer.OrdinalIgnoreCase);
            foreach (var root in bases)
            {
                foreach (var app in new[] { "Lantern Engine", "Bloodborne Launcher" })
                {
                    var folder = Path.Combine(root, app);
                    if (!Directory.Exists(folder)) continue;
                    foreach (var version in Directory.GetDirectories(folder))
                    {
                        var files = Directory.GetFiles(version, "*", SearchOption.AllDirectories);
                        if (files.Any(f => !Path.GetFileName(f).EndsWith("_cor3.dll", StringComparison.OrdinalIgnoreCase))) continue;
                        var bytes = files.Sum(f => new FileInfo(f).Length);
                        try
                        {
                            Directory.Delete(version, recursive: true);
                            OldExtractsRemoved++;
                            OldExtractBytes += bytes;
                        }
                        catch (Exception)
                        {
                        }
                    }
                    try
                    {
                        if (!Directory.EnumerateFileSystemEntries(folder).Any()) Directory.Delete(folder);
                    }
                    catch (Exception)
                    {
                    }
                }
            }
        }
        catch (Exception)
        {
        }
    }

    private static void RemoveOldLauncherRuntime()
    {
        if (OperatingSystem.IsWindows() || System.Runtime.CompilerServices.RuntimeFeature.IsDynamicCodeSupported) return;
        try
        {
            var folder = AppContext.BaseDirectory;
            if (!File.Exists(Path.Combine(folder, "Lantern Engine.runtimeconfig.json"))) return;
            foreach (var file in Directory.GetFiles(folder))
            {
                var name = Path.GetFileName(file);
                var old = name.EndsWith(".dll", StringComparison.Ordinal) ||
                          name is "createdump" or "Lantern Engine.deps.json" or "Lantern Engine.runtimeconfig.json" or "Lantern Engine.pdb" ||
                          (name.EndsWith(".so", StringComparison.Ordinal) &&
                           (name.StartsWith("libSystem.", StringComparison.Ordinal) || name.StartsWith("libclr", StringComparison.Ordinal) ||
                            name.StartsWith("libcoreclr", StringComparison.Ordinal) || name.StartsWith("libmscord", StringComparison.Ordinal) ||
                            name is "libhostfxr.so" or "libhostpolicy.so"));
                if (old)
                    try { File.Delete(file); OldRuntimeRemoved++; } catch (Exception) { }
            }
        }
        catch (Exception)
        {
        }
    }

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
            var name = OperatingSystem.IsWindows() ? Core.Updater.ExeName : Path.GetFileNameWithoutExtension(Core.Updater.ExeName);
            var launcher = Path.Combine(package, name);
            if (!Core.Paths.IsPortDir(package) || !File.Exists(launcher) ||
                string.Equals(Path.GetFullPath(launcher), Path.GetFullPath(self),
                    OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
                return false;
            var start = new System.Diagnostics.ProcessStartInfo(launcher) { UseShellExecute = false, WorkingDirectory = package };
            foreach (var arg in args) start.ArgumentList.Add(arg);
            start.Environment["BB_LAUNCHER_HANDOVER"] = "1";
            System.Diagnostics.Process.Start(start)?.Dispose();
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    /// <summary>Ends the launcher (an update restarts it, a layout change starts it again).</summary>
    public static void Quit()
    {
        if (Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.Shutdown();
        else
            Environment.Exit(0);
    }

    /// <summary>Packages before 0.9.34 beta 9 shipped lock on as Mouse3 and strong attack as Mouse2,
    /// meant as middle and right click; the engine counts buttons as SDL does (2 middle, 3 right),
    /// so they were swapped. Fixed once, and only while both are still those shipped values.</summary>
    private static void FixShippedMouseBinds(string package)
    {
        try
        {
            var ini = Path.Combine(package, "lantern.ini");
            if (!File.Exists(ini)) return;
            var lines = File.ReadAllLines(ini);
            var lockOn = Array.FindIndex(lines, l => l.Trim() == "bind.lock_on=Mouse3");
            var strong = Array.FindIndex(lines, l => l.Trim() == "bind.strong_attack=Mouse2");
            if (lockOn < 0 || strong < 0) return;
            lines[lockOn] = "bind.lock_on=Mouse2";
            lines[strong] = "bind.strong_attack=Mouse3";
            File.WriteAllLines(ini, lines);
        }
        catch (Exception)
        {
            // The binds stay as they were; the launcher's Controls page can change them.
        }
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
