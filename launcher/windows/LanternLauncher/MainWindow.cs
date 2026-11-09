using System.Collections.ObjectModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Controls.Documents;
using Avalonia.Controls.Shapes;
using Avalonia.Controls.Templates;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using BloodborneLauncher.Core;
using BloodborneLauncher.Views;
using Path = System.IO.Path;

namespace BloodborneLauncher
{
    /// <summary>
    /// The launcher window: a slim title bar (window buttons, the settings search, the title, Play),
    /// the tabs (Modern) or the source-list sidebar (Classic), the page, a folding activity log and
    /// a one-line status bar, over the game's artwork in the Modern layout. Built in code like the
    /// pages; the WPF launcher's MainWindow.xaml(.cs) is the same window.
    /// </summary>
    public sealed class MainWindow : Window
    {
        private readonly ObservableCollection<LogItem> _log = new();
        private readonly Dictionary<Type, LauncherPage> _pages = new();
        private bool _logExpanded;
        private bool _closing;

        public static MainWindow? Instance =>
            (Application.Current?.ApplicationLifetime as IClassicDesktopStyleApplicationLifetime)?.MainWindow as MainWindow;

        /// <summary>Where Prompt puts its dialogs: over the whole window, title bar included.</summary>
        public Panel DialogLayer { get; } = new() { ZIndex = 20 };

        // The window's parts (MainWindow.xaml's named elements).
        private readonly Panel _backdrop = new() { IsHitTestVisible = false, ClipToBounds = true };
        private readonly Image _backdropImage = new() { Stretch = Stretch.UniformToFill, IsVisible = false };
        private readonly Rectangle _backdropShade = new() { IsVisible = false };
        private readonly StackPanel _macButtons = new() { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Background = Brushes.Transparent };
        private readonly StackPanel _winButtons = new() { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 1), IsVisible = false };
        private Button _macClose = null!, _macMinimize = null!, _macMaximize = null!, _winClose = null!, _winMinimize = null!, _winMaximize = null!;
        private readonly Panel _searchArea = new() { HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Center, Width = 200 };
        private readonly TextBox _searchBox = new();
        private readonly Button _searchClear = new() { Width = 18, Height = 18, IsVisible = false, Focusable = false };
        private readonly Run _titlePage = new("");
        private readonly StackPanel _titleTools = new() { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
        private readonly Button _titleUpdate = new() { IsVisible = false, Margin = new Thickness(0, 0, 6, 0) };
        private readonly TextBlock _titleUpdateText = new() { FontSize = 11.5, VerticalAlignment = VerticalAlignment.Center };
        private readonly Button _titlePlay = new() { Margin = new Thickness(0, 0, 6, 0) };
        private readonly SymbolIcon _titlePlayIcon = new(SymbolRegular.Play24) { FontSize = 13, Margin = new Thickness(0, 0, 5, 0) };
        private readonly TextBlock _titlePlayText = new() { FontSize = 11.5, VerticalAlignment = VerticalAlignment.Center };
        private readonly Border _sidebar = new() { Width = 196, IsVisible = false, BorderThickness = new Thickness(0, 0, 1, 0) };
        private readonly Border _tabStrip = new() { HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 12, 0, 4), Padding = new Thickness(4), CornerRadius = new CornerRadius(20), BorderThickness = new Thickness(1) };
        private readonly ContentControl _pageHost = new() { Focusable = false, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch };
        private readonly Border _logPanel = new() { Height = 140, IsVisible = false, BorderThickness = new Thickness(0, 1, 0, 0) };
        private readonly ScrollViewer _logScroller = new() { VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto, Padding = new Thickness(16, 8, 48, 8) };
        private readonly Ellipse _statusDot = new() { Width = 5, Height = 5, VerticalAlignment = VerticalAlignment.Center };
        private readonly TextBlock _statusText = new() { FontSize = 10.5, Margin = new Thickness(8, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis, MaxWidth = 620 };
        private readonly TextBlock _savedText = new() { FontSize = 10.5, Margin = new Thickness(0, 0, 12, 0), VerticalAlignment = VerticalAlignment.Center };
        private readonly TextBlock _logCount = new() { FontSize = 10.5, Margin = new Thickness(0, 0, 12, 0), VerticalAlignment = VerticalAlignment.Center };
        private readonly TextBlock _versionText = new() { FontSize = 10.5, VerticalAlignment = VerticalAlignment.Center };
        private readonly Border _searchResults = new() { HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top, Width = 420, Padding = new Thickness(4), CornerRadius = new CornerRadius(10), BorderThickness = new Thickness(1), IsVisible = false, ZIndex = 10 };
        private readonly StackPanel _searchList = new();

        // The tabs (Modern) and the sidebar's entries (Classic), by page.
        private readonly List<RadioButton> _tabs = new(), _classic = new();
        private RadioButton _savesItem = null!, _classicSaves = null!;
        private RadioButton _playItem = null!, _controlsItem = null!, _onlineItem = null!, _settingsItem = null!, _moreItem = null!;
        private RadioButton _classicPlay = null!, _classicControls = null!, _classicMods = null!, _classicOnline = null!,
            _classicSettings = null!, _classicSettingsBottom = null!, _classicEngine = null!, _classicOutput = null!;
        private RadioButton? _classicCheats; // only when the engine applies cheats
        private bool _sidebarNav; // the sidebar shown instead of the tabs (Classic, or Settings › Navigation)

        public MainWindow()
        {
            Title = "Lantern Engine";
            Width = 1040;
            Height = 780;
            MinWidth = 820;
            MinHeight = 560;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            this.SetResourceReference(BackgroundProperty, "WindowBaseBrush");
            this.SetResourceReference(FontFamilyProperty, "UiFont");
            this.SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
            // Our own title bar. Windows keeps its resize borders, snap and shadow under it; elsewhere
            // the window has no frame and the edges below resize it.
            if (OperatingSystem.IsWindows())
            {
                ExtendClientAreaToDecorationsHint = true;
                ExtendClientAreaChromeHints = Avalonia.Platform.ExtendClientAreaChromeHints.NoChrome;
                ExtendClientAreaTitleBarHeightHint = -1;
            }
            else
                SystemDecorations = SystemDecorations.None;

            var ws = Workspace.Current;
            // Layout, read once at start (Settings › Layout restarts the launcher to switch).
            var args = Environment.GetCommandLineArgs();
            var forced = Array.IndexOf(args, "--layout") is var at and >= 0 && at + 1 < args.Length ? args[at + 1] : null;
            SettingRow.Classic = (forced ?? ws.Settings.Str("ui_layout")) == "classic";
            LauncherTheme.Install(glass: !SettingRow.Classic);

            Content = Build();

            _logScroller.Content = new ItemsControl
            {
                ItemsSource = _log,
                ItemTemplate = new FuncDataTemplate<LogItem>((item, _) => LogLine(item), supportsRecycling: false),
            };

            ws.Logged += OnLogged;
            ws.Saved += () => _savedText.Text = L.F($"Saved {DateTime.Now:HH:mm:ss}");
            ws.GameStateChanged += UpdatePlayButton;
            ws.GameStateChanged += UpdateDiscord;
            ws.GameCrashed += OnGameCrashed;
            ws.SignInRefused += OnSignInRefused;
            ws.Initialise();
            RestoreWindowPlacement(ws.Settings);
            AddHandler(KeyDownEvent, Window_KeyDown, RoutingStrategies.Bubble);
            AddHandler(PointerPressedEvent, Window_PointerPressed, RoutingStrategies.Tunnel, handledEventsToo: true);
            AddHandler(PointerWheelChangedEvent, (_, e) =>
            {
                if (_modal && !(e.Source is Visual source && DialogLayer.IsVisualAncestorOf(source)))
                    e.Handled = true;
            }, RoutingStrategies.Tunnel, handledEventsToo: true);
            Deactivated += (_, _) => _searchResults.IsVisible = false;

            if (Array.IndexOf(args, "--test-prompt") is var shot and >= 0 && shot + 1 < args.Length)
            {
                Prompt.SnapshotPath = args[shot + 1];
                ShowActivated = false;
                ShowInTaskbar = false;
                WindowStartupLocation = WindowStartupLocation.Manual;
                Position = new PixelPoint(-32000, -32000);
            }
            // --play (Bloodborne.exe): the game straight away with the saved settings, the launcher
            // out of sight meanwhile (its prompts show on their own, in the middle of the screen).
            _directPlay = args.Contains("--play");
            if (_directPlay)
                WindowState = WindowState.Minimized;
            ApplyNavigation();
            LauncherTheme.Apply(ws.Settings, glass: !SettingRow.Classic);
            if (!SettingRow.Classic)
                ApplyBackdrop();
            ApplyWindowButtons();
            ApplyIcon();

            _versionText.Text = Updater.CurrentLabel;

            ws.UpdateStateChanged += () =>
            {
                _titleUpdate.IsVisible = ws.Update != null;
                if (ws.Update != null) _titleUpdate.SetTip(L.F($"{ws.Update.Name} is available"));
            };

            PositionChanged += (_, _) => RememberNormalBounds();
            Resized += (_, _) => RememberNormalBounds();
            Opened += (_, _) =>
            {
                if (OperatingSystem.IsWindows())
                    Win32Properties.AddWndProcHookCallback(this, SnapLayoutHook);
                RememberNormalBounds();
                PadNavigation.Attach(this);
                if (_directPlay) Hide();
                Updater.CleanUp();
                // Running a beta means choosing beta: kept after updating to a stable release.
                if (Updater.CurrentBeta > 0 && ws.Settings.Str("update_channel").Length == 0)
                {
                    ws.Settings.Set("update_channel", "beta");
                    ws.SaveSoon();
                }
                // Releases carry the Windows package: elsewhere the launcher does not offer them.
                if (ws.Settings.Bool("check_updates") && Updater.Supported)
                    _ = CheckForUpdatesAtStartAsync();
                Dispatcher.UIThread.Post(Started, DispatcherPriority.Background);
            };
            Closing += OnClosing;
        }

        /// <summary>The first page, the sample dialogs of the test switches, and the questions asked once.</summary>
        private void Started()
        {
            var ws = Workspace.Current;
            (_sidebarNav ? _classicPlay : _playItem).IsChecked = true;
            UpdatePlayButton();
            var args = Environment.GetCommandLineArgs();
            // --test-trophy: a sample notification, to check how it looks over the game.
            if (args.Contains("--test-trophy"))
                TrophyToast.Announce(TrophyTracker.ById[14]);
            ws.Log(Paths.IsPortDir(Paths.PortDir)
                    ? L.F($"Lantern Engine ready. Engine: {Paths.PortDir}")
                    : L.T("Lantern Engine ready. Choose the engine folder in Settings."),
                Paths.IsPortDir(Paths.PortDir) ? LogLevel.Good : LogLevel.Warn);
            if (NativeRuntime.UnpackedBytes > 0 && NativeRuntime.Folder != null)
                ws.Log(L.F($"First start of this launcher version: unpacked the graphics libraries it carries (Skia, HarfBuzz and ANGLE, {NativeRuntime.UnpackedBytes / 1048576.0:0.#} MB) to {NativeRuntime.Folder}. Nothing is installed in Windows.") +
                       (NativeRuntime.RemovedOld > 0 ? " " + L.T("The previous version's copy was removed.") : ""), LogLevel.Step);
            if (LanternApp.OldRuntimeRemoved > 0)
                ws.Log(L.F($"Removed {LanternApp.OldRuntimeRemoved} files of the old launcher's .NET runtime from its folder: this launcher is native code and doesn't use them."), LogLevel.Step);
            if (LanternApp.OldExtractsRemoved > 0)
                ws.Log(L.F($"Removed {LanternApp.OldExtractsRemoved} copies of the old launcher's Windows libraries from the temp folder ({LanternApp.OldExtractBytes / 1048576.0:0.#} MB): each older version unpacked its own there, and this launcher doesn't use them."), LogLevel.Step);
            if (Updater.CleanOldFiles(ws.Settings, Paths.PortDir) is > 0 and var cleaned)
                ws.Log(L.F($"Removed {cleaned} leftover files from older versions."), LogLevel.Step);
            Dispatcher.UIThread.Post(() =>
            {
                // --test-prompt <png>: a picture of a sample dialog over the launcher, drawn off-screen.
                // --test-import: the import dialog with a long pasted code instead.
                if (Prompt.SnapshotPath != null && args.Contains("--test-import"))
                    Prompt.Ask(L.T("Import settings"), L.F($"Paste the code someone shared (it starts with {SharedSettings.CodePrefix})."),
                        new Prompt.Field(L.T("Code"), SharedSettings.ToCode(SharedSettings.Take(ws.Ini, ws.Settings)), "", Multiline: true));
                else if (Prompt.SnapshotPath != null)
                    Prompt.Confirm(L.T("Switch layout"), L.T("The launcher restarts to change its layout."), L.T("Restart now"));
                else if (_directPlay)
                    _ = PlayDirectAsync();
                // The setup guide: with --setup, or once on a first start whose game folder is not
                // ready (a game already set up never sees it). The settings for this PC follow it.
                else if (Prompt.SnapshotPath == null && (Environment.GetCommandLineArgs().Contains("--setup") ||
                         (!ws.Settings.Bool("setup_shown") && Paths.IsPortDir(Paths.PortDir) &&
                          GameCheck.Inspect(GameEnvironment.GameDir(ws.Settings)).Health != Health.Good)))
                    SetupGuide.Open(firstRun: true);
                // Once: the settings that suit this PC, shown before anything changes.
                else if (!ws.Settings.Bool("hardware_offered") && Paths.IsPortDir(Paths.PortDir))
                    OfferBestSettings(firstRun: true);
                else if (ws.Settings.Bool("hardware_offered") && !ws.Settings.Bool("thanks_maximilian_dood"))
                    ShowThanks();
                // Once, after updating from an older launcher (a new install starts on New): the
                // newer engine or the one 0.9.33 ran, the player's choice. Nothing changes unasked.
                else if (ws.Settings.Bool("hardware_offered") && !ws.Settings.Bool("engine_preset_asked") && Paths.IsPortDir(Paths.PortDir))
                    AskEnginePreset();
                else if (ws.Settings.Bool("hardware_offered") && TestedSettings.ShouldAsk(ws.Ini, ws.Settings) && Paths.IsPortDir(Paths.PortDir))
                    AskTestedSettings();
            }, DispatcherPriority.ApplicationIdle);
        }

        // ---------------------------------------------------------------- building the window

        private Control Build()
        {
            var root = new Grid { RowDefinitions = new RowDefinitions("34,*,Auto,Auto") };

            // Glass (Modern): the game's own artwork, blurred and darkened, behind the frosted panes.
            _backdrop.SetResourceReference(Panel.BackgroundProperty, "WindowBaseBrush");
            _backdrop.Children.Add(_backdropImage);
            _backdrop.Children.Add(_backdropShade);
            Grid.SetRowSpan(_backdrop, 4);
            root.Children.Add(_backdrop);

            root.Children.Add(TitleBar());

            // ==================== Tabs (Modern) or sidebar (Classic) + page ====================
            var body = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), RowDefinitions = new RowDefinitions("Auto,*") };
            body.SetResourceReference(Panel.BackgroundProperty, "ContentAreaBrush");
            Grid.SetRow(body, 1);
            _sidebar.Child = Sidebar();
            _sidebar.SetResourceReference(Border.BackgroundProperty, "WindowBaseBrush");
            _sidebar.SetResourceReference(Border.BorderBrushProperty, "CardStrokeColorDefaultBrush");
            Grid.SetRowSpan(_sidebar, 2);
            body.Children.Add(_sidebar);
            _tabStrip.Child = Tabs();
            _tabStrip.SetResourceReference(Border.BackgroundProperty, "PillTrackBrush");
            _tabStrip.SetResourceReference(Border.BorderBrushProperty, "GlassPaneStrokeBrush");
            Grid.SetColumn(_tabStrip, 1);
            body.Children.Add(_tabStrip);
            _pageHost.Margin = new Thickness(0, 0, 8, 0);
            Grid.SetColumn(_pageHost, 1);
            Grid.SetRow(_pageHost, 1);
            body.Children.Add(_pageHost);
            root.Children.Add(body);

            // ==================== Activity log (folded by default) ====================
            _logPanel.SetResourceReference(Border.BackgroundProperty, "PanelDeepBrush");
            _logPanel.SetResourceReference(Border.BorderBrushProperty, "CardStrokeColorDefaultBrush");
            var logGrid = new Panel();
            logGrid.Children.Add(_logScroller);
            var clear = TitleButton(new SymbolIcon(SymbolRegular.Broom24) { FontSize = 13 }, L.T("Clear activity"), ClearLog);
            clear.HorizontalAlignment = HorizontalAlignment.Right;
            clear.VerticalAlignment = VerticalAlignment.Top;
            clear.Margin = new Thickness(0, 6, 14, 0);
            logGrid.Children.Add(clear);
            _logPanel.Child = logGrid;
            Grid.SetRow(_logPanel, 2);
            root.Children.Add(_logPanel);

            // ==================== Status bar ====================
            var status = new Border { Height = 24, Cursor = new Cursor(StandardCursorType.Hand), BorderThickness = new Thickness(0, 1, 0, 0) };
            status.SetResourceReference(Border.BackgroundProperty, "PillTrackBrush");
            status.SetResourceReference(Border.BorderBrushProperty, "CardStrokeColorDefaultBrush");
            status.SetTip(L.T("Show or hide activity"));
            status.PointerReleased += (_, e) => { if (e.InitialPressMouseButton == MouseButton.Left) ToggleLog(); };
            var statusGrid = new Grid { Margin = new Thickness(12, 0) };
            var left = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            _statusDot.SetResourceReference(Shape.FillProperty, "StatusDotIdleBrush");
            _statusText.Text = L.T("Ready");
            foreach (var faint in new[] { _statusText, _savedText, _logCount, _versionText })
                faint.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
            _versionText.SetResourceReference(TextBlock.FontFamilyProperty, "MonoFont");
            left.Children.Add(_statusDot);
            left.Children.Add(_statusText);
            statusGrid.Children.Add(left);
            var right = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
            right.Children.Add(_savedText);
            right.Children.Add(_logCount);
            right.Children.Add(_versionText);
            statusGrid.Children.Add(right);
            status.Child = statusGrid;
            Grid.SetRow(status, 3);
            root.Children.Add(status);

            // ==================== Settings search results (over everything, under the search box) ====================
            _searchResults.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
            _searchResults.SetResourceReference(Border.BorderBrushProperty, "DialogStrokeBrush");
            _searchResults.Child = _searchList;
            Grid.SetRow(_searchResults, 1);
            Grid.SetRowSpan(_searchResults, 3);
            root.Children.Add(_searchResults);

            Grid.SetRowSpan(DialogLayer, 4);
            KeyboardNavigation.SetTabNavigation(DialogLayer, KeyboardNavigationMode.Cycle);
            DialogLayer.Children.CollectionChanged += (_, _) => SetModal(root, DialogLayer.Children.Count > 0);
            root.Children.Add(DialogLayer);

            if (!OperatingSystem.IsWindows())
            {
                var frame = new Panel();
                frame.Children.Add(root);
                foreach (var edge in ResizeEdges()) frame.Children.Add(edge);
                return frame;
            }
            return root;
        }

        /// <summary>Without a window frame (Linux): thin edges and corners that resize the window.</summary>
        private IEnumerable<Control> ResizeEdges()
        {
            Control Edge(WindowEdge edge, HorizontalAlignment h, VerticalAlignment v, double w, double ht, StandardCursorType cursor)
            {
                var grip = new Border
                {
                    Background = Brushes.Transparent,
                    HorizontalAlignment = h,
                    VerticalAlignment = v,
                    Width = w,
                    Height = ht,
                    Cursor = new Cursor(cursor),
                    ZIndex = 30,
                };
                grip.PointerPressed += (_, e) =>
                {
                    if (WindowState == WindowState.Normal && e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
                        BeginResizeDrag(edge, e);
                };
                return grip;
            }
            const double s = 5, c = 10;
            yield return Edge(WindowEdge.West, HorizontalAlignment.Left, VerticalAlignment.Stretch, s, double.NaN, StandardCursorType.LeftSide);
            yield return Edge(WindowEdge.East, HorizontalAlignment.Right, VerticalAlignment.Stretch, s, double.NaN, StandardCursorType.RightSide);
            yield return Edge(WindowEdge.North, HorizontalAlignment.Stretch, VerticalAlignment.Top, double.NaN, s, StandardCursorType.TopSide);
            yield return Edge(WindowEdge.South, HorizontalAlignment.Stretch, VerticalAlignment.Bottom, double.NaN, s, StandardCursorType.BottomSide);
            yield return Edge(WindowEdge.NorthWest, HorizontalAlignment.Left, VerticalAlignment.Top, c, c, StandardCursorType.TopLeftCorner);
            yield return Edge(WindowEdge.NorthEast, HorizontalAlignment.Right, VerticalAlignment.Top, c, c, StandardCursorType.TopRightCorner);
            yield return Edge(WindowEdge.SouthWest, HorizontalAlignment.Left, VerticalAlignment.Bottom, c, c, StandardCursorType.BottomLeftCorner);
            yield return Edge(WindowEdge.SouthEast, HorizontalAlignment.Right, VerticalAlignment.Bottom, c, c, StandardCursorType.BottomRightCorner);
        }

        private static Button TitleButton(Control content, string tip, Action click)
        {
            var button = new Button { Content = content };
            button.Classes.Add("TitleIcon");
            button.SetTip(tip);
            button.Click += (_, _) => click();
            return button;
        }

        private Control TitleBar()
        {
            var bar = new Border { BorderThickness = new Thickness(0, 0, 0, 1), Background = Brushes.Transparent };
            bar.SetResourceReference(Border.BorderBrushProperty, "CardStrokeColorDefaultBrush");
            var grid = new Grid();
            // The empty parts of the title bar move the window (not its buttons or the search box);
            // a double click maximises it.
            bar.PointerPressed += (_, e) =>
            {
                if (!ReferenceEquals(e.Source, bar) && !ReferenceEquals(e.Source, grid)) return;
                if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
                if (e.ClickCount == 2) ToggleMaximize();
                else BeginMoveDrag(e);
            };

            Button Light(uint colour, string tip, string glyph, Action click)
            {
                var path = new Avalonia.Controls.Shapes.Path
                {
                    Data = Geometry.Parse(glyph),
                    Stroke = new SolidColorBrush(Color.FromUInt32(0x99000000)),
                    StrokeThickness = 1.1,
                    Opacity = 0,
                    Width = 12,
                    Height = 12,
                };
                path.Classes.Add("LightGlyph");
                var button = new Button { Background = new SolidColorBrush(Color.FromUInt32(colour)), Content = path, Margin = new Thickness(0, 0, 8, 0) };
                button.Classes.Add("TrafficLight");
                button.SetTip(tip);
                button.Click += (_, _) => click();
                return button;
            }
            _macButtons.Classes.Add("Lights");
            _macClose = Light(0xFFFF5F57, L.T("Close"), "M 3.5,3.5 L 8.5,8.5 M 8.5,3.5 L 3.5,8.5", Close);
            _macMinimize = Light(0xFFFEBC2E, L.T("Minimize"), "M 3,6 L 9,6", () => WindowState = WindowState.Minimized);
            _macMaximize = Light(0xFF28C840, L.T("Maximize"), "M 3,6 L 9,6 M 6,3 L 6,9", ToggleMaximize);
            grid.Children.Add(_macButtons);

            Button Caption(string tip, string glyph, Action click, bool close = false)
            {
                var path = new Avalonia.Controls.Shapes.Path { Data = Geometry.Parse(glyph), StrokeThickness = 1, Stretch = Stretch.None };
                var button = new Button { Content = path };
                path.Bind(Shape.StrokeProperty, button.GetObservable(ForegroundProperty));
                button.Classes.Add("Caption");
                if (close) button.Classes.Add("Close");
                button.SetTip(tip);
                button.Click += (_, _) => click();
                return button;
            }
            _winMinimize = Caption(L.T("Minimize"), "M 0,0.5 L 10,0.5", () => WindowState = WindowState.Minimized);
            _winMaximize = Caption(L.T("Maximize"), "M 0.5,0.5 L 9.5,0.5 L 9.5,9.5 L 0.5,9.5 Z", ToggleMaximize);
            _winClose = Caption(L.T("Close"), "M 0,0 L 10,10 M 10,0 L 0,10", Close, close: true);
            grid.Children.Add(_winButtons);

            // Settings search (Ctrl+F): on the left, after the window buttons when they are there
            // (ApplyWindowButtons places it); the results open under it.
            _searchBox.Classes.Add("TitleSearch");
            _searchBox.Watermark = L.T("Search settings");
            _searchBox.InnerLeftContent = new SymbolIcon(SymbolRegular.Search24) { FontSize = 12, Margin = new Thickness(9, 0, 6, 0), Opacity = 0.75 };
            _searchBox.TextChanged += (_, _) => SearchBox_TextChanged();
            _searchBox.AddHandler(KeyDownEvent, SearchBox_KeyDown, RoutingStrategies.Tunnel);
            _searchBox.GotFocus += (_, _) =>
            {
                _searchPagesReady ??= MakeSearchPagesAsync();
                ShowHits();
            };
            _searchBox.LostFocus += (_, _) => Dispatcher.UIThread.Post(() =>
            {
                // A click on a result takes the focus off the box first: the result handles it.
                if (!_searchBox.IsFocused && !_pickingHit) _searchResults.IsVisible = false;
            });
            _searchClear.Content = new Avalonia.Controls.Shapes.Path
            {
                Data = Geometry.Parse("M 0,0 L 7,7 M 7,0 L 0,7"),
                StrokeThickness = 1.2,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
            ((Shape)_searchClear.Content).SetResourceReference(Shape.StrokeProperty, "TextFillColorTertiaryBrush");
            _searchClear.Classes.Add("TitleIcon");
            _searchClear.MinWidth = 18;
            _searchClear.Padding = new Thickness(0);
            _searchClear.HorizontalAlignment = HorizontalAlignment.Right;
            _searchClear.VerticalAlignment = VerticalAlignment.Center;
            _searchClear.Margin = new Thickness(0, 0, 4, 0);
            _searchClear.SetTip("Clear (Esc)");
            _searchClear.Click += (_, _) =>
            {
                _searchDelay?.Stop();
                _searchBox.Text = "";
                _searchBox.Focus();
            };
            var shortcut = new TextBlock { Text = "Ctrl+F", FontSize = 10.5, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 0, 9, 0), IsHitTestVisible = false };
            shortcut.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
            void ShowShortcut() => shortcut.IsVisible = (_searchBox.Text ?? "").Length == 0 && !_searchBox.IsFocused;
            _searchBox.GotFocus += (_, _) => ShowShortcut();
            _searchBox.LostFocus += (_, _) => ShowShortcut();
            _searchBox.TextChanged += (_, _) => ShowShortcut();
            _searchArea.Children.Add(_searchBox);
            _searchArea.Children.Add(shortcut);
            _searchArea.Children.Add(_searchClear);
            grid.Children.Add(_searchArea);

            var title = new TextBlock { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, FontSize = 12, IsHitTestVisible = false };
            var name = new Run("Lantern Engine") { FontWeight = FontWeight.SemiBold };
            name.Bind(TextElement.ForegroundProperty, this.GetResourceObservable("TextFillColorPrimaryBrush"));
            _titlePage.Bind(TextElement.ForegroundProperty, this.GetResourceObservable("TextFillColorTertiaryBrush"));
            title.Inlines = new InlineCollection { name, _titlePage };
            grid.Children.Add(title);

            // Update, Play and the activity log on the right.
            var update = new StackPanel { Orientation = Orientation.Horizontal };
            var updateIcon = new SymbolIcon(SymbolRegular.ArrowDownload24) { FontSize = 13, Margin = new Thickness(0, 0, 5, 0) };
            updateIcon.SetResourceReference(TextBlock.ForegroundProperty, "GoodTextBrush");
            _titleUpdateText.Text = L.T("Update");
            _titleUpdateText.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
            update.Children.Add(updateIcon);
            update.Children.Add(_titleUpdateText);
            _titleUpdate.Content = update;
            _titleUpdate.Classes.Add("TitleIcon");
            _titleUpdate.SetTip(L.T("A new launcher version is available"));
            _titleUpdate.Click += async (_, _) => await InstallUpdateAsync();
            _titleTools.Children.Add(_titleUpdate);

            var play = new StackPanel { Orientation = Orientation.Horizontal };
            _titlePlayText.Text = L.T("Play");
            _titlePlayText.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
            play.Children.Add(_titlePlayIcon);
            play.Children.Add(_titlePlayText);
            _titlePlay.Content = play;
            _titlePlay.Classes.Add("TitleIcon");
            _titlePlay.Classes.Add("Filled");
            _titlePlay.SetTip(L.T("Start the game"));
            _titlePlay.Click += async (_, _) => await PlayOrStopAsync();
            _titleTools.Children.Add(_titlePlay);
            _titleTools.Children.Add(TitleButton(new SymbolIcon(SymbolRegular.Pulse24) { FontSize = 14 }, L.T("Activity"), ToggleLog));
            grid.Children.Add(_titleTools);

            bar.Child = grid;
            return bar;
        }

        /// <summary>Modern: the tabs across the top.</summary>
        private Control Tabs()
        {
            var strip = new StackPanel { Orientation = Orientation.Horizontal };
            RadioButton Tab(string label, Type page)
            {
                var tab = new RadioButton { Content = label, Tag = page, GroupName = "Pages" };
                tab.Classes.Add("Tab");
                tab.IsCheckedChanged += (_, _) => { if (tab.IsChecked == true) Navigate(tab); };
                strip.Children.Add(tab);
                _tabs.Add(tab);
                return tab;
            }
            _playItem = Tab(L.T("Play"), typeof(PlayPage));
            _savesItem = Tab(L.T("Saves"), typeof(SavesPage));
            Tab(L.T("Display"), typeof(DisplayPage));
            Tab(L.T("Graphics"), typeof(GraphicsPage));
            _controlsItem = Tab(L.T("Controls"), typeof(ControlsPage));
            Tab(L.T("Looks"), typeof(LooksPage));
            Tab(L.T("Trophies"), typeof(TrophiesPage));
            _onlineItem = Tab(L.T("Online"), typeof(OnlinePage));
            _settingsItem = Tab(L.T("Settings"), typeof(SettingsPage));
            _moreItem = Tab(L.T("More"), typeof(MorePage));
            return strip;
        }

        /// <summary>Classic, and Modern with Settings › Navigation: Sidebar: the source-list sidebar,
        /// with small-caps group labels.</summary>
        private Control Sidebar()
        {
            var dock = new DockPanel { Margin = new Thickness(0, 16, 0, 10) };
            var list = new StackPanel();
            RadioButton Item(SymbolRegular icon, string label, Type page, Panel? into = null)
            {
                var symbol = new SymbolIcon(icon) { FontSize = 13, Width = 16, Margin = new Thickness(0, 0, 8, 0) };
                var row = new StackPanel { Orientation = Orientation.Horizontal };
                row.Children.Add(symbol);
                row.Children.Add(new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center });
                var item = new RadioButton { Content = row, Tag = page, GroupName = "Classic" };
                item.Classes.Add("SidebarItem");
                item.IsCheckedChanged += (_, _) => { if (item.IsChecked == true) Navigate(item); };
                _classic.Add(item);
                if (into != null) into.Children.Add(item);
                else list.Children.Add(item);
                return item;
            }
            TextBlock Header(string text, Thickness? margin = null)
            {
                var header = new TextBlock { Text = text };
                header.Classes.Add("SidebarHeader");
                if (margin is { } m) header.Margin = m;
                list.Children.Add(header);
                return header;
            }
            // Settings at the bottom too, as in the WPF launcher.
            var bottom = new StackPanel();
            DockPanel.SetDock(bottom, Dock.Bottom);
            _classicSettingsBottom = Item(SymbolRegular.Settings24, L.T("Settings"), typeof(SettingsPage), bottom);
            dock.Children.Add(bottom);

            // The hunt, how it looks, the workshop's mods and cheats, the Lantern Engine itself.
            Header(L.T("HUNT"));
            _classicPlay = Item(SymbolRegular.Play24, L.T("Play"), typeof(PlayPage));
            _classicSaves = Item(SymbolRegular.Save24, L.T("Saves"), typeof(SavesPage));
            _classicControls = Item(SymbolRegular.XboxController24, L.T("Controls"), typeof(ControlsPage));
            Item(SymbolRegular.Sparkle24, L.T("Looks"), typeof(LooksPage));
            Item(SymbolRegular.Trophy24, L.T("Trophies"), typeof(TrophiesPage));
            _classicOnline = Item(SymbolRegular.Globe24, L.T("Online"), typeof(OnlinePage));
            Header(L.T("SIGHT"), new Thickness(18, 18, 0, 6));
            Item(SymbolRegular.Desktop24, L.T("Display"), typeof(DisplayPage));
            Item(SymbolRegular.Sparkle24, L.T("Graphics"), typeof(GraphicsPage));
            Header(L.T("WORKSHOP"), new Thickness(18, 18, 0, 6));
            _classicMods = Item(SymbolRegular.PuzzlePiece24, L.T("Mods & patches"), typeof(ModsPage));
            // Cheats beside Mods & patches, once the engine applies them (as More's Cheats tab).
            if (CheatsPage.Supported)
                _classicCheats = Item(SymbolRegular.Beaker24, L.T("Cheats"), typeof(CheatsPage));
            Item(SymbolRegular.Save24, L.T("Save editor"), typeof(SaveEditorPage));
            Header(L.T("LANTERN"), new Thickness(18, 18, 0, 6));
            _classicSettings = Item(SymbolRegular.Settings24, L.T("Settings"), typeof(SettingsPage));
            _classicEngine = Item(SymbolRegular.Wrench24, L.T("Engine switches"), typeof(EnginePage));
            _classicOutput = Item(SymbolRegular.WindowConsole20, L.T("Game output"), typeof(OutputPage));
            dock.Children.Add(list);
            return dock;
        }

        private static Control LogLine(LogItem item)
        {
            var grid = new Grid { Margin = new Thickness(0, 1), ColumnDefinitions = new ColumnDefinitions("Auto,*") };
            var time = new TextBlock { Text = item.Time, FontSize = 11, Margin = new Thickness(0, 0, 10, 0) };
            time.SetResourceReference(TextBlock.FontFamilyProperty, "MonoFont");
            time.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
            var message = new SelectableTextBlock { Text = item.Message, FontSize = 11.5, Foreground = item.Color, TextWrapping = TextWrapping.Wrap };
            message.SetResourceReference(TextBlock.FontFamilyProperty, "MonoFont");
            Grid.SetColumn(message, 1);
            grid.Children.Add(time);
            grid.Children.Add(message);
            return grid;
        }

        // ---------------------------------------------------------------- --play

        private bool _directPlay, _directKeepOpen;

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

        /// <summary>Graphics › Engine preset, asked once: New or Legacy (0.9.33). Closed without a
        /// choice, it asks again at the next start.</summary>
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

        /// <summary>Shows what would change to suit this PC, and applies it on Apply.</summary>
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
            var current = _pageHost.Content as LauncherPage;
            _pages.Clear();
            _searchPagesReady = null; // made again for the search when its box is next focused
            if (current == null) return;
            var page = _pages[current.GetType()] = Pages.Create(current.GetType());
            _pageHost.Content = page;
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

        // ---------------------------------------------------------------- appearance

        /// <summary>
        /// The Modern layout's background (Settings › Appearance, live): the game's own
        /// sce_sys/pic1.png (or pic0), the player's own picture, or none; blurred strongly, lightly or
        /// not at all, under a dark wash tinted by the theme. The launcher carries no picture of its own.
        /// </summary>
        public void ApplyBackdrop()
        {
            if (SettingRow.Classic)
                return;
            var s = Workspace.Current.Settings;
            var shade = LauncherTheme.ShadeColour(s);
            _backdropShade.Fill = new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
                EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
                GradientStops =
                {
                    new GradientStop(LauncherTheme.WithAlpha(shade, 0x59), 0),
                    new GradientStop(LauncherTheme.WithAlpha(shade, 0x80), 0.5),
                    new GradientStop(LauncherTheme.WithAlpha(shade, 0xB3), 1),
                },
            };
            var mode = s.Str("ui_background");
            var mica = mode == "mica" && OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000);
            ApplyMica(mica, shade);
            if (mode == "none" || mode == "mica")
            {
                _backdropLoad++; // a picture still loading is not shown
                _backdropImage.IsVisible = false;
                _backdropShade.IsVisible = false;
                return;
            }
            var blur = s.Str("ui_background_blur");
            var radius = blur == "off" ? 0 : blur == "light" ? 14 : 48;
            _backdropImage.Effect = radius == 0 ? null : new BlurEffect { Radius = radius };
            _backdropImage.Margin = new Thickness(radius == 0 ? 0 : -60);
            RenderOptions.SetBitmapInterpolationMode(_backdropImage, radius == 0 ? BitmapInterpolationMode.HighQuality : BitmapInterpolationMode.LowQuality);
            _backdropShade.IsVisible = true;
            _ = LoadBackdropAsync(mode, s.Str("ui_background_image"), GameEnvironment.GameDir(s),
                radius == 0 ? 2560 : radius < 20 ? 1600 : 960); // blurred: less is enough
        }

        private int _backdropLoad;

        private void ApplyMica(bool on, Color shade)
        {
            if (on)
            {
                TransparencyLevelHint = new[] { WindowTransparencyLevel.Mica };
                Background = Brushes.Transparent;
                _backdrop.Background = new SolidColorBrush(LauncherTheme.WithAlpha(shade, 0x66));
                return;
            }
            if (TransparencyLevelHint.Count > 0)
                TransparencyLevelHint = Array.Empty<WindowTransparencyLevel>();
            this.SetResourceReference(BackgroundProperty, "WindowBaseBrush");
            _backdrop.SetResourceReference(Panel.BackgroundProperty, "WindowBaseBrush");
        }

        public void CycleTab(int step)
        {
            var items = (_sidebarNav ? _classic : _tabs).Where(i => i.IsEffectivelyVisible).ToList();
            if (items.Count == 0) return;
            var at = items.FindIndex(i => i.IsChecked == true);
            var next = items[((at < 0 ? 0 : at + step) % items.Count + items.Count) % items.Count];
            next.IsChecked = true;
            next.Focus(NavigationMethod.Directional);
        }

        public void PlayFromPad()
        {
            if (!Workspace.Current.Game.Running) _ = PlayOrStopAsync();
        }

        private PixelRect _normalBounds;

        private void RememberNormalBounds()
        {
            if (WindowState != WindowState.Normal || !IsVisible) return;
            var scale = DesktopScaling;
            _normalBounds = new PixelRect(Position, PixelSize.FromSize(ClientSize, scale));
        }

        private void RestoreWindowPlacement(LauncherSettings settings)
        {
            try
            {
                var primary = Screens.Primary ?? Screens.All.FirstOrDefault();
                if (primary == null) return;
                var scale = primary.Scaling;
                var work = primary.WorkingArea.ToRect(scale);
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
                    var x = (int)Math.Round(settings.Int("window_x") * scale);
                    var y = (int)Math.Round(settings.Int("window_y") * scale);
                    var title = new PixelPoint(x + (int)(120 * scale), y + (int)(20 * scale));
                    if (Screens.All.Any(s => s.Bounds.Contains(title)))
                    {
                        WindowStartupLocation = WindowStartupLocation.Manual;
                        Position = new PixelPoint(x, y);
                    }
                }
                if (settings.Bool("window_maximized"))
                    WindowState = WindowState.Maximized;
            }
            catch (Exception)
            {
            }
        }

        private void SaveWindowPlacement()
        {
            try
            {
                if (_directPlay || WindowState == WindowState.Minimized || !IsVisible) return;
                RememberNormalBounds();
                var bounds = _normalBounds;
                if (bounds.Width < 1 || bounds.Height < 1) return;
                var scale = DesktopScaling;
                var settings = Workspace.Current.Settings;
                settings.Set("window_maximized", WindowState == WindowState.Maximized);
                settings.Set("window_width", (int)Math.Round(bounds.Width / scale));
                settings.Set("window_height", (int)Math.Round(bounds.Height / scale));
                settings.Set("window_x", (int)Math.Round(bounds.X / scale));
                settings.Set("window_y", (int)Math.Round(bounds.Y / scale));
            }
            catch (Exception)
            {
            }
        }

        private void ShowThanks()
        {
            var ws = Workspace.Current;
            ws.Settings.Set("thanks_maximilian_dood", true);
            ws.SaveSoon();
            Prompt.Tell(L.T("Thank you, Maximilian Dood!"), L.T("Thank you to Maximilian Dood for covering Lantern Engine.") + "\n\n" + L.T("Maximilian Dood, I'd love to reach out. My Discord is @awakenkn."));
        }

        private Button? VisibleMaximize()
        {
            if (_winButtons.IsVisible && _winMaximize.IsVisible) return _winMaximize;
            if (_macButtons.IsVisible && _macMaximize.IsVisible) return _macMaximize;
            return null;
        }

        private bool OverMaximize(IntPtr lParam)
        {
            if (VisibleMaximize() is not { } button) return false;
            var x = unchecked((short)(long)lParam);
            var y = unchecked((short)((long)lParam >> 16));
            var p = button.PointToClient(new PixelPoint(x, y));
            return p.X >= 0 && p.Y >= 0 && p.X < button.Bounds.Width && p.Y < button.Bounds.Height;
        }

        private void SetMaximizeHover(bool on)
        {
            if (on) _winMaximize.SetResourceReference(BackgroundProperty, "SidebarItemHoverBrush");
            else _winMaximize.ClearValue(BackgroundProperty);
        }

        private IntPtr SnapLayoutHook(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            const uint NcHitTest = 0x0084, NcMouseMove = 0x00A0, NcLButtonDown = 0x00A1, NcLButtonUp = 0x00A2, NcMouseLeave = 0x02A2;
            const int HtMaxButton = 9;
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
                    ToggleMaximize();
                    break;
            }
            return IntPtr.Zero;
        }

        /// <summary>Finds and decodes the background picture on a worker thread (the game folder may be
        /// on a slow or sleeping disk, and a big PNG takes a while to decode), then shows it.</summary>
        private async Task LoadBackdropAsync(string mode, string own, string game, int width)
        {
            var load = ++_backdropLoad;
            Bitmap? image;
            try
            {
                image = await Task.Run(() =>
                {
                    if (AppIcon.Backdrop(mode) is { } builtIn)
                    {
                        using var carried = typeof(AppIcon).Assembly.GetManifestResourceStream(builtIn);
                        if (carried != null) return Bitmap.DecodeToWidth(carried, width);
                    }
                    string? art = mode == "custom" && File.Exists(own)
                        ? own
                        : new[] { "pic1.png", "pic0.png" }
                            .Select(n => Path.Combine(game, "sce_sys", n))
                            .FirstOrDefault(File.Exists);
                    // No picture of our own: without the game's art the background stays plain.
                    if (art == null) return null;
                    using var file = File.OpenRead(art);
                    return Bitmap.DecodeToWidth(file, width);
                });
            }
            catch
            {
                image = null;
            }
            if (load != _backdropLoad) return; // a newer choice is loading
            // A picture that won't load: the glass sits on the plain dark background.
            _backdropImage.Source = image;
            _backdropImage.IsVisible = image != null;
        }

        /// <summary>The theme, again (Settings › Appearance › Theme, live).</summary>
        public void ApplyTheme()
        {
            LauncherTheme.Apply(Workspace.Current.Settings, glass: !SettingRow.Classic);
            ApplyBackdrop();
            UpdatePlayButton();
        }

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

        /// <summary>
        /// The window buttons (Settings › Appearance, live): macOS traffic lights or Windows caption
        /// buttons, on their usual side (macOS left, Windows right) or the other. Close stays on the
        /// outer edge; the title bar's own buttons move out of their way.
        /// </summary>
        public void ApplyWindowButtons()
        {
            var s = Workspace.Current.Settings;
            var windows = s.Str("ui_window_buttons") == "windows";
            var side = s.Str("ui_window_buttons_side");
            var right = side == "right" || (side != "left" && windows);
            _macButtons.IsVisible = !windows;
            _winButtons.IsVisible = windows;

            static void Order(Panel panel, params Control[] order)
            {
                panel.Children.Clear();
                foreach (var child in order) panel.Children.Add(child);
            }
            if (right)
            {
                Order(_macButtons, _macMinimize, _macMaximize, _macClose);
                Order(_winButtons, _winMinimize, _winMaximize, _winClose);
            }
            else
            {
                Order(_macButtons, _macClose, _macMinimize, _macMaximize);
                Order(_winButtons, _winClose, _winMaximize, _winMinimize);
            }
            foreach (var b in _macButtons.Children)
                b.Margin = right ? new Thickness(8, 0, 0, 0) : new Thickness(0, 0, 8, 0);
            _macButtons.HorizontalAlignment = right ? HorizontalAlignment.Right : HorizontalAlignment.Left;
            _macButtons.Margin = right ? new Thickness(0, 0, 14, 0) : new Thickness(14, 0, 0, 0);
            _winButtons.HorizontalAlignment = right ? HorizontalAlignment.Right : HorizontalAlignment.Left;
            // The traffic lights take 3 x 20 px plus their margin; the caption buttons 3 x 46.
            var taken = !right ? 0 : windows ? 3 * 46 : 14 + 3 * 20;
            _titleTools.Margin = new Thickness(0, 0, 10 + taken, 0);
            // The settings search on the left, after the buttons when they are on that side; a little
            // narrower beside the wider caption buttons, so it stays clear of the centred title.
            var left = right ? 12 : windows ? 3 * 46 + 10 : 14 + 3 * 20 + 4;
            _searchArea.Margin = new Thickness(left, 0, 0, 0);
            _searchArea.Width = !right && windows ? 168 : 200;
            _searchResults.Margin = new Thickness(left, 2, 0, 0);
        }

        private void ToggleMaximize() =>
            WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

        private DateTime? _gameStartedUtc;

        /// <summary>Discord status: shown while the game runs (Core/DiscordPresence).</summary>
        public void UpdateDiscord()
        {
            var running = Workspace.Current.Game.Running;
            if (running && _gameStartedUtc == null) _gameStartedUtc = DateTime.UtcNow;
            if (!running) _gameStartedUtc = null;
            DiscordPresence.Update(Workspace.Current.Settings, running, _gameStartedUtc);
        }

        private void OnClosing(object? sender, WindowClosingEventArgs e)
        {
            SaveWindowPlacement();
            Workspace.Current.SaveNow();
            if (_closing || !Workspace.Current.Game.Running)
            {
                DiscordPresence.Stop();
                return;
            }
            e.Cancel = true;
            // After this event: the question needs the window on screen.
            Dispatcher.UIThread.Post(async () =>
            {
                if (!IsVisible) ShowLauncher();
                var answer = Prompt.Choose("Lantern Engine", L.T("The game is still running. Stop it and close the launcher?"),
                    L.T("Cancel"), L.T("Keep it running"), L.T("Stop and close"));
                if (answer <= 0) return;
                _closing = true;
                if (answer == 2) await Workspace.Current.StopGameAsync();
                Close();
            });
        }

        // ---------------------------------------------------------------- navigation

        private void Navigate(RadioButton item)
        {
            if (item.Tag is not Type pageType) return;
            var page = PageOf(pageType);
            if (!ReferenceEquals(_pageHost.Content, page)) _pageHost.Content = page;
            page.OnShown();
            var name = ItemName(item);
            _titlePage.Text = string.IsNullOrEmpty(name) ? "" : $" — {name}";
        }

        /// <summary>Tabs or the sidebar (Settings › Navigation, switched live; Classic always has the
        /// sidebar), staying on the page shown when it switches.</summary>
        public void ApplyNavigation()
        {
            var sidebar = SettingRow.Classic || Workspace.Current.Settings.Str("ui_navigation") == "sidebar";
            var switching = _pageHost.Content != null && sidebar != _sidebarNav;
            _sidebarNav = sidebar;
            _sidebar.IsVisible = sidebar;
            _tabStrip.IsVisible = !sidebar;
            if (!SettingRow.Classic)
            {
                // A pane over the backdrop, as the tab strip is.
                _sidebar.Margin = new Thickness(12, 12, 0, 12);
                _sidebar.CornerRadius = new CornerRadius(14);
                _sidebar.BorderThickness = new Thickness(1);
                _sidebar.SetResourceReference(Border.BackgroundProperty, "PillTrackBrush");
                _sidebar.SetResourceReference(Border.BorderBrushProperty, "GlassPaneStrokeBrush");
                // Settings once, in the LANTERN group (Classic has it at the bottom too).
                _classicSettingsBottom.IsVisible = false;
                foreach (var item in _classic)
                    item.Classes.Add("Glass");
            }
            if (!switching)
                return;
            var shown = _pageHost.Content?.GetType();
            if (shown == typeof(MorePage) && _pages.TryGetValue(typeof(MorePage), out var more))
                shown = ((MorePage)more).ShownPage ?? typeof(PlayPage);
            // More holds Mods, Cheats, Engine and Log pages of its own: the ones of the navigation
            // left are dropped, so the other makes its own with the settings as they are now.
            foreach (var type in new[] { typeof(MorePage), typeof(ModsPage), typeof(CheatsPage), typeof(EnginePage), typeof(OutputPage) })
                _pages.Remove(type);
            _searchPagesReady = null; // made again for the search when its box is next focused
            var target = (sidebar ? _classic : _tabs).LastOrDefault(i => Equals(i.Tag, shown));
            if (target != null)
            {
                // Still checked from before the last switch: shown again all the same.
                if (target.IsChecked == true)
                    Navigate(target);
                target.IsChecked = true;
                return;
            }
            // A page of More's (Mods, Cheats, Engine, Log) back on the tabs: More, at that section.
            var key = shown == typeof(ModsPage) ? "mods" : shown == typeof(CheatsPage) ? "cheats"
                : shown == typeof(EnginePage) ? "engine" : shown == typeof(OutputPage) ? "output" : null;
            if (key != null) Show(key);
            else (sidebar ? _classicPlay : _playItem).IsChecked = true;
        }

        /// <summary>A page, made the first time it is asked for and kept.</summary>
        private LauncherPage PageOf(Type type)
        {
            if (!_pages.TryGetValue(type, out var page))
                _pages[type] = page = Pages.Create(type);
            return page;
        }

        /// <summary>A tab's or sidebar entry's label: its text, or the text beside its icon.</summary>
        private static string? ItemName(RadioButton item) =>
            item.Content as string ??
            (item.Content as Panel)?.Children.OfType<TextBlock>().FirstOrDefault(t => t is not SymbolIcon)?.Text;

        /// <summary>Shows a page: a tab ("online", "advanced": Settings), or one of More's ("output", "mods", "engine").</summary>
        public void Show(string page)
        {
            var item = _sidebarNav
                ? page switch
                {
                    "saves" => _classicSaves,
                    "output" => _classicOutput,
                    "online" => _classicOnline,
                    "advanced" => _classicSettings,
                    "mods" => _classicMods,
                    "cheats" when _classicCheats != null => _classicCheats,
                    "engine" => _classicEngine,
                    "controls" => _classicControls,
                    _ => _classicPlay,
                }
                : page switch
                {
                    "saves" => _savesItem,
                    "online" => _onlineItem,
                    "advanced" => _settingsItem,
                    "output" or "mods" or "engine" => _moreItem,
                    "cheats" when CheatsPage.Supported => _moreItem,
                    "controls" => _controlsItem,
                    _ => _playItem,
                };
            if (item.IsChecked == true)
                Navigate(item);
            item.IsChecked = true;
            if (item == _moreItem && _pages.TryGetValue(typeof(MorePage), out var more))
                ((MorePage)more).ShowSection(page);
        }

        // ---------------------------------------------------------------- settings search

        private List<SettingSearch.Hit> _hits = new();
        private int _hit;
        private bool _pickingHit;

        /// <summary>
        /// The pages this layout's tabs or sidebar lead to, in their order, each made if not shown yet
        /// (once; they are kept as when visited). More stands for its sections, each a place of its own.
        /// </summary>
        private IEnumerable<SettingSearch.Place> SearchPlaces()
        {
            // Classic lists Settings twice (top and bottom of the sidebar): the later entry is kept.
            var items = new List<RadioButton>();
            foreach (var item in _sidebarNav ? _classic : _tabs)
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
                if (!_pages.TryGetValue((Type)item.Tag!, out var made)) continue;
                if (made is MorePage more)
                {
                    foreach (var (key, label, page) in more.MadeSectionPages())
                        yield return new SettingSearch.Place($"{name} › {label}", page, () => { Open(); more.ShowSection(key); });
                }
                else
                    yield return new SettingSearch.Place(name, made, Open);
            }
        }

        private void Window_KeyDown(object? sender, KeyEventArgs e)
        {
            // Bubbling, so a key the page took first (the Controls page listening for a binding) stays its own.
            if (e.Key == Key.F && e.KeyModifiers == KeyModifiers.Control && DialogLayer.Children.Count == 0)
            {
                _searchBox.Focus();
                _searchBox.SelectAll();
                e.Handled = true;
            }
        }

        private bool _modal;
        private IPointer? _lastPointer;
        private readonly List<Control> _blocked = new();

        private void SetModal(Panel root, bool modal)
        {
            if (modal == _modal) return;
            _modal = modal;
            if (modal) _lastPointer?.Capture(null);
            if (modal)
            {
                _blocked.Clear();
                foreach (var child in root.Children)
                    if (!ReferenceEquals(child, DialogLayer) && child.IsHitTestVisible)
                    {
                        child.IsHitTestVisible = false;
                        _blocked.Add(child);
                    }
            }
            else
            {
                foreach (var child in _blocked) child.IsHitTestVisible = true;
                _blocked.Clear();
            }
            if (modal) _searchResults.IsVisible = false;
        }

        private void Window_PointerPressed(object? sender, PointerPressedEventArgs e)
        {
            _lastPointer = e.Pointer;
            // A click anywhere else closes the results (the text stays; typing opens them again).
            if (_searchResults.IsVisible && e.Source is Visual source &&
                !_searchResults.IsVisualAncestorOf(source) && !_searchArea.IsVisualAncestorOf(source))
                _searchResults.IsVisible = false;
        }

        /// <summary>Searches 150 ms after the last key, not at every key.</summary>
        private DispatcherTimer? _searchDelay;

        /// <summary>The pages the search looks through, made one at a time at background priority so
        /// typing never waits for them (null until the search box is first focused).</summary>
        private Task? _searchPagesReady;

        private void SearchBox_TextChanged()
        {
            var text = _searchBox.Text ?? "";
            _searchClear.IsVisible = text.Length > 0;
            if (text.Trim().Length == 0)
            {
                _searchDelay?.Stop();
                ShowHits();
                return;
            }
            if (_searchDelay == null)
            {
                _searchDelay = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
                _searchDelay.Tick += (_, _) =>
                {
                    _searchDelay.Stop();
                    ShowHits();
                };
            }
            _searchDelay.Stop();
            _searchDelay.Start();
        }

        /// <summary>Makes every page the search looks through, yielding to input and rendering between
        /// pages: the first search then finds them ready instead of building them all in one go.</summary>
        private async Task MakeSearchPagesAsync()
        {
            var items = (_sidebarNav ? _classic : _tabs).Select(i => i.Tag).OfType<Type>().Distinct().ToList();
            foreach (var type in items)
            {
                await Dispatcher.UIThread.InvokeAsync(() => { }, DispatcherPriority.Background);
                if (PageOf(type) is MorePage more)
                    foreach (var key in more.SectionKeys)
                    {
                        await Dispatcher.UIThread.InvokeAsync(() => { }, DispatcherPriority.Background);
                        more.MakeSection(key);
                    }
            }
            // What was typed while the pages were being made, searched again over all of them.
            if (_searchBox.IsFocused && (_searchBox.Text ?? "").Trim().Length > 0 && _searchDelay?.IsEnabled != true)
                ShowHits();
        }

        private void SearchBox_KeyDown(object? sender, KeyEventArgs e)
        {
            switch (e.Key)
            {
                case Key.Escape:
                    // Esc clears; on an empty box it hands the keyboard back to the page.
                    if ((_searchBox.Text ?? "").Length > 0) _searchBox.Text = "";
                    else (_pageHost.Content as Control)?.Focus();
                    e.Handled = true;
                    break;
                case Key.Enter:
                    // Typed faster than the search delay: search now, so Enter picks for what is typed.
                    if (_searchDelay?.IsEnabled == true)
                    {
                        _searchDelay.Stop();
                        ShowHits();
                    }
                    if (_hits.Count > 0 && _searchResults.IsVisible) PickHit(_hit);
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
            var query = _searchBox.Text ?? "";
            if (query.Trim().Length == 0 || !_searchBox.IsFocused)
            {
                _hits.Clear();
                _searchResults.IsVisible = false;
                return;
            }
            _hits = SettingSearch.Find(SearchPlaces(), query);
            _hit = 0;
            _searchList.Children.Clear();
            if (_hits.Count == 0)
                _searchList.Children.Add(SettingSearch.Nothing(query));
            for (var i = 0; i < _hits.Count; i++)
            {
                var index = i;
                var view = SettingSearch.View(_hits[i]);
                view.PointerEntered += (_, _) =>
                {
                    _hit = index;
                    MarkHit();
                };
                view.PointerPressed += (_, _) => _pickingHit = true;
                view.PointerReleased += (_, e) =>
                {
                    _pickingHit = false;
                    if (e.InitialPressMouseButton == MouseButton.Left) PickHit(index);
                };
                _searchList.Children.Add(view);
            }
            MarkHit();
            _searchResults.IsVisible = true;
        }

        /// <summary>Shades the result Enter picks (the first, until the arrow keys or the mouse move it).</summary>
        private void MarkHit()
        {
            for (var i = 0; i < _searchList.Children.Count; i++)
                if (_searchList.Children[i] is Border { Tag: SettingSearch.Hit } view)
                {
                    if (i == _hit) view.SetResourceReference(Border.BackgroundProperty, "SidebarItemSelectedBrush");
                    else
                    {
                        view.ClearValue(Border.BackgroundProperty);
                        view.Background = Brushes.Transparent;
                    }
                }
        }

        /// <summary>Opens the result's page and, once it is laid out, scrolls to the setting and flashes it.</summary>
        private void PickHit(int index)
        {
            if (index < 0 || index >= _hits.Count) return;
            var hit = _hits[index];
            _searchBox.Text = "";
            _searchResults.IsVisible = false;
            hit.Place.Show();
            // A page shown for the first time is put in, laid out and grouped into its boxes first.
            Dispatcher.UIThread.Post(() =>
            {
                if (!hit.Place.Page.Reveal(hit.Row))
                    hit.Place.Page.Focus();
            }, DispatcherPriority.Background);
        }

        // ---------------------------------------------------------------- updates

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
                while (DialogLayer.Children.Count > 0)
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
            if (_updatingEngine) return;
            _updatingEngine = true;
            try
            {
                var ws = Workspace.Current;
                if (!Updater.Supported)
                {
                    Prompt.Tell(L.T("Update engine"), L.F($"Updating from the launcher is for the Windows package. See {Updater.ReleasesPage}"));
                    return;
                }
                Updater.Release? release;
                try
                {
                    release = ws.Latest ?? await Updater.LatestAsync(Updater.BetaChannel(ws.Settings));
                }
                catch (Exception e) when (e is HttpRequestException or TaskCanceledException)
                {
                    Prompt.Tell(L.T("Update engine"),
                        L.F($"Couldn't reach GitHub to look up the latest version. Check your connection and try again, or see {Updater.ReleasesPage}"));
                    return;
                }
                if (release?.ZipUrl == null)
                {
                    Prompt.Tell(L.T("Update engine"), L.F($"No package is published yet. See {Updater.ReleasesPage}"));
                    return;
                }
                await InstallAsync(release, package: true);
            }
            finally
            {
                _updatingEngine = false;
            }
        }

        private bool _updatingEngine;

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
            if (!Updater.Supported)
            {
                Prompt.Tell(L.T("Update"), L.F($"Updating from the launcher is for the Windows package. See {Updater.ReleasesPage}"));
                return;
            }
            if (package && ws.Game.Running)
            {
                Prompt.Tell(L.T("Close the game first"), L.T("This update replaces the engine, so the game has to be closed."));
                return;
            }
            var notes = release.Notes.Trim();
            if (notes.Length > 6000) notes = notes[..6000] + "…";
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

            _titleUpdate.IsVisible = true;
            _titleUpdate.IsEnabled = false;
            var progress = new Progress<double>(p => _titleUpdateText.Text = L.F($"Downloading {p:P0}"));
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
                LanternApp.Quit();
            }
            catch (Exception ex)
            {
                _titleUpdate.IsEnabled = true;
                _titleUpdateText.Text = L.T("Update");
                ws.Log(L.T("Update failed: ") + ex.GetBaseException().Message, LogLevel.Bad);
                Prompt.Tell(L.T("Update failed"), ex.GetBaseException().Message + L.F($"\n\nYou can download it by hand from {Updater.ReleasesPage}"));
            }
        }

        // ---------------------------------------------------------------- play

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
            ini.Set("online_pool", s.HuntersDreamToken.Length > 0 && s.Str("hd_name").Length > 0 ? "huntersdream" : "");
            if (!HuntersDream.OnlinePaused && ini.Get("online_enabled") == "1" && ini.Get("online_pool") != "huntersdream")
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
                    var folder = Ui.PickFolder(L.T("Port folder (with Bloodborne.exe and run.py)"));
                    if (folder == null) return;
                    if (!Paths.IsPortDir(folder))
                    {
                        Prompt.Tell(L.T("Not the port folder"), L.T("That folder has no run.py with Bloodborne.exe or bin\\bb-probe.exe."));
                        return;
                    }
                    s.Set("port_dir", folder);
                    ws.ResolvePort();
                    ws.SaveNow();
                }
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

            // No game folder with eboot.bin: the engine would only log "No eboot.bin". The setup
            // guide says what is missing instead.
            if (!GameRunner.Custom(s) && !System.IO.File.Exists(System.IO.Path.Combine(GameEnvironment.GameDir(s), "eboot.bin")))
            {
                if (Prompt.Choose(L.T("The game is not set up yet"),
                        L.T("The launcher has no game folder with eboot.bin in it, so the game cannot start. The setup guide shows where your game files go and checks them."),
                        L.T("Cancel"), L.T("Open the setup guide")) == 1)
                    SetupGuide.Open();
                return;
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
                        using var wait = new CancellationTokenSource(5000);
                        await leftover.WaitForExitAsync(wait.Token); // off the UI thread
                    }
                    catch { /* already gone */ }
                    leftover.Dispose();
                }
                ws.Log(L.T("Closed the game left running in the background."), LogLevel.Step);
            }

            if (!s.Bool("memory_check_off") && MemoryCheck.Problem(ws.Ini) is { } memory)
            {
                var choice = Prompt.Choose(OperatingSystem.IsWindows() ? L.T("Windows may run out of memory") : L.T("The system may run out of memory"),
                    memory, L.T("Cancel"), L.T("Don't ask again"), L.T("Start anyway"));
                if (choice <= 0) return;
                if (choice == 1) s.Set("memory_check_off", true);
            }

            // bbport: the current profile has no saves, but saves were found elsewhere (a folder used
            // before, or SaveGuard's safety copies): asked before the game starts a new one.
            var savesRoot = GameEnvironment.SavesRoot(s);
            var savesProfile = PlayerProfiles.Current(s);
            var packageSaves = System.IO.Path.Combine(Paths.DataDir, "user");
            var lost = await System.Threading.Tasks.Task.Run(() => SaveGuard.Elsewhere(savesRoot, savesProfile, new[] { packageSaves }));
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
                Prompt.Tell(L.T("Could not start the game"), error);
                return;
            }
            if (s.Bool("switch_to_output"))
                Show("output");
        }

        private void UpdatePlayButton()
        {
            var running = Workspace.Current.Game.Running;
            _titlePlayText.Text = running ? L.T("Stop") : L.T("Play");
            _titlePlayIcon.Symbol = running ? SymbolRegular.Stop24 : SymbolRegular.Play24;
            _titlePlayIcon.SetResourceReference(TextBlock.ForegroundProperty, running ? "DangerTextBrush" : "AccentTextFillColorPrimaryBrush");
            _titlePlay.SetTip(running ? L.T("Stop the game") : L.T("Start the game"));
            foreach (var page in _pages.Values)
                (page as PlayPage)?.UpdateRunning();
        }

        // ---------------------------------------------------------------- log

        /// <summary>The activity log keeps the newest entries only (the game's output has its own page).</summary>
        private const int MaxLogItems = 500;

        private void OnLogged(string message, LogLevel level)
        {
            _log.Add(new LogItem { Time = DateTime.Now.ToString("HH:mm:ss"), Message = message, Color = BrushFor(level) });
            if (_log.Count > MaxLogItems) _log.RemoveAt(0);
            _logCount.Text = L.F($"{_log.Count} events");
            // Folded: nothing to scroll (ToggleLog scrolls to the end when it opens).
            if (_logExpanded) Dispatcher.UIThread.Post(_logScroller.ScrollToEnd, DispatcherPriority.Background);

            _statusText.Text = message;
            _statusText.SetTip(message);
            if (level == LogLevel.Info) _statusDot.SetResourceReference(Shape.FillProperty, "StatusDotIdleBrush");
            else
            {
                _statusDot.ClearValue(Shape.FillProperty);
                _statusDot.Fill = BrushFor(level);
            }
        }

        private static IBrush BrushFor(LogLevel level) => LauncherTheme.Brush(level switch
        {
            LogLevel.Good => "GoodTextBrush",
            LogLevel.Bad => "DangerTextBrush",
            LogLevel.Warn => "WarningTextBrush",
            LogLevel.Step => "AccentTextFillColorPrimaryBrush",
            _ => "TextFillColorSecondaryBrush",
        });

        private void ClearLog()
        {
            _log.Clear();
            _logCount.Text = "";
            _statusText.Text = L.T("Ready");
            _statusText.SetTip(null);
            _statusDot.SetResourceReference(Shape.FillProperty, "StatusDotIdleBrush");
        }

        private void ToggleLog()
        {
            _logExpanded = !_logExpanded;
            _logPanel.IsVisible = _logExpanded;
            if (_logExpanded)
                Dispatcher.UIThread.Post(_logScroller.ScrollToEnd, DispatcherPriority.Background);
        }
    }

    public class LogItem
    {
        public string Time { get; set; } = "";
        public string Message { get; set; } = "";
        public IBrush Color { get; set; } = Brushes.White;
    }
}
