using System.Diagnostics;
using System.IO;
using System.Text;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Starts and stops the game and streams its output. The Windows port's run.py does the
    /// preparation (mods, patches, the linked image) and starts bin\bb-probe.exe; packaged builds
    /// run it inside Bloodborne.exe (--run). A custom command can replace it.
    /// </summary>
    public sealed class GameRunner
    {
        private Process? _process;
        private StreamWriter? _log;

        /// <summary>Linux: asks a process to end (SIGTERM) through kill(1), present on every distribution.</summary>
        private static bool Terminate(int pid)
        {
            using var kill = Process.Start(new ProcessStartInfo("kill", $"-TERM {pid}") { UseShellExecute = false, CreateNoWindow = true });
            if (kill == null) return false;
            kill.WaitForExit(2000);
            return kill.HasExited && kill.ExitCode == 0;
        }

        /// <summary>Everything the last session printed: logs\last_session.log in the engine folder.</summary>
        public static string SessionLog => Path.Combine(Paths.IsPortDir(Paths.PortDir) ? Paths.PortDir : Paths.ConfigDir, "logs", "last_session.log");

        public bool Running => _process != null;
        public DateTime StartedAt { get; private set; }

        public event Action<string>? Output;
        public event Action<int>? Exited;

        public static bool Custom(LauncherSettings s) => s.Str("launch_method") == "custom";

        /// <summary>Whether a file can be made in <paramref name="dir"/> (made if missing): not under Program
        /// Files without administrator rights, where the engine's first write fails before any log.</summary>
        private static bool Writable(string dir)
        {
            try
            {
                Directory.CreateDirectory(dir);
                var probe = Path.Combine(dir, ".write-test");
                File.WriteAllText(probe, "");
                File.Delete(probe);
                return true;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return false;
            }
        }

        /// <summary>Starts the game; returns null, or why it could not start.</summary>
        public string? Start(LauncherSettings s)
        {
            if (_process != null)
                return L.T("The game is already running.");
            SdlPad.Stop(); // the game takes the controllers

            ProcessStartInfo info;
            if (Custom(s))
            {
                var command = Paths.Expand(s.Str("custom_command").Trim());
                if (command.Length == 0)
                    return L.T("No custom command is set. Choose one in Settings › Launch.");
                info = new ProcessStartInfo(command) { Arguments = s.Str("custom_args") };
            }
            else
            {
                // Linux (the Avalonia launcher): the package's out/bb-start, a script that runs the engine's
                // run.sh with --game-dir and this same environment (engine/packaging/linux).
                if (!Paths.IsPortDir(Paths.PortDir))
                    return "The engine folder is not set. Choose it in Settings › Lantern Engine.";
                if (!Writable(Path.Combine(Paths.EngineDir, "out")))
                    return $"Windows does not let the game write into its own folder ({Paths.PortDir}), which it needs for its " +
                           "prepared files, logs and saves. This happens under Program Files: move the Lantern Engine folder " +
                           "somewhere else, such as C:\\Games or your Documents.";
                if (File.Exists(Paths.Start))
                {
                    // The engine's native start: the boot image, mods, patches, then bb-probe.
                    info = new ProcessStartInfo(Paths.Start);
                }
                else
                {
                    // An engine older than 0.9.32 (a launcher update before the engine's): its Python
                    // runs scripts\run_windows.py.
                    var python = File.Exists(Paths.Python) ? Paths.Python : "py";
                    info = new ProcessStartInfo(python);
                    if (python == "py") info.ArgumentList.Add("-3");
                    info.ArgumentList.Add(Path.Combine(Paths.EngineDir, "scripts", "run_windows.py"));
                }
                info.ArgumentList.Add("--game-dir");
                info.ArgumentList.Add(GameEnvironment.GameDir(s));
            }

            info.WorkingDirectory = Paths.IsPortDir(Paths.PortDir) ? Paths.EngineDir : Environment.CurrentDirectory;
            foreach (var (key, value) in GameEnvironment.Build(s))
                info.Environment[key] = value;
            info.Environment["PYTHONUNBUFFERED"] = "1";
            // UTF-8 whatever the Windows language: the game title (param.sfo) has a ™ that GBK and other
            // code pages cannot write.
            info.Environment["PYTHONUTF8"] = "1";
            info.Environment["PYTHONIOENCODING"] = "utf-8";
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.RedirectStandardOutput = true;
            info.RedirectStandardError = true;
            info.StandardOutputEncoding = Encoding.UTF8;
            info.StandardErrorEncoding = Encoding.UTF8;

            try
            {
                var process = new Process { StartInfo = info, EnableRaisingEvents = true };
                OpenLog();
                // The first line of every log, the Output page's export included: which build ran.
                Emit($"Lantern Engine {Updater.CurrentLabel}, engine {Updater.LocalEngine() ?? "unknown"}, {DateTime.Now:yyyy-MM-dd HH:mm:ss}, saves {SavesFolder.ForLog(GameEnvironment.SavesRoot(s))}");
                process.OutputDataReceived += (_, e) => { if (e.Data != null && !LiveGame.Take(e.Data)) Emit(e.Data); };
                process.ErrorDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data); };
                process.Exited += (_, _) =>
                {
                    var code = -1;
                    try { process.WaitForExit(); code = process.ExitCode; } catch { /* gone */ }
                    _process = null;
                    LiveGame.Clear();
                    CloseLog();
                    Exited?.Invoke(code);
                };
                process.Start();
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                _process = process;
                StartedAt = DateTime.Now;
                return null;
            }
            catch (Exception e)
            {
                _process = null;
                CloseLog();
                return L.T("Could not start: ") + e.Message;
            }
        }

        private void OpenLog()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(SessionLog)!);
                // The one before is kept: a crash's log survives starting the game again.
                try
                {
                    if (File.Exists(SessionLog))
                        File.Copy(SessionLog, Path.Combine(Path.GetDirectoryName(SessionLog)!, "previous_session.log"), overwrite: true);
                }
                catch (IOException) { /* still open elsewhere: only this session's is kept */ }
                var stream = new FileStream(SessionLog, FileMode.Create, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
                _log = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };
            }
            catch { _log = null; /* read-only folder: the Log tab still has it */ }
        }

        private void Emit(string line)
        {
            lock (this)
            {
                try { _log?.WriteLine(line); } catch { /* disk full */ }
            }
            Output?.Invoke(line);
        }

        private void CloseLog()
        {
            lock (this)
            {
                _log?.Dispose();
                _log = null;
            }
        }

        /// <summary>Asks the game window to close, and kills the process tree if it has not ended in time.
        /// The engine's own close (window close request) saves the shader cache, lets the GPU finish
        /// its submitted work and ends itself within its 4 s watchdog (BB_EXIT_WATCHDOG_MS): killing it
        /// earlier, mid GPU work, made the driver tear down a busy context and stalled the desktop. So
        /// the wait is longer than that watchdog when the window took the request, and short when no
        /// window did (still loading). Nothing here blocks the UI thread.</summary>
        public async Task StopAsync()
        {
            var process = _process;
            if (process == null)
                return;

            var probes = Process.GetProcessesByName("bb-probe");
            try
            {
                var asked = false;
                foreach (var probe in probes)
                {
                    // Linux has no window close message for a process: SIGTERM, which SDL turns into
                    // the same quit (cache saved, "Exit: clean shutdown"). Killed at once, the log
                    // just stopped and the cache of that session was lost.
                    if (!OperatingSystem.IsWindows())
                    {
                        try { asked |= Terminate(probe.Id); } catch { /* no kill(1) */ }
                        continue;
                    }
                    try { asked |= probe.CloseMainWindow(); } catch { /* no window */ }
                }
                try { asked |= process.CloseMainWindow(); } catch { /* no window */ }

                var graceMs = asked ? 6000 : 1500;
                var waits = new List<Task> { process.WaitForExitAsync() };
                foreach (var probe in probes)
                {
                    try { waits.Add(probe.WaitForExitAsync()); } catch { /* gone */ }
                }
                await Task.WhenAny(Task.WhenAll(waits), Task.Delay(graceMs)).ConfigureAwait(false);

                // Hard kill whatever is left, off the UI thread (Kill and its wait can take a moment
                // while Windows frees the game's memory pool).
                await Task.Run(() =>
                {
                    foreach (var probe in probes)
                    {
                        try { if (!probe.HasExited) probe.Kill(entireProcessTree: true); } catch { /* already gone */ }
                    }
                    try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch { /* already gone */ }
                }).ConfigureAwait(false);
            }
            finally
            {
                foreach (var probe in probes)
                    probe.Dispose();
            }
        }
    }
}
