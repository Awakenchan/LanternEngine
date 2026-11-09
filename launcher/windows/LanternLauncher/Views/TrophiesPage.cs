using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Immutable;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>Trophies: completion, every trophy earned or locked, and the in-game notification.</summary>
    public sealed class TrophiesPage : LauncherPage
    {
        private readonly TextBlock _percent, _count, _points, _baseText, _dlcText;
        private readonly ProgressBar _overall, _base, _dlc;
        private readonly UniformGrid _grades = new() { Rows = 1, Margin = new Thickness(0, 16, 0, 0) };
        private readonly StackPanel _list = new();
        private readonly InfoBar _support;
        private readonly TextBlock _logHint;
        private string _filter = "all";

        public TrophiesPage() : base(L.T("Trophies"),
            L.T("Bloodborne's 40 trophies, your completion rate, and the notification shown in game when one unlocks."))
        {
            _support = new InfoBar { IsClosable = false, Severity = InfoBarSeverity.Warning, Margin = new Thickness(0, 14, 0, 0) };
            Body.Children.Add(_support);

            // ==================== Completion ====================
            var card = new Card { Margin = new Thickness(0, 18, 0, 0) };
            var stack = new StackPanel();

            var top = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
            _percent = new TextBlock { FontSize = 40, FontWeight = FontWeight.SemiBold, VerticalAlignment = VerticalAlignment.Center };
            _percent.SetResourceReference(TextBlock.FontFamilyProperty, "MonoFont");
            top.Children.Add(_percent);
            var summary = new StackPanel { Margin = new Thickness(18, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            summary.Children.Add(Text(L.T("COMPLETION"), "StatLabel"));
            _count = Text("", "RowTitle");
            _points = Text("", "Faint");
            summary.Children.Add(_count);
            summary.Children.Add(_points);
            Grid.SetColumn(summary, 1);
            top.Children.Add(summary);
            stack.Children.Add(top);

            _overall = Bar(10);
            _overall.Margin = new Thickness(0, 14, 0, 0);
            stack.Children.Add(_overall);

            var parts = new UniformGrid { Rows = 1, Margin = new Thickness(0, 14, 0, 0) };
            parts.Children.Add(Part("Bloodborne", out _baseText, out _base));
            parts.Children.Add(Part(L.T("The Old Hunters"), out _dlcText, out _dlc));
            stack.Children.Add(parts);
            stack.Children.Add(_grades);
            card.Content = stack;
            Body.Children.Add(card);

            // ==================== The list ====================
            var header = new Grid { Margin = new Thickness(2, 24, 0, 8), ColumnDefinitions = new ColumnDefinitions("*,Auto") };
            var title = Text(L.T("All trophies"), "GroupHeader");
            title.Margin = new Thickness(0);
            title.VerticalAlignment = VerticalAlignment.Center;
            header.Children.Add(title);
            var filter = new ComboBox { Width = 150 };
            foreach (var (label, value) in new[] { (L.T("All"), "all"), (L.T("Earned"), "earned"), (L.T("Locked"), "locked") })
                filter.Items.Add(new ComboBoxItem { Content = label, Tag = value });
            ComboSearch.Attach(filter);
            filter.SelectedIndex = 0;
            filter.SelectionChanged += (_, _) =>
            {
                if (filter.SelectedItem is not ComboBoxItem { Tag: string value }) return;
                _filter = value;
                Refresh();
            };
            Grid.SetColumn(filter, 1);
            header.Children.Add(filter);
            Body.Children.Add(header);
            Body.Children.Add(_list);

            // ==================== Notifications ====================
            Group(L.T("Notification"));
            SettingToggle(SymbolRegular.Alert24, L.T("Show in game"), L.T("A small banner in a corner of the game's screen, without taking focus. In fullscreen it needs Display › Borderless fullscreen"),
                "trophy_notifications");
            ComboRow(SymbolRegular.Pin24, L.T("Position"), null, new[]
            {
                new Choice(L.T("Top right"), "top-right"), new Choice(L.T("Top left"), "top-left"),
                new Choice(L.T("Bottom right"), "bottom-right"), new Choice(L.T("Bottom left"), "bottom-left"),
            }, () => S.Str("trophy_position"), v => S.Set("trophy_position", v));
            SettingToggle(SymbolRegular.Speaker224, L.T("Play a sound"), null, "trophy_sound");
            SliderRow(SymbolRegular.Speaker124, L.T("Volume"), null, 0, 100, 5, () => S.Int("trophy_volume"),
                v => S.Set("trophy_volume", (int)v), "0");
            Add(Card(SymbolRegular.MusicNote224, L.T("Sound file"), L.T("A sound of your own (WAV, MP3, MP4/M4A); empty uses the built-in chime"), Row(
                SmallButton(L.T("Choose…"), SymbolRegular.FolderOpen24, (_, _) => ChooseSound()),
                IconButton(SymbolRegular.ArrowUndo24, L.T("Use the built-in chime"), (_, _) =>
                {
                    S.Set("trophy_sound_file", "");
                    Ws.SaveSoon();
                    Ws.Log(L.T("Trophy sound: built-in chime."), LogLevel.Step);
                })), out _));
            Add(Card(SymbolRegular.PlayCircle24, L.T("Try it"), L.T("Shows a sample notification and plays the sound"),
                SmallButton(L.T("Test"), SymbolRegular.Trophy24, (_, _) => TrophyToast.Announce(TrophyTracker.ById[14])), out _));
            Add(Card(SymbolRegular.Folder24, L.T("Trophy log"), "", IconButton(SymbolRegular.Open24, L.T("Open the saves folder"),
                (_, _) => Paths.OpenFolder(GameEnvironment.UserDir(S))), out _logHint));
            Add(Card(SymbolRegular.ArrowReset24, L.T("Reset trophy progress"), L.T("Starts trophies and completion over, for a fresh run. The old log is kept as a backup"),
                SmallButton(L.T("Reset…"), SymbolRegular.ArrowReset24, (_, _) => ResetProgress()), out _));

            Ws.TrophiesChanged += Refresh;
        }

        private void ResetProgress()
        {
            if (Ws.Trophies.Count == 0)
            {
                Prompt.Tell(L.T("Reset trophy progress"), L.T("There is no trophy progress to reset."));
                return;
            }
            if (!Prompt.Confirm(L.T("Reset trophy progress"),
                    L.F($"All {Ws.Trophies.Count} unlocked trophies and the completion rate start over. Your saves are not touched, ") +
                    L.T("and the current trophy log is kept as a backup in the saves folder."), L.T("Reset")))
                return;
            try
            {
                var backup = Ws.Trophies.Reset();
                Ws.Log(backup == null ? L.T("Trophy progress reset.") : L.T("Trophy progress reset. Backup: ") + backup, LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not reset trophy progress: ") + e.Message, LogLevel.Bad);
            }
            Refresh();
        }

        private static ProgressBar Bar(double height)
        {
            var bar = new ProgressBar { Height = height, MinHeight = height, Minimum = 0, Maximum = 1, CornerRadius = new CornerRadius(height / 2) };
            bar.SetResourceReference(ProgressBar.ForegroundProperty, "AccentTextFillColorPrimaryBrush");
            bar.SetResourceReference(ProgressBar.BackgroundProperty, "PillTrackBrush");
            return bar;
        }

        private static StackPanel Part(string name, out TextBlock text, out ProgressBar bar)
        {
            var panel = new StackPanel { Margin = new Thickness(0, 0, 18, 0) };
            var row = new Grid();
            row.Children.Add(Text(name, "Muted"));
            text = Text("", "Faint");
            text.HorizontalAlignment = HorizontalAlignment.Right;
            row.Children.Add(text);
            panel.Children.Add(row);
            bar = Bar(5);
            bar.Margin = new Thickness(0, 6, 0, 0);
            panel.Children.Add(bar);
            return panel;
        }

        public override void OnShown()
        {
            Refresh();
            _ = CheckSupportAsync();
        }

        /// <summary>The probe's path, size and time with its answer: bb-probe.exe is read whole (tens
        /// of MB) only when it changed, and never on the UI thread.</summary>
        private static (string Path, long Length, DateTime Written, bool Reports)? _probeChecked;

        private async Task CheckSupportAsync()
        {
            var probe = Path.Combine(Paths.BinariesDir, "bb-probe.exe");
            var log = Ws.Trophies.LogPath;
            var cached = _probeChecked;
            var (reports, hasLog) = await Task.Run(() =>
            {
                var info = new FileInfo(probe);
                bool says;
                if (!info.Exists) says = false;
                else if (cached is { } c && c.Path == probe && c.Length == info.Length && c.Written == info.LastWriteTimeUtc)
                    says = c.Reports;
                else
                {
                    says = TrophyTracker.RuntimeReportsTrophies(probe);
                    _probeChecked = (probe, info.Length, info.LastWriteTimeUtc, says);
                }
                return (says, File.Exists(log));
            });
            _support.IsOpen = !reports && !hasLog;
            _support.Title = L.T("This build of the game does not report trophies");
            _support.Message = L.T("Its bb-probe.exe accepts unlocks silently, so nothing can be tracked. A build with trophy hooks ") +
                               L.T("writes trophies.log, and the ") +
                               L.T("launcher picks it up from there.");
        }

        private void Refresh()
        {
            var t = Ws.Trophies;
            SetHint(_logHint, L.F($"trophies.log in the saves folder of the {PlayerProfiles.Current(S)} profile: one line per unlock (id and time)"));
            _percent.Text = $"{t.Completion * 100:0}%";
            _count.Text = L.F($"{t.Count} of {TrophyTracker.All.Length} trophies");
            _points.Text = L.F($"{t.EarnedPoints} of {t.TotalPoints} points (weighted by grade, as on PlayStation)");
            _overall.Value = t.Completion;

            var (b, bt) = t.Progress(dlc: false);
            var (d, dt) = t.Progress(dlc: true);
            _baseText.Text = $"{b} / {bt}";
            _base.Value = (double)b / bt;
            _dlcText.Text = $"{d} / {dt}";
            _dlc.Value = (double)d / dt;

            // The grades and the 40 rows are made again only when the unlocks or the filter changed
            // (Refresh also runs at every trophies.log check and each time the page is shown).
            var shown = _filter + ":" + string.Join(",", t.Unlocked.OrderBy(p => p.Key).Select(p => $"{p.Key}@{p.Value}"));
            if (shown == _shown) return;
            _shown = shown;

            _grades.Children.Clear();
            foreach (var grade in new[] { Grade.Platinum, Grade.Gold, Grade.Silver, Grade.Bronze })
            {
                var all = TrophyTracker.All.Where(x => x.Grade == grade).ToList();
                var earned = all.Count(x => t.Unlocked.ContainsKey(x.Id));
                var icon = new SymbolIcon(SymbolRegular.Trophy24) { Filled = true, FontSize = 18, Foreground = GradeBrush(grade) };
                var value = Text($"{earned} / {all.Count}", "StatValue");
                value.Margin = new Thickness(8, 0, 0, 0);
                var label = Text(Trophy.GradeName(grade), "StatLabel");
                var col = new StackPanel();
                col.Children.Add(label);
                col.Children.Add(Row(icon, value));
                _grades.Children.Add(col);
            }

            _list.Children.Clear();
            foreach (var trophy in TrophyTracker.All)
            {
                var earned = t.Unlocked.TryGetValue(trophy.Id, out var when);
                if (_filter == "earned" && !earned || _filter == "locked" && earned) continue;

                var hint = trophy.Description + (trophy.Dlc ? L.T("  ·  The Old Hunters") : "");
                var status = earned
                    ? DateTimeOffset.FromUnixTimeSeconds(when).LocalDateTime.ToString("d MMM yyyy, HH:mm")
                    : L.T("Locked");
                var right = Text(status, earned ? "Muted" : "Faint");
                right.VerticalAlignment = VerticalAlignment.Center;
                right.TextAlignment = TextAlignment.Right;
                right.MinWidth = 130;

                var row = Card(SymbolRegular.Trophy24, trophy.Name, hint, right, out _);
                var symbol = row.Icon;
                row.ShowIcon = true;
                SetHint(row.Hint, hint);
                symbol.Filled = earned;
                if (earned) symbol.Foreground = GradeBrush(trophy.Grade);
                else symbol.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
                row.Opacity = earned ? 1 : 0.62;
                if (TrophyIcons.For(S, trophy.Id) is { } picture)
                    row.SetPicture(picture, 36, grey: !earned);
                row.SetTip(L.F($"{Trophy.GradeName(trophy.Grade)} · {trophy.Points} points"));
                _list.Children.Add(row);
            }
            if (_list.Children.Count == 0)
                _list.Children.Add(Text(_filter == "earned" ? L.T("No trophies earned yet.") : L.T("Every trophy is earned. Hats off!"), "Faint"));
        }

        private string? _shown;

        private static readonly Dictionary<Grade, IBrush> GradeBrushes = new();

        /// <summary>One brush per grade, shared by every row.</summary>
        private static IBrush GradeBrush(Grade grade)
        {
            if (!GradeBrushes.TryGetValue(grade, out var brush))
                GradeBrushes[grade] = brush = new ImmutableSolidColorBrush(TrophyToast.GradeColor(grade));
            return brush;
        }

        private void ChooseSound()
        {
            var file = Ui.OpenFile(L.T("Trophy sound"), L.T("Sound files|*.wav;*.mp3;*.mp4;*.m4a;*.aac;*.wma"));
            if (file == null) return;
            S.Set("trophy_sound_file", file);
            Ws.SaveSoon();
            Chime.Play(S.Int("trophy_volume"), file);
        }
    }
}
