using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The launcher's colours, as the WPF launcher's Themes/Charcoal.xaml and Themes/Glass.xaml have
    /// them, and the colour themes (Settings › Appearance › Theme, live): an accent for everything
    /// interactive and a faint tint of it in the surfaces, one more dictionary of the same keys.
    /// Fluent's own control colours (buttons, drop-downs, text boxes, switches) are pointed at the
    /// same colours here, so every control recolours as one.
    /// </summary>
    public static class LauncherTheme
    {
        public static readonly Choice[] Themes =
        {
            new(L.T("Yharnam"), "yharnam", L.T("The game's own colours: night, parchment and blood")),
            new(L.T("System accent"), "default", L.T("Grey, with your system's accent colour")),
            new(L.T("Crimson"), "crimson", L.T("The blood moon: deep red")),
            new(L.T("Moonlight"), "moon", L.T("Pale blue, like the Moonlight Greatsword")),
            new(L.T("Amber"), "amber", L.T("Lamplight gold")),
            new(L.T("Nightmare"), "nightmare", L.T("Arcane violet")),
            new(L.T("Verdant"), "verdant", L.T("Forbidden Woods green")),
            new(L.T("Custom"), "custom", L.T("Your own colour, below")),
        };

        private static readonly Dictionary<string, Color> Accents = new()
        {
            ["crimson"] = Color.FromRgb(0xC4, 0x1E, 0x3A),
            ["moon"] = Color.FromRgb(0x8F, 0xB8, 0xE8),
            ["amber"] = Color.FromRgb(0xD9, 0x9A, 0x2B),
            ["nightmare"] = Color.FromRgb(0x8E, 0x63, 0xD6),
            ["verdant"] = Color.FromRgb(0x3F, 0xA8, 0x6B),
        };

        public const string DefaultCustomColour = "#C41E3A";

        private static ResourceDictionary? _overlay;

        // ---------------------------------------------------------------- Yharnam

        // Taken from the game's own key art: teal-black night, the parchment of its logo, dried
        // blood, tarnished brass. Blood is the one main action (Play); brass is everything that is
        // on or chosen (switches, sliders, focus, links); bone is the thin lines.
        private static readonly Color Night = Color.FromRgb(0x0B, 0x0F, 0x11);
        private static readonly Color Stone = Color.FromRgb(0x0D, 0x13, 0x15);
        private static readonly Color Parchment = Color.FromRgb(0xEF, 0xE6, 0xD0);
        private static readonly Color Bone = Color.FromRgb(0xB3, 0xAB, 0x96);
        private static readonly Color Ash = Color.FromRgb(0x8C, 0x85, 0x76);
        private static readonly Color Blood = Color.FromRgb(0x7D, 0x1A, 0x1A);
        private static readonly Color Brass = Color.FromRgb(0xC2, 0xA0, 0x6A);
        private static readonly Color Line = Color.FromRgb(0xD6, 0xC9, 0xA8);

        public static bool IsYharnam(LauncherSettings s) => s.Str("ui_theme") == "yharnam";

        /// <summary>The whole Yharnam palette, over Charcoal or Glass: surfaces, text, lines and both accents.</summary>
        private static ResourceDictionary Yharnam(bool glass)
        {
            var d = new ResourceDictionary();
            void Solid(string key, Color c) => d[key] = new SolidColorBrush(c);
            void Tint(string key, Color c, byte alpha) => d[key] = new SolidColorBrush(WithAlpha(c, alpha));

            Solid("WindowBaseBrush", Night);
            Tint("ContentAreaBrush", Night, glass ? (byte)0 : (byte)0xFF);
            Tint("GlassPaneBrush", Stone, 0xD6);
            Tint("GlassPaneStrokeBrush", Line, 0x30);
            Tint("GlassDividerBrush", Line, 0x18);
            Tint("PillTrackBrush", Stone, 0xE0);
            Tint("SegmentSelectedBrush", Line, 0x2A);
            Tint("CardBackgroundFillColorDefaultBrush", Line, 0x10);
            Tint("CardBackgroundFillColorSecondaryBrush", Line, 0x0A);
            Tint("CardStrokeColorDefaultBrush", Line, 0x2C);
            Tint("ControlFillColorDefaultBrush", Line, 0x0F);
            Tint("ControlFillColorSecondaryBrush", Line, 0x1C);
            Tint("ControlFillColorTertiaryBrush", Line, 0x0A);
            Tint("ControlFillColorInputActiveBrush", Night, 0xE6);
            Tint("ControlStrokeColorDefaultBrush", Line, 0x40);
            Tint("ControlStrokeColorSecondaryBrush", Line, 0x24);
            Tint("SubtleFillColorSecondaryBrush", Line, 0x12);
            Tint("SidebarItemHoverBrush", Line, 0x14);
            Tint("SidebarItemSelectedBrush", Line, 0x24);
            Solid("SidebarHeaderBrush", Ash);
            Tint("PanelDeepBrush", Night, 0xCC);
            Tint("DialogScrimBrush", Colors.Black, 0xA6);
            Solid("DialogBackgroundBrush", Color.FromRgb(0x12, 0x1A, 0x1C));
            Tint("DialogStrokeBrush", Line, 0x40);
            Solid("TextFillColorPrimaryBrush", Parchment);
            Solid("TextFillColorSecondaryBrush", Bone);
            Solid("TextFillColorTertiaryBrush", Ash);
            Solid("AccentTextFillColorPrimaryBrush", Brass);
            Solid("StatusDotIdleBrush", Ash);
            Solid("GoodTextBrush", Color.FromRgb(0x9B, 0xBF, 0x8F));
            Solid("WarningTextBrush", Color.FromRgb(0xD9, 0xA4, 0x4F));
            Solid("DangerTextBrush", Color.FromRgb(0xD8, 0x6A, 0x5E));
            FluentKeys(d);

            // Brass for what is on or chosen; blood only for the main button (Play), and Stop in it.
            AccentKeys(d, Brass);
            var knob = new SolidColorBrush(Night);
            foreach (var key in new[] { "ToggleSwitchKnobFillOn", "ToggleSwitchKnobFillOnPointerOver", "ToggleSwitchKnobFillOnPressed" })
                d[key] = knob;
            var blood = new SolidColorBrush(Blood);
            var bloodHover = new SolidColorBrush(Color.FromRgb(0x95, 0x1F, 0x1F));
            var bloodPressed = new SolidColorBrush(Color.FromRgb(0x65, 0x14, 0x14));
            var bloodLine = new SolidColorBrush(Color.FromRgb(0xC4, 0x54, 0x4A));
            var onBlood = new SolidColorBrush(Parchment);
            d["AccentButtonBackground"] = blood;
            d["AccentButtonBackgroundPointerOver"] = bloodHover;
            d["AccentButtonBackgroundPressed"] = bloodPressed;
            d["AccentButtonBorderBrush"] = bloodLine;
            d["AccentButtonBorderBrushPointerOver"] = bloodLine;
            d["AccentButtonBorderBrushPressed"] = bloodLine;
            foreach (var key in new[] { "AccentButtonForeground", "AccentButtonForegroundPointerOver", "AccentButtonForegroundPressed" })
                d[key] = onBlood;
            d["SystemAccentColor"] = Brass;
            return d;
        }

        /// <summary>The theme's accent; null for the system accent (or a custom colour that doesn't parse).</summary>
        public static Color? Accent(LauncherSettings s)
        {
            var theme = s.Str("ui_theme");
            if (theme == "yharnam") return Brass;
            if (theme == "custom")
                return ParseColour(s.Str("ui_theme_colour"));
            return Accents.TryGetValue(theme, out var colour) ? colour : null;
        }

        public static Color? ParseColour(string text)
        {
            text = text.Trim();
            if (text.Length is 6 or 8 && !text.StartsWith('#')) text = "#" + text;
            return Color.TryParse(text, out var c) ? Color.FromRgb(c.R, c.G, c.B) : null;
        }

        // ---------------------------------------------------------------- the base colours

        /// <summary>Charcoal: neutral greys stepped a few shades apart, hairline borders, one accent.</summary>
        private static ResourceDictionary Charcoal()
        {
            var d = new ResourceDictionary();
            void Solid(string key, uint argb) => d[key] = new SolidColorBrush(Color.FromUInt32(argb));
            Solid("WindowBaseBrush", 0xFF242426);
            Solid("ContentAreaBrush", 0xFF29292B);
            Solid("CardBackgroundFillColorDefaultBrush", 0xFF2E2E31);
            Solid("CardBackgroundFillColorSecondaryBrush", 0xFF2B2B2E);
            Solid("CardStrokeColorDefaultBrush", 0xFF3A3A3E);
            Solid("ControlFillColorDefaultBrush", 0xFF303034);
            Solid("ControlFillColorSecondaryBrush", 0xFF38383C);
            Solid("ControlFillColorTertiaryBrush", 0xFF2B2B2E);
            Solid("ControlFillColorInputActiveBrush", 0xFF1E1E21);
            Solid("ControlStrokeColorDefaultBrush", 0xFF3C3C41);
            Solid("ControlStrokeColorSecondaryBrush", 0xFF333337);
            Solid("SubtleFillColorSecondaryBrush", 0xFF2E2E32);
            Solid("PanelDeepBrush", 0xFF1E1E20);
            Solid("DialogScrimBrush", 0x99000000);
            Solid("DialogBackgroundBrush", 0xFF2C2C2F);
            Solid("DialogStrokeBrush", 0xFF3E3E43);
            Solid("SidebarItemSelectedBrush", 0xFF37373B);
            Solid("SidebarItemHoverBrush", 0xFF2E2E31);
            Solid("SidebarHeaderBrush", 0xFF67676D);
            Solid("TextFillColorPrimaryBrush", 0xFFE6E6E8);
            Solid("TextFillColorSecondaryBrush", 0xFFB6B6BB);
            Solid("TextFillColorTertiaryBrush", 0xFF909096);
            Solid("AccentTextFillColorPrimaryBrush", 0xFF4C9DFF);
            Solid("StatusDotIdleBrush", 0xFF6E6E74);
            Solid("WarningTextBrush", 0xFFF2B84B);
            Solid("DangerTextBrush", 0xFFFF6B6B);
            Solid("GoodTextBrush", 0xFF5FD38A);
            Solid("SegmentSelectedBrush", 0xFF2D7FF0);
            Solid("GlassPaneBrush", 0xFF2E2E31);
            Solid("GlassPaneStrokeBrush", 0xFF3A3A3E);
            Solid("GlassDividerBrush", 0xFF3A3A3E);
            Solid("PillTrackBrush", 0xFF1E1E21);
            // Windows 11's UI font, made for small sizes on screen; elsewhere the system's own.
            d["UiFont"] = new FontFamily("Segoe UI Variable Text, Segoe UI, Inter, Cantarell, Noto Sans, DejaVu Sans");
            d["MonoFont"] = new FontFamily("Cascadia Mono, Consolas, JetBrains Mono, DejaVu Sans Mono, Noto Sans Mono, Liberation Mono");
            // A Victorian serif for the game's title and the group headers: the system's own, nothing bundled.
            d["DisplayFont"] = new FontFamily("Palatino Linotype, Book Antiqua, Palatino, URW Palladio L, P052, Georgia, DejaVu Serif, serif");
            // The accent of Default is the system's own (Windows' accent colour), as in WPF: Fluent
            // reads it; a theme sets its own over it (Apply).
            FluentKeys(d);
            return d;
        }

        /// <summary>Glass: the Modern layout's look. Frosted, translucent surfaces over the blurred,
        /// darkened artwork; dialogs stay opaque (glass over glass is unreadable).</summary>
        private static ResourceDictionary Glass()
        {
            var d = new ResourceDictionary();
            void Solid(string key, uint argb) => d[key] = new SolidColorBrush(Color.FromUInt32(argb));
            Solid("GlassPaneBrush", 0x1CFFFFFF);
            d["GlassPaneStrokeBrush"] = Vertical((0x66FFFFFF, 0), (0x1FFFFFFF, 0.35), (0x14FFFFFF, 1));
            Solid("GlassDividerBrush", 0x16FFFFFF);
            Solid("PillTrackBrush", 0x33000000);
            d["SegmentSelectedBrush"] = Vertical((0x70FFFFFF, 0), (0x38FFFFFF, 1));
            Solid("CardBackgroundFillColorDefaultBrush", 0x1CFFFFFF);
            Solid("CardBackgroundFillColorSecondaryBrush", 0x14FFFFFF);
            Solid("CardStrokeColorDefaultBrush", 0x26FFFFFF);
            Solid("ControlFillColorDefaultBrush", 0x22FFFFFF);
            Solid("ControlFillColorSecondaryBrush", 0x2EFFFFFF);
            Solid("ControlFillColorTertiaryBrush", 0x18FFFFFF);
            Solid("ControlFillColorInputActiveBrush", 0x40000000);
            Solid("ControlStrokeColorDefaultBrush", 0x2AFFFFFF);
            Solid("ControlStrokeColorSecondaryBrush", 0x1AFFFFFF);
            Solid("SubtleFillColorSecondaryBrush", 0x1AFFFFFF);
            Solid("SidebarItemHoverBrush", 0x1FFFFFFF);
            Solid("ContentAreaBrush", 0x00000000);
            Solid("WindowBaseBrush", 0xFF0C0D10);
            Solid("PanelDeepBrush", 0x66000000);
            Solid("DialogScrimBrush", 0xA6000000);
            d["DialogBackgroundBrush"] = Vertical((0xFF2A2D33, 0), (0xFF1F2126, 1));
            d["DialogStrokeBrush"] = Vertical((0x55FFFFFF, 0), (0x1AFFFFFF, 0.4));
            Solid("TextFillColorPrimaryBrush", 0xFFF5F6F8);
            Solid("TextFillColorSecondaryBrush", 0xFFC2C5CA);
            Solid("TextFillColorTertiaryBrush", 0xFF9A9DA3);
            FluentKeys(d);
            return d;
        }

        private static LinearGradientBrush Vertical(params (uint Argb, double Offset)[] stops)
        {
            var brush = new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
                EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
            };
            foreach (var (argb, offset) in stops)
                brush.GradientStops.Add(new GradientStop(Color.FromUInt32(argb), offset));
            return brush;
        }

        /// <summary>Fluent's control colours, taken from this dictionary's own (only those it sets,
        /// so a layer over another changes only what it means to).</summary>
        private static void FluentKeys(ResourceDictionary d)
        {
            void Alias(string from, params string[] keys)
            {
                if (!d.TryGetValue(from, out var value)) return;
                foreach (var key in keys) d[key] = value;
            }
            Alias("ControlFillColorDefaultBrush", "ButtonBackground", "ComboBoxBackground", "TextControlBackground",
                "ToggleSwitchFillOff", "ButtonBackgroundDisabled", "ComboBoxBackgroundDisabled", "TextControlBackgroundDisabled",
                "SliderTrackFill");
            Alias("ControlFillColorSecondaryBrush", "ButtonBackgroundPointerOver", "ComboBoxBackgroundPointerOver",
                "TextControlBackgroundPointerOver", "ToggleSwitchFillOffPointerOver", "SliderTrackFillPointerOver");
            Alias("ControlFillColorTertiaryBrush", "ButtonBackgroundPressed", "ComboBoxBackgroundPressed",
                "ToggleSwitchFillOffPressed", "SliderTrackFillPressed");
            Alias("ControlFillColorInputActiveBrush", "TextControlBackgroundFocused");
            Alias("ControlStrokeColorDefaultBrush", "ButtonBorderBrush", "ButtonBorderBrushPointerOver", "ButtonBorderBrushPressed",
                "ComboBoxBorderBrush", "ComboBoxBorderBrushPointerOver", "ComboBoxBorderBrushPressed",
                "TextControlBorderBrush", "TextControlBorderBrushPointerOver", "ToggleSwitchStrokeOff",
                "ToggleSwitchStrokeOffPointerOver", "ToggleSwitchStrokeOffPressed");
            Alias("ControlStrokeColorSecondaryBrush", "ButtonBorderBrushDisabled", "ComboBoxBorderBrushDisabled", "TextControlBorderBrushDisabled");
            Alias("TextFillColorPrimaryBrush", "ButtonForeground", "ButtonForegroundPointerOver", "ButtonForegroundPressed",
                "ComboBoxForeground", "ComboBoxForegroundFocused", "ComboBoxForegroundFocusedPressed", "TextControlForeground",
                "TextControlForegroundFocused", "TextControlForegroundPointerOver", "ToolTipForeground", "MenuFlyoutItemForeground",
                "MenuFlyoutItemForegroundPointerOver", "MenuFlyoutItemForegroundPressed", "ComboBoxItemForeground",
                "ComboBoxItemForegroundPointerOver", "ComboBoxItemForegroundPressed", "ComboBoxItemForegroundSelected",
                "ComboBoxItemForegroundSelectedPointerOver", "ComboBoxItemForegroundSelectedPressed", "ToggleSwitchContentForeground",
                "ComboBoxDropDownGlyphForeground", "ToggleSwitchKnobFillOff", "ToggleSwitchKnobFillOffPointerOver",
                "ToggleSwitchKnobFillOffPressed");
            Alias("TextFillColorTertiaryBrush", "TextControlPlaceholderForeground", "TextControlPlaceholderForegroundFocused",
                "TextControlPlaceholderForegroundPointerOver", "ComboBoxPlaceHolderForeground", "ButtonForegroundDisabled",
                "ComboBoxForegroundDisabled", "TextControlForegroundDisabled", "MenuFlyoutItemForegroundDisabled",
                "ComboBoxItemForegroundDisabled", "ToggleSwitchKnobFillOffDisabled", "ToggleSwitchStrokeOffDisabled");
            Alias("DialogBackgroundBrush", "ComboBoxDropDownBackground", "MenuFlyoutPresenterBackground", "ToolTipBackground",
                "FlyoutPresenterBackground");
            Alias("DialogStrokeBrush", "ComboBoxDropDownBorderBrush", "MenuFlyoutPresenterBorderBrush", "ToolTipBorderBrush",
                "FlyoutBorderThemeBrush");
            Alias("SidebarItemHoverBrush", "ComboBoxItemBackgroundPointerOver", "MenuFlyoutItemBackgroundPointerOver",
                "ComboBoxItemBackgroundPressed", "MenuFlyoutItemBackgroundPressed");
            Alias("SidebarItemSelectedBrush", "ComboBoxItemBackgroundSelected", "ComboBoxItemBackgroundSelectedPointerOver",
                "ComboBoxItemBackgroundSelectedPressed");
        }

        /// <summary>Everything Fluent draws in the accent: primary buttons, switches that are on,
        /// slider fills, progress bars, the focused text box's line.</summary>
        private static void AccentKeys(ResourceDictionary d, Color accent)
        {
            d["SystemAccentColor"] = accent;
            d["SystemAccentColorDark1"] = Mix(accent, Colors.Black, 0.15);
            d["SystemAccentColorDark2"] = Mix(accent, Colors.Black, 0.3);
            d["SystemAccentColorDark3"] = Mix(accent, Colors.Black, 0.45);
            d["SystemAccentColorLight1"] = Mix(accent, Colors.White, 0.15);
            d["SystemAccentColorLight2"] = Mix(accent, Colors.White, 0.3);
            d["SystemAccentColorLight3"] = Mix(accent, Colors.White, 0.45);
            var fill = new SolidColorBrush(Mix(accent, Colors.Black, 0.1));
            var hover = new SolidColorBrush(accent);
            var pressed = new SolidColorBrush(Mix(accent, Colors.Black, 0.25));
            var onAccent = new SolidColorBrush(Readable(accent));
            foreach (var key in new[] { "AccentButtonBackground", "ToggleSwitchFillOn", "ToggleSwitchStrokeOn", "SliderTrackValueFill",
                         "SliderThumbBackground", "SystemControlHighlightAccentBrush", "TextControlBorderBrushFocused",
                         "CheckBoxCheckBackgroundFillChecked", "CheckBoxCheckBackgroundStrokeChecked" })
                d[key] = fill;
            foreach (var key in new[] { "AccentButtonBackgroundPointerOver", "ToggleSwitchFillOnPointerOver", "ToggleSwitchStrokeOnPointerOver",
                         "SliderTrackValueFillPointerOver", "SliderThumbBackgroundPointerOver", "CheckBoxCheckBackgroundFillCheckedPointerOver" })
                d[key] = hover;
            foreach (var key in new[] { "AccentButtonBackgroundPressed", "ToggleSwitchFillOnPressed", "ToggleSwitchStrokeOnPressed",
                         "SliderTrackValueFillPressed", "SliderThumbBackgroundPressed", "CheckBoxCheckBackgroundFillCheckedPressed" })
                d[key] = pressed;
            foreach (var key in new[] { "AccentButtonForeground", "AccentButtonForegroundPointerOver", "AccentButtonForegroundPressed",
                         "ToggleSwitchKnobFillOn", "ToggleSwitchKnobFillOnPointerOver", "ToggleSwitchKnobFillOnPressed" })
                d[key] = onAccent;
            d["AccentButtonBorderBrush"] = fill;
            d["AccentButtonBorderBrushPointerOver"] = hover;
            d["AccentButtonBorderBrushPressed"] = pressed;
            d["TextControlSelectionHighlightColor"] = new SolidColorBrush(WithAlpha(accent, 0x80));
        }

        /// <summary>White text on a dark accent, near-black on a pale one (Moonlight, Amber).</summary>
        private static Color Readable(Color c) =>
            0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B > 165 ? Color.FromRgb(0x14, 0x15, 0x18) : Colors.White;

        // ---------------------------------------------------------------- applying

        /// <summary>The base colours, once at start: Charcoal, and Glass over it for the Modern layout.</summary>
        public static void Install(bool glass)
        {
            var resources = Application.Current!.Resources;
            resources.MergedDictionaries.Add(Charcoal());
            if (glass) resources.MergedDictionaries.Add(Glass());
        }

        /// <summary>Applies the saved theme; glass: the Modern layout's dictionary is in use.</summary>
        public static void Apply(LauncherSettings s, bool glass)
        {
            var resources = Application.Current!.Resources;
            if (_overlay != null)
            {
                resources.MergedDictionaries.Remove(_overlay);
                _overlay = null;
            }

            if (IsYharnam(s))
            {
                _overlay = Yharnam(glass);
                resources.MergedDictionaries.Add(_overlay);
                return;
            }
            if (Accent(s) is not { } a) return;
            var d = new ResourceDictionary();
            void Solid(string key, Color c) => d[key] = new SolidColorBrush(c);
            AccentKeys(d, a);
            Solid("AccentTextFillColorPrimaryBrush", Mix(a, Colors.White, 0.25));
            if (glass)
            {
                d["SegmentSelectedBrush"] = new LinearGradientBrush
                {
                    StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
                    EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
                    GradientStops =
                    {
                        new GradientStop(WithAlpha(Mix(a, Colors.White, 0.2), 0xC0), 0),
                        new GradientStop(WithAlpha(a, 0x80), 1),
                    },
                };
                Solid("WindowBaseBrush", Mix(Color.FromRgb(0x0C, 0x0D, 0x10), a, 0.10));
                Solid("PillTrackBrush", WithAlpha(Mix(Colors.Black, a, 0.15), 0x40));
            }
            else
            {
                Solid("SegmentSelectedBrush", Mix(a, Colors.Black, 0.1));
                Solid("WindowBaseBrush", Mix(Color.FromRgb(0x24, 0x24, 0x26), a, 0.06));
                Solid("ContentAreaBrush", Mix(Color.FromRgb(0x29, 0x29, 0x2B), a, 0.06));
                Solid("CardBackgroundFillColorDefaultBrush", Mix(Color.FromRgb(0x2E, 0x2E, 0x31), a, 0.07));
                Solid("CardStrokeColorDefaultBrush", Mix(Color.FromRgb(0x3A, 0x3A, 0x3E), a, 0.12));
                Solid("SidebarItemSelectedBrush", Mix(Color.FromRgb(0x37, 0x37, 0x3B), a, 0.20));
                Solid("SidebarItemHoverBrush", Mix(Color.FromRgb(0x2E, 0x2E, 0x31), a, 0.08));
                Solid("PillTrackBrush", Mix(Color.FromRgb(0x1E, 0x1E, 0x21), a, 0.05));
                Solid("DialogBackgroundBrush", Mix(Color.FromRgb(0x2C, 0x2C, 0x2F), a, 0.06));
                Solid("PanelDeepBrush", Mix(Color.FromRgb(0x1E, 0x1E, 0x20), a, 0.04));
                FluentKeys(d);
            }
            resources.MergedDictionaries.Add(d);
            _overlay = d;
        }

        /// <summary>The backdrop shade's colour (the dark wash over the background picture).</summary>
        public static Color ShadeColour(LauncherSettings s) =>
            IsYharnam(s) ? Night
            : Accent(s) is { } a ? Mix(Color.FromRgb(0x0A, 0x0B, 0x0E), a, 0.12) : Color.FromRgb(0x0A, 0x0B, 0x0E);

        public static Color Mix(Color from, Color to, double amount) => Color.FromRgb(
            (byte)Math.Round(from.R + (to.R - from.R) * amount),
            (byte)Math.Round(from.G + (to.G - from.G) * amount),
            (byte)Math.Round(from.B + (to.B - from.B) * amount));

        public static Color WithAlpha(Color c, byte alpha) => Color.FromArgb(alpha, c.R, c.G, c.B);

        /// <summary>A brush of the current colours by key (for code that needs the brush itself).</summary>
        public static IBrush Brush(string key) =>
            Application.Current!.TryGetResource(key, Application.Current.ActualThemeVariant, out var value) && value is IBrush brush
                ? brush
                : Brushes.Transparent;
    }
}
