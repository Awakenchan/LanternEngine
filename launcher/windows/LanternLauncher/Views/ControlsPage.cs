using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The engine's PC controls: mouse camera and menu pointer, profiles, and the 29 actions, each
    /// bound to one key and one mouse input (lantern.ini bind.*, the same records the in-game
    /// System › Key Bindings menu edits). Clicking an action listens for the next key, mouse button
    /// or wheel step: a key replaces the key, a mouse input the mouse input. × removes one,
    /// right-click unbinds the action. Inputs used twice, and the port's Insert key, are flagged.
    /// </summary>
    public sealed class ControlsPage : LauncherPage
    {
        private readonly StackPanel _table = new();
        private readonly InfoBar _conflicts;
        private readonly TextBox _search;
        private readonly TextBlock _padStatus;

        // Capture overlay
        private readonly Grid _overlay;
        private readonly TextBlock _captureTitle, _captureFeedback, _captureHelp, _captureBoxText;
        private readonly WrapPanel _padChoices;
        private readonly DispatcherTimer _padTimer;
        private string? _padInput;
        public static bool Capturing { get; private set; }
        private ControlDef? _padControl;
        private HashSet<string> _padHeld = new();
        private string? _padState;
        private DateTime? _padSeen;
        private ControlDef? _target;
        /// <summary>The capture adds an input (a second key, a combination) instead of replacing one.</summary>
        private bool _addMode;
        /// <summary>A modifier pressed during the capture: the modifier of the next key or click, or
        /// bound alone when it is let go first.</summary>
        private string? _pendingModifier;

        // Profiles
        private readonly ComboBox _profileBox;
        private readonly TextBlock _profileHint;
        private readonly AppButton _deleteProfile;
        private List<ControlProfiles.Profile> _profiles = new();
        private bool _fillingProfiles;

        public ControlsPage() : base(L.T("Controls"),
            L.T("Keyboard and mouse for every action, the mouse camera, and shareable profiles. Gamepads work as on PS4."))
        {
            // ==================== PC controls & mouse ====================
            Group(L.T("Keyboard & mouse"));
            var pc = ToggleRow(SymbolRegular.Keyboard24, L.T("PC controls"), L.T("The keyboard and mouse layout below; off keeps the gamepad-style keys"),
                () => Ini.Get("pc_controls", "1") != "0", v => Ini.Set("pc_controls", v ? "1" : "0"));
            ToggleRow(SymbolRegular.ArrowUp24, L.T("Jump on its own key"),
                L.T("The Jump key (left stick click, L3; Left Ctrl by default) jumps at once, sprinting or not, and the dodge button always dodges. Off: a second press of the dodge button while sprinting jumps, as on PS4. Needs the game's own action script; applied at the next start"),
                () => Ini.Get("jump_key", "0") == "1", v => Ini.Set("jump_key", v ? "1" : "0"));
            var mouse = new StackPanel();
            ToggleRow(SymbolRegular.CursorClick24, L.T("Mouse camera"), L.T("The mouse turns the game's own camera by raw counts, at any frame rate"),
                () => Ini.Get("mouse_camera", "1") != "0", v => Ini.Set("mouse_camera", v ? "1" : "0"), mouse);
            ToggleRow(SymbolRegular.CursorHover24, L.T("Mouse in menus"), L.T("The mouse becomes a pointer in the game's menus: hover and click items"),
                () => Ini.Get("mouse_menu", "1") != "0", v => Ini.Set("mouse_menu", v ? "1" : "0"), mouse);
            ToggleRow(SymbolRegular.Run24, L.T("Move and look in menus"),
                L.T("With a menu open in the world, the movement and look keys stay your character's and the arrow keys move through the menu. The mouse turns the camera while Mouse in menus is off. Whether the character can walk then is up to the game. Applied at the next start"),
                () => Ini.Get("menu_move", "0") == "1", v => Ini.Set("menu_move", v ? "1" : "0"), mouse);
            SliderRow(SymbolRegular.Gauge24, L.T("Mouse sensitivity"), L.T("5 is the default; below 0 for high-DPI mice (each step 20% slower)"), -10, 20, 1,
                () => ParseDouble(Ini.Get("mouse_sensitivity", "5"), 5), v => Ini.Set("mouse_sensitivity", ((int)v).ToString()), "0", mouse);
            IniToggle(SymbolRegular.ArrowSwap24, L.T("Invert horizontal"), null, "mouse_invert_x", mouse);
            IniToggle(SymbolRegular.ArrowSortDown24, L.T("Invert vertical"), null, "mouse_invert_y", mouse);
            IniToggle(SymbolRegular.ArrowRotateClockwise24, L.T("Camera auto-rotation"), L.T("Let the camera turn by itself with movement, as with a gamepad"),
                "mouse_auto_rotation", mouse);
            ComboRow(SymbolRegular.KeyboardShift24, L.T("Button prompts"),
                L.T("The button icons in the game's menus and prompts. Automatic shows your keys while the keyboard or mouse is in use, and the controller's buttons with a controller; also in-game under System › PC Controls"),
                Choices.ButtonPrompts, () => Ini.Get("button_prompts", "auto"), v => Ini.Set("button_prompts", v), mouse);
            Body.Children.Add(mouse);
            ToggleRow(SymbolRegular.CursorHoverOff24, L.T("Hide mouse cursor"), L.T("Hidden 2 seconds after the mouse stops moving; moving it brings it back"),
                () => Ini.Get("hide_cursor", "1") != "0", v => Ini.Set("hide_cursor", v ? "1" : "0"));
            void Enable() => mouse.IsEnabled = pc.IsChecked == true;
            pc.Click += (_, _) => { Enable(); Rebuild(); };
            Enable();

            // ==================== Profiles ====================
            Group(L.T("Profiles"));
            _profileBox = new ComboBox { Width = 260, VerticalAlignment = VerticalAlignment.Center };
            ComboSearch.Attach(_profileBox);
            _profileBox.SelectionChanged += (_, _) => ProfileChosen();
            _deleteProfile = IconButton(SymbolRegular.Delete24, L.T("Delete this profile"), (_, _) => DeleteProfile());
            Add(Card(SymbolRegular.PeopleCommunity24, L.T("Profile"), "", Row(_profileBox, _deleteProfile), out _profileHint));
            Add(Card(SymbolRegular.Share24, L.T("Save and share"), L.T("Profiles are small JSON files. Copy one and paste it on any paste site ") +
                L.T("(Pastebin, GitHub Gist, Rentry, hastebin…) to share it; others import it from the link."), null, out _));
            var share = new WrapPanel { Margin = new Thickness(46, -4, 0, 12) };
            share.Children.Add(SmallButton(L.T("Save as…"), SymbolRegular.Save24, (_, _) => SaveProfile()));
            share.Children.Add(SmallButton(L.T("Copy JSON"), SymbolRegular.Copy24, (_, _) => CopyProfile()));
            share.Children.Add(SmallButton(L.T("Export file…"), SymbolRegular.ArrowExportUp24, (_, _) => ExportProfile()));
            share.Children.Add(SmallButton(L.T("Import file…"), SymbolRegular.DocumentArrowDown20, (_, _) => ImportFile()));
            share.Children.Add(SmallButton(L.T("Import from link…"), SymbolRegular.Link24, async (_, _) => await ImportLinkAsync()));
            foreach (var b in share.Children) b.Margin = new Thickness(0, 4, 6, 0);
            Body.Children.Add(share);

            Group(L.T("Gamepad"));
            Add(Card(SymbolRegular.XboxController24, L.T("Controller"), "", null, out _padStatus));
            ComboRow(SymbolRegular.Games24, L.T("Controller icons"),
                L.T("The controller's buttons in the game's menus and prompts. Automatic shows PlayStation buttons with a PlayStation controller ") +
                L.T("and Xbox buttons (A, B, X, Y, LB, RT…) with any other; rebound buttons show as bound. Also in-game under System › PC Controls"),
                Choices.PadIcons, () => Ini.Get("pad_icons", "auto"), v => Ini.Set("pad_icons", v));
            IniToggle(SymbolRegular.ArrowSwap24, L.T("Swap sticks"), L.T("Move with the right stick and look with the left"), "pad_swap_sticks", changed: () => Rebuild());
            IniToggle(SymbolRegular.ArrowBidirectionalLeftRight24, L.T("Invert camera left / right"), L.T("The camera stick's horizontal direction"), "pad_invert_look_x");
            IniToggle(SymbolRegular.ArrowBidirectionalUpDown24, L.T("Invert camera up / down"), L.T("The camera stick's vertical direction"), "pad_invert_look_y");
            SliderRow(SymbolRegular.CircleSmall24, L.T("Stick dead zone"),
                L.T("Ignores small stick movement, for sticks that drift. 0 = off (the game's own)"), 0, 30, 1,
                () => double.TryParse(Ini.Get("pad_deadzone", "0"), out var v) ? v : 0,
                v => Ini.Set("pad_deadzone", ((int)Math.Round(v)).ToString()), format: "0'%'");

            ComboRow(SymbolRegular.Games24, L.T("Left trackpad"),
                L.T("Steam Deck and Steam Controller. Its click and its four directions can also be bound to any action below"),
                TrackModes(L.T("Automatic (touch surface)")), () => Ini.Get("pad_trackpad_left", "auto"), v => Ini.Set("pad_trackpad_left", v));
            ComboRow(SymbolRegular.Games24, L.T("Right trackpad"),
                L.T("Camera turns while your finger moves, like a mouse"),
                TrackModes(L.T("Automatic (camera)")), () => Ini.Get("pad_trackpad_right", "auto"), v => Ini.Set("pad_trackpad_right", v));
            SliderRow(SymbolRegular.TopSpeed24, L.T("Trackpad camera speed"), null, 10, 400, 5,
                () => double.TryParse(Ini.Get("pad_trackpad_sensitivity", "100"), out var v) ? v : 100,
                v => Ini.Set("pad_trackpad_sensitivity", ((int)Math.Round(v)).ToString()), format: "0'%'");
            IniToggle(SymbolRegular.ArrowSync24, L.T("Gyro camera"), L.T("Turn the camera by moving the controller (Steam, DualSense, DualShock 4, Switch Pro)"), "pad_gyro_camera");
            SliderRow(SymbolRegular.TopSpeed24, L.T("Gyro camera speed"), null, 10, 400, 5,
                () => double.TryParse(Ini.Get("pad_gyro_sensitivity", "100"), out var v) ? v : 100,
                v => Ini.Set("pad_gyro_sensitivity", ((int)Math.Round(v)).ToString()), format: "0'%'");

            // ==================== Bindings ====================
            var header = new Grid { Margin = new Thickness(2, 24, 0, 8), ColumnDefinitions = new ColumnDefinitions("*,Auto") };
            var title = Text(L.T("Actions"), "GroupHeader");
            title.Margin = new Thickness(0);
            title.VerticalAlignment = VerticalAlignment.Center;
            header.Children.Add(title);
            _search = new TextBox
            {
                Watermark = L.T("Filter actions or keys"),
                Width = 210,
                InnerLeftContent = new SymbolIcon(SymbolRegular.Search24) { FontSize = 13, Margin = new Thickness(8, 0, 0, 0), Opacity = 0.75 },
            };
            _search.TextChanged += (_, _) => Rebuild();
            var tools = Row(_search, SmallButton(L.T("Reset all"), SymbolRegular.ArrowUndo24, (_, _) =>
            {
                var chosen = S.Str("controls_profile");
                var profile = ControlProfiles.BuiltIns.FirstOrDefault(p => p.Name == chosen)
                              ?? ControlProfiles.BuiltIns.First(p => p.Name == "EldenRingStyle");
                ControlProfiles.Apply(S, Ini, profile);
                PadMap.ResetAll(Ini);
                Ws.Log(L.F($"Key bindings reset to {profile.Name} as built in, controller bindings to the defaults."), LogLevel.Step);
                Changed();
            }));
            Grid.SetColumn(tools, 1);
            header.Children.Add(tools);
            Body.Children.Add(header);

            var help = Text(L.T("Click an action and press a key, click a mouse button or turn the wheel. Each action takes one key and one ") +
                            L.T("mouse input. × removes one, right-click unbinds the action. In the Controller column, pick the button for an ") +
                            L.T("action: one already in use swaps over."), "Faint");
            help.Margin = new Thickness(2, 0, 0, 8);
            Body.Children.Add(help);

            _conflicts = new InfoBar { IsClosable = false, IsOpen = false, Severity = InfoBarSeverity.Warning, Margin = new Thickness(0, 0, 0, 8) };
            Body.Children.Add(_conflicts);
            Body.Children.Add(new Card { Content = _table, Padding = new Thickness(12, 8, 12, 10) });

            // ==================== Capture overlay ====================
            _captureTitle = Text("", "PageTitle");
            _captureHelp = Text("", "Muted");
            _captureHelp.Margin = new Thickness(0, 4, 0, 12);
            _captureFeedback = Text("", "Muted");
            _captureFeedback.SetResourceReference(TextBlock.ForegroundProperty, "WarningTextBrush");

            _captureBoxText = Text("", "Faint");
            _captureBoxText.HorizontalAlignment = HorizontalAlignment.Center;
            _captureBoxText.VerticalAlignment = VerticalAlignment.Center;
            _captureBoxText.TextAlignment = TextAlignment.Center;
            var capturePad = new Border
            {
                Height = 110,
                CornerRadius = new CornerRadius(6),
                BorderThickness = new Thickness(1),
                Child = _captureBoxText,
                Cursor = new Cursor(StandardCursorType.Hand),
                Margin = new Thickness(0, 0, 0, 10),
            };
            capturePad.SetResourceReference(Border.BackgroundProperty, "PanelDeepBrush");
            capturePad.SetResourceReference(Border.BorderBrushProperty, "AccentTextFillColorPrimaryBrush");
            capturePad.PointerPressed += (_, e) =>
            {
                e.Handled = true;
                var button = e.GetCurrentPoint(capturePad).Properties.PointerUpdateKind.GetMouseButton();
                if (_padInput == null && InputMap.MouseToken(button) is { } name) Commit(WithModifiers(name, e.KeyModifiers));
            };
            capturePad.PointerWheelChanged += (_, e) =>
            {
                e.Handled = true;
                if (_padInput == null && e.Delta.Y != 0) Commit(e.Delta.Y > 0 ? "WheelUp" : "WheelDown");
            };

            // Controller capture: the button pressed on the pad, or one picked from this list.
            _padChoices = new WrapPanel { Margin = new Thickness(0, 0, 0, 6) };
            foreach (var button in PadMap.Buttons.Where(b => b.Sdl.Length > 0))
            {
                var sdl = button.Sdl;
                var chip = new AppButton { Text = button.Label, Padding = new Thickness(8, 3, 8, 3), Margin = new Thickness(0, 0, 4, 4), FontSize = 12 };
                chip.Click += (_, _) => CommitPad(sdl);
                _padChoices.Children.Add(chip);
            }
            _padTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
            _padTimer.Tick += (_, _) => PollPad();

            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 8, 0, 0) };
            buttons.Children.Add(SmallButton(L.T("Unbind"), SymbolRegular.Dismiss24, (_, _) => { if (_padInput != null) CommitPad(""); else Commit(null); }));
            buttons.Children.Add(SmallButton(L.T("Cancel"), SymbolRegular.ArrowExit20, (_, _) => EndCapture()));

            var dialog = new StackPanel();
            dialog.Children.Add(_captureTitle);
            dialog.Children.Add(_captureHelp);
            dialog.Children.Add(capturePad);
            dialog.Children.Add(_padChoices);
            dialog.Children.Add(_captureFeedback);
            dialog.Children.Add(buttons);
            var dialogCard = new Border
            {
                Width = 500,
                Padding = new Thickness(22, 18, 22, 16),
                CornerRadius = new CornerRadius(8),
                BorderThickness = new Thickness(1),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Child = dialog,
            };
            // Opaque: the glass card colour would let the page show through the dialog.
            dialogCard.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
            dialogCard.SetResourceReference(Border.BorderBrushProperty, "DialogStrokeBrush");

            _overlay = new Grid { IsVisible = false, Background = new SolidColorBrush(Color.FromArgb(0xB0, 0x10, 0x10, 0x12)), Focusable = true };
            _overlay.Children.Add(dialogCard);
            _overlay.PointerPressed += (_, e) => { if (ReferenceEquals(e.Source, _overlay)) EndCapture(); };

            var root = new Grid();
            Content = null;
            root.Children.Add(Scroller);
            root.Children.Add(_overlay);
            Content = root;
            AddHandler(KeyDownEvent, Page_KeyDown, RoutingStrategies.Tunnel);
            AddHandler(KeyUpEvent, Page_KeyUp, RoutingStrategies.Tunnel);

            var status = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1.5) };
            status.Tick += (_, _) => { if (IsShown) UpdatePadStatus(); };
            // A receiver stays connected while its pad is off: only input changing shows the pad is on.
            var activity = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            activity.Tick += (_, _) =>
            {
                if (!IsShown || Ws.Game.Running || _target != null || _padInput != null || !SdlPad.Available) return;
                var state = SdlPad.State();
                if (_padState != null && state != _padState)
                {
                    var first = _padSeen == null || DateTime.Now - _padSeen > TimeSpan.FromMinutes(2);
                    _padSeen = DateTime.Now;
                    if (first) UpdatePadStatus();
                }
                _padState = state;
            };
            AttachedToVisualTree += (_, _) => { status.Start(); activity.Start(); };
            DetachedFromVisualTree += (_, _) => { status.Stop(); activity.Stop(); };
            Ws.PortChanged += () => { Rebuild(); FillProfiles(); };
            Rebuild();
            FillProfiles();
        }

        public override void OnShown()
        {
            UpdatePadStatus();
            Rebuild();
            FillProfiles();
        }


        private static Choice[] TrackModes(string auto) => new Choice[]
        {
            new(auto, "auto"), new(L.T("Camera"), "camera"), new(L.T("Movement"), "move"),
            new(L.T("PS4 touch surface"), "touch"), new(L.T("Off"), "off"),
        };

        private void UpdatePadStatus()
        {
            // The engine's SDL sees pads exactly as the game will (while the game runs it owns them,
            // so the launcher's own detection stands in).
            var devices = Ws.Game.Running ? null : SdlPad.Devices();
            if (devices != null)
            {
                if (devices.Count == 0)
                {
                    SetHint(_padStatus, L.T("No controller connected. Plug one in or pair it over Bluetooth: it shows here right away. ") +
                        L.T("DualShock 4, DualSense, Xbox, Switch Pro and many generic pads work without extra tools."));
                    return;
                }
                // A pad behind a 2.4 GHz receiver can show as two: one line per name.
                devices = devices.GroupBy(d => (d.Name, d.Supported)).Select(g => g.First()).ToList();
                var lines = devices.Select(d => d.Supported
                    ? $"● {d.Name}"
                    : L.F($"● {d.Name}: connected, but the game has no button layout for it. Switch the pad to its XInput / PC mode if it has one, or use Steam Input"));
                var responding = _padSeen != null && DateTime.Now - _padSeen < TimeSpan.FromMinutes(2);
                SetHint(_padStatus, string.Join("\n", lines) + (devices.Any(d => d.Supported)
                    ? responding ? L.T("\nResponding: ready to play. The game uses the pad you press.")
                    : L.T("\nDetected. Wireless pads show here while only the receiver is plugged in: press any button to check the pad is on.")
                    : ""));
                return;
            }
            var pads = Gamepads.Detect();
            SetHint(_padStatus, pads.Count == 0
                ? L.T("No controller connected. Plug one in or pair it over Bluetooth: it shows here right away.")
                : "● " + string.Join("   ● ", pads.Select(p => p.Name + (p.Bluetooth ? L.T(" (Bluetooth)") : ""))) +
                  (Ws.Game.Running ? L.T("\nIn use by the game.") : L.T("\nConnected.")));
        }

        // ---------------------------------------------------------------- the table

        private Dictionary<string, List<ControlDef>> Usage()
        {
            var usage = new Dictionary<string, List<ControlDef>>(StringComparer.OrdinalIgnoreCase);
            foreach (var control in InputMap.Controls)
                foreach (var name in Bindings.Get(Ini, control))
                {
                    if (!usage.TryGetValue(name, out var list)) usage[name] = list = new List<ControlDef>();
                    list.Add(control);
                }
            return usage;
        }

        private void Rebuild()
        {
            _table.Children.Clear();
            // Keyboard and mouse follow the PC controls switch; the controller column always works.
            var pcControls = Ini.Get("pc_controls", "1") != "0";
            var usage = Usage();
            var filter = (_search.Text ?? "").Trim();

            _table.Children.Add(TableRow(Header(L.T("Action")), Header(L.T("Keyboard & mouse")), Header(L.T("Controller")), new Control(), header: true));

            foreach (var group in InputMap.Controls.GroupBy(c => c.Group))
            {
                var rows = group.Where(c => Matches(c, filter)).ToList();
                if (rows.Count == 0) continue;

                var groupLabel = Text(group.Key.ToUpperInvariant(), "StatLabel");
                groupLabel.FontWeight = FontWeight.SemiBold;
                groupLabel.Margin = new Thickness(4, 14, 0, 4);
                _table.Children.Add(groupLabel);

                foreach (var control in rows)
                {
                    var name = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                    name.Children.Add(new TextBlock { Text = control.Label, FontSize = 13.5 });
                    if (control.Hint.Length > 0) name.Children.Add(Text(control.Hint, "Faint"));

                    var c = control;
                    var reset = new AppButton
                    {
                        Icon = Icon(SymbolRegular.ArrowUndo24),
                        Appearance = ControlAppearance.Transparent,
                        Padding = new Thickness(6),
                        VerticalAlignment = VerticalAlignment.Center,
                        IsVisible = !(Bindings.IsDefault(Ini, control) && (PadMap.InputFor(control) is not { } padInput || PadMap.IsDefault(Ini, padInput))),
                    };
                    reset.SetTip(L.F($"Back to the default ({Show(control.Default)})"));
                    reset.Click += (_, _) =>
                    {
                        if (PadMap.InputFor(c) is { } input) PadMap.Reset(Ini, input);
                        Store(c, InputMap.Split(c.Default));
                    };

                    var keys = Cell(control, usage);
                    keys.IsEnabled = pcControls;
                    var actions = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
                    if (!NoToggle.Contains(c.Name))
                    {
                        var toggles = Toggles();
                        var on = toggles.Contains(c.Name);
                        var mode = new Button
                        {
                            Content = new TextBlock { Text = on ? L.T("Toggle") : L.T("Hold"), FontSize = 11.5 },
                            Padding = new Thickness(8, 3),
                            MinWidth = 58,
                            Opacity = on ? 1 : 0.6,
                            IsEnabled = pcControls,
                            VerticalAlignment = VerticalAlignment.Center,
                        };
                        mode.SetTip(on
                            ? L.T("Toggle: one press turns this on and the next press turns it off. Click for Hold. Applied at the next start")
                            : L.T("Hold: active while the key is held. Click for Toggle, where one press turns it on and the next turns it off. Applied at the next start"));
                        mode.Click += (_, _) =>
                        {
                            var set = Toggles();
                            if (!set.Remove(c.Name)) set.Add(c.Name);
                            Ini.Set("toggle_keys", string.Join(",", set));
                            Ini.Set("sprint_toggle", "0");
                            Changed();
                        };
                        actions.Children.Add(mode);
                    }
                    actions.Children.Add(reset);
                    _table.Children.Add(TableRow(name, keys, PadCell(control), actions));
                }
            }

            var shared = usage.Where(u => u.Value.Count > 1).Select(u => $"{InputMap.Display(u.Key)} ({string.Join(", ", u.Value.Select(c => c.Label))})").ToList();
            var reserved = usage.Keys.Where(InputMap.Reserved.ContainsKey).Select(k => InputMap.Reserved[k]).ToList();
            var lines = new List<string>();
            if (shared.Count > 0) lines.Add(L.T("Used by more than one action: ") + string.Join("; ", shared));
            if (reserved.Count > 0) lines.Add(L.T("Also used by the port: ") + string.Join("; ", reserved));
            _conflicts.IsOpen = lines.Count > 0;
            _conflicts.Title = shared.Count > 0 ? L.T("Shared inputs") : L.T("Reserved keys");
            _conflicts.Message = string.Join("\n", lines);
        }

        private static readonly HashSet<string> NoToggle = new() { "menu", "confirm", "back", "dodge", "backstep", "l3" };

        private List<string> Toggles()
        {
            var list = Ini.Get("toggle_keys", "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Distinct().ToList();
            if (Ini.Get("sprint_toggle", "0") == "1" && !list.Contains("sprint")) list.Add("sprint");
            return list;
        }

        private static string Show(string value) =>
            InputMap.Split(value) is { Count: > 0 } names ? string.Join(", ", names.Select(InputMap.Display)) : L.T("not bound");

        private bool Matches(ControlDef control, string filter)
        {
            if (filter.Length == 0) return true;
            bool Has(string s) => s.Contains(filter, StringComparison.OrdinalIgnoreCase);
            return Has(control.Label) || Has(control.Hint) || (control.Pad != null && Has(control.Pad)) ||
                   Bindings.Get(Ini, control).Any(n => Has(n) || Has(InputMap.Display(n)));
        }

        private static TextBlock Header(string text)
        {
            var block = Text(text, "StatLabel");
            block.FontWeight = FontWeight.SemiBold;
            return block;
        }

        private static Grid TableRow(Control name, Control keys, Control pad, Control action, bool header = false)
        {
            var grid = new Grid
            {
                Margin = new Thickness(0, header ? 4 : 1, 0, header ? 2 : 1),
                MinHeight = header ? 0 : 40,
                ColumnDefinitions = new ColumnDefinitions("240,1.6*,*,112"),
            };
            name.Margin = new Thickness(4, 0, 8, 0);
            Grid.SetColumn(keys, 1);
            Grid.SetColumn(pad, 2);
            Grid.SetColumn(action, 3);
            keys.Margin = new Thickness(header ? 10 : 4, 0, 4, 0);
            pad.Margin = new Thickness(10, 0, 0, 0);
            grid.Children.Add(name);
            grid.Children.Add(keys);
            grid.Children.Add(pad);
            grid.Children.Add(action);
            return grid;
        }

        /// <summary>One action's inputs: chips, and the whole cell as a capture button.</summary>
        private Control Cell(ControlDef control, Dictionary<string, List<ControlDef>> usage)
        {
            var names = Bindings.Get(Ini, control);
            var chips = new WrapPanel { VerticalAlignment = VerticalAlignment.Center };

            foreach (var name in names)
            {
                var others = usage.TryGetValue(name, out var list) ? list.Where(c => c != control).ToList() : new List<ControlDef>();
                var reserved = InputMap.Reserved.TryGetValue(name, out var why) ? why : null;

                var label = new TextBlock { Text = InputMap.Display(name), FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
                label.SetResourceReference(TextBlock.FontFamilyProperty, "MonoFont");
                var icon = new SymbolIcon(InputMap.IsMouse(name) ? SymbolRegular.CursorClick24 : SymbolRegular.Keyboard24)
                {
                    FontSize = 11,
                    Margin = new Thickness(0, 0, 5, 0),
                };
                icon.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
                var remove = new Button
                {
                    Content = new SymbolIcon(SymbolRegular.Dismiss16) { FontSize = 10 },
                    Margin = new Thickness(6, 0, -3, 0),
                    Padding = new Thickness(2),
                    MinWidth = 0,
                    MinHeight = 0,
                };
                remove.Classes.Add("Bare");
                remove.SetTip(L.T("Remove"));
                var n = name;
                remove.Click += (_, e) =>
                {
                    e.Handled = true;
                    Store(control, Bindings.Get(Ini, control).Where(b => !b.Equals(n, StringComparison.OrdinalIgnoreCase)).ToList());
                };
                var chip = new Border { Child = Row(icon, label, remove) };
                chip.Classes.Add("Chip");
                if (others.Count > 0 || reserved != null)
                {
                    chip.SetResourceReference(Border.BorderBrushProperty, "WarningTextBrush");
                    label.SetResourceReference(TextBlock.ForegroundProperty, "WarningTextBrush");
                    chip.SetTip(string.Join("\n", new[]
                    {
                        others.Count > 0 ? L.T("Also bound to: ") + string.Join(", ", others.Select(c => c.Label)) : null,
                        reserved,
                    }.Where(t => t != null)));
                }
                chips.Children.Add(chip);
            }

            // Another input for the action: a second key, a combination (Shift+E), a mouse button.
            if (names.Count < Bindings.MaxInputs)
            {
                var add = new Button
                {
                    Content = new SymbolIcon(SymbolRegular.Add24) { FontSize = 11 },
                    Margin = new Thickness(2, 0, 4, 0),
                    Padding = new Thickness(4, 2, 4, 2),
                    MinWidth = 0,
                    MinHeight = 0,
                    VerticalAlignment = VerticalAlignment.Center,
                };
                add.Classes.Add("Bare");
                add.SetTip(L.T("Add another key, mouse button or combination (hold Ctrl, Shift or Alt)"));
                add.Click += (_, e) =>
                {
                    e.Handled = true;
                    BeginCapture(control, add: true);
                };
                chips.Children.Add(add);
            }

            if (names.Count == 0)
            {
                var none = Text(L.T("Not bound"), "Faint");
                none.FontStyle = FontStyle.Italic;
                none.Margin = new Thickness(2, 0, 8, 0);
                none.VerticalAlignment = VerticalAlignment.Center;
                chips.Children.Add(none);
            }

            if (!Bindings.IsDefault(Ini, control))
            {
                var dot = new Ellipse { Width = 5, Height = 5, Margin = new Thickness(6, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
                dot.SetTip(L.T("Changed from the default"));
                dot.SetResourceReference(Shape.FillProperty, "AccentTextFillColorPrimaryBrush");
                chips.Children.Add(dot);
            }

            var cell = new Button
            {
                Content = chips,
                Padding = new Thickness(6, 2, 6, 2),
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
            };
            cell.SetTip(L.F($"Click to set a key or mouse input for {control.Label}. Right-click to unbind."));
            cell.Classes.Add("Bare");
            cell.Click += (_, _) => BeginCapture(control);
            cell.AddHandler(PointerReleasedEvent, (_, e) =>
            {
                if (e.InitialPressMouseButton != MouseButton.Right) return;
                e.Handled = true;
                Store(control, new List<string>());
            }, RoutingStrategies.Bubble, handledEventsToo: true);
            return cell;
        }

        // ---------------------------------------------------------------- storing

        /// <summary>The controller column: the button that does this action, changeable when it is a PS4 button.</summary>
        private Control PadCell(ControlDef control)
        {
            if (PadMap.InputFor(control) is not { } input)
            {
                var label = control.Pad ?? "—";
                if (Ini.Flag("pad_swap_sticks"))
                    label = label == "Left stick" ? L.T("Right stick") : label == "Right stick" ? L.T("Left stick") : label;
                var text = Text(label, "Faint");
                text.VerticalAlignment = VerticalAlignment.Center;
                return text;
            }
            // A chip like the keyboard ones: click and press the button on the pad (a drop-down here
            // opened under the pointer and picked whatever the release landed on).
            var current = PadMap.Main(Ini, input);
            var menu = PadMap.IsMenu(input);
            var chip = new AppButton
            {
                Text = current.Length > 0 ? PadMap.Label(current) : menu ? L.T("As in the world") : L.T("Not bound"),
                Icon = Icon(SymbolRegular.XboxController24),
                Padding = new Thickness(8, 3, 10, 3),
                FontSize = 12.5,
                VerticalAlignment = VerticalAlignment.Center,
                HorizontalAlignment = HorizontalAlignment.Left,
            };
            chip.SetTip(menu
                ? L.T("The controller button that does this in the game's menus, whatever it does in the world (Circle moved to L2 for sprinting still goes back here). Not set: menus use the buttons as in the world. Click, then press a button on the pad.")
                : L.F($"The controller button for this action (the game's {control.Pad}). Click, then press a button on the pad."));
            if (!PadMap.IsDefault(Ini, input)) chip.FontWeight = FontWeight.SemiBold;
            chip.Click += (_, _) => BeginPadCapture(control, input);
            return chip;
        }

        private void Store(ControlDef control, List<string> names)
        {
            Bindings.Set(Ini, control, names);
            Changed();
        }

        private void Changed()
        {
            Ws.SaveSoon();
            Rebuild();
            FillProfiles();
        }

        // ---------------------------------------------------------------- capture

        private void BeginCapture(ControlDef control, bool add = false)
        {
            _target = control;
            _padInput = null;
            _addMode = add;
            _pendingModifier = null;
            _captureTitle.Text = add ? L.F($"Add an input to {control.Label}") : L.F($"Bind {control.Label}");
            _captureHelp.Text = L.T("Press a key or a combination (hold Ctrl, Shift or Alt, then a key or click), or click / scroll inside the box. Esc cancels.");
            _captureBoxText.Text = L.T("Click or scroll here");
            _padChoices.IsVisible = false;
            _captureFeedback.Text = "";
            _overlay.IsVisible = Capturing = true;
            _overlay.Focus();
        }

        private void EndCapture()
        {
            _overlay.IsVisible = Capturing = false;
            _target = null;
            _addMode = false;
            _pendingModifier = null;
            _padInput = null;
            _padControl = null;
            _padTimer.Stop();
        }

        private void BeginPadCapture(ControlDef control, string input)
        {
            _target = null;
            _padControl = control;
            _padInput = input;
            _captureTitle.Text = L.F($"Controller button for {control.Label}");
            var live = !Ws.Game.Running && SdlPad.Available;
            _captureHelp.Text = live
                ? L.T("Press the button on your controller, or pick one below. Esc cancels. A button another action uses swaps over.")
                : Ws.Game.Running ? L.T("Pick a button below (pressing one works while the game is closed).")
                : L.T("Pick a button below.");
            _captureBoxText.Text = live ? L.T("Waiting for a button…") : L.T("Controller input is not available right now");
            _padChoices.IsVisible = true;
            _captureFeedback.Text = "";
            _overlay.IsVisible = Capturing = true;
            _overlay.Focus();
            if (live)
            {
                // Buttons already held when the dialog opens don't count until released.
                _padHeld = SdlPad.Held();
                _padTimer.Start();
            }
        }

        private void PollPad()
        {
            if (_padInput == null || Ws.Game.Running) { _padTimer.Stop(); return; }
            var held = SdlPad.Held();
            var pressed = held.Except(_padHeld).FirstOrDefault();
            _padHeld = held;
            if (pressed != null) CommitPad(pressed);
        }

        private void CommitPad(string button)
        {
            if (_padInput == null || _padControl == null) return;
            var input = _padInput;
            var control = _padControl;
            EndCapture();
            PadMap.Assign(Ini, input, button);
            Changed();
            Ws.Log(L.F($"Controller: {control.Label} on {(button.Length > 0 ? PadMap.Label(button) : "nothing")}."), LogLevel.Step);
        }

        /// <summary>name: the captured input (it replaces the one of its kind), or null to unbind.</summary>
        private void Commit(string? name)
        {
            if (_target == null) return;
            var control = _target;
            EndCapture();
            if (name == null)
                Bindings.Set(Ini, control, new List<string>());
            else if (_addMode)
                Bindings.Add(Ini, control, name);
            else
                Bindings.Assign(Ini, control, name, keepOtherKind: true);
            Changed();
            Ws.Log($"{control.Label}: {Show(InputMap.Join(Bindings.Get(Ini, control)))}", LogLevel.Step);
        }

        private void Page_KeyDown(object? sender, KeyEventArgs e)
        {
            if (_target == null && _padInput == null) return;
            e.Handled = true;
            var key = e.Key;
            if (_padInput != null)
            {
                if (key == Key.Escape) EndCapture();
                return;
            }
            if (key == Key.Escape && e.KeyModifiers == KeyModifiers.None)
            {
                EndCapture();
                return;
            }
            var name = InputMap.SdlKeyName(key);
            if (name == null)
            {
                _captureFeedback.Text = L.F($"{key} cannot be bound; try another key.");
                return;
            }
            // A modifier waits for the key or click it goes with; let go alone, it is bound by itself.
            if (InputMap.IsModifierKey(key))
            {
                _pendingModifier = name;
                _captureFeedback.Text = L.F($"{InputMap.Display(name)} + … : press a key or click, or let go to bind {InputMap.Display(name)} alone.");
                return;
            }
            Commit(WithModifiers(name, e.KeyModifiers));
        }

        private void Page_KeyUp(object? sender, KeyEventArgs e)
        {
            if (_target == null || _pendingModifier == null) return;
            e.Handled = true;
            if (InputMap.SdlKeyName(e.Key) == _pendingModifier) Commit(_pendingModifier);
        }

        /// <summary>A key or click with the modifiers held as it was pressed ("Shift+E").</summary>
        private static string WithModifiers(string name, KeyModifiers held) =>
            InputMap.WithModifiers(name, held.HasFlag(KeyModifiers.Control), held.HasFlag(KeyModifiers.Shift), held.HasFlag(KeyModifiers.Alt));

        // ---------------------------------------------------------------- profiles

        private static bool SameBindings(Dictionary<string, string> a, Dictionary<string, string> b) =>
            InputMap.Controls.All(c =>
            {
                var x = InputMap.Split(a.TryGetValue(c.Name, out var va) ? va : c.Default);
                var y = InputMap.Split(b.TryGetValue(c.Name, out var vb) ? vb : c.Default);
                return x.Count == y.Count && x.Zip(y).All(p => p.First.Equals(p.Second, StringComparison.OrdinalIgnoreCase));
            });

        private ControlProfiles.Profile? Matching()
        {
            var current = ControlProfiles.Current(Ini);
            var chosen = S.Str("controls_profile");
            return _profiles.FirstOrDefault(p => p.Name == chosen && SameBindings(p.Bindings, current))
                   ?? _profiles.FirstOrDefault(p => SameBindings(p.Bindings, current));
        }

        private void FillProfiles()
        {
            _fillingProfiles = true;
            _profiles = ControlProfiles.All();
            _profileBox.Items.Clear();
            var match = Matching();
            if (match == null)
                _profileBox.Items.Add(new ComboBoxItem { Content = L.T("Custom (not saved)"), Tag = null });
            foreach (var p in _profiles)
                _profileBox.Items.Add(new ComboBoxItem { Content = p.BuiltIn ? L.F($"{p.Name} (built in)") : p.Name, Tag = p });
            _profileBox.SelectedItem = _profileBox.Items.OfType<ComboBoxItem>().First(i => ReferenceEquals(i.Tag, match));
            SetHint(_profileHint, match == null
                ? L.T("Your current bindings are not saved in a profile. Use Save as… to keep them")
                : string.Join("  ·  ", new[] { match.Description, match.Author.Length > 0 ? L.T("by ") + match.Author : "" }.Where(t => t.Length > 0)));
            _deleteProfile.IsEnabled = match is { BuiltIn: false };
            _fillingProfiles = false;
        }

        private void ProfileChosen()
        {
            if (_fillingProfiles || _profileBox.SelectedItem is not ComboBoxItem { Tag: ControlProfiles.Profile profile }) return;
            // After the drop-down has closed: the question is a dialog of its own.
            Dispatcher.UIThread.Post(() =>
            {
                if (Matching() == null && !Prompt.Confirm(L.T("Replace your bindings?"),
                        L.F($"Your current bindings are not saved in a profile. Using \"{profile.Name}\" replaces them."), L.T("Use profile")))
                {
                    FillProfiles();
                    return;
                }
                ControlProfiles.Apply(S, Ini, profile);
                Ws.Log(L.F($"Control profile: {profile.Name}"), LogLevel.Step);
                Changed();
            });
        }

        private ControlProfiles.Profile CurrentAsProfile()
        {
            var match = Matching();
            return new ControlProfiles.Profile(
                match?.Name ?? (S.Str("controls_profile") is { Length: > 0 } n ? n : L.T("My controls")),
                match?.Author ?? S.Str("profile_author"),
                match?.Description ?? "",
                ControlProfiles.Current(Ini), Pad: PadMap.Current(Ini));
        }

        private void SaveProfile()
        {
            var current = CurrentAsProfile();
            var values = Prompt.Ask(L.T("Save profile"), L.T("Saves your current bindings so you can switch back to them or share them."),
                new Prompt.Field(L.T("Name"), current.BuiltIn ? "" : current.Name, L.T("My controls")),
                new Prompt.Field(L.T("Author"), S.Str("profile_author"), L.T("Optional")),
                new Prompt.Field(L.T("Description"), current.Description, L.T("What the layout is good for (optional)"), Multiline: true));
            if (values == null) return;

            var name = values[0].Trim();
            if (name.Length == 0) name = L.T("My controls");
            if (ControlProfiles.BuiltIns.Any(b => b.Name.Equals(name, StringComparison.OrdinalIgnoreCase)))
            {
                Prompt.Tell(L.T("Choose another name"), L.F($"\"{name}\" is a built-in profile."));
                return;
            }
            if (_profiles.Any(p => !p.BuiltIn && p.Name.Equals(name, StringComparison.OrdinalIgnoreCase)) &&
                !Prompt.Confirm(L.T("Replace the profile?"), L.F($"A profile called \"{name}\" already exists."), L.T("Replace")))
                return;

            S.Set("profile_author", values[1].Trim());
            var profile = new ControlProfiles.Profile(name, values[1].Trim(), values[2].Trim(), ControlProfiles.Current(Ini), Pad: PadMap.Current(Ini));
            try
            {
                ControlProfiles.Save(profile);
                S.Set("controls_profile", name);
                Ws.SaveSoon();
                Ws.Log(L.F($"Saved control profile \"{name}\"."), LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not save the profile: ") + e.Message, LogLevel.Bad);
            }
            FillProfiles();
        }

        private void DeleteProfile()
        {
            if (_profileBox.SelectedItem is not ComboBoxItem { Tag: ControlProfiles.Profile { BuiltIn: false } profile }) return;
            if (!Prompt.Confirm(L.T("Delete the profile?"), L.F($"\"{profile.Name}\" is removed from this PC. Your current bindings stay as they are."), L.T("Delete")))
                return;
            try
            {
                ControlProfiles.Delete(profile);
                Ws.Log(L.F($"Deleted control profile \"{profile.Name}\"."), LogLevel.Step);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not delete the profile: ") + e.Message, LogLevel.Bad);
            }
            FillProfiles();
        }

        private void CopyProfile()
        {
            if (ClipboardText.Set(ControlProfiles.ToJson(CurrentAsProfile())))
                Ws.Log(L.T("Profile JSON copied. Paste it on any paste site and share the link."), LogLevel.Good);
            else
                Ws.Log(L.T("Could not copy: another program is using the clipboard. Try again."), LogLevel.Warn);
        }

        private void ExportProfile()
        {
            var profile = CurrentAsProfile();
            var file = Ui.SaveFile(L.T("Export control profile"), profile.Name + ".json", L.T("Control profile|*.json"));
            if (file == null) return;
            try
            {
                File.WriteAllText(file, ControlProfiles.ToJson(profile));
                Ws.Log(L.T("Profile exported: ") + file, LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not export the profile: ") + e.Message, LogLevel.Bad);
            }
        }

        private void ImportFile()
        {
            var file = Ui.OpenFile(L.T("Import control profile"), L.T("Control profile|*.json|All files|*.*"));
            if (file == null) return;
            try
            {
                Offer(ControlProfiles.Parse(File.ReadAllText(file)));
            }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Could not import"), e.Message);
            }
        }

        private async Task ImportLinkAsync()
        {
            var values = Prompt.Ask(L.T("Import from a link"),
                L.T("Paste a link to a shared profile: a Pastebin, GitHub Gist or file, Rentry, hastebin, paste.rs, pastes.dev or dpaste link, ") +
                L.T("or any link that shows the raw JSON."),
                new Prompt.Field(L.T("Link"), "", "https://pastebin.com/…"));
            if (values == null || values[0].Trim().Length == 0) return;
            try
            {
                Ws.Log(L.T("Downloading the profile…"), LogLevel.Step);
                Offer(await ControlProfiles.DownloadAsync(values[0]));
            }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Could not import"), e is FormatException ? e.Message : L.T("The link could not be read: ") + e.GetBaseException().Message);
            }
        }

        /// <summary>Shows what an imported profile changes and, if accepted, saves and uses it.</summary>
        private void Offer(ControlProfiles.Parsed parsed)
        {
            var profile = parsed.Profile;
            var current = ControlProfiles.Current(Ini);
            var changes = InputMap.Controls
                .Where(c => !SameBindings(new() { [c.Name] = profile.Bindings.GetValueOrDefault(c.Name, c.Default) }, new() { [c.Name] = current[c.Name] }))
                .Select(c => $"  {c.Label}: {Show(profile.Bindings.GetValueOrDefault(c.Name, c.Default))}")
                .ToList();

            var text = new List<string>();
            if (profile.Author.Length > 0) text.Add(L.T("by ") + profile.Author);
            if (profile.Description.Length > 0) text.Add(profile.Description);
            text.Add(changes.Count == 0
                ? L.T("It is the same as your current bindings.")
                : L.F($"Changes {changes.Count} of {InputMap.Controls.Length} actions:\n") + string.Join("\n", changes.Take(12)) + (changes.Count > 12 ? "\n  …" : ""));
            if (parsed.Notes.Count > 0)
                text.Add(L.T("Notes:\n  ") + string.Join("\n  ", parsed.Notes.Take(6)));

            if (!Prompt.Confirm(L.F($"Import \"{profile.Name}\"?"), string.Join("\n\n", text), L.T("Save and use")))
                return;

            var name = profile.Name;
            for (var i = 2; _profiles.Any(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase)); i++)
                name = $"{profile.Name} ({i})";
            profile = profile with { Name = name };
            try
            {
                ControlProfiles.Save(profile);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not save the imported profile: ") + e.Message, LogLevel.Bad);
            }
            ControlProfiles.Apply(S, Ini, profile);
            Ws.Log(L.F($"Imported control profile \"{name}\"."), LogLevel.Good);
            Changed();
        }
    }
}
