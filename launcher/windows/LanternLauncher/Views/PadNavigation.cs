// SPDX-License-Identifier: GPL-3.0-or-later
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
using Avalonia.VisualTree;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    public static class PadNavigation
    {
        public static readonly Choice[] Modes =
        {
            new(L.T("Automatic"), "", L.T("On on the Steam Deck and in Steam's Game Mode")),
            new(L.T("On"), "on"),
            new(L.T("Off"), "off"),
        };

        private static readonly string[] Repeating = { "dpup", "dpdown", "dpleft", "dpright", "leftstick-up", "leftstick-down", "leftstick-left", "leftstick-right" };
        private static DispatcherTimer? _timer;
        private static HashSet<string> _held = new();
        private static string? _repeat;
        private static DateTime _repeatAt;
        private static MainWindow? _window;

        public static bool Enabled(LauncherSettings s) => s.Str("pad_navigation") switch
        {
            "on" => true,
            "off" => false,
            _ => SteamDeck.Detected || SteamDeck.GameMode,
        };

        public static void Attach(MainWindow window)
        {
            _window = window;
            if (SteamDeck.GameMode)
                window.AddHandler(InputElement.PointerReleasedEvent, (_, e) =>
                {
                    if (e.Pointer.Type == PointerType.Touch && (e.Source as Visual)?.FindAncestorOfType<TextBox>(includeSelf: true) != null)
                        SteamDeck.OpenKeyboard();
                }, RoutingStrategies.Bubble, handledEventsToo: true);
            Apply();
        }

        public static void Apply()
        {
            if (_window == null) return;
            var on = Enabled(Workspace.Current.Settings);
            if (on && _timer == null)
            {
                _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(40) };
                _timer.Tick += (_, _) => Poll();
                _timer.Start();
            }
            else if (!on && _timer != null)
            {
                _timer.Stop();
                _timer = null;
                _held.Clear();
            }
        }

        private static void Poll()
        {
            var window = _window;
            if (window == null || !window.IsActive || !window.IsVisible || Workspace.Current.Game.Running || ControlsPage.Capturing || !SdlPad.Available)
            {
                _held.Clear();
                _repeat = null;
                return;
            }
            var held = SdlPad.Held(stick: true);
            var pressed = held.Where(b => !_held.Contains(b)).ToList();
            _held = held;
            foreach (var button in pressed)
            {
                Press(window, button);
                if (Repeating.Contains(button))
                {
                    _repeat = button;
                    _repeatAt = DateTime.UtcNow.AddMilliseconds(400);
                }
            }
            if (_repeat != null && !held.Contains(_repeat)) _repeat = null;
            if (_repeat != null && DateTime.UtcNow >= _repeatAt)
            {
                Press(window, _repeat);
                _repeatAt = DateTime.UtcNow.AddMilliseconds(110);
            }
        }

        private static void Press(MainWindow window, string button)
        {
            var focus = window.FocusManager?.GetFocusedElement() as Control;
            switch (button)
            {
                case "dpup" or "leftstick-up":
                    Move(window, focus, Key.Up, NavigationDirection.Previous);
                    break;
                case "dpdown" or "leftstick-down":
                    Move(window, focus, Key.Down, NavigationDirection.Next);
                    break;
                case "dpleft" or "leftstick-left":
                    Move(window, focus, Key.Left, NavigationDirection.Previous);
                    break;
                case "dpright" or "leftstick-right":
                    Move(window, focus, Key.Right, NavigationDirection.Next);
                    break;
                case "a":
                    Activate(window, focus);
                    break;
                case "b":
                    if (focus is ComboBox { IsDropDownOpen: true } open) open.IsDropDownOpen = false;
                    else SendKey(window, focus, Avalonia.Input.Key.Escape);
                    break;
                case "leftshoulder":
                    window.CycleTab(-1);
                    break;
                case "rightshoulder":
                    window.CycleTab(1);
                    break;
                case "start":
                    window.PlayFromPad();
                    break;
            }
        }

        private static void Move(MainWindow window, Control? focus, Key key, NavigationDirection direction)
        {
            if (focus == null || !focus.IsEffectivelyVisible ||
                (window.DialogLayer.Children.Count > 0 && !window.DialogLayer.IsVisualAncestorOf(focus)))
            {
                First(window);
                return;
            }
            var sideways = key is Avalonia.Input.Key.Left or Avalonia.Input.Key.Right;
            var adjusts = focus is Slider || focus is ComboBox { IsDropDownOpen: true } || (sideways && focus is ComboBox or TextBox) ||
                          focus is ListBoxItem or ComboBoxItem;
            if (adjusts && SendKey(window, focus, key)) return;
            var next = KeyboardNavigationHandler.GetNext(focus, direction);
            if (next is InputElement element)
            {
                element.Focus(NavigationMethod.Directional);
                (element as Control)?.BringIntoView();
            }
        }

        private static void First(MainWindow window)
        {
            Visual scope = window.DialogLayer.Children.Count > 0 ? window.DialogLayer : window;
            foreach (var element in scope.GetVisualDescendants().OfType<InputElement>())
                if (element is { Focusable: true, IsEffectivelyEnabled: true, IsEffectivelyVisible: true } and (Button or ToggleButton or ComboBox or Slider or TextBox))
                {
                    element.Focus(NavigationMethod.Directional);
                    (element as Control)?.BringIntoView();
                    return;
                }
        }

        private static void Activate(MainWindow window, Control? focus)
        {
            switch (focus)
            {
                case null:
                    First(window);
                    break;
                case ComboBox box:
                    if (box.IsDropDownOpen) SendKey(window, box, Avalonia.Input.Key.Enter);
                    else box.IsDropDownOpen = true;
                    break;
                case TextBox:
                    SteamDeck.OpenKeyboard();
                    break;
                case Slider:
                    break;
                default:
                    if (!SendKey(window, focus, Avalonia.Input.Key.Enter))
                    {
                        SendKey(window, focus, Avalonia.Input.Key.Space);
                        SendKeyUp(window, focus, Avalonia.Input.Key.Space);
                    }
                    break;
            }
        }

        private static bool SendKey(MainWindow window, Control? target, Key key)
        {
            var at = (Interactive?)target ?? window;
            var args = new KeyEventArgs { RoutedEvent = InputElement.KeyDownEvent, Key = key, Source = at };
            at.RaiseEvent(args);
            return args.Handled;
        }

        private static void SendKeyUp(MainWindow window, Control? target, Key key)
        {
            var at = (Interactive?)target ?? window;
            at.RaiseEvent(new KeyEventArgs { RoutedEvent = InputElement.KeyUpEvent, Key = key, Source = at });
        }
    }
}
