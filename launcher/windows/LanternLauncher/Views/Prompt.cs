using Avalonia;
using Avalonia.Animation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// A small modal dialog in the launcher's style: a title, text, optional fields, and buttons.
    /// Over the launcher it is a layer of the window (the launcher dimmed behind it); without a
    /// launcher on screen (--play, before it opens) a small window of its own. Either way the call
    /// returns when it is answered, as WPF's ShowDialog did.
    /// </summary>
    public sealed class Prompt
    {
        public sealed record Field(string Label, string Value = "", string Placeholder = "", bool Multiline = false);

        private readonly List<TextBox> _boxes = new();
        private readonly Border _card;
        private readonly List<Button> _buttons = new();
        private readonly bool _hasChoices;

        /// <summary>Choose: the index of the button pressed; -1 when closed with Escape.</summary>
        private int _chosen = -1;
        private bool _result;
        private DispatcherFrame? _frame;
        private Window? _window;

        private Prompt(string title, string? message, IReadOnlyList<Field> fields, string ok, bool showCancel, string[]? choices = null)
        {
            var stack = new StackPanel();
            stack.Children.Add(LauncherPage.Text(title, "PageTitle"));
            if (!string.IsNullOrEmpty(message))
            {
                Control text;
                if (MarkdownText.Looks(message))
                    text = new Border { Child = MarkdownText.Build(message), Margin = new Thickness(0, 8, 0, 4), Padding = new Thickness(0, 0, 10, 0) };
                else
                {
                    var plain = new SelectableTextBlock { Text = message, FontSize = 13, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 4) };
                    plain.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
                    text = plain;
                }
                // A long message (an import's list of changes) scrolls instead of pushing the buttons
                // out of a small window.
                stack.Children.Add(new ScrollViewer
                {
                    Content = text,
                    MaxHeight = 300,
                    VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                });
            }
            foreach (var field in fields)
            {
                var label = LauncherPage.Text(field.Label, "StatLabel");
                label.Margin = new Thickness(0, 12, 0, 4);
                stack.Children.Add(label);
                var box = new TextBox
                {
                    Text = field.Value,
                    Watermark = field.Placeholder,
                    AcceptsReturn = field.Multiline,
                    TextWrapping = field.Multiline ? TextWrapping.Wrap : TextWrapping.NoWrap,
                    MinHeight = field.Multiline ? 70 : 0,
                    // A pasted code is one long line: it wraps and scrolls inside the box, which keeps
                    // the dialog's buttons on screen in a small window.
                    MaxHeight = field.Multiline ? 160 : double.PositiveInfinity,
                };
                ScrollViewer.SetVerticalScrollBarVisibility(box, field.Multiline ? ScrollBarVisibility.Auto : ScrollBarVisibility.Hidden);
                _boxes.Add(box);
                stack.Children.Add(box);
            }

            // Equal-width buttons along the bottom, the action on the right.
            var buttons = new UniformGrid { Rows = 1, Margin = new Thickness(0, 22, 0, 0), HorizontalAlignment = HorizontalAlignment.Right };
            _hasChoices = choices != null;
            if (choices != null)
            {
                // Several actions: equal widths, the last one the main action.
                buttons.HorizontalAlignment = HorizontalAlignment.Stretch;
                for (var i = 0; i < choices.Length; i++)
                {
                    var index = i;
                    var last = i == choices.Length - 1;
                    var button = new AppButton
                    {
                        Text = choices[i],
                        Margin = new Thickness(0, 0, last ? 0 : 8, 0),
                        HorizontalAlignment = HorizontalAlignment.Stretch,
                        Appearance = last ? ControlAppearance.Primary : ControlAppearance.Secondary,
                    };
                    button.Click += (_, _) => { _chosen = index; Close(true); };
                    buttons.Children.Add(button);
                    _buttons.Add(button);
                }
            }
            else if (showCancel)
            {
                var cancel = new AppButton { Text = L.T("Cancel"), Margin = new Thickness(0, 0, 8, 0), MinWidth = 110, HorizontalAlignment = HorizontalAlignment.Stretch };
                cancel.Click += (_, _) => Close(false);
                buttons.Children.Add(cancel);
                _buttons.Add(cancel);
            }
            if (choices == null)
            {
                var accept = new AppButton { Text = ok, Appearance = ControlAppearance.Primary, MinWidth = 110, HorizontalAlignment = HorizontalAlignment.Stretch };
                accept.Click += (_, _) => Close(true);
                buttons.Children.Add(accept);
                _buttons.Add(accept);
            }
            stack.Children.Add(buttons);

            _card = new Border
            {
                Width = 440,
                Padding = new Thickness(24, 20, 24, 20),
                CornerRadius = new CornerRadius(16),
                BorderThickness = new Thickness(1),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Child = stack,
                BoxShadow = new BoxShadows(new BoxShadow { Blur = 40, OffsetY = 8, Color = Color.FromArgb(0x8C, 0, 0, 0) }),
                Opacity = 0,
                Transitions = new Transitions { new DoubleTransition { Property = Visual.OpacityProperty, Duration = TimeSpan.FromMilliseconds(140) } },
            };
            _card.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
            _card.SetResourceReference(Border.BorderBrushProperty, "DialogStrokeBrush");
            _card.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
            _card.SetResourceReference(TextBlock.FontFamilyProperty, "UiFont");
            _card.KeyDown += (_, e) =>
            {
                if (e.Key == Key.Escape)
                {
                    Close(false);
                    e.Handled = true;
                }
                else if (e.Key == Key.Enter && !(e.Source is TextBox { AcceptsReturn: true }) && e.Source is not Button)
                {
                    // Enter is the main action (the last button), as IsDefault made it in WPF.
                    if (_hasChoices) _chosen = _buttons.Count - 1;
                    Close(true);
                    e.Handled = true;
                }
            };
        }

        /// <summary>--test-prompt &lt;png&gt;: where to save a picture of the first dialog over the launcher, then close.</summary>
        internal static string? SnapshotPath;

        private void Close(bool result)
        {
            _result = result;
            if (_frame != null) _frame.Continue = false;
            _window?.Close();
        }

        /// <summary>Shows the dialog and returns once it is answered.</summary>
        private bool Run()
        {
            _frame = new DispatcherFrame();
            var main = MainWindow.Instance;
            if (main is { IsVisible: true } && main.WindowState != WindowState.Minimized)
            {
                var scrim = new Border { Child = _card, Background = Brushes.Transparent };
                scrim.SetResourceReference(Border.BackgroundProperty, "DialogScrimBrush");
                // A click on the scrim does nothing (as a modal window over the launcher).
                scrim.PointerPressed += (_, e) => e.Handled = true;
                var focused = TopLevel.GetTopLevel(main)?.FocusManager?.GetFocusedElement();
                main.DialogLayer.Children.Add(scrim);
                Shown(main);
                Dispatcher.UIThread.PushFrame(_frame);
                main.DialogLayer.Children.Remove(scrim);
                if (focused is InputElement back && back.IsEffectivelyVisible) back.Focus();
            }
            else
            {
                _card.Margin = new Thickness(24);
                _window = new Window
                {
                    Title = "Lantern Engine",
                    SizeToContent = SizeToContent.WidthAndHeight,
                    WindowStartupLocation = WindowStartupLocation.CenterScreen,
                    SystemDecorations = SystemDecorations.None,
                    TransparencyLevelHint = new[] { WindowTransparencyLevel.Transparent },
                    Background = Brushes.Transparent,
                    CanResize = false,
                    Topmost = main == null,
                    Content = _card,
                };
                _window.SetResourceReference(Window.FontFamilyProperty, "UiFont");
                Core.AppIcon.Apply(_window, Core.Workspace.Current.Settings.Str("app_icon"));
                _card.PointerPressed += (_, e) =>
                {
                    if (e.Source is Border or StackPanel && e.GetCurrentPoint(_card).Properties.IsLeftButtonPressed)
                        _window.BeginMoveDrag(e);
                };
                _window.Closed += (_, _) => _frame.Continue = false;
                _window.Opened += (_, _) => Shown(null);
                if (main != null)
                    _window.Show(main);
                else
                    _window.Show();
                Dispatcher.UIThread.PushFrame(_frame);
                _window = null;
            }
            return _result;
        }

        private void Shown(MainWindow? over)
        {
            Dispatcher.UIThread.Post(() =>
            {
                _card.Opacity = 1;
                if (_boxes.Count > 0)
                {
                    _boxes[0].Focus();
                    _boxes[0].SelectAll();
                }
                else
                    _buttons.LastOrDefault()?.Focus();
                if (SnapshotPath != null && over != null) Snapshot(over);
            }, DispatcherPriority.Loaded);
        }

        private void Snapshot(MainWindow over)
        {
            DispatcherTimer.RunOnce(() =>
            {
                try
                {
                    var size = new PixelSize((int)over.Bounds.Width, (int)over.Bounds.Height);
                    using var shot = new RenderTargetBitmap(size);
                    shot.Render(over);
                    shot.Save(SnapshotPath!);
                }
                catch (Exception)
                {
                    // No picture: the test run still ends.
                }
                Close(false);
                over.Close();
            }, TimeSpan.FromMilliseconds(500));
        }

        /// <summary>The fields' values, or null if cancelled.</summary>
        public static string[]? Ask(string title, string? message, params Field[] fields)
        {
            var dialog = new Prompt(title, message, fields, L.T("OK"), showCancel: true);
            return dialog.Run() ? dialog._boxes.Select(b => b.Text ?? "").ToArray() : null;
        }

        public static bool Confirm(string title, string message, string ok) =>
            new Prompt(title, message, Array.Empty<Field>(), ok, showCancel: true).Run();

        /// <summary>One of several actions (the last is the main one): its index, or -1 if dismissed.</summary>
        public static int Choose(string title, string message, params string[] choices)
        {
            var dialog = new Prompt(title, message, Array.Empty<Field>(), "", showCancel: false, choices);
            dialog.Run();
            return dialog._chosen;
        }

        public static void Tell(string title, string message) =>
            new Prompt(title, message, Array.Empty<Field>(), L.T("OK"), showCancel: false).Run();
    }
}
