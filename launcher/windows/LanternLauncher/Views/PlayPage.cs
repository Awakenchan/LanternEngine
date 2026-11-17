using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Shapes;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// Home, as a title screen over its own night (<see cref="TitleBackdrop"/>): the game's name,
    /// a newer launcher when there is one, Play, what will start (the game's check and the picture
    /// settings), and the ways on. The game's own settings are on the Game page.
    /// </summary>
    public sealed class PlayPage : LauncherPage
    {
        private readonly TitleBackdrop _backdrop = new();
        private readonly InfoBar _problem;
        private readonly Border _engineFrame, _playGlow;
        private readonly AppButton _play;
        private readonly Button _update;
        private readonly TextBlock _updateText, _playStatus, _gameText, _stats;
        private readonly Ellipse _gameDot;
        private readonly DispatcherTimer _clock = new() { Interval = TimeSpan.FromSeconds(1) };

        /// <summary>The game folder's last check (null until the first one is done).</summary>
        private GameStatus? _status;
        private int _refreshes;

        public PlayPage() : base(L.T("Play"), L.T("Check the game folder and start Bloodborne."))
        {
            var column = new StackPanel
            {
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                MaxWidth = 600,
                // A little more room under it than over it: the column sits just above the middle.
                Margin = new Thickness(0, 24, 0, 64),
            };

            // ==================== The title ====================
            column.Children.Add(Text("Bloodborne", "HeroTitle"));
            column.Children.Add(Ornament());
            column.Children.Add(new TextBlock { Text = L.F($"Lantern Engine · {Updater.CurrentLabel}"), Classes = { "HeroSub" }, Margin = new Thickness(0, 10, 0, 0) });

            // A newer launcher, when there is one: the title bar's Update (download, swap in, restart),
            // said where the version is.
            _updateText = AccentText();
            var updateIcon = new SymbolIcon(SymbolRegular.ArrowDownload24) { FontSize = 13, Margin = new Thickness(0, 0, 7, 0), VerticalAlignment = VerticalAlignment.Center };
            updateIcon.SetResourceReference(TextBlock.ForegroundProperty, "AccentTextFillColorPrimaryBrush");
            _update = HeroLink(Centred(updateIcon, _updateText, 20), L.T("Downloads the new launcher, swaps it in and restarts. Your settings, saves and mods stay"),
                async () => { if (MainWindow.Instance is { } window) await window.InstallUpdateAsync(); });
            _update.Margin = new Thickness(0, 6, 0, 0);
            _update.IsVisible = false;
            column.Children.Add(_update);

            // ==================== Play ====================
            _play = new AppButton
            {
                Appearance = ControlAppearance.Primary,
                Text = L.T("Play"),
                Icon = Icon(SymbolRegular.Play24),
                Padding = new Thickness(44, 11, 44, 11),
                MinWidth = 260,
                FontSize = 21,
            };
            _play.SetResourceReference(TemplatedControl.FontFamilyProperty, "DisplayFont");
            _play.Click += async (_, _) => await MainWindow.Instance!.PlayOrStopAsync();
            // Its glow is on a frame around it: the button's own template clips a shadow.
            _playGlow = new Border { Child = _play, CornerRadius = new CornerRadius(3), HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 36, 0, 0) };
            column.Children.Add(_playGlow);
            _playStatus = CentredText("Muted", new Thickness(0, 12, 0, 0));
            column.Children.Add(_playStatus);

            // What will start: the game's check (a dot and words, never the colour alone), opening the
            // Game page, then the picture.
            _gameDot = new Ellipse { Width = 7, Height = 7, Margin = new Thickness(0, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center };
            _gameDot.SetResourceReference(Shape.FillProperty, "StatusDotIdleBrush");
            _gameText = new TextBlock { Text = L.T("Checking the game…"), FontSize = 13, VerticalAlignment = VerticalAlignment.Center };
            var gameLine = HeroLink(Centred(_gameDot, _gameText, 15), L.T("The game folder, its DLC and the Lantern extras"),
                () => MainWindow.Instance?.Show("game"));
            gameLine.Margin = new Thickness(0, 22, 0, 0);
            column.Children.Add(gameLine);
            _stats = CentredText("Faint", new Thickness(0));
            column.Children.Add(_stats);

            _problem = new InfoBar
            {
                IsClosable = false,
                IsOpen = false,
                Margin = new Thickness(0, 20, 0, 0),
                Content = SmallButton(L.T("Open the setup guide"), SymbolRegular.Lightbulb24, (_, _) => SetupGuide.Open()),
            };
            column.Children.Add(_problem);

            // A package from before this launcher's engine (it updated itself, the engine did not).
            var engineCard = Card(SymbolRegular.ArrowDownload24, L.T("Engine update available"),
                L.T("This package has an older engine than this launcher: the crash fixes since then are missing. ") +
                L.T("Updating downloads the latest package and keeps your settings, saves and mods."),
                SmallButton(L.T("Update engine"), SymbolRegular.ArrowDownload24, async (_, _) => await MainWindow.Instance!.UpdateEngineAsync()), out _);
            engineCard.BorderThickness = new Thickness(0);
            _engineFrame = new Border
            {
                Child = engineCard,
                CornerRadius = new CornerRadius(4),
                BorderThickness = new Thickness(1),
                Margin = new Thickness(0, 20, 0, 0),
                IsVisible = false,
            };
            _engineFrame.SetResourceReference(Border.BackgroundProperty, "GlassPaneBrush");
            _engineFrame.SetResourceReference(Border.BorderBrushProperty, "WarningTextBrush");
            column.Children.Add(_engineFrame);

            // ==================== The ways on ====================
            var links = new StackPanel { Margin = new Thickness(0, 26, 0, 0), HorizontalAlignment = HorizontalAlignment.Center };
            foreach (var (text, page) in new[]
                     {
                         (L.T("Game folder and options"), "game"), (L.T("Saves and profiles"), "saves"),
                         (L.T("Mods & patches"), "mods"), (L.T("Settings"), "advanced"),
                     })
                links.Children.Add(HeroLink(text, null, () => MainWindow.Instance?.Show(page)));
            column.Children.Add(links);

            // The community: help, news and people to play with. Hidden for good with the X
            // (Settings keeps the link). The link on the centre line, its X beside it.
            if (!S.Bool("discord_hidden"))
            {
                var join = HeroLink(L.T("Join the Discord"), L.T("Help, news, co-op partners and beta feedback"), () => Ui.OpenLink(LauncherSettings.Discord));
                join.FontSize = 13;
                var discord = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,*"), Margin = new Thickness(0, 14, 0, 0) };
                var hide = IconButton(SymbolRegular.Dismiss24, L.T("Hide"), (_, _) =>
                {
                    S.Set("discord_hidden", true);
                    Ws.SaveSoon();
                    column.Children.Remove(discord);
                });
                hide.HorizontalAlignment = HorizontalAlignment.Left;
                Grid.SetColumn(join, 1);
                Grid.SetColumn(hide, 2);
                discord.Children.Add(join);
                discord.Children.Add(hide);
                column.Children.Add(discord);
            }

            // A plain box (not a row), so the page's grouping leaves the title screen as it is; as tall
            // as the view, so the column sits in the middle of the window (and scrolls in a short one).
            var screen = new Border { Child = column };
            Body.Children.Add(screen);
            Scroller.SizeChanged += (_, e) => screen.MinHeight = Math.Max(0, e.NewSize.Height - Body.Margin.Top - Body.Margin.Bottom);

            // The night behind the page.
            var scroller = Scroller;
            Content = null;
            Content = new Panel { Children = { _backdrop, scroller } };

            _clock.Tick += (_, _) => UpdateRunning();
            Ws.PortChanged += Refresh;
            Ws.UpdateStateChanged += ShowUpdate;
            Ws.Saved += RefreshSummary;
            Ws.TrophiesChanged += RefreshSummary;
        }

        public override void OnShown()
        {
            _backdrop.ApplyTheme();
            // Play's lamplight in the Yharnam colours only: a blood glow under another theme's accent would clash.
            _playGlow.Classes.Set("PlayGlow", LauncherTheme.IsYharnam(S));
            Refresh();
        }

        /// <summary>"Open in Explorer" on Windows; the file manager elsewhere.</summary>
        internal static string OpenTip => OperatingSystem.IsWindows() ? L.T("Open in Explorer") : L.T("Open in the file manager");

        // ---------------------------------------------------------------- state

        private void Refresh()
        {
            ShowUpdate();
            _engineFrame.IsVisible = Updater.EngineOutdated;
            RefreshSummary();
            UpdateRunning();
            _ = CheckGameAsync(GameEnvironment.GameDir(S), S.Str("dlc_dir"));
        }

        /// <summary>The update line, while a newer launcher is waiting.</summary>
        private void ShowUpdate()
        {
            var release = Ws.Update;
            _update.IsVisible = release != null;
            if (release != null)
                _updateText.Text = L.F($"{Updater.Label(release.Version, release.Beta)} is available · Update");
        }

        private async Task CheckGameAsync(string gameDir, string dlcChosen)
        {
            var refresh = ++_refreshes;
            var (status, dlc) = await GamePage.InspectAsync(gameDir, dlcChosen);
            if (refresh != _refreshes) return; // a newer check is on its way
            _status = status;

            _problem.IsOpen = status.Health != Health.Good;
            _problem.Severity = status.Health == Health.Bad ? InfoBarSeverity.Error : InfoBarSeverity.Warning;
            _problem.Title = status.Health == Health.Bad ? L.T("The game folder needs attention") : L.T("Check the game");
            _problem.Message = status.Summary;

            _gameDot.SetResourceReference(Shape.FillProperty, GamePage.HealthBrush(status.Health));
            var version = $"{status.TitleId} {status.Version}".Trim();
            var dlcOn = S.Bool("dlc_enabled") && (dlc.Length > 0 || status.TitleId == "CUSA03173");
            _gameText.Text = status.Health switch
            {
                Health.Bad => L.T("Game not found: choose its folder"),
                Health.Warning => L.F($"Check the game · {version}"),
                _ => dlcOn ? L.F($"Game ready · {version} · The Old Hunters on") : L.F($"Game ready · {version}"),
            };
            UpdateRunning();
        }

        private void RefreshSummary()
        {
            var upscaler = Label(Choices.Upscalers, Ini.Get("upscaler"));
            var (outW, outH) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            var fps = Label(Choices.FpsModes, GameEnvironment.Fps(S));
            _stats.Text = L.F($"{upscaler} · {outW} × {outH} · {fps} · Trophies {Ws.Trophies.Count} / {TrophyTracker.All.Length}");
        }

        private static string Label(IReadOnlyList<Choice> choices, string value) =>
            choices.FirstOrDefault(c => c.Value == value)?.Label.Split(" (")[0] ?? value;

        public void UpdateRunning()
        {
            var game = Ws.Game;
            _play.Text = game.Running ? L.T("Stop") : L.T("Play");
            _play.Icon = Icon(game.Running ? SymbolRegular.Stop24 : SymbolRegular.Play24);
            _play.Appearance = game.Running ? ControlAppearance.Danger : ControlAppearance.Primary;

            if (game.Running)
            {
                var up = DateTime.Now - game.StartedAt;
                _playStatus.Text = L.F($"Running for {(int)up.TotalMinutes}:{up.Seconds:00}  ·  output on the Game output page");
                _clock.Start();
                return;
            }
            _clock.Stop();
            // The last check's answer (Refresh checks the folder again off the UI thread).
            _playStatus.Text = _status?.Health == Health.Bad
                ? L.T("Choose the game folder first")
                : Ws.Session is { Length.TotalSeconds: > 10 } last
                    ? L.T("Last session: ") + last.Summary
                    : L.T("Settings are saved as you change them");
        }

        // ---------------------------------------------------------------- building blocks

        /// <summary>A quiet text button of the title screen.</summary>
        private static Button HeroLink(object content, string? tip, Action click)
        {
            var link = new Button { Content = content };
            link.Classes.Add("HeroLink");
            if (tip != null) link.SetTip(tip);
            link.Click += (_, _) => click();
            return link;
        }

        /// <summary>A mark and its words, with a spacer as wide as the mark on the other side, so the
        /// words sit on the column's centre line.</summary>
        private static StackPanel Centred(Control mark, Control words, double markWidth) =>
            Row(mark, words, new Border { Width = markWidth });

        private static TextBlock AccentText()
        {
            var text = new TextBlock { FontSize = 13, VerticalAlignment = VerticalAlignment.Center };
            text.SetResourceReference(TextBlock.ForegroundProperty, "AccentTextFillColorPrimaryBrush");
            return text;
        }

        private static TextBlock CentredText(string style, Thickness margin)
        {
            var text = Text("", style);
            text.HorizontalAlignment = HorizontalAlignment.Center;
            text.TextAlignment = TextAlignment.Center;
            text.Margin = margin;
            return text;
        }

        /// <summary>A thin rule with a diamond in the middle, under the title.</summary>
        private static Control Ornament()
        {
            var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,*"), Width = 260, Margin = new Thickness(0, 14, 0, 0), HorizontalAlignment = HorizontalAlignment.Center };
            Border Rule(int column)
            {
                var rule = new Border { Height = 1, Opacity = 0.55, VerticalAlignment = VerticalAlignment.Center };
                rule.SetResourceReference(Border.BackgroundProperty, "AccentTextFillColorPrimaryBrush");
                Grid.SetColumn(rule, column);
                return rule;
            }
            var diamond = new Border
            {
                Width = 7,
                Height = 7,
                BorderThickness = new Thickness(1),
                Margin = new Thickness(10, 0),
                RenderTransform = new RotateTransform(45),
                VerticalAlignment = VerticalAlignment.Center,
            };
            diamond.SetResourceReference(Border.BorderBrushProperty, "AccentTextFillColorPrimaryBrush");
            Grid.SetColumn(diamond, 1);
            grid.Children.Add(Rule(0));
            grid.Children.Add(diamond);
            grid.Children.Add(Rule(2));
            return grid;
        }
    }
}
