using System.IO;
#if AVALONIA
using Avalonia.Threading;
#else
using System.Windows.Threading;
#endif

namespace BloodborneLauncher.Core
{
    public enum LogLevel { Info, Step, Good, Warn, Bad }

    /// <summary>
    /// The launcher's shared state: settings, lantern.ini, the game process and its output, and the
    /// activity log. Every edit calls SaveSoon, so nothing waits for the window to close and an
    /// in-game change to lantern.ini is only overwritten by a change made here.
    /// </summary>
    public sealed class Workspace
    {
        public static Workspace Current { get; } = new();

        public const int MaxOutputLines = 5000;

        public LauncherSettings Settings { get; } = new();
        public IniFile Ini { get; } = new();
        public GameRunner Game { get; } = new();
        public TrophyTracker Trophies { get; } = new();

        /// <summary>A trophy was unlocked (or the saves folder, and so the list, changed).</summary>
        public event Action? TrophiesChanged;

        private DispatcherTimer? _trophyTimer;
        private DispatcherTimer? _outputTimer;
        private readonly System.Collections.Concurrent.ConcurrentQueue<string> _pendingOutput = new();

        /// <summary>The game's output so far (this run), oldest first.</summary>
        public List<string> OutputLines { get; } = new();

        public event Action<string, LogLevel>? Logged;
        public event Action? Saved;
        public event Action? GameStateChanged;
        /// <summary>New output lines, in batches (on the UI thread).</summary>
        public event Action<IReadOnlyList<string>>? GameOutput;
        public event Action? OutputCleared;
        /// <summary>The port folder changed: pages that show paths or files reload.</summary>
        public event Action? PortChanged;
        public event Action? SettingsReloaded;

        private DispatcherTimer? _saveTimer;
        private DispatcherTimer? _playBackupTimer;
        private bool _iniWarned;

        /// <summary>The session being played, then the last one played (null before the first).</summary>
        public SessionStats? Session { get; private set; }
        /// <summary>The game ended by itself with an error (not stopped from here): exit code and session.</summary>
        public event Action<int, SessionStats>? GameCrashed;
        /// <summary>The game ended after The Hunter's Dream refused this PC's sign-in: account name and reason.</summary>
        public event Action<string, string>? SignInRefused;
        public event Action<string>? SeamlessRefused;
        private bool _stopping;

        public void Initialise()
        {
            Settings.Load();
            ResolvePort();

            // Trophies: what the saves folder already holds is read silently; new unlocks after
            // this are announced. trophies.log and last_run.log are checked every two seconds, so
            // games started without the launcher (Play Bloodborne.exe, Steam) are announced too.
            Trophies.SetUserDir(GameEnvironment.UserDir(Settings));
            _trophyTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            _trophyTimer.Tick += (_, _) =>
            {
                if (Settings.ReloadIfChanged())
                {
                    _settingsRedraw = true;
                    SettingsReloaded?.Invoke();
                }
                var userDir = GameEnvironment.UserDir(Settings);
                if (!string.Equals(Trophies.LogPath, System.IO.Path.Combine(userDir, TrophyTracker.LogName), StringComparison.OrdinalIgnoreCase))
                {
                    Trophies.SetUserDir(userDir);
                    TrophiesChanged?.Invoke();
                }
                Announce(Trophies.Poll(readRunLog: !Game.Running));
            };
            _trophyTimer.Start();

            _playBackupTimer = new DispatcherTimer { Interval = SaveBackups.PlayEvery };
            _playBackupTimer.Tick += (_, _) => BackUpDuringPlay();
            _playBackupTimer.Start();

            // Output arrives on the reader threads, often thousands of lines at once (the engine's
            // statistics when it exits): it is queued and handed on in batches, ten times a second.
            Game.Output += line => _pendingOutput.Enqueue(line);
            _outputTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            _outputTimer.Tick += (_, _) => FlushOutput();
            _outputTimer.Start();
            Game.Exited += code => Dispatch(() =>
            {
                Log(code == 0 ? L.T("The game exited normally.") : L.F($"The game exited with code {code}."),
                    code == 0 ? LogLevel.Good : LogLevel.Warn);
                _pendingOutput.Enqueue(L.F($"— the game exited (code {code}) —"));
                FlushOutput();
                if (_hdrBefore.Count > 0 && WindowsHdr.Restore(_hdrBefore) is > 0 and var restored)
                    Log(restored == 1 ? L.T("Windows HDR was switched off during the game; turned it back on.")
                                      : L.F($"Windows HDR was switched off on {restored} displays during the game; turned it back on."), LogLevel.Info);
                _hdrBefore = new();
                SecureSaves();
                var stopped = _stopping;
                _stopping = false;
                if (Session is { } session)
                {
                    session.Finish();
                    if (session.Length > TimeSpan.FromSeconds(10))
                    {
                        SessionStats.AddPlaytime(Settings, PlayerProfiles.Current(Settings), session.Length);
                        SaveSoon();
                        Log(L.T("Session: ") + session.Summary, LogLevel.Info);
                    }
                    if (session.RoomFull is { } full)
                        SeamlessRefused?.Invoke(full);
                    else if (code != 0 && !stopped)
                        GameCrashed?.Invoke(code, session);
                    // The server refused the account's token (the session log's THD line): kept until the
                    // player signs in again, so the Online page marks the account, and told now.
                    if (session.SignInRefused is { } refused)
                    {
                        var name = refused.Name.Length > 0 ? refused.Name : Settings.Str("hd_name");
                        Settings.Set("hd_refused", refused.Reason);
                        SaveSoon();
                        Log($"The Hunter's Dream refused the sign-in as {name} ({refused.Reason}): sign in again on the Online page.", LogLevel.Warn);
                        SignInRefused?.Invoke(name, refused.Reason);
                    }
                }
                GameStateChanged?.Invoke();
            });
        }

        /// <summary>The engine folder: the package this launcher is in, else the setting, else one
        /// beside the game folder.</summary>
        public void ResolvePort()
        {
            var chosen = Settings.Str("port_dir");
            var game = Paths.Expand(Settings.Str("game_dir"));
            // The Bloodborne PC package this launcher ships in always wins: it has everything.
            if (Paths.DetectPortDir() is { } own)
                Paths.PortDir = own;
            else if (Paths.IsPortDir(chosen))
                Paths.PortDir = chosen;
            else if ((Paths.PortBesideGame(game) ?? Paths.PortNearGame(game) ?? Paths.DetectPortDir()) is { } found)
            {
                // Remembered, so moving the game folder later does not lose it.
                Paths.PortDir = found;
                Settings.Set("port_dir", found);
            }
            else
                Paths.PortDir = chosen;

            if (Settings.Str("game_dir").Length == 0 && Paths.IsPortDir(Paths.PortDir))
            {
                var guess = Path.Combine(Directory.GetParent(Paths.PortDir)?.FullName ?? Paths.PortDir, "CUSA03173");
                Settings.Set("game_dir", guess);
            }

            Ini.Load();
            // The DLSS model moved from the launcher's settings to lantern.ini (dlss_preset), where the
            // in-game menu changes it live: an older launcher's choice is carried over once.
            if (Paths.IsPortDir(Paths.PortDir) && Settings.Str("dlss_preset") is { Length: > 0 } dlss && dlss != "k")
            {
                Ini.Set("dlss_preset", dlss);
                Settings.Set("dlss_preset", "k");
                SaveSoon();
            }
            _iniWarned = false;
            PortChanged?.Invoke();
        }

        private bool _settingsRedraw;

        public bool SettingsChangedOutside()
        {
            var changed = Settings.ReloadIfChanged();
            if (changed) SettingsReloaded?.Invoke();
            changed |= _settingsRedraw;
            _settingsRedraw = false;
            return changed;
        }

        // ---------------------------------------------------------------- updates

        /// <summary>The latest release when it is newer than this build.</summary>
        public Updater.Release? Update { get; private set; }
        public Updater.Release? Latest { get; private set; }
        public DateTime? CheckedAt { get; private set; }
        public event Action? UpdateStateChanged;

        public async Task CheckForUpdatesAsync(bool quiet)
        {
            try
            {
                var beta = Updater.BetaChannel(Settings);
                Latest = await Updater.LatestAsync(beta);
                CheckedAt = DateTime.Now;
                // On stable with a beta installed, the latest stable release is offered even when it is
                // older: that is the way back.
                Update = Latest != null && (Latest.IsNewerThan(Updater.Current, Updater.CurrentBeta) ||
                                            !beta && Updater.CurrentBeta > 0 && Latest.Beta == 0)
                    ? Latest
                    : null;
                if (Update != null)
                    Log(L.F($"Launcher update available: {Update.Tag} (you have {Updater.CurrentLabel})."), LogLevel.Good);
                else if (!quiet)
                    Log(Latest == null ? L.T("No launcher release is published yet.") : L.F($"The launcher is up to date ({Updater.CurrentLabel})."), LogLevel.Good);
            }
            catch (Exception e)
            {
                if (!quiet) Log(L.T("Could not check for updates: ") + e.GetBaseException().Message, LogLevel.Warn);
            }
            UpdateStateChanged?.Invoke();
        }

        /// <summary>Trophies the loaded save had already earned: one line in the log, and a notification
        /// only for a platinum they complete.</summary>
        private void AnnounceFromSave(List<int> ids)
        {
            if (ids.Count == 0) return;
            var names = ids.Where(id => id != TrophyTracker.PlatinumId && TrophyTracker.ById.ContainsKey(id))
                .Select(id => TrophyTracker.ById[id].Name).ToList();
            if (names.Count > 0) Log(L.F($"Trophies already earned in this save: {string.Join(", ", names)}"), LogLevel.Good);
            Announce(ids.Where(id => id == TrophyTracker.PlatinumId).ToList());
            TrophiesChanged?.Invoke();
        }

        private void Announce(List<int> ids)
        {
            if (ids.Count == 0) return;
            foreach (var id in ids)
            {
                if (!TrophyTracker.ById.TryGetValue(id, out var trophy)) continue;
                Log(L.F($"Trophy unlocked: {trophy.Name} ({Trophy.GradeName(trophy.Grade)})"), LogLevel.Good);
                if (Settings.Bool("trophy_notifications"))
                    Views.TrophyToast.Announce(trophy);
            }
            TrophiesChanged?.Invoke();
        }

        public void SaveSoon()
        {
            if (_saveTimer == null)
            {
                _saveTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(350) };
                _saveTimer.Tick += (_, _) =>
                {
                    _saveTimer.Stop();
                    SaveNow();
                };
            }
            _saveTimer.Stop();
            _saveTimer.Start();
        }

        public void SaveNow()
        {
            _saveTimer?.Stop();
            try
            {
                Settings.Save();
                if (Paths.IsPortDir(Paths.PortDir) || Environment.GetEnvironmentVariable("BB_CONFIG") is { Length: > 0 })
                    Ini.Save();
                else if (!_iniWarned)
                {
                    _iniWarned = true;
                    Log("lantern.ini not written: the engine folder is not set (Settings › Lantern Engine).", LogLevel.Warn);
                }
                Saved?.Invoke();
            }
            catch (Exception e)
            {
                Log(L.T("Could not save settings: ") + e.Message, LogLevel.Bad);
            }
        }

        /// <summary>Windows' HDR state per display when the game started (restored after it exits).</summary>
        private List<WindowsHdr.Display> _hdrBefore = new();

        /// <summary>A safety copy of every profile's saves outside the saves folder (SaveGuard), on a
        /// worker thread: at each start and when the game exits, whatever Save backups says.</summary>
        public void SecureSaves()
        {
            var root = GameEnvironment.SavesRoot(Settings);
            System.Threading.Tasks.Task.Run(() =>
            {
                if (SaveGuard.Secure(root) > 0)
                    Log(L.F($"Saves: safety copy made in {SaveGuard.VaultDir}."), LogLevel.Info);
            });
        }

        public string? StartGame()
        {
            SaveNow();
            var savesRoot = GameEnvironment.SavesRoot(Settings);
            OutputLines.Clear();
            OutputCleared?.Invoke();
            SecureSaves();
            if (Settings.Bool("save_backups"))
            {
                // Copied on a worker thread: the game takes ~20 s to reach its first save, and a
                // large save folder held the launcher (Play looked stuck) while it was copied here.
                var settings = Settings;
                System.Threading.Tasks.Task.Run(() =>
                {
                    try
                    {
                        if (SaveBackups.Create(settings) != null)
                            Log(L.F($"Saves backed up before playing (the newest {SaveBackups.Keep} are kept)."), LogLevel.Step);
                    }
                    catch (Exception e)
                    {
                        Log(L.T("Could not back up the saves: ") + e.Message, LogLevel.Warn);
                    }
                });
            }
            var safeMode = Settings.Bool("safe_mode_next");
            Session = new SessionStats();
            _hdrBefore = WindowsHdr.Displays();
            var error = Game.Start(Settings);
            if (error != null)
            {
                Log(error, LogLevel.Bad);
                return error;
            }
            Log(GameRunner.Custom(Settings) ? L.T("Started the game with the custom command.") : L.T("Started Bloodborne."), LogLevel.Step);
            Settings.Set(SavesFolder.LastRootKey, SavesFolder.Full(savesRoot));
            SaveSoon();
            if (Settings.Bool("section_stats"))
            {
                // Timing every render stage costs frame rate: one game only.
                Settings.Set("section_stats", false);
                SaveSoon();
            }
            if (safeMode)
            {
                // One run only: the next start uses the settings as chosen again.
                Settings.Set("safe_mode_next", false);
                SaveSoon();
                Log(L.T("Safe mode for this run: experimental engine options and mods are off."), LogLevel.Step);
            }
            GameStateChanged?.Invoke();
            return null;
        }

        private void BackUpDuringPlay()
        {
            if (!Game.Running || !Settings.Bool("save_backups")) return;
            var settings = Settings;
            System.Threading.Tasks.Task.Run(() =>
            {
                try
                {
                    if (SaveBackups.CreateDuringPlay(settings) != null)
                        Log(L.F($"Saves backed up while playing (the newest {SaveBackups.PlayKeep} of these are kept)."), LogLevel.Step);
                }
                catch (Exception e)
                {
                    Log(L.T("Could not back up the saves: ") + e.Message, LogLevel.Warn);
                }
            });
        }

        public async Task StopGameAsync()
        {
            Log(L.T("Stopping the game…"), LogLevel.Step);
            _stopping = true;
            await Game.StopAsync();
            GameStateChanged?.Invoke();
        }

        public void ClearOutput()
        {
            OutputLines.Clear();
            OutputCleared?.Invoke();
        }

        public void Log(string message, LogLevel level = LogLevel.Info) =>
            Dispatch(() => Logged?.Invoke(message, level));

        private void FlushOutput()
        {
            if (_pendingOutput.IsEmpty) return;
            var batch = new List<string>();
            while (_pendingOutput.TryDequeue(out var line)) batch.Add(line);
            foreach (var line in batch)
            {
                Announce(Trophies.Observe(line));
                AnnounceFromSave(Trophies.ObserveSave(line));
                Session?.Observe(line);
            }
            OutputLines.AddRange(batch);
            if (OutputLines.Count > MaxOutputLines)
                OutputLines.RemoveRange(0, OutputLines.Count - MaxOutputLines);
            GameOutput?.Invoke(batch);
        }

        private static void Dispatch(Action action)
        {
#if AVALONIA
            if (Dispatcher.UIThread.CheckAccess())
                action();
            else
                Dispatcher.UIThread.Post(action);
#else
            var dispatcher = System.Windows.Application.Current?.Dispatcher;
            if (dispatcher == null || dispatcher.CheckAccess())
                action();
            else
                dispatcher.BeginInvoke(action);
#endif
        }
    }
}
