using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using BloodborneLauncher.Core;
using BloodborneLauncher.Views;
using Wpf.Ui.Controls;
using MessageBoxButton = System.Windows.MessageBoxButton;
using MessageBoxResult = System.Windows.MessageBoxResult;

namespace BloodborneLauncher
{
    public partial class MainWindow
    {
        private readonly ObservableCollection<LogItem> _log = new();
        private readonly Dictionary<Type, LauncherPage> _pages = new();
        private bool _logExpanded;
        private bool _closing;

        public static MainWindow? Instance => Application.Current.MainWindow as MainWindow;

        public MainWindow()
        {
            InitializeComponent();

            LogList.ItemsSource = _log;
            var ws = Workspace.Current;
            ws.Logged += OnLogged;
            ws.Saved += () => SavedText.Text = L.F($"Saved {DateTime.Now:HH:mm:ss}");
            ws.GameStateChanged += UpdatePlayButton;
            ws.GameStateChanged += UpdateDiscord;
            ws.GameCrashed += OnGameCrashed;
            ws.SignInRefused += OnSignInRefused;
            ws.SeamlessRefused += reason => Prompt.Tell(L.T("Seamless co-op"), reason);
            ws.Initialise();
            // The settings are read by Initialise: the saved window placement comes after it.
            RestoreWindowPlacement(ws.Settings);
            KeyDown += Window_KeyDown;
            PreviewMouseDown += Window_PreviewMouseDown;
            Deactivated += (_, _) => SearchResults.Visibility = Visibility.Collapsed;
            Activated += (_, _) => { if (ws.SettingsChangedOutside()) ReloadPages(); };

            // Layout, read once at start (Settings › Layout restarts the launcher to switch).
            var args = Environment.GetCommandLineArgs();
            var forced = Array.IndexOf(args, "--layout") is var at and >= 0 && at + 1 < args.Length ? args[at + 1] : null;
            SettingRow.Classic = (forced ?? ws.Settings.Str("ui_layout")) == "classic";
            if (Array.IndexOf(args, "--test-prompt") is var shot and >= 0 && shot + 1 < args.Length)
            {
                Prompt.SnapshotPath = args[shot + 1];
                ShowActivated = false;
                ShowInTaskbar = false;
                WindowStartupLocation = WindowStartupLocation.Manual;
                Left = -32000;
                Top = -32000;
            }
            // --play (Bloodborne.exe): the game straight away with the saved settings, the launcher
            // out of sight meanwhile (minimized, then hidden: its prompts show on their own, in the
            // middle of the screen, and it has no taskbar button to close the game by).
            _directPlay = args.Contains("--play");
            if (_directPlay)
                WindowState = WindowState.Minimized;
            // Cheats beside Mods & patches, once the engine applies them (as More's Cheats tab).
            ClassicCheats.Visibility = CheatsPage.Supported ? Visibility.Visible : Visibility.Collapsed;
            ApplyNavigation();
            if (!SettingRow.Classic)
                ApplyGlass();
            LauncherTheme.Apply(ws.Settings, glass: !SettingRow.Classic);
            ApplyWindowButtons();
            ApplyIcon();

            var version = typeof(MainWindow).Assembly.GetName().Version;
            VersionText.Text = version == null ? "" : Updater.CurrentLabel;

            ws.UpdateStateChanged += () =>
            {
                TitleUpdate.Visibility = ws.Update != null ? Visibility.Visible : Visibility.Collapsed;
                if (ws.Update != null) TitleUpdate.ToolTip = L.F($"{ws.Update.Name} is available");
            };

            Loaded += async (_, _) =>
            {
                Updater.CleanUp();
                // Running a beta means choosing beta: kept after updating to a stable release.
                if (Updater.CurrentBeta > 0 && ws.Settings.Str("update_channel").Length == 0)
                {
                    ws.Settings.Set("update_channel", "beta");
                    ws.SaveSoon();
                }
                if (ws.Settings.Bool("check_updates"))
                    _ = CheckForUpdatesAtStartAsync();
                await Task.Yield();
                (_sidebarNav ? ClassicPlay : PlayItem).IsChecked = true;
                UpdatePlayButton();
                // --test-trophy: a sample notification, to check how it looks over the game.
                if (Environment.GetCommandLineArgs().Contains("--test-trophy"))
                    TrophyToast.Announce(TrophyTracker.ById[14]);
                // --test-prompt <png>: a picture of a sample dialog over the launcher, drawn off-screen.
                // --test-import: the import dialog with a long pasted code instead.
                if (Prompt.SnapshotPath != null && Environment.GetCommandLineArgs().Contains("--test-import"))
                    Dispatcher.BeginInvoke(() => Prompt.Ask(L.T("Import settings"), L.F($"Paste the code someone shared (it starts with {SharedSettings.CodePrefix})."),
                            new Prompt.Field(L.T("Code"), SharedSettings.ToCode(SharedSettings.Take(ws.Ini, ws.Settings)), "", Multiline: true)),
                        System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                else if (Prompt.SnapshotPath != null)
                    Dispatcher.BeginInvoke(() => Prompt.Confirm(L.T("Switch layout"), L.T("The launcher restarts to change its layout."), L.T("Restart now")),
                        System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                ws.Log(Paths.IsPortDir(Paths.PortDir)
                        ? L.F($"Lantern Engine ready. Engine: {Paths.PortDir}")
                        : L.T("Lantern Engine ready. Choose the engine folder in Settings."),
                    Paths.IsPortDir(Paths.PortDir) ? LogLevel.Good : LogLevel.Warn);
                if (Updater.CleanOldFiles(ws.Settings, Paths.PortDir) is > 0 and var cleaned)
                    ws.Log(L.F($"Removed {cleaned} leftover files from older versions."), LogLevel.Step);
                // Once: the settings that suit this PC, shown before anything changes.
                if (_directPlay)
                    _ = Dispatcher.BeginInvoke(async () => await PlayDirectAsync(), System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                // The setup guide: with --setup, or once on a first start whose game folder is not
                // ready (a game already set up never sees it). The settings for this PC follow it.
                else if (Prompt.SnapshotPath == null && (Environment.GetCommandLineArgs().Contains("--setup") ||
                         (!ws.Settings.Bool("setup_shown") && Paths.IsPortDir(Paths.PortDir) &&
                          await Task.Run(() => GameCheck.Inspect(GameEnvironment.GameDir(ws.Settings)).Health) != Health.Good)))
                    _ = Dispatcher.BeginInvoke(() => SetupGuide.Open(firstRun: true), System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                else if (!ws.Settings.Bool("hardware_offered") && Prompt.SnapshotPath == null && Paths.IsPortDir(Paths.PortDir))
                    _ = Dispatcher.BeginInvoke(() => OfferBestSettings(firstRun: true), System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                else if (ws.Settings.Bool("hardware_offered") && !ws.Settings.Bool("thanks_maximilian_dood") && Prompt.SnapshotPath == null)
                    _ = Dispatcher.BeginInvoke(ShowThanks, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                // Once, after updating from an older launcher (a new install starts on New): the
                // newer engine or the one 0.9.33 ran, the player's choice. Nothing changes unasked.
                else if (ws.Settings.Bool("hardware_offered") && !ws.Settings.Bool("engine_preset_asked") &&
                         Prompt.SnapshotPath == null && Paths.IsPortDir(Paths.PortDir))
                    _ = Dispatcher.BeginInvoke(AskEnginePreset, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                else if (ws.Settings.Bool("hardware_offered") && TestedSettings.ShouldAsk(ws.Ini, ws.Settings) &&
                         Prompt.SnapshotPath == null && Paths.IsPortDir(Paths.PortDir))
                    _ = Dispatcher.BeginInvoke(AskTestedSettings, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
            };
            Closing += OnClosing;
        }

        private bool _directPlay, _directKeepOpen;

        /// <summary>The pages in the sidebar (Classic, or Settings › Navigation: Sidebar) rather than
        /// the tabs across the top.</summary>
        private bool _sidebarNav;

        /// <summary>Settings › Navigation (live): the Modern layout's tabs across the top, or the
        /// sidebar with the pages in groups (GAME, EXTRAS, SYSTEM), in the tabs' look. Classic always
        /// has the sidebar. The page shown stays, marked in the other one (More's sections are
        /// entries of their own in the sidebar).</summary>
        public void ApplyNavigation()
        {
            var sidebar = SettingRow.Classic || Workspace.Current.Settings.Str("ui_navigation") == "sidebar";
            var switching = IsLoaded && sidebar != _sidebarNav;
            _sidebarNav = sidebar;
            Sidebar.Visibility = sidebar ? Visibility.Visible : Visibility.Collapsed;
            TabStrip.Visibility = sidebar ? Visibility.Collapsed : Visibility.Visible;
            if (!SettingRow.Classic)
            {
                // A pane over the backdrop, as the tab strip is.
                Sidebar.Margin = new Thickness(12, 12, 0, 12);
                Sidebar.CornerRadius = new CornerRadius(14);
                Sidebar.BorderThickness = new Thickness(1);
                Sidebar.SetResourceReference(Border.BackgroundProperty, "PillTrackBrush");
                Sidebar.SetResourceReference(Border.BorderBrushProperty, "GlassPaneStrokeBrush");
                // Settings once, in the LANTERN group (Classic has it at the bottom too).
                ClassicSettingsBottom.Visibility = Visibility.Collapsed;
                var style = (Style)FindResource("GlassSidebarItem");
                foreach (var item in SettingSearch.Descendants(Sidebar).OfType<System.Windows.Controls.RadioButton>())
                    item.Style = style;
            }
            if (!switching)
                return;
            var shown = PageFrame.Content?.GetType();
            if (shown == typeof(MorePage) && _pages.TryGetValue(typeof(MorePage), out var more))
                shown = ((MorePage)more).ShownPage ?? typeof(PlayPage);
            // More holds Mods, Cheats, Engine and Log pages of its own: the ones of the navigation
            // left are dropped, so the other makes its own with the settings as they are now.
            foreach (var type in new[] { typeof(MorePage), typeof(ModsPage), typeof(CheatsPage), typeof(EnginePage), typeof(OutputPage) })
                _pages.Remove(type);
            _searchPagesReady = null; // made again for the search when its box is next focused
            var nav = sidebar ? (FrameworkElement)Sidebar : TabStrip;
            var target = SettingSearch.Descendants(nav).OfType<System.Windows.Controls.RadioButton>()
                .LastOrDefault(i => Equals(i.Tag, shown) && i.Visibility == Visibility.Visible);
            if (target != null)
            {
                // Still checked from before the last switch: shown again all the same.
                if (target.IsChecked == true)
                    SidebarItem_Checked(target, new RoutedEventArgs());
                target.IsChecked = true;
                return;
            }
            // A page of More's (Mods, Cheats, Engine, Log) back on the tabs: More, at that section.
            var key = shown == typeof(ModsPage) ? "mods" : shown == typeof(CheatsPage) ? "cheats"
                : shown == typeof(EnginePage) ? "engine" : shown == typeof(OutputPage) ? "output" : null;
            if (key != null) Show(key);
            else (sidebar ? ClassicPlay : PlayItem).IsChecked = true;
        }

        /// <summary>--play: starts the game; the launcher closes when it ends normally, and opens when
        /// the start did not happen, the game crashed (the crash dialog first) or The Hunter's Dream
        /// refused the sign-in (the Online page is where the player signs in again).</summary>
        private async Task PlayDirectAsync()
        {
            var ws = Workspace.Current;
            Hide();
            await PlayOrStopAsync();
            if (!ws.Game.Running)
            {
                _directPlay = false;
                ShowLauncher();
                return;
            }
            ws.GameCrashed += (_, _) => _directKeepOpen = true;
            ws.SignInRefused += (_, _) => _directKeepOpen = true;
            ws.GameStateChanged += () =>
            {
                if (!_directPlay || ws.Game.Running) return;
                _directPlay = false;
                if (_directKeepOpen)
                    ShowLauncher();
                else
                    Close();
            };
        }

        private void ShowLauncher()
        {
            Show();
            WindowState = WindowState.Normal;
            Activate();
        }

        /// <summary>Shows what would change to suit this PC, and applies it on Apply.</summary>
        /// <summary>Graphics › Engine preset, asked once: New or Legacy (0.9.33). Closed without a
        /// choice, it asks again at the next start.</summary>
        private void ShowThanks()
        {
            var ws = Workspace.Current;
            ws.Settings.Set("thanks_maximilian_dood", true);
            ws.SaveSoon();
            Prompt.Tell(L.T("Thank you, Maximilian Dood!"), L.T("Thank you to Maximilian Dood for covering Lantern Engine.") + "\n\n" + L.T("Maximilian Dood, I'd love to reach out. My Discord is @awakenkn."));
        }

        private void AskTestedSettings()
        {
            var ws = Workspace.Current;
            var lines = string.Join("\n", TestedSettings.Differences(ws.Ini, ws.Settings).Select(d => TestedSettings.Describe(d, ws.Ini, ws.Settings)));
            var choice = Prompt.Choose(L.T("This beta was tested with these settings"),
                L.T("Some of yours are different, so your game may not run the way it did in testing. If you use the tested settings, yours are saved first, and Settings › Beta tested settings brings them back.\n\n") + lines,
                L.T("Keep mine"), L.T("Use tested settings"));
            if (choice < 0) return;
            TestedSettings.MarkAsked(ws.Settings);
            if (choice == 1)
            {
                try
                {
                    var file = TestedSettings.Use(ws.Ini, ws.Settings);
                    ws.Log(L.F($"Using the settings this beta was tested with. Yours are saved in {file}"), LogLevel.Good);
                }
                catch (Exception e) when (e is System.IO.IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
                {
                    Prompt.Tell(L.T("Your settings were not changed"), L.T("They could not be saved first: ") + e.Message);
                }
            }
            ws.SaveSoon();
        }

        private void AskEnginePreset()
        {
            var ws = Workspace.Current;
            var choice = Prompt.Choose(L.T("New engine or the 0.9.33 one?"),
                L.T("This update brings a newer engine: a faster draw thread, quicker shader compiling and more VRAM for textures. " +
                    "Most players run faster with it. Legacy keeps the engine as 0.9.33 ran it.\n\n" +
                    "You can change it any time under Graphics > Engine preset."),
                L.T("Stay on Legacy"), L.T("Use New (recommended)"));
            if (choice < 0) return;
            EnginePreset.Apply(ws.Settings, choice == 1 ? EnginePreset.New : EnginePreset.Legacy);
            ws.Settings.Set("engine_preset_asked", true);
            ws.SaveSoon();
            ws.Log(choice == 1 ? L.T("Engine preset: New") : L.T("Engine preset: Legacy (0.9.33)"), LogLevel.Good);
        }

        public void OfferBestSettings(bool firstRun)
        {
            var ws = Workspace.Current;
            ws.Settings.Set("hardware_offered", true);
            // A new install starts on the New engine preset: nothing to ask it later.
            if (firstRun) ws.Settings.Set("engine_preset_asked", true);
            ws.SaveSoon();
            var hw = Hardware.Detect();
            var changes = hw.Recommend(ws.Settings, ws.Ini);
            if (changes.Count == 0)
            {
                if (!firstRun) Prompt.Tell(L.T("Best settings for this PC"), L.F($"{hw.Summary}\n\nYour settings already suit this PC."));
                return;
            }
            var list = string.Join("\n", changes.Select(c => $"{c.Label}: {c.From} → {c.To}"));
            if (!Prompt.Confirm(firstRun ? L.T("Settings for this PC") : L.T("Best settings for this PC"),
                    L.F($"{hw.Summary}\n\n{list}\n\nYou can change any of them later."), L.T("Apply")))
                return;
            foreach (var change in changes) change.Apply();
            ws.SaveSoon();
            ws.Log(L.T("Settings for this PC applied: ") + string.Join(", ", changes.Select(c => $"{c.Label.ToLowerInvariant()} {c.To}")) + ".", LogLevel.Good);
            ReloadPages();
        }

        /// <summary>Builds the pages again after settings changed underneath them, staying on the current one.</summary>
        public void ReloadPages()
        {
            var current = PageFrame.Content as LauncherPage;
            _pages.Clear();
            _searchPagesReady = null; // made again for the search when its box is next focused
            if (current == null) return;
            var page = _pages[current.GetType()] = (LauncherPage)Activator.CreateInstance(current.GetType())!;
            PageFrame.Navigate(page);
            page.OnShown();
        }

        /// <summary>The game ended with an error by itself: what happened, safe mode, a report to copy.</summary>
        private void OnGameCrashed(int code, SessionStats session)
        {
            var ws = Workspace.Current;
            var choice = Prompt.Choose(L.T("The game crashed"),
                L.F($"{CrashReport.Explain(code)} It ran for {SessionStats.Duration(session.Length)}.\n\n") +
                L.T("Safe mode starts the next run with the experimental engine options and mods off. ") +
                L.T("Copy report puts the details and the end of the log on the clipboard, for a bug report."),
                L.T("Close"), L.T("Copy report"), L.T("Use safe mode"));
            if (choice == 1)
            {
                try
                {
                    if (ClipboardText.Set(CrashReport.Build(ws.Settings, ws.Ini, code)))
                        ws.Log(L.T("Crash report copied to the clipboard."), LogLevel.Good);
                    else
                        ws.Log(L.T("Could not copy the report: another program is using the clipboard."), LogLevel.Warn);
                }
                catch (Exception e)
                {
                    ws.Log(L.T("Could not copy the report: ") + e.Message, LogLevel.Warn);
                }
            }
            else if (choice == 2)
            {
                ws.Settings.Set("safe_mode_next", true);
                ws.SaveSoon();
                ws.Log(L.T("The next start uses safe mode (once)."), LogLevel.Step);
            }
        }

        /// <summary>The Hunter's Dream refused this PC's sign-in during the game (the engine's THD line):
        /// nobody could summon or invite the player. The Online page marks the account until they sign in again.</summary>
        private void OnSignInRefused(string name, string reason)
        {
            var choice = Prompt.Choose(L.T("The Hunter's Dream refused the sign-in"),
                L.F($"During the game, The Hunter's Dream refused this PC's sign-in as {name} ({reason}). ") +
                L.T("Until you sign in again, nobody can summon or invite you.\n\n") +
                L.T("Sign in again on the Online page: Hunter's Dream account › Sign in again."),
                L.T("Later"), L.T("Open the Online page"));
            if (choice == 1)
                Show("online");
        }

        /// <summary>
        /// Modern layout: glass panes over a picture. Glass.xaml recolours the shared brushes;
        /// ApplyBackdrop puts the picture behind them.
        /// </summary>
        private void ApplyGlass()
        {
            Application.Current.Resources.MergedDictionaries.Add(new ResourceDictionary
            {
                Source = new Uri("pack://application:,,,/Themes/Glass.xaml", UriKind.Absolute),
            });
            ApplyBackdrop();
        }

        /// <summary>
        /// The Modern layout's background (Settings › Appearance, live): the dump's sce_sys\pic1.png
        /// (or pic0, else the bundled art), the player's own picture, or none; blurred strongly,
        /// lightly or not at all, under a dark wash tinted by the theme.
        /// </summary>
        public void ApplyBackdrop()
        {
            if (SettingRow.Classic)
                return;
            var s = Workspace.Current.Settings;
            var shade = LauncherTheme.ShadeColour(s);
            var wash = new LinearGradientBrush(new GradientStopCollection
            {
                new(LauncherTheme.WithAlpha(shade, 0x59), 0),
                new(LauncherTheme.WithAlpha(shade, 0x80), 0.5),
                new(LauncherTheme.WithAlpha(shade, 0xB3), 1),
            }, 90);
            wash.Freeze(); // never changes: frozen brushes skip change tracking
            BackdropShade.Fill = wash;
            var mode = s.Str("ui_background");
            var mica = mode == "mica" && Environment.OSVersion.Version.Build >= 22000;
            ApplyMica(mica, shade);
            if (mode == "none" || mode == "mica")
            {
                _backdropLoad++; // a picture still loading is not shown
                BackdropImage.Visibility = Visibility.Collapsed;
                BackdropShade.Visibility = Visibility.Collapsed;
                return;
            }
            var blur = s.Str("ui_background_blur");
            var radius = blur == "off" ? 0 : blur == "light" ? 14 : 48;
            BackdropImage.Effect = radius == 0 ? null : new System.Windows.Media.Effects.BlurEffect
            {
                Radius = radius,
                KernelType = System.Windows.Media.Effects.KernelType.Gaussian,
                RenderingBias = System.Windows.Media.Effects.RenderingBias.Performance,
            };
            BackdropImage.Margin = new Thickness(radius == 0 ? 0 : -60);
            RenderOptions.SetBitmapScalingMode(BackdropImage, radius == 0 ? BitmapScalingMode.HighQuality : BitmapScalingMode.LowQuality);
            BackdropShade.Visibility = Visibility.Visible;
            _ = LoadBackdropAsync(mode, s.Str("ui_background_image"), GameEnvironment.GameDir(s),
                radius == 0 ? 2560 : radius < 20 ? 1600 : 960); // blurred: less is enough
        }

        private int _backdropLoad;

        private void ApplyMica(bool on, Color shade)
        {
            if (on)
            {
                var tint = new SolidColorBrush(LauncherTheme.WithAlpha(shade, 0x66));
                tint.Freeze();
                BaseLayer.Background = tint;
                Background = Brushes.Transparent;
                if (WindowBackdropType != Wpf.Ui.Controls.WindowBackdropType.Mica)
                    WindowBackdropType = Wpf.Ui.Controls.WindowBackdropType.Mica;
                return;
            }
            if (WindowBackdropType != Wpf.Ui.Controls.WindowBackdropType.None)
                WindowBackdropType = Wpf.Ui.Controls.WindowBackdropType.None;
            BaseLayer.SetResourceReference(System.Windows.Controls.Panel.BackgroundProperty, "WindowBaseBrush");
            SetResourceReference(BackgroundProperty, "WindowBaseBrush");
        }

        /// <summary>Finds and decodes the background picture on a worker thread (the game folder may be
        /// on a slow or sleeping disk, and a big PNG takes a while to decode), then shows it.</summary>
        private async Task LoadBackdropAsync(string mode, string own, string game, int width)
        {
            var load = ++_backdropLoad;
            System.Windows.Media.Imaging.BitmapImage? image;
            try
            {
                image = await Task.Run(() =>
                {
                    var builtIn = AppIcon.Backdrop(mode);
                    string? art = builtIn != null
                        ? $"pack://application:,,,/Assets/{builtIn}"
                        : mode == "custom" && System.IO.File.Exists(own)
                        ? own
                        : new[] { "pic1.png", "pic0.png" }
                            .Select(n => System.IO.Path.Combine(game, "sce_sys", n))
                            .FirstOrDefault(System.IO.File.Exists);
                    // No picture of our own: without the game's art the background stays plain.
                    if (art == null) return null;
                    var bitmap = new System.Windows.Media.Imaging.BitmapImage();
                    bitmap.BeginInit();
                    bitmap.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad;
                    bitmap.DecodePixelWidth = width;
                    bitmap.UriSource = new Uri(art, UriKind.Absolute);
                    bitmap.EndInit();
                    bitmap.Freeze(); // decoded here, usable on the UI thread
                    return bitmap;
                });
            }
            catch
            {
                image = null;
            }
            if (load != _backdropLoad) return; // a newer choice is loading
            // A picture that won't load: the glass sits on the plain dark background.
            BackdropImage.Source = image;
            BackdropImage.Visibility = image != null ? Visibility.Visible : Visibility.Collapsed;
        }

        /// <summary>The theme, again (Settings › Appearance › Theme, live).</summary>
        public void ApplyTheme()
        {
            LauncherTheme.Apply(Workspace.Current.Settings, glass: !SettingRow.Classic);
            ApplyBackdrop();
        }

        /// <summary>
        /// The window buttons (Settings › Appearance, live): macOS traffic lights or Windows caption
        /// buttons, on their usual side (macOS left, Windows right) or the other. Close stays on the
        /// outer edge; the title bar's own buttons move out of their way.
        /// </summary>
        /// <summary>Settings › App icon: this window's, and the game window's from its next start.</summary>
        public void ApplyIcon()
        {
            var choice = Workspace.Current.Settings.Str("app_icon");
            AppIcon.Apply(this, choice);
            if (Paths.DetectPortDir() is { } package)
            {
                AppIcon.SyncGameWindow(package, choice);
                AppIcon.SyncShortcuts(package, choice);
            }
        }

        public void ApplyWindowButtons()
        {
            var s = Workspace.Current.Settings;
            var windows = s.Str("ui_window_buttons") == "windows";
            var side = s.Str("ui_window_buttons_side");
            var right = side == "right" || (side != "left" && windows);
            MacButtons.Visibility = windows ? Visibility.Collapsed : Visibility.Visible;
            WinButtons.Visibility = windows ? Visibility.Visible : Visibility.Collapsed;

            static void Order(System.Windows.Controls.Panel panel, params UIElement[] order)
            {
                panel.Children.Clear();
                foreach (var child in order) panel.Children.Add(child);
            }
            var mac = MacButtons.Children.OfType<System.Windows.Controls.Button>().ToList();
            var close = mac.First(b => (string)b.ToolTip == L.T("Close"));
            var minimize = mac.First(b => (string)b.ToolTip == L.T("Minimize"));
            var maximize = mac.First(b => (string)b.ToolTip == L.T("Maximize"));
            if (right)
            {
                Order(MacButtons, minimize, maximize, close);
                Order(WinButtons, WinMinimize, WinMaximize, WinClose);
            }
            else
            {
                Order(MacButtons, close, minimize, maximize);
                Order(WinButtons, WinClose, WinMaximize, WinMinimize);
            }
            foreach (var b in MacButtons.Children.OfType<FrameworkElement>())
                b.Margin = right ? new Thickness(8, 0, 0, 0) : new Thickness(0, 0, 8, 0);
            MacButtons.HorizontalAlignment = right ? HorizontalAlignment.Right : HorizontalAlignment.Left;
            MacButtons.Margin = right ? new Thickness(0, 0, 14, 0) : new Thickness(14, 0, 0, 0);
            WinButtons.HorizontalAlignment = right ? HorizontalAlignment.Right : HorizontalAlignment.Left;
            // The traffic lights take 3 x 20 px plus their margin; the caption buttons 3 x 46.
            var taken = !right ? 0 : windows ? 3 * 46 : 14 + 3 * 20;
            TitleTools.Margin = new Thickness(0, 0, 10 + taken, 0);
            // The settings search on the left, after the buttons when they are on that side; a little
            // narrower beside the wider caption buttons, so it stays clear of the centred title.
            var left = right ? 12 : windows ? 3 * 46 + 10 : 14 + 3 * 20 + 4;
            SearchArea.Margin = new Thickness(left, 0, 0, 0);
            SearchArea.Width = !right && windows ? 168 : 200;
            SearchResults.Margin = new Thickness(left, 2, 0, 0);
        }

        /// <summary>A wider resize border than FluentWindow's 4px, and the top strip handed back
        /// to Windows as a real caption.</summary>
        protected override void SetWindowChrome()
        {
            base.SetWindowChrome();
            if (System.Windows.Shell.WindowChrome.GetWindowChrome(this) is { } chrome)
            {
                if (ResizeMode != ResizeMode.NoResize)
                    chrome.ResizeBorderThickness = new Thickness(8);
                chrome.CaptionHeight = 34;
            }
        }

        private DateTime? _gameStartedUtc;

        /// <summary>Discord status: shown while the game runs (Core/DiscordPresence).</summary>
        public void UpdateDiscord()
        {
            var running = Workspace.Current.Game.Running;
            if (running && _gameStartedUtc == null) _gameStartedUtc = DateTime.UtcNow;
            if (!running) _gameStartedUtc = null;
            DiscordPresence.Update(Workspace.Current.Settings, running, _gameStartedUtc);
        }

        /// <summary>The size and position the window had when it was last closed (settings.json:
        /// window_width, window_height, window_x, window_y, window_maximized), else the default size.
        /// Either way it is made to fit the screen: 1040 x 780 does not fit a 1080p screen at 150% or
        /// 200% Windows scaling. A saved position no screen shows any more (a monitor unplugged) is
        /// dropped and the window opens centred.</summary>
        private void RestoreWindowPlacement(LauncherSettings settings)
        {
            try
            {
                var work = SystemParameters.WorkArea;
                double width = Width, height = Height;
                int savedWidth = settings.Int("window_width"), savedHeight = settings.Int("window_height");
                if (savedWidth >= 400 && savedHeight >= 300)
                {
                    width = savedWidth;
                    height = savedHeight;
                }
                MinWidth = Math.Min(MinWidth, work.Width - 16);
                MinHeight = Math.Min(MinHeight, work.Height - 16);
                width = Math.Max(MinWidth, Math.Min(width, work.Width - 16));
                height = Math.Max(MinHeight, Math.Min(height, work.Height - 16));
                Width = width;
                Height = height;
                if (settings.Str("window_x").Length > 0 && settings.Str("window_y").Length > 0)
                {
                    double x = settings.Int("window_x"), y = settings.Int("window_y");
                    double left = SystemParameters.VirtualScreenLeft, top = SystemParameters.VirtualScreenTop;
                    double right = left + SystemParameters.VirtualScreenWidth, bottom = top + SystemParameters.VirtualScreenHeight;
                    // The title bar must be on a screen, or the window could not be moved back.
                    if (x + 120 >= left && x + 120 < right && y + 20 >= top && y + 20 < bottom)
                    {
                        WindowStartupLocation = WindowStartupLocation.Manual;
                        Left = Math.Max(left, Math.Min(x, right - width));
                        Top = Math.Max(top, Math.Min(y, bottom - height));
                    }
                }
                if (settings.Bool("window_maximized"))
                    WindowState = WindowState.Maximized;
            }
            catch (Exception)
            {
                // The default size, centred.
            }
        }

        /// <summary>Called as the window closes. A maximised window keeps the size and position it
        /// had before (RestoreBounds); a minimised one (the --play start) saves nothing.</summary>
        private void SaveWindowPlacement()
        {
            try
            {
                if (_directPlay || WindowState == WindowState.Minimized || !IsVisible) return;
                var bounds = WindowState == WindowState.Normal ? new Rect(Left, Top, ActualWidth, ActualHeight) : RestoreBounds;
                if (bounds.IsEmpty || double.IsNaN(bounds.Left) || double.IsNaN(bounds.Top) || bounds.Width < 1 || bounds.Height < 1) return;
                var settings = Workspace.Current.Settings;
                settings.Set("window_maximized", WindowState == WindowState.Maximized);
                settings.Set("window_width", (int)Math.Round(bounds.Width));
                settings.Set("window_height", (int)Math.Round(bounds.Height));
                settings.Set("window_x", (int)Math.Round(bounds.Left));
                settings.Set("window_y", (int)Math.Round(bounds.Top));
            }
            catch (Exception)
            {
                // The window closes without remembering where it was.
            }
        }

        private async void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
        {
            SaveWindowPlacement();
            Workspace.Current.SaveNow();
            DiscordPresence.Stop();
            if (_closing || !Workspace.Current.Game.Running)
                return;

            var answer = System.Windows.MessageBox.Show(this,
                L.T("The game is still running. Stop it and close the launcher?"),
                "Lantern Engine", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
            if (answer == MessageBoxResult.Cancel)
            {
                e.Cancel = true;
                return;
            }
            if (answer == MessageBoxResult.Yes)
            {
                e.Cancel = true;
                _closing = true;
                await Workspace.Current.StopGameAsync();
                Close();
            }
        }

        // ---------------------------------------------------------------- window buttons

        private void CloseWindow_Click(object sender, RoutedEventArgs e) => Close();
        private void MinimizeWindow_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
        private void MaximizeWindow_Click(object sender, RoutedEventArgs e) =>
            WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

        protected override void OnSourceInitialized(EventArgs e)
        {
            base.OnSourceInitialized(e);
            if (System.Windows.Interop.HwndSource.FromHwnd(new System.Windows.Interop.WindowInteropHelper(this).Handle) is { } source)
                source.AddHook(SnapLayoutHook);
        }

        private System.Windows.Controls.Button? VisibleMaximize()
        {
            if (WinButtons.IsVisible && WinMaximize.IsVisible) return WinMaximize;
            if (!MacButtons.IsVisible) return null;
            return MacButtons.Children.OfType<System.Windows.Controls.Button>()
                .FirstOrDefault(b => (string)b.ToolTip == L.T("Maximize") && b.IsVisible);
        }

        private bool OverMaximize(IntPtr lParam)
        {
            if (VisibleMaximize() is not { } button) return false;
            var x = unchecked((short)(long)lParam);
            var y = unchecked((short)((long)lParam >> 16));
            try
            {
                var p = button.PointFromScreen(new Point(x, y));
                return p.X >= 0 && p.Y >= 0 && p.X < button.ActualWidth && p.Y < button.ActualHeight;
            }
            catch (InvalidOperationException)
            {
                return false;
            }
        }

        private void SetMaximizeHover(bool on)
        {
            if (on) WinMaximize.SetResourceReference(BackgroundProperty, "SidebarItemHoverBrush");
            else WinMaximize.ClearValue(BackgroundProperty);
        }

        private IntPtr SnapLayoutHook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            const int NcHitTest = 0x0084, NcMouseMove = 0x00A0, NcLButtonDown = 0x00A1, NcLButtonUp = 0x00A2,
                NcMouseLeave = 0x02A2, HtMaxButton = 9;
            switch (msg)
            {
                case NcHitTest:
                    if (OverMaximize(lParam))
                    {
                        handled = true;
                        return new IntPtr(HtMaxButton);
                    }
                    SetMaximizeHover(false);
                    break;
                case NcMouseMove:
                    SetMaximizeHover(wParam.ToInt32() == HtMaxButton);
                    break;
                case NcMouseLeave:
                    SetMaximizeHover(false);
                    break;
                case NcLButtonDown when wParam.ToInt32() == HtMaxButton:
                    handled = true;
                    break;
                case NcLButtonUp when wParam.ToInt32() == HtMaxButton:
                    handled = true;
                    SetMaximizeHover(false);
                    MaximizeWindow_Click(this, new RoutedEventArgs());
                    break;
            }
            return IntPtr.Zero;
        }

        // ---------------------------------------------------------------- navigation

        private void SidebarItem_Checked(object sender, RoutedEventArgs e)
        {
            if (sender is not System.Windows.Controls.RadioButton { Tag: Type pageType } item)
                return;

            var page = PageOf(pageType);
            PageFrame.Navigate(page);
            page.OnShown();

            var name = ItemName(item);
            TitlePage.Text = string.IsNullOrEmpty(name) ? "" : $" — {name}";
        }

        /// <summary>A page, made the first time it is asked for and kept.</summary>
        private LauncherPage PageOf(Type type)
        {
            if (!_pages.TryGetValue(type, out var page))
                _pages[type] = page = (LauncherPage)Activator.CreateInstance(type)!;
            return page;
        }

        /// <summary>A tab's or sidebar entry's label: its text, or the text beside its icon.</summary>
        private static string? ItemName(System.Windows.Controls.RadioButton item) =>
            item.Content as string ??
            (item.Content as Panel)?.Children.OfType<System.Windows.Controls.TextBlock>().FirstOrDefault()?.Text;

        /// <summary>Shows a page: a tab ("online", "advanced": Settings), or one of More's ("output", "mods", "engine").</summary>
        public void Show(string page)
        {
            var item = _sidebarNav
                ? page switch
                {
                    "saves" => ClassicSaves,
                    "output" => ClassicOutput,
                    "online" => ClassicOnline,
                    "advanced" => ClassicSettings,
                    "mods" => ClassicMods,
                    "cheats" when CheatsPage.Supported => ClassicCheats,
                    "engine" => ClassicEngine,
                    "controls" => ClassicControls,
                    _ => ClassicPlay,
                }
                : page switch
                {
                    "saves" => SavesItem,
                    "online" => OnlineItem,
                    "advanced" => SettingsItem,
                    "output" or "mods" or "engine" => MoreItem,
                    "cheats" when CheatsPage.Supported => MoreItem,
                    "controls" => ControlsItem,
                    _ => PlayItem,
                };
            if (item.IsChecked == true)
                SidebarItem_Checked(item, new RoutedEventArgs());
            item.IsChecked = true;
            if (item == MoreItem && _pages.TryGetValue(typeof(MorePage), out var more))
                ((MorePage)more).ShowSection(page);
        }

        private void PageFrame_Navigated(object sender, System.Windows.Navigation.NavigationEventArgs e)
        {
            while (PageFrame.CanGoBack)
                PageFrame.RemoveBackEntry();
        }

        // ---------------------------------------------------------------- settings search

        private List<SettingSearch.Hit> _hits = new();
        private int _hit;

        /// <summary>
        /// The pages this layout's tabs or sidebar lead to, in their order, each made if not shown yet
        /// (once; they are kept as when visited). More stands for its sections, each a place of its own.
        /// </summary>
        private IEnumerable<SettingSearch.Place> SearchPlaces()
        {
            // Classic lists Settings twice (top and bottom of the sidebar): the later entry is kept.
            var items = new List<System.Windows.Controls.RadioButton>();
            foreach (var item in SettingSearch.Descendants(_sidebarNav ? Sidebar : TabStrip).OfType<System.Windows.Controls.RadioButton>())
            {
                if (item.Tag is not Type) continue;
                items.RemoveAll(i => Equals(i.Tag, item.Tag));
                items.Add(item);
            }
            foreach (var item in items)
            {
                var name = ItemName(item) ?? "";
                void Open() { if (item.IsChecked != true) item.IsChecked = true; }
                // Only pages already made: MakeSearchPagesAsync makes the rest in the background and
                // searches again once they are all there.
                if (!_pages.TryGetValue((Type)item.Tag, out var made)) continue;
                if (made is MorePage more)
                {
                    foreach (var (key, label, page) in more.MadeSectionPages())
                        yield return new SettingSearch.Place($"{name} › {label}", page, () => { Open(); more.ShowSection(key); });
                }
                else
                    yield return new SettingSearch.Place(name, made, Open);
            }
        }

        private void Window_KeyDown(object sender, KeyEventArgs e)
        {
            // Bubbling, so a key the page took first (the Controls page listening for a binding) stays its own.
            if (e.Key == Key.F && Keyboard.Modifiers == ModifierKeys.Control)
            {
                SearchBox.Focus();
                SearchBox.SelectAll();
                e.Handled = true;
            }
        }

        private void Window_PreviewMouseDown(object sender, MouseButtonEventArgs e)
        {
            // A click anywhere else closes the results (the text stays; typing opens them again).
            if (SearchResults.IsVisible && e.OriginalSource is Visual source &&
                !SearchResults.IsAncestorOf(source) && !SearchArea.IsAncestorOf(source))
                SearchResults.Visibility = Visibility.Collapsed;
        }

        /// <summary>Searches 150 ms after the last key, not at every key.</summary>
        private System.Windows.Threading.DispatcherTimer? _searchDelay;

        /// <summary>The pages the search looks through, made one at a time at background priority so
        /// typing never waits for them (null until the search box is first focused).</summary>
        private Task? _searchPagesReady;

        private void SearchBox_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
        {
            SearchClear.Visibility = SearchBox.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            if (SearchBox.Text.Trim().Length == 0)
            {
                _searchDelay?.Stop();
                ShowHits();
                return;
            }
            if (_searchDelay == null)
            {
                _searchDelay = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
                _searchDelay.Tick += (_, _) =>
                {
                    _searchDelay.Stop();
                    ShowHits();
                };
            }
            _searchDelay.Stop();
            _searchDelay.Start();
        }

        private void SearchBox_GotKeyboardFocus(object sender, KeyboardFocusChangedEventArgs e)
        {
            _searchPagesReady ??= MakeSearchPagesAsync();
            ShowHits();
        }

        /// <summary>Makes every page the search looks through, yielding to input and rendering between
        /// pages: the first search then finds them ready instead of building them all in one go.</summary>
        private async Task MakeSearchPagesAsync()
        {
            var items = SettingSearch.Descendants(_sidebarNav ? Sidebar : TabStrip)
                .OfType<System.Windows.Controls.RadioButton>()
                .Select(i => i.Tag).OfType<Type>().Distinct().ToList();
            foreach (var type in items)
            {
                await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Background);
                if (PageOf(type) is MorePage more)
                    foreach (var key in more.SectionKeys)
                    {
                        await System.Windows.Threading.Dispatcher.Yield(System.Windows.Threading.DispatcherPriority.Background);
                        more.MakeSection(key);
                    }
            }
            // What was typed while the pages were being made, searched again over all of them.
            if (SearchBox.IsKeyboardFocused && SearchBox.Text.Trim().Length > 0 && _searchDelay?.IsEnabled != true)
                ShowHits();
        }

        private void SearchBox_LostKeyboardFocus(object sender, KeyboardFocusChangedEventArgs e) =>
            SearchResults.Visibility = Visibility.Collapsed;

        private void SearchClear_Click(object sender, RoutedEventArgs e)
        {
            _searchDelay?.Stop();
            SearchBox.Clear();
            SearchBox.Focus();
        }

        private void SearchBox_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            switch (e.Key)
            {
                case Key.Escape:
                    // Esc clears; on an empty box it hands the keyboard back to the page.
                    if (SearchBox.Text.Length > 0) SearchBox.Clear();
                    else Keyboard.Focus(this);
                    e.Handled = true;
                    break;
                case Key.Enter:
                    // Typed faster than the search delay: search now, so Enter picks for what is typed.
                    if (_searchDelay?.IsEnabled == true)
                    {
                        _searchDelay.Stop();
                        ShowHits();
                    }
                    if (_hits.Count > 0 && SearchResults.IsVisible) PickHit(_hit);
                    e.Handled = true;
                    break;
                case Key.Down or Key.Up when _hits.Count > 0:
                    _hit = (_hit + (e.Key == Key.Down ? 1 : _hits.Count - 1)) % _hits.Count;
                    MarkHit();
                    e.Handled = true;
                    break;
            }
        }

        /// <summary>Searches again for what is typed, and shows the results under the box.</summary>
        private void ShowHits()
        {
            var query = SearchBox.Text;
            if (query.Trim().Length == 0 || !SearchBox.IsKeyboardFocused)
            {
                _hits.Clear();
                SearchResults.Visibility = Visibility.Collapsed;
                return;
            }
            _hits = SettingSearch.Find(SearchPlaces(), query);
            _hit = 0;
            SearchList.Children.Clear();
            if (_hits.Count == 0)
                SearchList.Children.Add(SettingSearch.Nothing(query));
            for (var i = 0; i < _hits.Count; i++)
            {
                var index = i;
                var view = SettingSearch.View(_hits[i]);
                view.MouseEnter += (_, _) =>
                {
                    _hit = index;
                    MarkHit();
                };
                view.MouseLeftButtonUp += (_, _) => PickHit(index);
                SearchList.Children.Add(view);
            }
            MarkHit();
            SearchResults.Visibility = Visibility.Visible;
        }

        /// <summary>Shades the result Enter picks (the first, until the arrow keys or the mouse move it).</summary>
        private void MarkHit()
        {
            for (var i = 0; i < SearchList.Children.Count; i++)
                if (SearchList.Children[i] is System.Windows.Controls.Border { Tag: SettingSearch.Hit } view)
                {
                    if (i == _hit) view.SetResourceReference(System.Windows.Controls.Border.BackgroundProperty, "SidebarItemSelectedBrush");
                    else view.Background = Brushes.Transparent;
                }
        }

        /// <summary>Opens the result's page and, once it is laid out, scrolls to the setting and flashes it.</summary>
        private void PickHit(int index)
        {
            if (index < 0 || index >= _hits.Count) return;
            var hit = _hits[index];
            SearchBox.Clear();
            SearchResults.Visibility = Visibility.Collapsed;
            hit.Place.Show();
            // A page shown for the first time is navigated to, loaded and grouped into its boxes first.
            Dispatcher.BeginInvoke(() =>
            {
                if (!hit.Place.Page.Reveal(hit.Row))
                    Keyboard.Focus(this);
            }, System.Windows.Threading.DispatcherPriority.ContextIdle);
        }

        // ---------------------------------------------------------------- play

        private async void TitlePlay_Click(object sender, RoutedEventArgs e) => await PlayOrStopAsync();

        private async void TitleUpdate_Click(object sender, RoutedEventArgs e) => await InstallUpdateAsync();

        /// <summary>Asks, downloads the newer launcher, swaps it in and restarts.</summary>
        public async Task InstallUpdateAsync()
        {
            var ws = Workspace.Current;
            if (ws.Update is not { } release) return;
            await InstallAsync(release, await Updater.NeedsPackageAsync(release));
        }

        /// <summary>The check at start: an update found is offered, Download or Later (Later keeps the
        /// Update button in the title bar; the next start asks again). Never over the game, a --play
        /// start or another question.</summary>
        private async Task CheckForUpdatesAtStartAsync()
        {
            var ws = Workspace.Current;
            try
            {
                await ws.CheckForUpdatesAsync(quiet: true);
                while (Application.Current.Windows.OfType<Prompt>().Any())
                    await Task.Delay(500);
                if (ws.Update is not { } release || ws.Game.Running || _directPlay) return;
                await InstallAsync(release, await Updater.NeedsPackageAsync(release));
            }
            catch (Exception e)
            {
                ws.Log(L.T("Could not check for updates: ") + e.GetBaseException().Message, LogLevel.Warn);
            }
        }

        /// <summary>An older package's engine: download the latest package and update it in place.</summary>
        public async Task UpdateEngineAsync()
        {
            var ws = Workspace.Current;
            var release = ws.Latest ?? await Updater.LatestAsync(Updater.BetaChannel(ws.Settings));
            if (release?.ZipUrl == null)
            {
                Prompt.Tell(L.T("Update engine"), L.F($"No package is published yet. See {Updater.ReleasesPage}"));
                return;
            }
            await InstallAsync(release, package: true);
        }

        /// <summary>A release the player picked in Settings › Updates › Version, older ones included.</summary>
        public async Task InstallVersionAsync(Updater.Release release)
        {
            if (release.IsCurrent)
            {
                Prompt.Tell(L.T("Already installed"), L.F($"You already have {Updater.CurrentLabel}."));
                return;
            }
            await InstallAsync(release, await Updater.SwitchNeedsPackageAsync(release), chosen: true);
        }

        private async Task InstallAsync(Updater.Release release, bool package, bool chosen = false)
        {
            var ws = Workspace.Current;
            if (package && ws.Game.Running)
            {
                Prompt.Tell(L.T("Close the game first"), L.T("This update replaces the engine, so the game has to be closed."));
                return;
            }
            var notes = release.Notes.Trim();
            if (notes.Length > 1200) notes = notes[..1200] + "…";
            var size = package ? release.ZipSize : release.ExeSize;
            var older = !release.IsNewerThan(Updater.Current, Updater.CurrentBeta);
            var body = L.F($"You have {Updater.CurrentLabel}. ") +
                       (package
                           ? L.F($"{(chosen ? "This version has a different engine" : "This update includes a new engine")}, so the whole package is downloaded ({size / 1048576} MB) and replaced in place.")
                           : L.F($"The launcher downloads {release.Tag} ({size / 1048576} MB), replaces itself and restarts.")) +
                       L.T(" Your settings, saves, mods and profiles stay.") +
                       (chosen && older
                           ? L.T("\n\nThis is an older version: settings it doesn't know are kept but not used, and the launcher may offer the newest version again at start.")
                           : "") +
                       (notes.Length > 0 ? $"\n\n{(chosen && older ? L.T("This version's notes") : L.T("What's new"))}:\n" + notes : "");
            // An update: Download, or Later (the Update button in the title bar stays). A version picked
            // in Settings › Updates: Switch.
            var verb = L.T("Switch");
            if (chosen
                    ? !Prompt.Confirm(L.F($"{verb} to {release.Name}?"), body, L.F($"{verb} now"))
                    : Prompt.Choose(L.F($"Update available: {release.Name}"), body, L.T("Later"), L.T("Download")) != 1)
                return;

            TitleUpdate.Visibility = Visibility.Visible;
            TitleUpdate.IsEnabled = false;
            var progress = new Progress<double>(p => TitleUpdateText.Text = L.F($"Downloading {p:P0}"));
            try
            {
                ws.SaveNow();
                var savesRoot = GameEnvironment.SavesRoot(ws.Settings);
                if (await System.Threading.Tasks.Task.Run(() => SaveGuard.Secure(savesRoot)) > 0)
                    ws.Log(L.F($"Saves: safety copy made in {SaveGuard.VaultDir} before the update."), LogLevel.Info);
                var skipped = await Updater.InstallAsync(release, package, progress);
                if (skipped.Count > 0)
                    Prompt.Tell(L.T("Updated, with files left out"),
                        L.T("Windows or an antivirus would not let these through, so the old ones were kept:\n") +
                        string.Join("\n", skipped.Take(5)) +
                        L.T("\n\nThe game runs without them; online play may need ") +
                        L.F($"them. Allow the Lantern Engine folder in the antivirus and update again, or take them from {Updater.ReleasesPage}"));
                Application.Current.Shutdown();
            }
            catch (Exception ex)
            {
                TitleUpdate.IsEnabled = true;
                TitleUpdateText.Text = L.T("Update");
                ws.Log(L.T("Update failed: ") + ex.GetBaseException().Message, LogLevel.Bad);
                Prompt.Tell(L.T("Update failed"), ex.GetBaseException().Message + L.F($"\n\nYou can download it by hand from {Updater.ReleasesPage}"));
            }
        }

        /// <summary>The one place the game is started from, so the title bar and the Play page agree.</summary>
        public async Task PlayOrStopAsync()
        {
            var ws = Workspace.Current;
            if (ws.Game.Running)
            {
                await ws.StopGameAsync();
                return;
            }

            // The launcher updated itself but the package kept an older engine: its crash fixes are
            // missing (players reported crashes long fixed). Asked at each Play until it is updated.
            if (Updater.EngineOutdated)
            {
                var choice = Prompt.Choose(L.T("Engine update needed"),
                    L.F($"This launcher is {Updater.CurrentLabel}, but the engine in this folder is older ({Updater.LocalEngine() ?? "unknown"}): ") +
                    L.T("the crash fixes since then are missing. Updating downloads the latest package and keeps your settings, saves and mods."),
                    L.T("Cancel"), L.T("Play anyway"), L.T("Update engine"));
                if (choice <= 0) return;
                if (choice == 2)
                {
                    await UpdateEngineAsync();
                    return;
                }
            }

            var s = ws.Settings;
            var ini = ws.Ini;
            // A connected Hunter's Dream account plays in its own co-op pool (the engine's thd backend);
            // decided here, not only when the Online page is opened.
            SuperMedo.Apply(ini, s);
            if (!HuntersDream.OnlinePaused && ini.Get("online_enabled") == "1" && SuperMedo.Selected(ini) &&
                ini.Get("seamless_enabled") != "1" && SuperMedo.Problem(ini, s) is { } medoProblem)
            {
                Prompt.Tell(L.T("Online"), medoProblem + L.T("\n\nOr turn \"Play online\" off to play offline."));
                Show("online");
                return;
            }
            var seamlessRoom = ini.Get("seamless_enabled") == "1";
            if (seamlessRoom)
            {
                var host = ini.Get("seamless_role", "guest") == "host";
                var direct = ini.Get("seamless_direct") == "1";
                var missing = direct ? !host && ini.Get("seamless_direct_host").Length == 0 : ini.Get("seamless_room").Length == 0;
                if (missing)
                {
                    Prompt.Tell(L.T("Seamless co-op"), host
                        ? L.T("Open the Online page once so the launcher makes your room's invite code, then press Play.")
                        : L.T("Paste the host's invite code on the Online page, then press Play."));
                    Show("online");
                    return;
                }
            }
            if (!HuntersDream.OnlinePaused && !seamlessRoom && ini.Get("online_enabled") == "1" && !SuperMedo.Selected(ini) && ini.Get("online_pool") != "huntersdream")
            {
                // Online play is The Hunter's Dream's: without its account the engine stays offline.
                Prompt.Tell(L.T("Online"), L.T("Connect your Hunter's Dream account on the Online page first (with Discord, or without it).") +
                                      L.T("\n\nOr turn \"Play online\" off to play offline."));
                Show("online");
                return;
            }

            // No port folder yet: look for it again, then let the player point at it right here.
            if (!GameRunner.Custom(s) && !Paths.IsPortDir(Paths.PortDir))
            {
                ws.ResolvePort();
                if (!Paths.IsPortDir(Paths.PortDir))
                {
                    if (!Prompt.Confirm(L.T("Where is the engine?"),
                            L.T("The launcher needs the Lantern Engine (the folder with run.py and bin\\bb-probe.exe). ") +
                            L.T("It is normally the folder the launcher came in."), L.T("Choose folder")))
                        return;
                    var dialog = new Microsoft.Win32.OpenFolderDialog { Title = L.T("Port folder (with Bloodborne.exe and run.py)") };
                    if (dialog.ShowDialog(this) != true) return;
                    if (!Paths.IsPortDir(dialog.FolderName))
                    {
                        Prompt.Tell(L.T("Not the port folder"), L.T("That folder has no run.py with Bloodborne.exe or bin\\bb-probe.exe."));
                        return;
                    }
                    s.Set("port_dir", dialog.FolderName);
                    ws.ResolvePort();
                    ws.SaveNow();
                }
            }

            // No game folder with eboot.bin: the engine would only log "No eboot.bin". The setup
            // guide says what is missing instead.
            if (!GameRunner.Custom(s) && !System.IO.File.Exists(System.IO.Path.Combine(GameEnvironment.GameDir(s), "eboot.bin")))
            {
                if (Prompt.Choose(L.T("The game is not set up yet"),
                        L.T("The launcher has no game folder with eboot.bin in it, so the game cannot start. The setup guide shows where your Bloodborne files go and checks them."),
                        L.T("Cancel"), L.T("Open the setup guide")) == 1)
                {
                    ShowLauncher();
                    SetupGuide.Open();
                }
                return;
            }

            // A patch that changes the game's region (Unlock Game Region) leaves the game without a
            // Bloodborne server: online, ask whether to leave it out for this session.
            GameEnvironment.RegionPatchOnline = null;
            if (!HuntersDream.OnlinePaused && ini.Get("online_enabled") == "1" && ini.Get("online_connected", "1") == "1" &&
                Patches.RegionChanging(GameEnvironment.PatchesDir(s)) is { Count: > 0 } region)
            {
                var names = string.Join(", ", region.Select(p => $"\"{p.Name}\""));
                var choice = Prompt.Choose(L.T("Play online with ") + names + "?",
                    names + L.T(" moves the game to a region that has no Bloodborne server, so Play online fails with ") +
                    L.T("\"Failure to acquire information from the Bloodborne server\".\n\n") +
                    L.T("Leave it out for this session? It stays on for offline play."),
                    L.T("Cancel"), L.T("Keep it on"), L.T("Leave it out"));
                if (choice <= 0) return;
                GameEnvironment.RegionPatchOnline = choice == 1 ? "keep" : "skip";
                ws.Log(choice == 1 ? L.F($"{names}: kept on while online; Play online will not reach the server.")
                    : L.F($"{names}: left out while online; it applies again with online off."), LogLevel.Step);
            }

            if (!HuntersDream.OnlinePaused && ini.Get("online_enabled") == "1")
                await Online.PrepareTrustAsync();

            // A third-party resolution patch while the output resolution sets the game's resolution
            // and UI itself: it would keep its own shape (black bars) and stop the widescreen hooks.
            GameEnvironment.ResolutionPatchChoice = null;
            var (outW, outH) = Choices.OutputSize(ini.Get("output_res", "1920x1080"));
            // With a 1920 × 1080 output, such a patch makes the game draw at its size itself: the
            // upscaler then runs at 1:1 (DLAA whatever the preset) and in-game preset changes crashed
            // (Discord, 3440 × 1440). Its size as the output resolution keeps the shape and lets the
            // presets work: offered once per set of patches.
            if ((outW, outH) == (1920, 1080) &&
                Patches.ResolutionSetting(GameEnvironment.PatchesDir(s)) is { Count: > 0 } native &&
                native.Select(p => System.Text.RegularExpressions.Regex.Match(p.Name, @"(\d{3,5})x(\d{3,5})"))
                    .FirstOrDefault(m => m.Success) is { } size &&
                (int.Parse(size.Groups[1].Value), int.Parse(size.Groups[2].Value)) is var (patchW, patchH) &&
                (patchW, patchH) != (1920, 1080))
            {
                var names = string.Join(", ", native.Select(p => $"\"{p.Name}\""));
                if (s.Str("resolution_patch_native") != names)
                {
                    var choice = Prompt.Choose(L.F($"Use {patchW} × {patchH} as the output resolution?"),
                        names + L.F($" makes the game draw at {patchW} × {patchH} by itself, so the upscaler preset can't lower ") +
                        L.T("the render resolution (the upscaler runs as DLAA / native AA).\n\n") +
                        L.F($"Setting the output resolution to {patchW} × {patchH} instead gives the same picture and shape, ") +
                        L.T("and the presets (Quality, Balanced, Performance) work. The patch is then left out while that output is set."),
                        L.T("Cancel"), L.F($"Use {patchW} × {patchH}"), L.T("Keep the patch"));
                    if (choice <= 0) return;
                    if (choice == 1)
                    {
                        ini.Set("output_res", $"{patchW}x{patchH}");
                        ini.Save();
                        (outW, outH) = (patchW, patchH);
                        GameEnvironment.ResolutionPatchChoice = "skip";
                        ws.Log(L.F($"Output resolution {patchW} × {patchH}; {names} left out (presets work)."), LogLevel.Step);
                    }
                    else
                    {
                        s.Set("resolution_patch_native", names);
                        ws.SaveNow();
                    }
                }
            }
            if ((outW, outH) != (1920, 1080) && GameEnvironment.ResolutionPatchChoice is null &&
                Patches.ResolutionSetting(GameEnvironment.PatchesDir(s)) is { Count: > 0 } sizing)
            {
                var names = string.Join(", ", sizing.Select(p => $"\"{p.Name}\""));
                // "Always on" is remembered for this set of patches (#18); another set asks again.
                var choice = s.Str("resolution_patch_always") == names ? 1 : Prompt.Choose(L.T("Play with ") + names + "?",
                    L.F($"The output resolution ({outW} × {outH}) already sets the game's resolution and widens the picture and UI to fill the screen. ") +
                    names + L.T(" sets its own on top: the picture keeps the patch's shape, with black bars.\n\n") +
                    L.T("Leave it out? It still applies with a 1920 × 1080 output. \"Always on\" keeps it and does not ask again ") +
                    L.T("until the resolution patches switched on in Mods change."),
                    L.T("Cancel"), L.T("Keep on"), L.T("Leave out"), L.T("Always on"));
                if (choice <= 0) return;
                if (choice == 3)
                {
                    s.Set("resolution_patch_always", names);
                    ws.SaveNow();
                    choice = 1;
                }
                GameEnvironment.ResolutionPatchChoice = choice == 1 ? "keep" : "skip";
                ws.Log(choice == 1 ? L.F($"{names}: kept on with the {outW} × {outH} output.")
                    : L.F($"{names}: left out; the output resolution sets the picture."), LogLevel.Step);
            }

            // A game left running in the background (an earlier start that never ended, or another
            // launcher's) keeps its whole memory pool, and a new start fails (Windows error 1450).
            var leftovers = System.Diagnostics.Process.GetProcessesByName("bb-probe");
            if (leftovers.Length > 0)
            {
                var choice = Prompt.Choose(L.T("The game is still running in the background"),
                    L.T("bb-probe.exe from an earlier start is still running and holds the game's memory, so a new start would fail. ") +
                    L.T("Close it and start?"), L.T("Cancel"), L.T("Close it and start"));
                if (choice <= 0) return;
                foreach (var leftover in leftovers)
                {
                    try
                    {
                        leftover.Kill(entireProcessTree: true);
                        using var wait = new System.Threading.CancellationTokenSource(5000);
                        await leftover.WaitForExitAsync(wait.Token); // off the UI thread
                    }
                    catch { /* already gone */ }
                    leftover.Dispose();
                }
                ws.Log(L.T("Closed the game left running in the background."), LogLevel.Step);
            }

            if (!s.Bool("memory_check_off") && MemoryCheck.Problem(ws.Ini) is { } memory)
            {
                var choice = Prompt.Choose(L.T("Windows may run out of memory"), memory, L.T("Cancel"), L.T("Don't ask again"), L.T("Start anyway"));
                if (choice <= 0) return;
                if (choice == 1) s.Set("memory_check_off", true);
            }

            // bbport: the current profile has no saves, but saves were found elsewhere (a folder used
            // before, or SaveGuard's safety copies): asked before the game starts a new one.
            var savesRoot = GameEnvironment.SavesRoot(s);
            var asked = false;
            if (SavesFolder.Moved(s.Str("user_dir"), s.Str(SavesFolder.LastRootKey), savesRoot) is { } lastRoot)
            {
                asked = true;
                var here = SavesFolder.Full(savesRoot);
                var choice = Prompt.Choose(L.T("A different saves folder"),
                    L.F($"The last start played with the saves in {lastRoot}.\n\n") +
                    L.F($"This start would use {here}: this copy of Lantern Engine's own folder, because the Saves page names no folder.\n\n") +
                    L.T("\"Use the last folder\" keeps playing with the saves from the last start and sets it on the Saves page. \"Use this copy's folder\" plays with the saves here; the other folder stays as it is."),
                    L.T("Cancel"), L.T("Use this copy's folder"), L.T("Use the last folder"));
                if (choice <= 0) return;
                if (choice == 2)
                {
                    s.Set("user_dir", lastRoot);
                    ws.SaveNow();
                    ws.Log(L.F($"Saves folder: {lastRoot}"), LogLevel.Good);
                    ReloadPages();
                    savesRoot = GameEnvironment.SavesRoot(s);
                }
            }
            var savesProfile = PlayerProfiles.Current(s);
            var packageSaves = System.IO.Path.Combine(Paths.DataDir, "user");
            var lost = asked ? new List<SaveGuard.Found>() : await System.Threading.Tasks.Task.Run(() => SaveGuard.Elsewhere(savesRoot, savesProfile, new[] { packageSaves }));
            if (lost.FirstOrDefault() is { } foundSaves)
            {
                var (start, changed) = SaveGuard.Ask(s, foundSaves, Prompt.Choose, Prompt.Tell);
                if (changed)
                {
                    ws.SaveNow();
                    ws.Log(foundSaves.Copy == null ? L.F($"Saves folder: {foundSaves.Root}")
                                                   : L.T("Saves restored from the safety copy."), LogLevel.Good);
                    ReloadPages();
                }
                if (!start) return;
            }

            var error = ws.StartGame();
            if (error != null)
            {
                System.Windows.MessageBox.Show(this, error, L.T("Could not start the game"), MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            if (s.Bool("switch_to_output"))
                Show("output");
        }

        private void UpdatePlayButton()
        {
            var running = Workspace.Current.Game.Running;
            TitlePlayText.Text = running ? L.T("Stop") : L.T("Play");
            TitlePlayIcon.Symbol = running ? SymbolRegular.Stop24 : SymbolRegular.Play24;
            TitlePlayIcon.Foreground = (Brush)FindResource(running ? "DangerTextBrush" : "AccentTextFillColorPrimaryBrush");
            TitlePlay.ToolTip = running ? L.T("Stop the game") : L.T("Start the game");
            foreach (var page in _pages.Values)
                (page as PlayPage)?.UpdateRunning();
        }

        // ---------------------------------------------------------------- log

        /// <summary>The activity log keeps the newest entries only (the game's output has its own page).</summary>
        private const int MaxLogItems = 500;

        /// <summary>The log list's own scroller (inside its template, so the list can virtualize).</summary>
        private System.Windows.Controls.ScrollViewer? LogScroller =>
            LogList.Template?.FindName("LogScroller", LogList) as System.Windows.Controls.ScrollViewer;

        private void OnLogged(string message, LogLevel level)
        {
            _log.Add(new LogItem { Time = DateTime.Now.ToString("HH:mm:ss"), Message = message, Color = BrushFor(level) });
            if (_log.Count > MaxLogItems) _log.RemoveAt(0);
            LogCount.Text = L.F($"{_log.Count} events");
            // Folded: nothing to scroll (ToggleLog scrolls to the end when it opens).
            if (_logExpanded) LogScroller?.ScrollToEnd();

            StatusText.Text = message;
            StatusText.ToolTip = message;
            StatusDot.Fill = level == LogLevel.Info ? (Brush)FindResource("StatusDotIdleBrush") : BrushFor(level);
        }

        private Brush BrushFor(LogLevel level) => (Brush)FindResource(level switch
        {
            LogLevel.Good => "GoodTextBrush",
            LogLevel.Bad => "DangerTextBrush",
            LogLevel.Warn => "WarningTextBrush",
            LogLevel.Step => "AccentTextFillColorPrimaryBrush",
            _ => "TextFillColorSecondaryBrush",
        });

        private void ClearLog_Click(object sender, RoutedEventArgs e)
        {
            _log.Clear();
            LogCount.Text = "";
            StatusText.Text = L.T("Ready");
            StatusText.ToolTip = null;
            StatusDot.Fill = (Brush)FindResource("StatusDotIdleBrush");
        }

        private void ToggleLog_Click(object sender, RoutedEventArgs e) => ToggleLog();
        private void StatusBar_Click(object sender, MouseButtonEventArgs e) => ToggleLog();

        private void ToggleLog()
        {
            _logExpanded = !_logExpanded;
            LogPanel.Visibility = _logExpanded ? Visibility.Visible : Visibility.Collapsed;
            if (_logExpanded)
                Dispatcher.BeginInvoke(() => LogScroller?.ScrollToEnd(), System.Windows.Threading.DispatcherPriority.Loaded);
        }
    }

    public class LogItem
    {
        public string Time { get; set; } = "";
        public string Message { get; set; } = "";
        public Brush Color { get; set; } = Brushes.White;
    }
}
