using System.Diagnostics;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using BloodborneLauncher.Build;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The launcher itself: where the port is, how the game is started, and rebuilding.</summary>
    public sealed class SettingsPage : LauncherPage
    {
        private readonly TextBlock _portHint, _buildHint;
        private readonly StackPanel _customRows = new();
        private readonly TextBlock _updateHint;
        private readonly AppButton _updateNow;
        private readonly ComboBox _versions = new() { Width = 300, VerticalAlignment = VerticalAlignment.Center };
        private readonly AppButton _installVersion;
        private readonly TextBlock _versionHint;
        private IReadOnlyList<Updater.Release>? _releases;
        private bool _versionsLoading;
        private readonly StackPanel _themeColour = new(), _picture = new(), _iconPicture = new();
        private TextBlock _pictureHint = null!, _iconPictureHint = null!;

        public SettingsPage() : base(L.T("Settings"), L.T("The Lantern Engine, how the game is started, and keeping this launcher up to date."))
        {
            Group(L.T("Appearance"));
            ComboRow(SymbolRegular.LocalLanguage24, L.T("Launcher language"), L.T("Restart the launcher to apply the language."),
                L.Languages.Select(l => new Choice(l.Name, l.Code)).ToArray(),
                () => S.Str("ui_language"), v => S.Set("ui_language", v), changed: () =>
                {
                    Ws.SaveNow();
                    if (Prompt.Confirm(L.T("Change language"), L.T("Restart the launcher to apply the language."), L.T("Restart now"))) Restart();
                });
            Control layout = null!;
            var reverting = false;
            layout = ComboRow(SymbolRegular.PanelLeft24, L.T("Layout"), L.T("Modern: tabs and compact rows. Classic: the sidebar and one card per setting"),
                new[] { new Choice(L.T("Modern"), "modern"), new Choice(L.T("Classic"), "classic") },
                () => S.Str("ui_layout") == "classic" ? "classic" : "modern", v => S.Set("ui_layout", v), changed: () =>
                {
                    if (reverting || RestartForLayout()) return;
                    // Cancelled: put back the layout that is showing, so the next start doesn't switch either.
                    reverting = true;
                    var shown = SettingRow.Classic ? "classic" : "modern";
                    S.Set("ui_layout", shown);
                    Ws.SaveNow();
                    var index = shown == "classic" ? 1 : 0;
                    if (layout is Segmented segmented) segmented.Select(index, notify: false);
                    else if (layout is ComboBox box) box.SelectedIndex = index;
                    reverting = false;
                });

            // The Modern layout's pages: tabs across the top, or a sidebar in groups (live).
            if (!SettingRow.Classic)
                ComboRow(SymbolRegular.TextBulletListSquare24, L.T("Navigation"), L.T("Tabs across the top, or a sidebar with the pages in groups"),
                    new[] { new Choice(L.T("Tabs"), "tabs"), new Choice(L.T("Sidebar"), "sidebar") },
                    () => S.Str("ui_navigation") == "sidebar" ? "sidebar" : "tabs", v => S.Set("ui_navigation", v),
                    changed: () => MainWindow.Instance?.ApplyNavigation());

            SettingCombo(SymbolRegular.PaintBrush24, L.T("Theme"), L.T("The accent colour, and a faint tint of it in the launcher's surfaces"),
                LauncherTheme.Themes, "ui_theme", changed: () =>
                {
                    ShowAppearance();
                    MainWindow.Instance?.ApplyTheme();
                });
            var swatch = new Border
            {
                Width = 22, Height = 22, CornerRadius = new CornerRadius(11), BorderThickness = new Thickness(1),
                Margin = new Thickness(0, 0, 10, 0), VerticalAlignment = VerticalAlignment.Center,
            };
            swatch.SetResourceReference(Border.BorderBrushProperty, "CardStrokeColorDefaultBrush");
            var colour = new TextBox
            {
                Text = S.Str("ui_theme_colour"), Watermark = LauncherTheme.DefaultCustomColour,
                Width = 120, VerticalAlignment = VerticalAlignment.Center,
            };
            void ShowSwatch() => swatch.Background = LauncherTheme.ParseColour(colour.Text ?? "") is { } c
                ? new SolidColorBrush(c)
                : Brushes.Transparent;
            ShowSwatch();
            colour.TextChanged += (_, _) =>
            {
                ShowSwatch();
                if (LauncherTheme.ParseColour(colour.Text ?? "") == null) return;
                S.Set("ui_theme_colour", (colour.Text ?? "").Trim());
                Ws.SaveSoon();
                MainWindow.Instance?.ApplyTheme();
            };
            var presets = Row(swatch, colour);
            foreach (var preset in new[] { "#C41E3A", "#E0643A", "#D99A2B", "#3FA86B", "#2BB3B3", "#4C9DFF", "#8FB8E8", "#8E63D6", "#D65DA8", "#B8B8BE" })
            {
                var dot = new Border
                {
                    Width = 16, Height = 16, CornerRadius = new CornerRadius(8), Margin = new Thickness(6, 0, 0, 0),
                    Background = new SolidColorBrush(LauncherTheme.ParseColour(preset)!.Value),
                    Cursor = new Cursor(StandardCursorType.Hand), VerticalAlignment = VerticalAlignment.Center,
                };
                dot.SetTip(preset);
                dot.PointerReleased += (_, _) => colour.Text = preset;
                presets.Children.Add(dot);
            }
            Add(Card(SymbolRegular.ColorBackground24, L.T("Theme colour"),
                L.T("Pick a dot or type a colour as #RRGGBB"), presets, out _), _themeColour);
            Body.Children.Add(_themeColour);

            SettingCombo(SymbolRegular.ImageEdit24, L.T("Background"),
                SettingRow.Classic ? L.T("Shown in the Modern layout (Classic's panes are solid)") : L.T("The picture behind the glass panes"),
                new[] { new Choice(L.T("Purple lantern"), "lantern"), new Choice(L.T("Orange lantern"), "lantern-orange"), new Choice(L.T("Game artwork"), "game"), new Choice(L.T("Your picture"), "custom") }
                    .Concat(OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000) ? new[] { new Choice(L.T("Windows Mica"), "mica") } : Array.Empty<Choice>())
                    .Append(new Choice(L.T("None"), "none")).ToArray(),
                "ui_background", changed: () =>
                {
                    ShowAppearance();
                    MainWindow.Instance?.ApplyBackdrop();
                });
            Add(Card(SymbolRegular.ImageEdit24, L.T("Picture"), L.T("A PNG, JPG or BMP; it fills the window"),
                SmallButton(L.T("Choose"), SymbolRegular.FolderOpen24, (_, _) => ChoosePicture()), out _pictureHint), _picture);
            Body.Children.Add(_picture);
            SettingCombo(SymbolRegular.Blur24, L.T("Background blur"), null,
                new[] { new Choice(L.T("Strong"), "strong"), new Choice(L.T("Light"), "light"), new Choice(L.T("Off"), "off") },
                "ui_background_blur", changed: () => MainWindow.Instance?.ApplyBackdrop());
            SettingCombo(SymbolRegular.Image24, L.T("App icon"), L.T("The launcher's and the game window's icon. The game window takes it at its next start"),
                AppIcon.Choices, "app_icon", changed: () =>
                {
                    ShowAppearance();
                    MainWindow.Instance?.ApplyIcon();
                });
            Add(Card(SymbolRegular.ImageEdit24, L.T("Icon picture"), L.T("A PNG, JPG, BMP or ICO; square pictures look best"),
                SmallButton(L.T("Choose"), SymbolRegular.FolderOpen24, (_, _) => ChooseIcon()), out _iconPictureHint), _iconPicture);
            Body.Children.Add(_iconPicture);
            SettingCombo(SymbolRegular.Window24, L.T("Window buttons"), L.T("macOS: the red, yellow and green dots. Windows: – □ ✕"),
                new[] { new Choice("macOS", "mac"), new Choice("Windows", "windows") },
                "ui_window_buttons", changed: () => MainWindow.Instance?.ApplyWindowButtons());
            SettingCombo(SymbolRegular.ArrowBidirectionalLeftRight24, L.T("Window buttons side"), null,
                new[] { new Choice(L.T("Usual"), "usual", L.T("macOS on the left, Windows on the right")), new Choice(L.T("Left"), "left"), new Choice(L.T("Right"), "right") },
                "ui_window_buttons_side", changed: () => MainWindow.Instance?.ApplyWindowButtons());
            ShowAppearance();

            Group("Lantern Engine");
            Add(Card(SymbolRegular.FolderLink24, L.T("Engine folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) => ChoosePort()),
                IconButton(SymbolRegular.Open24, PlayPage.OpenTip, (_, _) => Paths.OpenFolder(Paths.PortDir)),
                IconButton(SymbolRegular.ArrowUndo24, L.T("Detect automatically"), (_, _) =>
                {
                    S.Set("port_dir", "");
                    Ws.ResolvePort();
                    Ws.SaveSoon();
                    Refresh();
                })), out _portHint));

            Group(L.T("Launch"));
            SettingCombo(SymbolRegular.XboxController24, L.T("Controller navigation"),
                L.T("Move through the launcher with a controller: D-pad or left stick to move, A to choose, B to go back, LB and RB for the tabs, Start to play"),
                PadNavigation.Modes, "pad_navigation", changed: () => { PadNavigation.Apply(); Ws.SaveSoon(); });
            SettingCombo(SymbolRegular.Rocket24, L.T("Start the game with"), null, Choices.LaunchMethods, "launch_method", changed: () => { ShowMethod(); Ws.SaveSoon(); });

            TextRow(SymbolRegular.Code24, L.T("Command"), L.T("A program to run instead of Bloodborne.exe --run, with the settings as environment variables"),
                OperatingSystem.IsWindows() ? @"C:\path\to\game.exe" : "/path/to/game", () => S.Str("custom_command"), v => S.Set("custom_command", v), _customRows, 380);
            TextRow(SymbolRegular.TextDescription24, L.T("Arguments"), null, L.T("Optional"),
                () => S.Str("custom_args"), v => S.Set("custom_args", v), _customRows, 380);
            Body.Children.Add(_customRows);

            Group(L.T("Performance"));
            SettingCombo(SymbolRegular.Flow20, L.T("Two-stage GPU pipeline"), L.T("20–30% faster; turn off if the game is unstable"),
                Choices.DrawPipe, "draw_pipe");
            SettingCombo(SymbolRegular.Speaker224, L.T("Sound output"),
                L.T("If the sound jitters or crackles, try DirectSound. The larger buffer is steadier but the sound comes a little later"),
                Choices.SoundOutput, "sound_output");
            SettingToggle(SymbolRegular.Speaker224, L.T("Mute in the background"),
                L.T("No sound while the game window is not in front or is minimized"), "mute_unfocused");
            var perfMore = Advanced();
            SettingCombo(SymbolRegular.ArrowSwap24, L.T("CPU reads of GPU data"), L.T("Relaxed is the default"), Choices.Readbacks, "readbacks", perfMore);
            SettingToggle(SymbolRegular.Speaker224, L.T("Sound threads first"),
                L.T("For CPUs with many slower cores (older Xeons and similar): the game's sound runs before its other threads, against late or stuttering sound. Off by default"),
                "audio_priority", perfMore);

            Group(L.T("Diagnostics"));
            Add(Card(SymbolRegular.Wrench24, L.T("Engine switches"),
                L.T("The engine's other player-facing BB_ variables (GPU choice, texture filtering, memory, sound, fixes) as switches, sliders and lists at the engine's defaults: no typing needed"),
                SmallButton(L.T("Open"), SymbolRegular.Open24, (_, _) => MainWindow.Instance?.Show("engine")), out _));
            var logs = Advanced(L.T("Log, capture and extra variables"));
            SettingToggle(SymbolRegular.TextBulletListSquare24, L.T("Frame statistics in the log"), "BB_FRAME_STATS", "frame_stats", logs);
            SettingToggle(SymbolRegular.DeveloperBoard24, L.T("FSR 4 profile in the log"), "BB_FSR4_PROFILE", "fsr_profile", logs);
            SettingToggle(SymbolRegular.Speaker224, L.T("Audio statistics in the log"), "BB_AUDIO_STATS", "audio_stats", logs);
            SettingToggle(SymbolRegular.HourglassHalf24, L.T("Loading profile in the log"),
                L.T("BB_SAMPLE_THREADS: where the game's main, render and file threads spend their time, every 5 seconds. For slow loading; costs a little speed"), "load_profile", logs);
            SettingToggle(SymbolRegular.Timer24, L.T("Render stage timings in the log"), L.T("BB_SECTIONS: where each frame's CPU time goes (with frame statistics). It costs FPS, so it applies to the next game only"), "section_stats", logs);
            SettingToggle(SymbolRegular.Camera24, L.T("Capture shaders"),
                "BB_SHADER_CAPTURE: every shader the game uses is saved once to logs\\shaders (a few MB), for testing the new native shader compiler offline. Costs a little time the first time each shader is seen", "shader_capture", logs);
            TextRow(SymbolRegular.Code24, L.T("Extra variables"),
                L.T("NAME=value pairs separated by spaces, passed to the game. For anything Engine switches doesn't list; a variable set here wins over its switch there"),
                "BB_EXAMPLE=1", () => S.Str("extra_env"), v => S.Set("extra_env", v.Trim()), logs, width: 300);

            Group(L.T("Files"));
            Add(Card(SymbolRegular.DocumentSettings20, L.T("Launcher settings"), Paths.ConfigFile,
                IconButton(SymbolRegular.Open24, PlayPage.OpenTip, (_, _) => Paths.OpenFolder(Paths.ConfigDir)), out _));
            Add(Card(SymbolRegular.DocumentSettings20, L.T("Port settings (lantern.ini)"), Paths.IniPath,
                IconButton(SymbolRegular.Open24, PlayPage.OpenTip, (_, _) => Paths.OpenFolder(Path.GetDirectoryName(Paths.IniPath)!)), out _));
            Add(Card(SymbolRegular.Broom24, L.T("Shader cache"),
                L.T("The game's compiled shaders, kept between starts (a new engine version clears them itself). Clear them if ") +
                L.T("something draws wrong in every save, such as missing skin or hair; the next start compiles them again and stutters for a while"),
                SmallButton(L.T("Clear"), SymbolRegular.Broom24, (_, _) => ClearShaderCache()), out _));
            Add(Card(SymbolRegular.Share24, L.T("Shader catalog"),
                L.T("Save your shaders as a catalog file, or add someone else's: areas they have played don't pop in or stutter for you. ") +
                L.T("It works with the same kind of graphics card; a catalog adds to your shaders, it never replaces them"),
                Row(SmallButton(L.T("Export catalog"), SymbolRegular.Save24, (_, _) => ExportShaderCache()),
                    SmallButton(L.T("Add catalog"), SymbolRegular.FolderOpen24, (_, _) => ImportShaderCache())), out _));

            Group(L.T("Beta tested settings"));
            Add(Card(SymbolRegular.ArrowReset24, L.T("Tested settings"),
                L.T("Use the settings this beta was tested with, or go back to your own. Yours are saved before anything changes"), Row(
                SmallButton(L.T("Use tested"), SymbolRegular.CheckmarkCircle24, (_, _) => UseTestedSettings()),
                SmallButton(L.T("Back to mine"), SymbolRegular.ArrowUndo24, (_, _) => RestoreMySettings())), out _));

            Group(L.T("Share settings"));
            Add(Card(SymbolRegular.Share24, L.T("Share your settings"),
                L.T("Graphics, effects, camera, controls, cheats and engine options, as a code to paste in Discord or a file. ") +
                L.T("Not included: display and HDR, the online account, folders and languages"),
                Row(SmallButton(L.T("Copy code"), SymbolRegular.Copy24, (_, _) => CopySettingsCode()),
                    SmallButton(L.T("Save as file"), SymbolRegular.Save24, (_, _) => SaveSettingsFile())), out _));
            Add(Card(SymbolRegular.ArrowImport24, L.T("Import settings"), L.T("Someone's code or file. You see what changes before anything is applied"),
                Row(SmallButton(L.T("Paste code"), SymbolRegular.ClipboardPaste24, (_, _) => ImportSettingsCode()),
                    SmallButton(L.T("Open file"), SymbolRegular.FolderOpen24, (_, _) => ImportSettingsFile())), out _));

            Group(L.T("Community"));
            // Discord status: a preview of the card, and for Custom every part Discord lets a game set.
            var discordPreview = new DiscordPreview();
            var discordCustom = new StackPanel();
            var discordGame = new StackPanel();
            var discordLink = new StackPanel();
            var discordOwn = new StackPanel();
            void DiscordChanged()
            {
                var mode = S.Str("discord_status");
                discordGame.IsVisible = mode is "lantern" or "custom";
                discordCustom.IsVisible = mode == "custom";
                discordLink.IsVisible = S.Str("discord_picture") == "url";
                discordOwn.IsVisible = S.Str("discord_app") == "own";
                discordPreview.Show(DiscordPresence.LookOf(S, LiveGame.Example));
                Ws.SaveSoon();
                MainWindow.Instance?.UpdateDiscord();
            }
            TextBox DiscordText(string title, string placeholder, string key, string? hint = null) =>
                TextRow(SymbolRegular.TextDescription24, title, hint, placeholder, () => S.Str(key),
                    v => { S.Set(key, v); DiscordChanged(); }, discordCustom, 380);
            SettingCombo(SymbolRegular.PeopleCommunity24, L.T("Discord status"),
                L.T("Shows that you're playing on your Discord profile while the game runs. Custom: your own lines, picture and buttons"),
                Choices.DiscordStatus, "discord_status", changed: DiscordChanged);
            Body.Children.Add(discordPreview);
            ToggleRow(SymbolRegular.Globe24, L.T("Show where you are"), L.T("The area you're in, or the boss you're fighting"),
                () => S.Bool("discord_game_place"), v => S.Set("discord_game_place", v), discordGame, DiscordChanged);
            ToggleRow(SymbolRegular.Trophy24, L.T("Show your level, Blood Echoes and Insight"), null,
                () => S.Bool("discord_game_stats"), v => S.Set("discord_game_stats", v), discordGame, DiscordChanged);
            Body.Children.Add(discordGame);
            ComboRow(SymbolRegular.Rename24, L.T("Name"), L.T("The name in bold is the Discord app's. My own Discord app: make one on discord.com/developers with any name and paste its Application ID"),
                DiscordPresence.Names, () => S.Str("discord_app") is { Length: > 0 } v ? v : "lantern",
                v => S.Set("discord_app", v), discordCustom, changed: DiscordChanged);
            TextRow(SymbolRegular.Key24, L.T("Application ID"), L.T("From your app's General Information page"), L.T("e.g. 1234567890123456789"),
                () => S.Str("discord_app_id"), v => { S.Set("discord_app_id", v); DiscordChanged(); }, discordOwn, 380);
            TextRow(SymbolRegular.Rename24, L.T("Your app's name"), L.T("Only for the preview: Discord shows the name you gave the app"), "Bloodborne",
                () => S.Str("discord_app_name"), v => { S.Set("discord_app_name", v); DiscordChanged(); }, discordOwn, 380);
            discordCustom.Children.Add(discordOwn);
            DiscordText(L.T("First line"), "Hunting in Yharnam", "discord_details", L.T("{version} shows the launcher's version; {area}, {boss}, {level}, {echoes} and {insight} show the game's"));
            DiscordText(L.T("Second line"), L.T("Optional"), "discord_state");
            ComboRow(SymbolRegular.Image24, L.T("Picture"), null, DiscordPresence.Pictures, () => S.Str("discord_picture") is { Length: > 0 } v ? v : "logo",
                v => S.Set("discord_picture", v), discordCustom, changed: DiscordChanged);
            TextRow(SymbolRegular.Link24, L.T("Picture link"), L.T("An https link to a PNG, JPG or GIF: Discord downloads it, so a file on your PC can't be used"),
                "https://…", () => S.Str("discord_image_url"), v => { S.Set("discord_image_url", v); DiscordChanged(); }, discordLink, 380);
            discordCustom.Children.Add(discordLink);
            DiscordText(L.T("Picture hover text"), "Lantern Engine", "discord_image_text");
            DiscordText(L.T("Small badge link"), L.T("Optional https link"), "discord_badge_url", L.T("A small round picture on the corner of the big one"));
            DiscordText(L.T("Badge hover text"), L.T("Optional"), "discord_badge_text");
            ToggleRow(SymbolRegular.Timer24, L.T("Show play time"), null, () => S.Bool("discord_timer"), v => S.Set("discord_timer", v), discordCustom, DiscordChanged);
            DiscordText(L.T("Button 1 text"), L.T("Optional, e.g. Join my Discord"), "discord_button1_label", L.T("Other people see the buttons; Discord hides them on your own profile"));
            DiscordText(L.T("Button 1 link"), "https://…", "discord_button1_url");
            DiscordText(L.T("Button 2 text"), L.T("Optional"), "discord_button2_label");
            DiscordText(L.T("Button 2 link"), "https://…", "discord_button2_url");
            Body.Children.Add(discordCustom);
            DiscordChanged();
            Add(Card(SymbolRegular.PeopleCommunity24, "Discord", L.T("Help, news, co-op partners and beta feedback"),
                SmallButton(L.T("Join"), SymbolRegular.Open24, (_, _) => Ui.OpenLink(LauncherSettings.Discord)), out _));

            Group(L.T("Updates"));
            _updateNow = SmallButton(L.T("Update now"), SymbolRegular.ArrowDownload24, async (_, _) => await MainWindow.Instance!.InstallUpdateAsync(),
                ControlAppearance.Primary);
            Add(Card(SymbolRegular.ArrowDownload24, L.T("Launcher updates"), "", Row(
                SmallButton(L.T("Check now"), SymbolRegular.ArrowClockwise24, async (_, _) => await Ws.CheckForUpdatesAsync(quiet: false)),
                _updateNow,
                IconButton(SymbolRegular.Open24, L.T("Open the releases page"), (_, _) => Ui.OpenLink(Updater.ReleasesPage))), out _updateHint));
            ComboRow(SymbolRegular.Branch24, L.T("Update channel"),
                L.T("Stable: releases that have been tested. Beta: every change as soon as it is done, before it is tested"),
                new[] { new Choice(L.T("Stable"), "stable"), new Choice(L.T("Beta"), "beta") },
                () => Updater.BetaChannel(S) ? "beta" : "stable",
                v =>
                {
                    if (v == (Updater.BetaChannel(S) ? "beta" : "stable")) return;
                    S.Set("update_channel", v);
                    Ws.SaveSoon();
                    Ws.Log(v == "beta" ? L.T("Update channel: beta.") : L.T("Update channel: stable."), LogLevel.Step);
                    _ = Ws.CheckForUpdatesAsync(quiet: false);
                });
            SettingToggle(SymbolRegular.Alert24, L.T("Check for updates at start"), L.F($"Looks at github.com/{Updater.Repository}/releases"), "check_updates");
            _installVersion = SmallButton(L.T("Install"), SymbolRegular.ArrowDownload24, async (_, _) => await InstallChosenVersionAsync());
            _installVersion.IsEnabled = false;
            ComboSearch.Attach(_versions);
            _versions.SelectionChanged += (_, _) =>
                _installVersion.IsEnabled = Updater.Supported && _versions.SelectedItem is ComboBoxItem { Tag: Updater.Release r } && !r.IsCurrent;
            Add(Card(SymbolRegular.History24, L.T("Version"),
                L.T("Every published release, stable and beta, newest first. Install any of them, older ones too, to go back if a ") +
                L.T("version gives you trouble. Your settings, saves, mods and profiles stay"),
                Row(_versions, _installVersion,
                    IconButton(SymbolRegular.ArrowClockwise24, L.T("Reload the list"), async (_, _) => await LoadVersionsAsync(force: true))),
                out _versionHint));
            SetHint(_versionHint, L.F($"You have {Updater.CurrentLabel}"));
            Ws.UpdateStateChanged += UpdateUpdates;
            UpdateUpdates();

            // Only a copy built from the source checkout can rebuild itself.
            var build = Card(SymbolRegular.ArrowSync24, L.T("Build"), "", Row(
                SmallButton(L.T("Rebuild"), SymbolRegular.ArrowClockwise24, (_, _) => Rebuild(force: false)),
                SmallButton(L.T("Force rebuild"), SymbolRegular.Wrench24, (_, _) => Rebuild(force: true))), out _buildHint);
            if (Paths.SourceRoot != null)
            {
                Group(L.T("Build from source"));
                Add(build);
            }

            Ws.PortChanged += Refresh;
        }

        public override void OnShown()
        {
            Refresh();
            ShowMethod();
            _ = LoadVersionsAsync(force: false);
        }

        /// <summary>Fills the version selector from the releases on GitHub (once, or again on reload).</summary>
        private async Task LoadVersionsAsync(bool force)
        {
            if (_versionsLoading || (_releases != null && !force)) return;
            _versionsLoading = true;
            _versions.IsEnabled = false;
            _installVersion.IsEnabled = false;
            _versions.Items.Clear();
            _versions.Items.Add(new ComboBoxItem { Content = L.T("Loading…") });
            _versions.SelectedIndex = 0;
            try
            {
                _releases = await Updater.AllAsync();
                _versions.Items.Clear();
                ComboBoxItem? current = null;
                foreach (var release in _releases)
                {
                    var label = Updater.Label(release.Version, release.Beta) +
                                (release.Beta > 0 ? "" : L.T("  · stable")) +
                                (release.IsCurrent ? L.T("  (installed)") : "");
                    var item = new ComboBoxItem { Content = label, Tag = release };
                    _versions.Items.Add(item);
                    if (release.IsCurrent) current = item;
                }
                _versions.SelectedItem = current ?? (_versions.Items.Count > 0 ? _versions.Items[0] : null);
                _versions.IsEnabled = _versions.Items.Count > 0;
                SetHint(_versionHint, _releases.Count == 0
                    ? L.F($"You have {Updater.CurrentLabel}; no release is published yet")
                    : L.F($"You have {Updater.CurrentLabel}. {_releases.Count} versions to choose from") +
                      (Updater.Supported ? "" : L.T(". Installing from here is for the Windows package")));
            }
            catch (Exception e)
            {
                _releases = null;
                _versions.Items.Clear();
                SetHint(_versionHint, L.F($"You have {Updater.CurrentLabel}. The list could not be loaded: {e.GetBaseException().Message}"));
            }
            finally
            {
                _versionsLoading = false;
            }
        }

        private async Task InstallChosenVersionAsync()
        {
            if (_versions.SelectedItem is not ComboBoxItem { Tag: Updater.Release release }) return;
            await MainWindow.Instance!.InstallVersionAsync(release);
        }

        private void Refresh()
        {
            SetHint(_portHint, Paths.IsPortDir(Paths.PortDir)
                ? Paths.PortDir + (Paths.DetectPortDir() == Paths.PortDir ? L.T("  ·  the package this launcher is in") : "") + L.T("\nThe engine that runs your game: out\\bb-probe.exe, its scripts and Python. Your game files stay where they are")
                : L.T("Not found. It is normally the folder this launcher is in (with bin\\bb-probe.exe and run.py)"));
            SetHint(_buildHint, BuildStatus());
        }

        private void UpdateUpdates()
        {
            var update = Ws.Update;
            var latest = Ws.Latest;
            _updateNow.IsVisible = update != null && Updater.Supported;
            SetHint(_updateHint,
                !Updater.Supported ? L.F($"You have {Updater.CurrentLabel}. Updating from the launcher is for the Windows package; see the releases page")
                : update != null ? L.F($"{update.Name} is available (you have {Updater.CurrentLabel})") +
                                   (update.IsNewerThan(Updater.Current, Updater.CurrentBeta) ? "" : L.T(": back to stable"))
                : Ws.CheckedAt == null ? L.F($"You have {Updater.CurrentLabel}")
                : latest == null ? L.F($"You have {Updater.CurrentLabel}; no release is published yet")
                : L.F($"You have {Updater.CurrentLabel}, the latest ({latest.Tag})"));
        }

        /// <summary>Shows the rows that belong to the chosen theme and background.</summary>
        private void ShowAppearance()
        {
            _themeColour.IsVisible = S.Str("ui_theme") == "custom";
            _picture.IsVisible = S.Str("ui_background") == "custom";
            var own = S.Str("ui_background_image");
            SetHint(_pictureHint, own.Length == 0 ? L.T("None chosen yet: the game artwork shows")
                : File.Exists(own) ? own : L.F($"{own} is missing: the game artwork shows"));
            _iconPicture.IsVisible = S.Str("app_icon") == "custom";
            SetHint(_iconPictureHint, S.Str("app_icon_image") is { Length: > 0 } icon && File.Exists(AppIcon.CustomFile)
                ? icon : L.T("None chosen yet: the build's own lantern shows"));
        }

        /// <summary>Your picture for the app icon, made into an icon file at once (the picture may move later).</summary>
        private void ChooseIcon()
        {
            var file = Ui.OpenFile(L.T("Icon picture"), L.T("Pictures|*.png;*.jpg;*.jpeg;*.bmp;*.ico|All files|*.*"));
            if (file == null) return;
            if (!AppIcon.MakeCustom(file))
            {
                Prompt.Tell(L.T("Icon picture"), L.F($"{Path.GetFileName(file)} could not be read as a picture."));
                return;
            }
            S.Set("app_icon_image", file);
            Ws.SaveSoon();
            ShowAppearance();
            MainWindow.Instance?.ApplyIcon();
        }

        private void ChoosePicture()
        {
            var file = Ui.OpenFile(L.T("Background picture"), L.T("Pictures|*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff|All files|*.*"));
            if (file == null) return;
            S.Set("ui_background_image", file);
            Ws.SaveSoon();
            ShowAppearance();
            MainWindow.Instance?.ApplyBackdrop();
        }

        /// <summary>Restarts into the chosen layout; false if the player cancelled.</summary>
        private static bool RestartForLayout()
        {
            if (!Prompt.Confirm(L.T("Switch layout"), L.T("The launcher restarts to change its layout."), L.T("Restart now"))) return false;
            Restart();
            return true;
        }

        private static void Restart()
        {
            Ws.SaveNow();
            if (Environment.ProcessPath is { } self)
                Process.Start(new ProcessStartInfo(self) { UseShellExecute = OperatingSystem.IsWindows(), WorkingDirectory = Path.GetDirectoryName(self)! });
            LanternApp.Quit();
        }

        /// <summary>Deletes the engine's shader and pipeline caches (user\cache beside the engine;
        /// scripts\run_windows.py also clears them when the engine changes).</summary>
        private static async void ClearShaderCache()
        {
            if (Process.GetProcessesByName("bb-probe").Length > 0)
            {
                Prompt.Tell(L.T("Shader cache"), L.T("Close the game first: it is using the cache."));
                return;
            }
            var cache = Path.Combine(Paths.PortDir, "user", "cache");
            // Thousands of files: deleted on a worker thread, the launcher stays responsive.
            var failed = await Task.Run(() =>
            {
                var failures = 0;
                if (Directory.Exists(cache))
                {
                    foreach (var entry in new DirectoryInfo(cache).EnumerateFileSystemInfos())
                    {
                        if (entry.Name == "engine.txt") continue;
                        try
                        {
                            if (entry is DirectoryInfo directory) directory.Delete(recursive: true);
                            else entry.Delete();
                        }
                        catch (Exception) { failures++; }
                    }
                }
                return failures;
            });
            Ws.Log(failed == 0 ? L.T("Shader cache cleared: the next start compiles the shaders again.")
                               : L.F($"Shader cache: {failed} entries could not be removed ({cache})."),
                   failed == 0 ? LogLevel.Good : LogLevel.Warn);
        }

        private static async void ExportShaderCache()
        {
            var file = Ui.SaveFile(L.T("Export shader catalog"), "Lantern Engine shader catalog.zip", L.T("Shader catalog|*.zip"));
            if (file == null) return;
            try
            {
                Ws.Log(L.T("Saving the shader catalog..."), LogLevel.Info);
                var count = await Task.Run(() => ShaderCacheShare.Export(file));
                Ws.Log(L.F($"Shader catalog saved: {count} pipelines in {file}"), LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not export the shader catalog: ") + e.Message, LogLevel.Bad);
            }
        }

        private static async void ImportShaderCache()
        {
            if (Process.GetProcessesByName("bb-probe").Length > 0)
            {
                Prompt.Tell(L.T("Shader cache"), L.T("Close the game first: it is using the cache."));
                return;
            }
            var file = Ui.OpenFile(L.T("Add a shader catalog"), L.T("Shader catalog|*.zip|All files|*.*"));
            if (file == null) return;
            bool catalog;
            try { catalog = ShaderCacheShare.IsCatalog(file); }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Shader cache not used"), L.T("This file doesn't fit: ") + e.Message + ".");
                return;
            }
            if (catalog)
            {
                var path = file;
                var game = GameEnvironment.GameDir(Ws.Settings);
                Ws.Log(L.T("Adding the shader catalog: its shaders are compiled now, this can take a minute..."), LogLevel.Info);
                try
                {
                    var added = await Task.Run(() => ShaderCacheShare.Merge(path, game));
                    Ws.Log(L.F($"Shader catalog added: {added} new pipelines; the game builds them at the next start."), LogLevel.Good);
                }
                catch (Exception e)
                {
                    Prompt.Tell(L.T("Shader catalog not used"), L.T("This file doesn't fit: ") + e.Message + ".");
                }
                return;
            }
            var mine = ShaderCacheShare.Pipelines(Path.Combine(ShaderCacheShare.CacheDir, "CUSA03173"));
            if (mine > 0 && !Prompt.Confirm(L.T("Replace your shader cache?"),
                    L.F($"Yours has {mine} pipelines; it is kept as a backup (user\\cache\\CUSA03173.bak)."), L.T("Replace")))
                return;
            try
            {
                var count = ShaderCacheShare.Import(file);
                Ws.Log(L.F($"Shader cache: {count} pipelines taken; the game loads them at the next start."), LogLevel.Good);
            }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Shader cache not used"), L.T("This file doesn't fit: ") + e.Message + ".");
            }
        }

        // ---------------------------------------------------------------- shared settings

        private void CopySettingsCode()
        {
            if (!ClipboardText.Set(SharedSettings.ToCode(SharedSettings.Take(Ini, S))))
            {
                Ws.Log(L.T("Could not copy: another program is using the clipboard. Try again, or use Save settings file."), LogLevel.Warn);
                return;
            }
            Ws.Log(L.T("Settings code copied. Paste it in Discord; others import it in Settings › Share settings."), LogLevel.Good);
            Prompt.Tell(L.T("Settings code copied"), L.T("Paste it in Discord or anywhere else. Others use Import settings › Paste code."));
        }

        private void SaveSettingsFile()
        {
            var file = Ui.SaveFile(L.T("Save settings"), "Lantern Engine settings.json", L.T("Settings|*.json"));
            if (file == null) return;
            try
            {
                File.WriteAllText(file, SharedSettings.ToJson(SharedSettings.Take(Ini, S)));
                Ws.Log(L.T("Settings saved: ") + file, LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not save the settings: ") + e.Message, LogLevel.Bad);
            }
        }

        private void ImportSettingsCode()
        {
            var clipboard = Ui.ClipboardText();
            var values = Prompt.Ask(L.T("Import settings"), L.F($"Paste the code someone shared (it starts with {SharedSettings.CodePrefix})."),
                new Prompt.Field(L.T("Code"), clipboard.Contains(SharedSettings.CodePrefix, StringComparison.OrdinalIgnoreCase) ? clipboard.Trim() : "",
                    SharedSettings.CodePrefix + "…", Multiline: true));
            if (values == null || values[0].Trim().Length == 0) return;
            OfferSettings(values[0]);
        }

        private void ImportSettingsFile()
        {
            var file = Ui.OpenFile(L.T("Import settings"), L.T("Settings|*.json;*.txt|All files|*.*"));
            if (file == null) return;
            try
            {
                if (new FileInfo(file).Length > 256 * 1024)
                    throw new FormatException(L.T("That file is far too large to be shared settings."));
                OfferSettings(File.ReadAllText(file));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException)
            {
                Prompt.Tell(L.T("Could not import"), e.Message);
            }
        }

        /// <summary>Shows what imported settings change and, if accepted, applies them (the previous ones are
        /// kept in a file) and restarts the launcher so every page shows them.</summary>
        private void OfferSettings(string text)
        {
            if (Ws.Game.Running)
            {
                Prompt.Tell(L.T("Close the game first"), L.T("The game changes these settings too while it runs."));
                return;
            }
            SharedSettings.Snapshot shared;
            try
            {
                shared = SharedSettings.Parse(text);
            }
            catch (FormatException e)
            {
                Prompt.Tell(L.T("Could not import"), e.Message);
                return;
            }

            var everything = new HashSet<SharedSettings.Part> { SharedSettings.Part.Settings, SharedSettings.Part.Controls, SharedSettings.Part.Cheats };
            var changes = SharedSettings.Changes(shared, Ini, S, everything);
            if (changes.Count == 0)
            {
                Prompt.Tell(L.T("Nothing to change"), L.T("These settings are the same as yours."));
                return;
            }
            var settings = changes.Count(c => c.Part == SharedSettings.Part.Settings);
            var controls = changes.Count(c => c.Part == SharedSettings.Part.Controls);
            var cheats = changes.Count(c => c.Part == SharedSettings.Part.Cheats);
            var counts = string.Join(", ", new[]
            {
                settings > 0 ? L.F($"{settings} graphics and engine") : null,
                controls > 0 ? L.F($"{controls} controls") : null,
                cheats > 0 ? L.F($"{cheats} cheats") : null,
            }.Where(p => p != null));
            const int shown = 12;
            var list = string.Join("\n", changes.Take(shown).Select(SharedSettings.Describe)) +
                       (changes.Count > shown ? L.F($"\n…and {changes.Count - shown} more") : "");
            var message = L.F($"From {shared.From}: {changes.Count} changes ({counts}).\n\n{list}\n\n") +
                          L.T("Your current settings are saved first. The launcher restarts to show the new ones.");

            HashSet<SharedSettings.Part> parts;
            if (settings > 0 && controls + cheats > 0)
            {
                var choice = Prompt.Choose(L.T("Import settings"), message, L.T("Cancel"), L.T("Keep my controls and cheats"), L.T("Import all"));
                if (choice <= 0) return;
                parts = choice == 2 ? everything : new() { SharedSettings.Part.Settings };
            }
            else if (!Prompt.Confirm(L.T("Import settings"), message, L.T("Import")))
            {
                return;
            }
            else
            {
                parts = everything;
            }

            try
            {
                var backup = Path.Combine(Paths.ConfigDir, "settings-before-import.json");
                File.WriteAllText(backup, SharedSettings.ToJson(SharedSettings.Take(Ini, S)));
                SharedSettings.Apply(shared, Ini, S, parts);
                Ws.Log(L.F($"Imported settings from {shared.From}. Your previous ones: {backup} (Import settings › Open file)."), LogLevel.Good);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Prompt.Tell(L.T("Could not import"), e.Message);
                return;
            }
            Restart();
        }

        private void UseTestedSettings()
        {
            if (TestedSettings.Differences(Ini, S).Count == 0)
            {
                Prompt.Tell(L.T("Tested settings"), L.T("You are already using them."));
                return;
            }
            try
            {
                var file = TestedSettings.Use(Ini, S);
                TestedSettings.MarkAsked(S);
                Ws.Log(L.F($"Using the settings this beta was tested with. Yours are saved in {file}"), LogLevel.Good);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
            {
                Prompt.Tell(L.T("Your settings were not changed"), L.T("They could not be saved first: ") + e.Message);
                return;
            }
            Restart();
        }

        private void RestoreMySettings()
        {
            if (TestedSettings.SavedFile() == null)
            {
                Prompt.Tell(L.T("Tested settings"), L.T("You have no saved settings yet: they are saved when you first use the tested ones."));
                return;
            }
            try
            {
                TestedSettings.Restore(Ini, S);
                Ws.Log(L.T("Your own settings are back."), LogLevel.Good);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or System.Text.Json.JsonException)
            {
                Prompt.Tell(L.T("Tested settings"), L.T("Could not bring them back: ") + e.Message);
                return;
            }
            Restart();
        }

        private void ShowMethod() => _customRows.IsVisible = GameRunner.Custom(S);

        private void ChoosePort()
        {
            var path = PickFolder(L.T("Engine folder (with run.py and bin\\bb-probe.exe)"), Paths.PortDir);
            if (path == null) return;
            if (!Paths.IsPortDir(path))
            {
                Prompt.Tell(L.T("Engine folder"), L.T("That folder has no run.py with Bloodborne.exe or bin\\bb-probe.exe."));
                return;
            }
            S.Set("port_dir", path);
            Ws.ResolvePort();
            Ws.SaveSoon();
            Refresh();
        }

        // ---------------------------------------------------------------- rebuild

        private static string? BuilderPath()
        {
            var beside = Path.Combine(AppContext.BaseDirectory, SourceStamp.BuilderExe);
            if (File.Exists(beside)) return beside;
            if (Paths.SourceRoot is { } root)
            {
                var installed = Path.Combine(SourceStamp.AppDirOf(root), SourceStamp.BuilderExe);
                if (File.Exists(installed)) return installed;
            }
            return null;
        }

        private static string BuildStatus()
        {
            var version = typeof(SettingsPage).Assembly.GetName().Version;
            var root = Paths.SourceRoot;
            if (root == null)
                return L.F($"Version {version?.ToString(3)}. The sources (launcher/windows) are not next to this copy, so it cannot rebuild itself.");

            var stamp = SourceStamp.Read(SourceStamp.AppDirOf(root));
            var current = SourceStamp.Compute(root, SourceStamp.LauncherProject);
            var state = stamp.Launcher == current
                ? L.T("Up to date with the sources")
                : L.T("The sources changed since this build: rebuild to pick the changes up");
            var built = stamp.Built == default ? "" : L.F($" · built {stamp.Built.ToLocalTime():yyyy-MM-dd HH:mm}");
            return L.F($"Version {version?.ToString(3)}{built}. {state}.");
        }

        private void Rebuild(bool force)
        {
            var builder = BuilderPath();
            if (builder == null)
            {
                Prompt.Tell(L.T("Rebuild"), L.F($"{SourceStamp.BuilderExe} was not found next to the launcher."));
                return;
            }
            if (Ws.Game.Running)
            {
                Prompt.Tell(L.T("Rebuild"), L.T("Stop the game before rebuilding the launcher."));
                return;
            }

            Ws.SaveNow();
            var args = $"--wait-pid {Environment.ProcessId}" + (force ? " --force" : "");
            Process.Start(new ProcessStartInfo(builder, args) { UseShellExecute = OperatingSystem.IsWindows(), WorkingDirectory = Path.GetDirectoryName(builder)! });
            LanternApp.Quit();
        }
    }
}
