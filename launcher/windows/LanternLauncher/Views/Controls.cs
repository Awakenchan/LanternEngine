using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace BloodborneLauncher.Views
{
    /// <summary>How a button looks: the WPF-UI appearances the launcher used.</summary>
    public enum ControlAppearance { Secondary, Primary, Danger, Transparent }

    /// <summary>A button with a Fluent icon before its text, in one of the launcher's appearances.</summary>
    public class AppButton : Button
    {
        protected override Type StyleKeyOverride => typeof(Button);

        private readonly StackPanel _row = new() { Orientation = Orientation.Horizontal };
        private readonly TextBlock _text = new() { VerticalAlignment = VerticalAlignment.Center };
        private SymbolIcon? _icon;
        private ControlAppearance _appearance;

        public AppButton()
        {
            Content = _row;
            HorizontalContentAlignment = HorizontalAlignment.Center;
            VerticalContentAlignment = VerticalAlignment.Center;
        }

        public string Text
        {
            get => _text.Text ?? "";
            set { _text.Text = value; Arrange(); }
        }

        public SymbolIcon? Icon
        {
            get => _icon;
            set { _icon = value; Arrange(); }
        }

        public ControlAppearance Appearance
        {
            get => _appearance;
            set
            {
                _appearance = value;
                Classes.Set("accent", value == ControlAppearance.Primary);
                Classes.Set("danger", value == ControlAppearance.Danger);
                Classes.Set("subtle", value == ControlAppearance.Transparent);
            }
        }

        private void Arrange()
        {
            _row.Children.Clear();
            var hasText = (_text.Text ?? "").Length > 0;
            if (_icon != null)
            {
                _icon.FontSize = _icon.FontSize is 16 ? 15 : _icon.FontSize;
                _icon.Margin = new Thickness(0, 0, hasText ? 6 : 0, 0);
                _row.Children.Add(_icon);
            }
            if (hasText) _row.Children.Add(_text);
        }
    }

    public enum InfoBarSeverity { Informational, Success, Warning, Error }

    /// <summary>A coloured notice across the page: an icon, a bold title and a message.</summary>
    public sealed class InfoBar : Border
    {
        private readonly SymbolIcon _icon = new() { FontSize = 18, Margin = new Thickness(0, 0, 12, 0), VerticalAlignment = VerticalAlignment.Top };
        private readonly SelectableTextBlock _title = new() { FontWeight = FontWeight.SemiBold, FontSize = 13.5, TextWrapping = TextWrapping.Wrap };
        private readonly SelectableTextBlock _message = new() { FontSize = 13, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 0) };
        private readonly ContentControl _content = new() { Margin = new Thickness(0, 8, 0, 0), IsVisible = false };
        private InfoBarSeverity _severity;

        public object? Content
        {
            get => _content.Content;
            set { _content.Content = value; _content.IsVisible = value != null; }
        }

        public InfoBar()
        {
            CornerRadius = new CornerRadius(8);
            BorderThickness = new Thickness(1);
            Padding = new Thickness(14, 11, 14, 11);
            IsVisible = false;
            _title.Bind(TextBlock.ForegroundProperty, this.GetResourceObservable("TextFillColorPrimaryBrush"));
            _message.Bind(TextBlock.ForegroundProperty, this.GetResourceObservable("TextFillColorSecondaryBrush"));
            var text = new StackPanel();
            text.Children.Add(_title);
            text.Children.Add(_message);
            text.Children.Add(_content);
            var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
            grid.Children.Add(_icon);
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            Child = grid;
            Severity = InfoBarSeverity.Informational;
        }

        /// <summary>WPF-UI's switch: only ever false here (the bars are not closable).</summary>
        public bool IsClosable { get; set; }

        public bool IsOpen
        {
            get => IsVisible;
            set => IsVisible = value;
        }

        public string Title
        {
            get => _title.Text ?? "";
            set { _title.Text = value; _title.IsVisible = value.Length > 0; }
        }

        public string Message
        {
            get => _message.Text ?? "";
            set { _message.Text = value; _message.IsVisible = value.Length > 0; }
        }

        public InfoBarSeverity Severity
        {
            get => _severity;
            set
            {
                _severity = value;
                var (fallback, key, symbol) = value switch
                {
                    InfoBarSeverity.Error => (Color.FromRgb(0xFF, 0x6B, 0x6B), "DangerTextBrush", SymbolRegular.ErrorCircle24),
                    InfoBarSeverity.Warning => (Color.FromRgb(0xF2, 0xB8, 0x4B), "WarningTextBrush", SymbolRegular.Warning24),
                    InfoBarSeverity.Success => (Color.FromRgb(0x5F, 0xD3, 0x8A), "GoodTextBrush", SymbolRegular.CheckmarkCircle24),
                    _ => (Color.FromRgb(0x4C, 0x9D, 0xFF), "AccentTextFillColorPrimaryBrush", SymbolRegular.Info16),
                };
                // The theme's own colour for it, so a notice matches the page around it.
                var colour = LauncherTheme.Brush(key) is ISolidColorBrush { Color: var themed } ? themed : fallback;
                Background = new SolidColorBrush(Color.FromArgb(0x26, colour.R, colour.G, colour.B));
                BorderBrush = new SolidColorBrush(Color.FromArgb(0x55, colour.R, colour.G, colour.B));
                _icon.Symbol = symbol;
                _icon.Foreground = new SolidColorBrush(colour);
            }
        }
    }

    /// <summary>A plain card: WPF-UI's Card, a padded rounded box in the card colours.</summary>
    public sealed class Card : Border
    {
        public Card()
        {
            CornerRadius = new CornerRadius(SettingRow.Classic ? 8 : 16);
            BorderThickness = new Thickness(1);
            Padding = new Thickness(16);
            this.SetResourceReference(BackgroundProperty, SettingRow.Classic ? "CardBackgroundFillColorDefaultBrush" : "GlassPaneBrush");
            this.SetResourceReference(BorderBrushProperty, SettingRow.Classic ? "CardStrokeColorDefaultBrush" : "GlassPaneStrokeBrush");
        }

        public Control? Content
        {
            get => Child;
            set => Child = value;
        }
    }

    /// <summary>The WPF calls the pages are written with, in Avalonia's terms.</summary>
    public static class Ui
    {
        /// <summary>A property that follows a theme colour (WPF's SetResourceReference).</summary>
        public static void SetResourceReference(this Control control, AvaloniaProperty property, string key) =>
            control.Bind(property, control.GetResourceObservable(key));

        public static void SetTip(this Control control, object? tip)
        {
            ToolTip.SetTip(control, tip);
            ToolTip.SetShowDelay(control, 400);
        }

        public static Window? MainWindowOrNull =>
            (Application.Current?.ApplicationLifetime as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime)?.MainWindow;

        /// <summary>
        /// Waits for a task while the launcher keeps drawing and taking input (a nested dispatcher
        /// frame, as WPF's ShowDialog runs one), so a dialog can answer where it was asked.
        /// </summary>
        public static T Wait<T>(Task<T> task)
        {
            if (!task.IsCompleted)
            {
                var frame = new DispatcherFrame();
                task.ContinueWith(_ => Dispatcher.UIThread.Post(() => frame.Continue = false), TaskScheduler.Default);
                Dispatcher.UIThread.PushFrame(frame);
            }
            return task.GetAwaiter().GetResult();
        }

        private static IStorageProvider? Storage => (MainWindowOrNull ?? TopWindow())?.StorageProvider;

        private static Window? TopWindow() =>
            (Application.Current?.ApplicationLifetime as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime)
                ?.Windows.LastOrDefault(w => w.IsVisible);

        private static IStorageFolder? Folder(IStorageProvider storage, string? path)
        {
            try
            {
                return !string.IsNullOrEmpty(path) && Directory.Exists(path)
                    ? Wait(storage.TryGetFolderFromPathAsync(Path.GetFullPath(path)))
                    : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>
        /// A file type list as the WPF dialogs take it ("Pictures|*.png;*.jpg|All files|*.*").
        /// </summary>
        private static List<FilePickerFileType> Types(string? filter)
        {
            var types = new List<FilePickerFileType>();
            if (string.IsNullOrEmpty(filter)) return types;
            var parts = filter.Split('|');
            for (var i = 0; i + 1 < parts.Length; i += 2)
                types.Add(new FilePickerFileType(parts[i]) { Patterns = parts[i + 1].Split(';', StringSplitOptions.RemoveEmptyEntries) });
            return types;
        }

        public static string? PickFolder(string title, string? start = null)
        {
            if (Storage is not { CanPickFolder: true } storage) return null;
            var picked = Wait(storage.OpenFolderPickerAsync(new FolderPickerOpenOptions
            {
                Title = title,
                AllowMultiple = false,
                SuggestedStartLocation = Folder(storage, start),
            }));
            return picked.Count > 0 ? picked[0].TryGetLocalPath() : null;
        }

        public static string? OpenFile(string title, string? filter = null, string? start = null)
        {
            if (Storage is not { CanOpen: true } storage) return null;
            var picked = Wait(storage.OpenFilePickerAsync(new FilePickerOpenOptions
            {
                Title = title,
                AllowMultiple = false,
                FileTypeFilter = Types(filter),
                SuggestedStartLocation = Folder(storage, start),
            }));
            return picked.Count > 0 ? picked[0].TryGetLocalPath() : null;
        }

        public static string? SaveFile(string title, string fileName, string? filter = null)
        {
            if (Storage is not { CanSave: true } storage) return null;
            var types = Types(filter);
            var picked = Wait(storage.SaveFilePickerAsync(new FilePickerSaveOptions
            {
                Title = title,
                SuggestedFileName = fileName,
                FileTypeChoices = types,
                DefaultExtension = Path.GetExtension(fileName).TrimStart('.'),
                ShowOverwritePrompt = true,
            }));
            return picked?.TryGetLocalPath();
        }

        /// <summary>The clipboard's text, or empty (no window yet, or another program holds it).</summary>
        public static string ClipboardText()
        {
            try
            {
                return MainWindowOrNull?.Clipboard is { } clipboard ? Wait(clipboard.GetTextAsync()) ?? "" : "";
            }
            catch (Exception)
            {
                return "";
            }
        }

        public static void OpenLink(string url)
        {
            try
            {
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true });
            }
            catch (Exception) when (!OperatingSystem.IsWindows())
            {
                // Some desktops have no shell handler for .NET to call: xdg-open is the one they all have.
                try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("xdg-open", url) { UseShellExecute = false }); }
                catch (Exception) { }
            }
        }
    }
}
