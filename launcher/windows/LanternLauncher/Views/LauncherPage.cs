using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.LogicalTree;
using Avalonia.Media;
using Avalonia.Threading;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// One setting: its title on the left (descriptions in an ⓘ tooltip), its control on the right,
    /// or below it for wide controls such as segmented buttons. Hint is a line under the title for
    /// live status (a path, "not available on this GPU"); it stays hidden until something is set.
    /// </summary>
    public sealed class SettingRow : Border
    {
        public SymbolIcon Icon { get; }
        public TextBlock Hint { get; }
        public string Title { get; }
        /// <summary>The description (the ⓘ tooltip, or the line under the title in Classic).</summary>
        public string? Info { get; }

        /// <summary>A setting the settings search finds: set for rows a page puts in with LauncherPage.Add,
        /// not for list entries such as trophies, mods or connection test results.</summary>
        public bool Searchable { get; set; }
        /// <summary>The group header the row is under ("Experimental"), for the search results.</summary>
        public string? Section { get; set; }
        /// <summary>A choice row's options ("FSR 4 · DLSS · TAA"), so searching for an option finds its row.</summary>
        public string Choices { get; set; } = "";

        public bool ShowIcon
        {
            get => Icon.IsVisible;
            set => Icon.IsVisible = value;
        }

        /// <summary>Classic layout: one card per setting with its icon and description, as before 0.9.4.</summary>
        public static bool Classic { get; set; }

        public SettingRow(SymbolRegular icon, string title, string? info, object? control, bool wide)
        {
            Title = title;
            Info = info;
            Padding = Classic ? new Thickness(14, 9, 14, 9) : new Thickness(14, 7, 14, 7);
            MinHeight = Classic ? 52 : 38;
            BorderThickness = Classic ? new Thickness(1) : new Thickness(0, 1, 0, 0);
            this.SetResourceReference(BorderBrushProperty, Classic ? "CardStrokeColorDefaultBrush" : "GlassDividerBrush");
            if (Classic)
            {
                CornerRadius = new CornerRadius(6);
                Margin = new Thickness(0, 0, 0, 3);
            }
            PlainBackground();

            Icon = new SymbolIcon(icon)
            {
                FontSize = Classic ? 20 : 18,
                Margin = new Thickness(0, 0, Classic ? 16 : 12, 0),
                VerticalAlignment = VerticalAlignment.Center,
                IsVisible = Classic,
            };

            var name = new TextBlock { Text = title, FontSize = 13.5, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            var titleLine = new StackPanel { Orientation = Orientation.Horizontal };
            titleLine.Children.Add(name);
            if (!string.IsNullOrWhiteSpace(info) && !Classic)
            {
                var mark = new SymbolIcon(SymbolRegular.Info16) { FontSize = 12, Margin = new Thickness(6, 1, 0, 0), Cursor = new Cursor(StandardCursorType.Help) };
                mark.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
                ToolTip.SetTip(mark, Wrapped(info));
                ToolTip.SetShowDelay(mark, 150);
                titleLine.Children.Add(mark);
            }

            Hint = new TextBlock { FontSize = 11.5, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 0), IsVisible = false };
            Hint.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");

            var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(titleLine);
            if (Classic && !string.IsNullOrWhiteSpace(info))
            {
                // Classic shows the description under the title instead of in a tooltip: its first
                // sentence there, the whole text when the mouse is on it.
                var brief = LauncherPage.Brief(info);
                var description = new TextBlock { Text = brief, FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 1, 0, 0) };
                if (brief != info) ToolTip.SetTip(description, Wrapped(info));
                description.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
                text.Children.Add(description);
            }

            var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto") };
            grid.Children.Add(Icon);
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);

            if (control is Control element)
            {
                if (wide)
                {
                    // Title on its own line, the control across the row, the hint under it.
                    grid.RowDefinitions = new RowDefinitions("Auto,Auto,Auto");
                    element.Margin = new Thickness(0, 8, 0, 0);
                    Grid.SetRow(element, 1);
                    Grid.SetColumn(element, 1);
                    Grid.SetColumnSpan(element, 2);
                    grid.Children.Add(element);
                    Grid.SetRow(Hint, 2);
                    Grid.SetColumn(Hint, 1);
                    Grid.SetColumnSpan(Hint, 2);
                    grid.Children.Add(Hint);
                }
                else
                {
                    text.Children.Add(Hint);
                    element.Margin = new Thickness(12, 0, 0, 0);
                    element.VerticalAlignment = VerticalAlignment.Center;
                    Grid.SetColumn(element, 2);
                    grid.Children.Add(element);
                }
            }
            else
                text.Children.Add(Hint);

            Child = grid;
        }

        /// <summary>A picture in the icon's place (a trophy's own), <paramref name="size"/> pixels square.</summary>
        public void SetPicture(string file, double size, bool grey = false)
        {
            if (Icon.Parent is not Grid grid || Picture(file, (int)(size * 2), grey) is not { } source) return;
            grid.Children.Add(new Image
            {
                Source = source, Width = size, Height = size, Margin = Icon.Margin, VerticalAlignment = VerticalAlignment.Center,
            });
            Icon.IsVisible = false;
        }

        /// <summary>A picture file decoded at <paramref name="pixels"/> wide; null if it cannot be read.</summary>
        public static Avalonia.Media.Imaging.Bitmap? Picture(string file, int pixels, bool grey = false)
        {
            try
            {
                using var stream = System.IO.File.OpenRead(file);
                var bitmap = Avalonia.Media.Imaging.Bitmap.DecodeToWidth(stream, pixels);
                if (!grey) return bitmap;
                using (bitmap) return Grey(bitmap);
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static unsafe Avalonia.Media.Imaging.Bitmap Grey(Avalonia.Media.Imaging.Bitmap source)
        {
            var size = source.PixelSize;
            var target = new Avalonia.Media.Imaging.WriteableBitmap(size, source.Dpi, Avalonia.Platform.PixelFormat.Bgra8888, Avalonia.Platform.AlphaFormat.Premul);
            using var frame = target.Lock();
            source.CopyPixels(new PixelRect(size), frame.Address, frame.RowBytes * size.Height, frame.RowBytes);
            for (var y = 0; y < size.Height; y++)
            {
                var pixel = (byte*)frame.Address + y * frame.RowBytes;
                for (var x = 0; x < size.Width; x++, pixel += 4)
                {
                    var luma = (byte)((pixel[2] * 299 + pixel[1] * 587 + pixel[0] * 114) / 1000);
                    pixel[0] = pixel[1] = pixel[2] = luma;
                }
            }
            return target;
        }

        /// <summary>A long tooltip as a wrapped block, not one line across the screen.</summary>
        public static TextBlock Wrapped(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = 420 };

        /// <summary>A card in Classic; nothing in Modern, where the group's glass box shows through.</summary>
        private void PlainBackground()
        {
            if (Classic) this.SetResourceReference(BackgroundProperty, "CardBackgroundFillColorDefaultBrush");
            else ClearValue(BackgroundProperty);
        }

        private DispatcherTimer? _fade;

        /// <summary>Lights the row up in the accent colour for a moment, then fades back: the settings
        /// search found it.</summary>
        public void Flash()
        {
            var accent = LauncherTheme.Brush("AccentTextFillColorPrimaryBrush") is ISolidColorBrush { Color: var c } ? c : Colors.SteelBlue;
            var glow = new SolidColorBrush(Color.FromArgb(0x55, accent.R, accent.G, accent.B));
            Background = glow;
            _fade?.Stop();
            var started = DateTime.Now;
            _fade = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
            _fade.Tick += (_, _) =>
            {
                var t = (DateTime.Now - started).TotalSeconds - 0.9;
                if (t < 0) return;
                glow.Opacity = Math.Max(0, 1 - t / 1.2);
                if (glow.Opacity > 0) return;
                _fade!.Stop();
                // A second search for the same row while this one fades has its own glow: leave that one be.
                if (ReferenceEquals(Background, glow)) PlainBackground();
            };
            _fade.Start();
        }
    }

    /// <summary>A row of options where one is chosen, like the PS4/macOS segmented control.</summary>
    public sealed class Segmented : Border
    {
        private readonly List<Border> _items = new();
        public int SelectedIndex { get; private set; } = -1;
        public event Action<int>? Changed;

        public Segmented(IReadOnlyList<Choice> choices, Func<Choice, bool>? enabled = null)
        {
            CornerRadius = new CornerRadius(14);
            Padding = new Thickness(3);
            this.SetResourceReference(BackgroundProperty, "PillTrackBrush");

            var grid = new Grid();
            for (var i = 0; i < choices.Count; i++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition(1, GridUnitType.Star));
                var index = i;
                var choice = choices[i];
                var label = new TextBlock
                {
                    Text = Short(choice.Label),
                    FontSize = 12.5,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                    TextTrimming = TextTrimming.CharacterEllipsis,
                };
                var item = new Border
                {
                    CornerRadius = new CornerRadius(11),
                    Padding = new Thickness(8, 5, 8, 5),
                    Child = label,
                    Cursor = new Cursor(StandardCursorType.Hand),
                    Background = Brushes.Transparent,
                };
                if (choice.Label != Short(choice.Label) || choice.Note != null)
                    ToolTip.SetTip(item, SettingRow.Wrapped(string.Join("\n", new[] { choice.Label, choice.Note }.Where(t => t != null))));
                var on = enabled?.Invoke(choice) ?? true;
                item.IsEnabled = on;
                if (!on) label.Opacity = 0.35;
                item.PointerReleased += (_, e) =>
                {
                    if (e.InitialPressMouseButton == MouseButton.Left) Select(index, notify: true);
                };
                item.PointerEntered += (_, _) => { if (index != SelectedIndex) item.SetResourceReference(BackgroundProperty, "SidebarItemHoverBrush"); };
                item.PointerExited += (_, _) => { if (index != SelectedIndex) Plain(item); };
                Grid.SetColumn(item, i);
                grid.Children.Add(item);
                _items.Add(item);
            }
            Child = grid;
        }

        private static void Plain(Border item)
        {
            item.ClearValue(BackgroundProperty);
            item.Background = Brushes.Transparent;
        }

        /// <summary>"TAA (native anti-aliasing)" → "TAA": the full label is in the tooltip. Full-width and
        /// unspaced brackets count too, as Chinese and Korean write them.</summary>
        public static string Short(string label) =>
            label.IndexOfAny(new[] { '(', '（' }) is > 0 and var i ? label[..i].TrimEnd() : label;

        public void Select(int index, bool notify)
        {
            if (index < 0 || index >= _items.Count || !_items[index].IsEnabled) return;
            SelectedIndex = index;
            for (var i = 0; i < _items.Count; i++)
            {
                var label = (TextBlock)_items[i].Child!;
                if (i == index)
                {
                    _items[i].SetResourceReference(BackgroundProperty, "SegmentSelectedBrush");
                    label.ClearValue(TextBlock.ForegroundProperty);
                    label.Foreground = Brushes.White;
                    label.FontWeight = FontWeight.SemiBold;
                }
                else
                {
                    Plain(_items[i]);
                    label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
                    label.FontWeight = FontWeight.Normal;
                }
            }
            if (notify) Changed?.Invoke(index);
        }
    }

    /// <summary>The rows inside an Advanced section (folded away until it is opened).</summary>
    public sealed class AdvancedRows : StackPanel { }

    /// <summary>
    /// A folded part of a group: an "Advanced (n)" line that opens and closes the rows under it.
    /// Rows put in Rows stay searchable; the search opens the section to show one.
    /// </summary>
    public sealed class AdvancedSection : StackPanel
    {
        public AdvancedRows Rows { get; } = new() { IsVisible = false };
        private readonly SymbolIcon _chevron;
        private readonly TextBlock _label;
        private readonly string _title;

        public AdvancedSection(string title)
        {
            _title = title;
            _chevron = new SymbolIcon(SymbolRegular.ChevronRight16) { FontSize = 12, Margin = new Thickness(0, 0, 8, 0) };
            _chevron.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            _label = new TextBlock { Text = title, FontSize = 12.5, VerticalAlignment = VerticalAlignment.Center };
            _label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            var header = new Border
            {
                Padding = new Thickness(14, 7, 14, 7),
                BorderThickness = SettingRow.Classic ? new Thickness(0) : new Thickness(0, 1, 0, 0),
                Background = Brushes.Transparent,
                Cursor = new Cursor(StandardCursorType.Hand),
                Focusable = true,
                Child = LauncherPage.Row(_chevron, _label),
            };
            header.SetTip(L.T("Settings most players never need to change"));
            header.SetResourceReference(Border.BorderBrushProperty, "GlassDividerBrush");
            header.PointerReleased += (_, e) => { if (e.InitialPressMouseButton == MouseButton.Left) IsOpen = !IsOpen; };
            header.KeyDown += (_, e) =>
            {
                if (e.Key is Key.Enter or Key.Space) { IsOpen = !IsOpen; e.Handled = true; }
            };
            header.PointerEntered += (_, _) => _label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
            header.PointerExited += (_, _) => _label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            Children.Add(header);
            Children.Add(Rows);
            Loaded += (_, _) => ShowCount();
        }

        public bool IsOpen
        {
            get => Rows.IsVisible;
            set
            {
                Rows.IsVisible = value;
                _chevron.Symbol = value ? SymbolRegular.ChevronDown16 : SymbolRegular.ChevronRight16;
                ShowCount();
            }
        }

        private void ShowCount()
        {
            var count = Rows.Children.OfType<SettingRow>().Count(r => r.IsVisible);
            _label.Text = count > 0 ? $"{_title} ({count})" : _title;
        }
    }

    /// <summary>
    /// A settings page: groups of compact rows, each group one rounded box (Sectionize puts the
    /// rows of a group into it once the page is built). The helpers bind a control straight to a
    /// settings.json or lantern.ini key and save on change.
    /// </summary>
    public abstract class LauncherPage : UserControl
    {
        protected static Workspace Ws => Workspace.Current;
        protected static LauncherSettings S => Ws.Settings;
        protected static IniFile Ini => Ws.Ini;

        protected StackPanel Body { get; }
        protected ScrollViewer Scroller { get; }
        private bool _sectioned;

        protected LauncherPage(string title, string subtitle)
        {
            Background = Brushes.Transparent;
            this.SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");

            // The tab says which page this is; no page title.
            Body = new StackPanel { Margin = new Thickness(20, 6, 20, 24), MaxWidth = 880 };
            Scroller = new ScrollViewer
            {
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                Content = Body,
            };
            Content = Scroller;
            if (SettingRow.Classic) Body.MaxWidth = double.PositiveInfinity;
            Loaded += (_, _) =>
            {
                if (_sectioned || SettingRow.Classic) return;
                _sectioned = true;
                Sectionize();
            };
        }

        /// <summary>Called each time the page is shown; refresh anything read from disk.</summary>
        public virtual void OnShown() { }

        /// <summary>Whether the page is on screen now (pages are kept while another one shows).</summary>
        protected bool IsShown => TopLevel.GetTopLevel(this) != null && IsEffectivelyVisible;

        /// <summary>
        /// Puts the rows that follow each group header into one rounded box. Rows, panels of rows
        /// and short captions go in; anything else (an info bar, a table, a summary card) ends the box.
        /// </summary>
        private void Sectionize()
        {
            var children = Body.Children.ToList();
            Body.Children.Clear();
            StackPanel? box = null;
            foreach (var child in children)
            {
                if (child is TextBlock { Tag: "group" })
                {
                    Body.Children.Add(child);
                    box = new StackPanel { Margin = new Thickness(0, -1, 0, 0) };
                    var frame = new Border
                    {
                        CornerRadius = new CornerRadius(16),
                        BorderThickness = new Thickness(1),
                        ClipToBounds = true, // to the rounded shape: the first row's divider stays inside the corners
                        Child = box,
                    };
                    frame.SetResourceReference(Border.BackgroundProperty, "GlassPaneBrush");
                    frame.SetResourceReference(Border.BorderBrushProperty, "GlassPaneStrokeBrush");
                    Body.Children.Add(frame);
                    continue;
                }
                var fits = child is SettingRow || child is StackPanel || child is WrapPanel ||
                           (child is TextBlock && box != null);
                if (box != null && fits)
                {
                    if (child is TextBlock caption) caption.Margin = new Thickness(14, 8, 14, 4);
                    if (child is WrapPanel wrap) wrap.Margin = new Thickness(10, 4, 10, 10);
                    box.Children.Add(child);
                }
                else
                {
                    box = null;
                    Body.Children.Add(child);
                }
            }
        }

        // ---------------------------------------------------------------- building blocks

        /// <summary>A text in one of the launcher's text styles (Themes/Styles.axaml, by class).</summary>
        public static TextBlock Text(string text, string style)
        {
            var block = new TextBlock { Text = text };
            block.Classes.Add(style);
            return block;
        }

        protected TextBlock Group(string text, Panel? into = null)
        {
            var header = Text(text, "GroupHeader");
            header.Tag = "group";
            (into ?? Body).Children.Add(header);
            _group = text;
            return header;
        }

        /// <summary>
        /// A folded "Advanced" part of the current group, for settings few players change. Put rows in
        /// with into: the panel this returns. The settings search still finds them and opens it.
        /// </summary>
        protected Panel Advanced(string title = "Advanced", Panel? into = null)
        {
            var section = new AdvancedSection(title == "Advanced" ? L.T("Advanced") : title);
            (into ?? Body).Children.Add(section);
            return section.Rows;
        }

        /// <summary>The first sentence of a long description, for a line under a row (the whole
        /// text goes in its tooltip). Short text is returned as it is.</summary>
        public static string Brief(string text, int max = 90)
        {
            if (text.Length <= max) return text;
            foreach (var end in new[] { ". ", "; ", ": " })
            {
                var at = text.IndexOf(end, StringComparison.Ordinal);
                if (at >= 20 && at < max) return text[..at] + (end == ". " ? "." : "…");
            }
            var cut = text.LastIndexOf(' ', max);
            return (cut > 20 ? text[..cut] : text[..max]).TrimEnd(',', ';', ':', ' ') + "…";
        }

        /// <summary>The last group header, which the rows added after it belong to.</summary>
        private string? _group;

        public static SymbolIcon Icon(SymbolRegular symbol) => new(symbol);

        /// <summary>A setting row. info goes in the ⓘ tooltip; hintBlock is a visible status line, empty until set.</summary>
        public static SettingRow Card(SymbolRegular icon, string title, string? info, object? control, out TextBlock hintBlock, bool wide = false)
        {
            var row = new SettingRow(icon, title, info, control, wide);
            hintBlock = row.Hint;
            return row;
        }

        public static void SetHint(TextBlock hint, string? text)
        {
            hint.Text = text ?? "";
            hint.IsVisible = !string.IsNullOrEmpty(text);
        }

        /// <summary>Puts a setting on the page; the settings search finds every row added here.</summary>
        protected SettingRow Add(SettingRow row, Panel? into = null)
        {
            (into ?? Body).Children.Add(row);
            row.Searchable = true;
            row.Section = _group;
            _searchRows.Add(row);
            return row;
        }

        // ---------------------------------------------------------------- settings search

        /// <summary>Every row put in with Add, in page order: the settings search's index of this
        /// page, built as the page is (no walk through the page's tree at each search).</summary>
        private readonly List<SettingRow> _searchRows = new();

        /// <summary>
        /// This page's settings that can be seen right now: the rows put in with Add, minus those a
        /// choice hides (Custom resolution while a named size is chosen, a cheat's amount while it is
        /// off) and those no longer on the page.
        /// </summary>
        public IEnumerable<SettingRow> SearchRows() =>
            _searchRows.Where(row => row.Searchable && Shown(row));

        private bool Shown(ILogical? at)
        {
            for (; at != null && !ReferenceEquals(at, this); at = at.GetLogicalParent())
                if (at is Control { IsVisible: false } && at is not AdvancedRows)
                    return false; // a folded Advanced section still counts: the search opens it
            return at != null;
        }

        /// <summary>
        /// Scrolls a row of this page to the middle of the view, flashes it and puts the keyboard on
        /// its control, so Space or the arrow keys change it. False when it has no control to focus.
        /// </summary>
        public bool Reveal(SettingRow row)
        {
            for (ILogical? at = row; at != null; at = at.GetLogicalParent())
                if (at is AdvancedSection section) section.IsOpen = true;
            UpdateLayout();
            if (!row.IsEffectivelyVisible) return false;
            var focused = SettingSearch.Descendants(row).OfType<InputElement>()
                .FirstOrDefault(c => c is { Focusable: true, IsEnabled: true, IsEffectivelyVisible: true } && c is not SettingRow)?.Focus() == true;
            if (row.TranslatePoint(new Point(0, 0), Scroller) is { } at2)
            {
                var top = at2.Y + Scroller.Offset.Y;
                Scroller.Offset = new Vector(Scroller.Offset.X,
                    Math.Max(0, top - Math.Max(0, Scroller.Viewport.Height - row.Bounds.Height) / 2));
            }
            else
                row.BringIntoView();
            row.Flash();
            return focused;
        }

        public static AppButton SmallButton(string text, SymbolRegular icon, EventHandler<Avalonia.Interactivity.RoutedEventArgs> click,
            ControlAppearance look = ControlAppearance.Secondary)
        {
            var button = new AppButton
            {
                Text = text,
                Icon = Icon(icon),
                Appearance = look,
                Margin = new Thickness(6, 0, 0, 0),
                Padding = new Thickness(10, 5, 10, 5),
                VerticalAlignment = VerticalAlignment.Center,
            };
            button.Click += click;
            return button;
        }

        public static AppButton IconButton(SymbolRegular icon, string tooltip, EventHandler<Avalonia.Interactivity.RoutedEventArgs> click)
        {
            var button = new AppButton
            {
                Icon = Icon(icon),
                Appearance = ControlAppearance.Transparent,
                Padding = new Thickness(6),
                Margin = new Thickness(2, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
            };
            button.SetTip(tooltip);
            button.Click += click;
            return button;
        }

        public static StackPanel Row(params Control[] children)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            foreach (var child in children)
                panel.Children.Add(child);
            return panel;
        }

        // ---------------------------------------------------------------- bound rows

        /// <summary>A choice: segmented buttons for a few short options, a drop-down otherwise.
        /// The chosen option's note shows under the row.</summary>
        protected Control ComboRow(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
            Func<string> get, Action<string> set, Panel? into = null, double width = 240, Action? changed = null,
            Func<Choice, bool>? enabled = null)
        {
            TextBlock note = null!;
            void ShowNote(int index)
            {
                var full = index >= 0 ? choices[index].Note : null;
                var brief = full == null ? null : Brief(full, 110);
                SetHint(note, brief);
                ToolTip.SetTip(note, brief == full ? null : SettingRow.Wrapped(full!));
            }
            void Picked(int index)
            {
                set(choices[index].Value);
                ShowNote(index);
                Ws.SaveSoon();
                changed?.Invoke();
            }

            var options = string.Join(" · ", choices.Select(c => c.Label));
            var shortLabels = choices.Sum(c => Segmented.Short(c.Label).Length);
            if (!SettingRow.Classic && choices.Count is >= 2 and <= 6 && shortLabels <= 72)
            {
                var segmented = new Segmented(choices, enabled);
                var row = Add(Card(icon, title, hint, segmented, out note, wide: true), into);
                row.Choices = options;
                segmented.Select(Core.Choices.IndexOf(choices, get()), notify: false);
                ShowNote(segmented.SelectedIndex);
                segmented.Changed += Picked;
                row.Tag = note;
                return segmented;
            }

            var box = new ComboBox { Width = width, VerticalAlignment = VerticalAlignment.Center };
            foreach (var choice in choices)
            {
                var item = new ComboBoxItem { Content = choice.Label, Tag = choice.Value };
                if (enabled != null && !enabled(choice)) item.IsEnabled = false;
                box.Items.Add(item);
            }
            ComboSearch.Attach(box);
            var card = Add(Card(icon, title, hint, box, out note), into);
            card.Choices = options;
            box.SelectedIndex = Core.Choices.IndexOf(choices, get());
            ShowNote(box.SelectedIndex);
            box.SelectionChanged += (_, _) => { if (box.SelectedIndex >= 0) Picked(box.SelectedIndex); };
            card.Tag = note;
            return box;
        }

        protected Control SettingCombo(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
            string key, Panel? into = null, Action? changed = null) =>
            ComboRow(icon, title, hint, choices, () => S.Str(key), v => S.Set(key, v), into, changed: changed);

        protected Control IniCombo(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
            string key, Panel? into = null, Action? changed = null, Func<Choice, bool>? enabled = null) =>
            ComboRow(icon, title, hint, choices, () => Ini.Get(key), v => Ini.Set(key, v), into, changed: changed, enabled: enabled);

        protected ToggleSwitch ToggleRow(SymbolRegular icon, string title, string? hint, Func<bool> get, Action<bool> set,
            Panel? into = null, Action? changed = null)
        {
            var toggle = new ToggleSwitch { IsChecked = get(), VerticalAlignment = VerticalAlignment.Center };
            toggle.Click += (_, _) =>
            {
                set(toggle.IsChecked == true);
                Ws.SaveSoon();
                changed?.Invoke();
            };
            Add(Card(icon, title, hint, toggle, out _), into);
            return toggle;
        }

        protected ToggleSwitch SettingToggle(SymbolRegular icon, string title, string? hint, string key, Panel? into = null, Action? changed = null) =>
            ToggleRow(icon, title, hint, () => S.Bool(key), v => S.Set(key, v), into, changed);

        protected ToggleSwitch IniToggle(SymbolRegular icon, string title, string? hint, string key, Panel? into = null, Action? changed = null) =>
            ToggleRow(icon, title, hint, () => Ini.Flag(key), v => Ini.Set(key, v ? "1" : "0"), into, changed);

        /// <summary>A slider with a small number box beside it: type a value and press Enter (or click
        /// away) to set it exactly. The value is kept in the slider's range and on its steps; text that
        /// is not a number puts the current value back. Every slider in the launcher is made here.</summary>
        protected Slider SliderRow(SymbolRegular icon, string title, string? hint, double min, double max, double step,
            Func<double> get, Action<double> set, string format = "0.00", Panel? into = null, double valueWidth = 44)
        {
            var slider = new Slider
            {
                Minimum = min,
                Maximum = max,
                SmallChange = step,
                LargeChange = step * 4,
                TickFrequency = step,
                IsSnapToTickEnabled = true,
                Width = 240,
                VerticalAlignment = VerticalAlignment.Center,
                Value = Math.Clamp(get(), min, max),
            };
            var culture = System.Globalization.CultureInfo.InvariantCulture;
            var value = new TextBox
            {
                Width = Math.Max(valueWidth + 24, 66),
                MinHeight = 0,
                Padding = new Thickness(6, 3, 6, 3),
                TextAlignment = TextAlignment.Right,
                VerticalAlignment = VerticalAlignment.Center,
                VerticalContentAlignment = VerticalAlignment.Center,
                Margin = new Thickness(10, 0, 0, 0),
            };
            value.SetTip(L.F($"Type a value from {min.ToString(format, culture)} to {max.ToString(format, culture)} and press Enter"));
            value.SetResourceReference(TextBox.FontFamilyProperty, "MonoFont");
            void Show() => value.Text = slider.Value.ToString(format, culture);
            void Commit()
            {
                var match = System.Text.RegularExpressions.Regex.Match(value.Text ?? "", @"-?(\d+([.,]\d*)?|[.,]\d+)");
                if (!match.Success ||
                    !double.TryParse(match.Value.Replace(',', '.'), System.Globalization.NumberStyles.Float, culture, out var typed))
                {
                    Show(); // not a number: back to the current value
                    return;
                }
                typed = Math.Clamp(typed, min, max);
                if (step > 0) typed = Math.Clamp(min + Math.Round((typed - min) / step) * step, min, max);
                typed = Math.Round(typed, 6);
                if (Math.Abs(slider.Value - typed) > 1e-9) slider.Value = typed; // saves through ValueChanged
                Show();
            }
            Show();
            value.KeyDown += (_, e) =>
            {
                if (e.Key == Key.Enter) { Commit(); value.SelectAll(); e.Handled = true; }
                else if (e.Key == Key.Escape) { Show(); value.SelectAll(); e.Handled = true; }
            };
            value.LostFocus += (_, _) => Commit();
            value.GotFocus += (_, _) => Dispatcher.UIThread.Post(value.SelectAll);
            slider.ValueChanged += (_, _) =>
            {
                Show();
                set(slider.Value);
                Ws.SaveSoon();
            };
            Add(Card(icon, title, hint, Row(slider, value), out _), into);
            return slider;
        }

        /// <summary>A text field row; saves as you type.</summary>
        protected TextBox TextRow(SymbolRegular icon, string title, string? hint, string placeholder,
            Func<string> get, Action<string> set, Panel? into = null, double width = 300)
        {
            var box = new TextBox
            {
                Text = get(),
                Watermark = placeholder,
                Width = width,
                VerticalAlignment = VerticalAlignment.Center,
            };
            box.TextChanged += (_, _) =>
            {
                set(box.Text ?? "");
                Ws.SaveSoon();
            };
            Add(Card(icon, title, hint, box, out _), into);
            return box;
        }

        protected static string Fmt(double value) => value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);

        protected static double ParseDouble(string text, double fallback) =>
            double.TryParse(text, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var v)
                ? v
                : fallback;

        protected static string? PickFolder(string title, string current) => Ui.PickFolder(title, current);
    }
}
