using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

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
            get => Icon.Visibility == Visibility.Visible;
            set => Icon.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
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
            SetResourceReference(BorderBrushProperty, Classic ? "CardStrokeColorDefaultBrush" : "GlassDividerBrush");
            if (Classic)
            {
                CornerRadius = new CornerRadius(6);
                Margin = new Thickness(0, 0, 0, 3);
            }
            PlainBackground();

            Icon = new SymbolIcon { Symbol = icon, FontSize = Classic ? 20 : 18, Margin = new Thickness(0, 0, Classic ? 16 : 12, 0), VerticalAlignment = VerticalAlignment.Center,
                Visibility = Classic ? Visibility.Visible : Visibility.Collapsed };

            var name = new TextBlock { Text = title, FontSize = 13.5, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            var titleLine = new StackPanel { Orientation = Orientation.Horizontal };
            titleLine.Children.Add(name);
            if (!string.IsNullOrWhiteSpace(info) && !Classic)
            {
                var mark = new SymbolIcon { Symbol = SymbolRegular.Info16, FontSize = 12, Margin = new Thickness(6, 1, 0, 0), VerticalAlignment = VerticalAlignment.Center, ToolTip = info, Cursor = Cursors.Help };
                mark.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorTertiaryBrush");
                ToolTipService.SetInitialShowDelay(mark, 150);
                titleLine.Children.Add(mark);
            }

            Hint = new TextBlock { FontSize = 11.5, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 0), Visibility = Visibility.Collapsed };
            Hint.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");

            var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(titleLine);
            if (Classic && !string.IsNullOrWhiteSpace(info))
            {
                // Classic shows the description under the title instead of in a tooltip: its first
                // sentence there, the whole text when the mouse is on it.
                var brief = LauncherPage.Brief(info);
                var description = new TextBlock { Text = brief, FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 1, 0, 0),
                    ToolTip = brief == info ? null : info };
                description.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
                text.Children.Add(description);
            }

            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.Children.Add(Icon);
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);

            if (control is UIElement element)
            {
                if (wide)
                {
                    // Title on its own line, the control across the row, the hint under it.
                    grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    if (element is FrameworkElement fe) fe.Margin = new Thickness(0, 8, 0, 0);
                    Grid.SetRow(element, 1);
                    Grid.SetColumn(element, 1);
                    Grid.SetColumnSpan(element, 2);
                    grid.Children.Add(element);
                    grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    Grid.SetRow(Hint, 2);
                    Grid.SetColumn(Hint, 1);
                    Grid.SetColumnSpan(Hint, 2);
                    grid.Children.Add(Hint);
                }
                else
                {
                    text.Children.Add(Hint);
                    if (element is FrameworkElement fe) fe.Margin = new Thickness(12, 0, 0, 0);
                    Grid.SetColumn(element, 2);
                    grid.Children.Add(element);
                }
            }
            else
                text.Children.Add(Hint);

            Child = grid;
        }

        /// <summary>A picture in the icon's place (a trophy's own), <paramref name="size"/> pixels
        /// square; grey when <paramref name="grey"/>.</summary>
        public void SetPicture(string file, double size, bool grey)
        {
            if (Icon.Parent is not Grid grid || Picture(file, (int)(size * 2), grey) is not { } source) return;
            var image = new System.Windows.Controls.Image
            {
                Source = source, Width = size, Height = size, Margin = Icon.Margin, VerticalAlignment = VerticalAlignment.Center,
            };
            RenderOptions.SetBitmapScalingMode(image, BitmapScalingMode.HighQuality);
            grid.Children.Add(image);
            Icon.Visibility = Visibility.Collapsed;
        }

        /// <summary>A picture file decoded at <paramref name="pixels"/> wide, frozen; null if it cannot be read.</summary>
        public static ImageSource? Picture(string file, int pixels, bool grey = false)
        {
            try
            {
                var bitmap = new System.Windows.Media.Imaging.BitmapImage();
                bitmap.BeginInit();
                bitmap.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad;
                bitmap.DecodePixelWidth = pixels;
                bitmap.UriSource = new Uri(file);
                bitmap.EndInit();
                bitmap.Freeze();
                if (!grey) return bitmap;
                var gray = new System.Windows.Media.Imaging.FormatConvertedBitmap(bitmap, PixelFormats.Gray8, null, 0);
                gray.Freeze();
                return gray;
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>A card in Classic; nothing in Modern, where the group's glass box shows through.</summary>
        private void PlainBackground()
        {
            if (Classic) SetResourceReference(BackgroundProperty, "CardBackgroundFillColorDefaultBrush");
            else ClearValue(BackgroundProperty);
        }

        /// <summary>Lights the row up in the accent colour for a moment, then fades back: the settings
        /// search found it.</summary>
        public void Flash()
        {
            var accent = TryFindResource("AccentTextFillColorPrimaryBrush") is SolidColorBrush { Color: var c } ? c : Colors.SteelBlue;
            var glow = new SolidColorBrush(Color.FromArgb(0x55, accent.R, accent.G, accent.B));
            Background = glow;
            var fade = new System.Windows.Media.Animation.DoubleAnimation(1, 0, TimeSpan.FromSeconds(1.2)) { BeginTime = TimeSpan.FromSeconds(0.9) };
            // A second search for the same row while this one fades has its own glow: leave that one be.
            fade.Completed += (_, _) => { if (ReferenceEquals(Background, glow)) PlainBackground(); };
            glow.BeginAnimation(Brush.OpacityProperty, fade);
        }
    }

    /// <summary>A row of options where one is chosen, like the PS4/macOS segmented control.</summary>
    public sealed class Segmented : Border
    {
        private readonly List<Border> _items = new();
        private readonly IReadOnlyList<Choice> _choices;
        public int SelectedIndex { get; private set; } = -1;
        public event Action<int>? Changed;

        public Segmented(IReadOnlyList<Choice> choices, Func<Choice, bool>? enabled = null)
        {
            _choices = choices;
            CornerRadius = new CornerRadius(14);
            Padding = new Thickness(3);
            SetResourceReference(BackgroundProperty, "PillTrackBrush");

            var grid = new Grid();
            for (var i = 0; i < choices.Count; i++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
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
                    Cursor = Cursors.Hand,
                    Background = Brushes.Transparent,
                    ToolTip = choice.Label != Short(choice.Label) || choice.Note != null
                        ? string.Join("\n", new[] { choice.Label, choice.Note }.Where(t => t != null))
                        : null,
                };
                var on = enabled?.Invoke(choice) ?? true;
                item.IsEnabled = on;
                if (!on) label.Opacity = 0.35;
                item.MouseLeftButtonUp += (_, _) => Select(index, notify: true);
                item.MouseEnter += (_, _) => { if (index != SelectedIndex) item.SetResourceReference(BackgroundProperty, "SidebarItemHoverBrush"); };
                item.MouseLeave += (_, _) => { if (index != SelectedIndex) item.Background = Brushes.Transparent; };
                Grid.SetColumn(item, i);
                grid.Children.Add(item);
                _items.Add(item);
            }
            Child = grid;
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
                var label = (TextBlock)_items[i].Child;
                if (i == index)
                {
                    _items[i].SetResourceReference(BackgroundProperty, "SegmentSelectedBrush");
                    label.Foreground = Brushes.White;
                    label.FontWeight = FontWeights.SemiBold;
                }
                else
                {
                    _items[i].Background = Brushes.Transparent;
                    label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
                    label.FontWeight = FontWeights.Normal;
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
        public AdvancedRows Rows { get; } = new() { Visibility = Visibility.Collapsed };
        private readonly SymbolIcon _chevron;
        private readonly TextBlock _label;
        private readonly string _title;

        public AdvancedSection(string title)
        {
            _title = title;
            _chevron = new SymbolIcon { Symbol = SymbolRegular.ChevronRight16, FontSize = 12, Margin = new Thickness(0, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center };
            _chevron.SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorSecondaryBrush");
            _label = new TextBlock { Text = title, FontSize = 12.5, VerticalAlignment = VerticalAlignment.Center };
            _label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            var header = new Border
            {
                Padding = new Thickness(14, 7, 14, 7),
                BorderThickness = SettingRow.Classic ? new Thickness(0) : new Thickness(0, 1, 0, 0),
                Background = Brushes.Transparent,
                Cursor = Cursors.Hand,
                Focusable = true,
                ToolTip = L.T("Settings most players never need to change"),
                Child = LauncherPage.Row(_chevron, _label),
            };
            header.SetResourceReference(Border.BorderBrushProperty, "GlassDividerBrush");
            header.MouseLeftButtonUp += (_, _) => IsOpen = !IsOpen;
            header.KeyDown += (_, e) =>
            {
                if (e.Key is Key.Enter or Key.Space) { IsOpen = !IsOpen; e.Handled = true; }
            };
            header.MouseEnter += (_, _) => _label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
            header.MouseLeave += (_, _) => _label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            Children.Add(header);
            Children.Add(Rows);
            Loaded += (_, _) => ShowCount();
        }

        public bool IsOpen
        {
            get => Rows.Visibility == Visibility.Visible;
            set
            {
                Rows.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
                _chevron.Symbol = value ? SymbolRegular.ChevronDown16 : SymbolRegular.ChevronRight16;
                ShowCount();
            }
        }

        private void ShowCount()
        {
            var count = Rows.Children.OfType<SettingRow>().Count(r => r.Visibility == Visibility.Visible);
            _label.Text = count > 0 ? $"{_title} ({count})" : _title;
        }

        /// <summary>Counts the rows again: a choice elsewhere on the page shows and hides them (the HDR
        /// source, which leaves Native+ its tone mapper and takes Native's UI colour away).</summary>
        public void RefreshCount() => ShowCount();
    }

    /// <summary>
    /// A settings page: groups of compact rows, each group one rounded box (Sectionize puts the
    /// rows of a group into it once the page is built). The helpers bind a control straight to a
    /// settings.json or lantern.ini key and save on change.
    /// </summary>
    public abstract class LauncherPage : Page
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
            // A Frame does not pass inherited properties on to its page: the window's crisp text
            // settings stopped at it, and every page drew WPF's default "Ideal" text (soft, blurry
            // at small sizes). Set again here.
            TextOptions.SetTextFormattingMode(this, TextFormattingMode.Display);
            TextOptions.SetTextRenderingMode(this, TextRenderingMode.ClearType);
            TextOptions.SetTextHintingMode(this, TextHintingMode.Fixed);
            RenderOptions.SetClearTypeHint(this, ClearTypeHint.Enabled);
            SetResourceReference(FontFamilyProperty, "UiFont");
            SetResourceReference(System.Windows.Documents.TextElement.FontFamilyProperty, "UiFont");
            UseLayoutRounding = true;
            SnapsToDevicePixels = true;
            SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");
            SetResourceReference(System.Windows.Documents.TextElement.ForegroundProperty, "TextFillColorPrimaryBrush");

            // The tab says which page this is; no page title.
            Body = new StackPanel { Margin = new Thickness(20, 6, 20, 24), MaxWidth = 880 };
            Scroller = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Content = Body };
            Content = Scroller;
            PageScroll.Attach(Scroller);
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

        /// <summary>
        /// Puts the rows that follow each group header into one rounded box. Rows, panels of rows
        /// and short captions go in; anything else (an info bar, a table, a summary card) ends the box.
        /// </summary>
        private void Sectionize()
        {
            var children = Body.Children.Cast<UIElement>().ToList();
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
                        Child = box,
                    };
                    // Clip to the rounded shape (ClipToBounds is a plain rectangle, which let the
                    // first row's divider run across the rounded corners).
                    frame.SizeChanged += (_, e) =>
                        frame.Clip = new RectangleGeometry(new Rect(0, 0, e.NewSize.Width, e.NewSize.Height), 16, 16);
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

        public static TextBlock Text(string text, string style)
        {
            var block = new TextBlock { Text = text };
            block.SetResourceReference(StyleProperty, style);
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

        public static SymbolIcon Icon(SymbolRegular symbol) => new() { Symbol = symbol };

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
            hint.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
        }

        /// <summary>Puts a setting on the page; the settings search finds every row added here.</summary>
        protected SettingRow Add(SettingRow row, Panel? into = null)
        {
            (into ?? Body).Children.Add(row);
            LastRow = row;
            row.Searchable = true;
            row.Section = _group;
            _searchRows.Add(row);
            return row;
        }

        // ---------------------------------------------------------------- settings search

        /// <summary>Every row put in with Add, in page order: the settings search's index of this
        /// page, built as the page is (no walk through the page's tree at each search).</summary>
        protected SettingRow? LastRow { get; private set; }
        private readonly List<SettingRow> _searchRows = new();

        /// <summary>
        /// This page's settings that can be seen right now: the rows put in with Add, minus those a
        /// choice hides (Custom resolution while a named size is chosen, a cheat's amount while it is
        /// off) and those no longer on the page.
        /// </summary>
        public IEnumerable<SettingRow> SearchRows() =>
            _searchRows.Where(row => row.Searchable && Shown(row));

        private bool Shown(DependencyObject? at)
        {
            for (; at != null && !ReferenceEquals(at, this); at = LogicalTreeHelper.GetParent(at))
                if (at is UIElement { Visibility: not Visibility.Visible } && at is not AdvancedRows)
                    return false; // a folded Advanced section still counts: the search opens it
            return at != null;
        }

        /// <summary>
        /// Scrolls a row of this page to the middle of the view, flashes it and puts the keyboard on
        /// its control, so Space or the arrow keys change it. False when it has no control to focus.
        /// </summary>
        public bool Reveal(SettingRow row)
        {
            for (DependencyObject? at = row; at != null; at = LogicalTreeHelper.GetParent(at))
                if (at is AdvancedSection section) section.IsOpen = true;
            UpdateLayout();
            if (!row.IsVisible) return false;
            // Focus first: focusing scrolls the control into view, and the centring below comes after it.
            var focused = SettingSearch.Descendants(row).OfType<System.Windows.Controls.Control>()
                .FirstOrDefault(c => c is { Focusable: true, IsTabStop: true, IsEnabled: true, IsVisible: true })?.Focus() == true;
            try
            {
                var top = row.TransformToAncestor(Scroller).Transform(new Point(0, 0)).Y + Scroller.VerticalOffset;
                Scroller.ScrollToVerticalOffset(Math.Max(0, top - Math.Max(0, Scroller.ViewportHeight - row.ActualHeight) / 2));
            }
            catch (InvalidOperationException)
            {
                row.BringIntoView(); // not under this page's scroller after all
            }
            row.Flash();
            return focused;
        }

        public static Button SmallButton(string text, SymbolRegular icon, RoutedEventHandler click, ControlAppearance look = ControlAppearance.Secondary)
        {
            var button = new Button
            {
                Content = text,
                Icon = Icon(icon),
                Appearance = look,
                Margin = new Thickness(6, 0, 0, 0),
                Padding = new Thickness(10, 5, 10, 5),
                VerticalAlignment = VerticalAlignment.Center,
            };
            button.Click += click;
            return button;
        }

        public static Button IconButton(SymbolRegular icon, string tooltip, RoutedEventHandler click)
        {
            var button = new Button
            {
                Icon = Icon(icon),
                ToolTip = tooltip,
                Appearance = ControlAppearance.Transparent,
                Padding = new Thickness(6),
                Margin = new Thickness(2, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
            };
            button.Click += click;
            return button;
        }

        public static StackPanel Row(params UIElement[] children)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            foreach (var child in children)
                panel.Children.Add(child);
            return panel;
        }

        // ---------------------------------------------------------------- bound rows

        /// <summary>A choice: segmented buttons for a few short options, a drop-down otherwise.
        /// The chosen option's note shows under the row.</summary>
        protected FrameworkElement ComboRow(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
            Func<string> get, Action<string> set, Panel? into = null, double width = 240, Action? changed = null,
            Func<Choice, bool>? enabled = null)
        {
            TextBlock note = null!;
            void ShowNote(int index)
            {
                var full = index >= 0 ? choices[index].Note : null;
                var brief = full == null ? null : Brief(full, 110);
                SetHint(note, brief);
                note.ToolTip = brief == full ? null : full;
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
                segmented.Select(Choices.IndexOf(choices, get()), notify: false);
                ShowNote(segmented.SelectedIndex);
                segmented.Changed += Picked;
                row.Tag = note;
                return segmented;
            }

            var box = new ComboBox { Width = width, VerticalAlignment = VerticalAlignment.Center };
            foreach (var choice in choices)
                box.Items.Add(new ComboBoxItem { Content = choice.Label, Tag = choice.Value });
            ComboSearch.Attach(box);
            var card = Add(Card(icon, title, hint, box, out note), into);
            card.Choices = options;
            box.SelectedIndex = Choices.IndexOf(choices, get());
            ShowNote(box.SelectedIndex);
            box.SelectionChanged += (_, _) => { if (box.SelectedIndex >= 0) Picked(box.SelectedIndex); };
            card.Tag = note;
            return box;
        }

        protected FrameworkElement SettingCombo(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
            string key, Panel? into = null, Action? changed = null) =>
            ComboRow(icon, title, hint, choices, () => S.Str(key), v => S.Set(key, v), into, changed: changed);

        protected FrameworkElement IniCombo(SymbolRegular icon, string title, string? hint, IReadOnlyList<Choice> choices,
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
        /// is not a number puts the current value back.</summary>
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
                ClearButtonEnabled = false,
                Margin = new Thickness(10, 0, 0, 0),
            };
            // The range, shown at once when a typed value was outside it (it is then kept at the
            // nearest end, e.g. 0 for an opacity typed as -0.5) instead of changing silently.
            var rangeTip = new System.Windows.Controls.ToolTip
            {
                Content = L.F($"Type a value from {min.ToString(format, culture)} to {max.ToString(format, culture)} and press Enter"),
                PlacementTarget = value,
                Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom,
            };
            value.ToolTip = rangeTip;
            var tipTimer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
            tipTimer.Tick += (_, _) => { tipTimer.Stop(); rangeTip.IsOpen = false; };
            value.SetResourceReference(System.Windows.Controls.Control.FontFamilyProperty, "MonoFont");
            void Show() => value.Text = slider.Value.ToString(format, culture);
            void Commit()
            {
                var match = System.Text.RegularExpressions.Regex.Match(value.Text, @"-?(\d+([.,]\d*)?|[.,]\d+)");
                if (!match.Success ||
                    !double.TryParse(match.Value.Replace(',', '.'), System.Globalization.NumberStyles.Float, culture, out var typed))
                {
                    Show(); // not a number: back to the current value
                    return;
                }
                if (typed < min || typed > max)
                {
                    rangeTip.IsOpen = true;
                    tipTimer.Stop();
                    tipTimer.Start();
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
            value.LostKeyboardFocus += (_, _) => Commit();
            value.GotKeyboardFocus += (_, _) => value.SelectAll();
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
                PlaceholderText = placeholder,
                PlaceholderEnabled = true,
                Width = width,
                VerticalAlignment = VerticalAlignment.Center,
            };
            box.TextChanged += (_, _) =>
            {
                set(box.Text);
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

        protected static string? PickFolder(string title, string current)
        {
            var dialog = new Microsoft.Win32.OpenFolderDialog { Title = title };
            try
            {
                if (System.IO.Directory.Exists(current))
                    dialog.InitialDirectory = System.IO.Path.GetFullPath(current);
                return dialog.ShowDialog() == true ? dialog.FolderName : null;
            }
            catch (ArgumentException)
            {
                // The shell refused the starting folder (a path it cannot parse): open without it.
                dialog = new Microsoft.Win32.OpenFolderDialog { Title = title };
                return dialog.ShowDialog() == true ? dialog.FolderName : null;
            }
        }
    }
}
