namespace BloodborneLauncher.Core
{
    /// <summary>A drop-down entry. Note is shown under the row while this entry is selected.</summary>
    public sealed record Choice(string Label, string Value, string? Note = null);

    public sealed record Effect(string Key, string Title, bool Default, string? Hint = null);

    /// <summary>Every list the launcher offers, in English, with the values bbport expects.</summary>
    public static class Choices
    {
        public static readonly Choice[] Upscalers = OperatingSystem.IsWindows()
            ? new Choice[]
            {
                new("FSR", "fsr"),
                new(L.T("FSR 4.1.1 (fsr4vk, experimental)"), "fsr411"),
                new("DLSS (NVIDIA RTX)", "dlss"),
                new(L.T("TAA (native anti-aliasing)"), "taa"),
                new(L.T("Off"), "off"),
            }
            : new Choice[]
            {
                new("FSR 4", "fsr4"),
                new("FSR 4.1.1", "fsr411"),
                new("FSR 3", "fsr3"),
                new("DLSS (NVIDIA RTX)", "dlss"),
                new(L.T("TAA (native anti-aliasing)"), "taa"),
                new(L.T("Off"), "off"),
            };

        /// <summary>frame_gen in lantern.ini: generated frames between the game's own (the engine's
        /// vk_frame_generation.h; DLSS through NVIDIA Streamline, vk_streamline.h). Off by default.</summary>
        public static readonly Choice[] FrameGeneration =
        {
            new(L.T("Off"), "off"),
            new("FSR 3.1 (2x)", "fsr3", L.T("Needs an upscaler. Not in menus, movies or with HDR output")),
            new("DLSS (NVIDIA RTX 40/50)", "dlss",
                L.T("NVIDIA's frame generation with Reflex: 2x on RTX 40, up to 6x on RTX 50. Needs an upscaler and Hardware-accelerated GPU scheduling. Applied when the game starts")),
        };

        /// <summary>frame_gen_frames: DLSS generated frames per real one (more than one: RTX 50).</summary>
        public static readonly Choice[] FrameGenFrames =
        {
            new("2x", "1", L.T("One generated frame per real one (RTX 40 and 50)")),
            new("3x", "2", L.T("RTX 50 only")),
            new("4x", "3", L.T("RTX 50 only")),
            new("5x", "4", L.T("RTX 50 only")),
            new("6x", "5", L.T("RTX 50 only")),
        };

        /// <summary>reflex: NVIDIA Reflex low latency while DLSS Frame Generation is loaded.</summary>
        public static readonly Choice[] Reflex =
        {
            new(L.T("On"), "on", L.T("Lower input lag. Always on while DLSS Frame Generation runs")),
            new(L.T("On + Boost"), "boost", L.T("Also keeps the GPU clocks up: a little lower lag, more power")),
            new(L.T("Off"), "off", L.T("Only while DLSS Frame Generation is off")),
        };

        public static readonly Choice[] Presets =
        {
            new(L.T("Native AA (no upscaling; DLAA with DLSS)"), "0"),
            new(L.T("Quality (x1.5)"), "1"),
            new(L.T("Balanced (x1.7)"), "2"),
            new(L.T("Performance (x2)"), "3"),
            new(L.T("Ultra Performance (x3)"), "4"),
            new(L.T("Custom (render scale below)"), "5"),
        };

        public static readonly Choice[] OutputResolutions =
        {
            new(L.T("1280 × 720 (Steam Deck)"), "1280x720"),
            new("1600 × 900", "1600x900"),
            new("1920 × 1080", "1920x1080"),
            new("2560 × 1440", "2560x1440"),
            new("3840 × 2160", "3840x2160"),
            // Ultrawide: the engine widens the camera to the screen (Widescreen, on by default).
            new("2560 × 1080 (21:9)", "2560x1080"),
            new("3440 × 1440 (21:9)", "3440x1440"),
            new("3840 × 1600 (24:10)", "3840x1600"),
            new("5120 × 2160 (21:9)", "5120x2160"),
            new("3840 × 1080 (32:9)", "3840x1080"),
            new("5120 × 1440 (32:9)", "5120x1440"),
            new("7680 × 2160 (32:9)", "7680x2160"),
            // Narrower than 16:9: the camera and UI made taller to fill it (Widescreen).
            new("1280 × 1024 (5:4)", "1280x1024"),
            new("1680 × 1050 (16:10)", "1680x1050"),
            new("1920 × 1200 (16:10)", "1920x1200"),
            new("2560 × 1600 (16:10)", "2560x1600"),
            // Any other display: its own size, the camera and UI widened to fill it.
            new(L.T("Display (auto)"), "auto", L.T("Your display's own resolution, whatever its shape: the picture fills the screen")),
            new(L.T("Custom"), Custom, L.T("Any width and height: set them below")),
        };

        /// <summary>The Output resolution list's entry for a size it does not name.</summary>
        public const string Custom = "custom";

        /// <summary>A size the engine takes as a custom output: 640 × 360 to 16384 × 8640, even.</summary>
        public static bool ValidCustomSize(int w, int h) => w >= 640 && h >= 360 && w <= 16384 && h <= 8640;

        /// <summary>The size an output_res value names; "auto" is the display's own (1920 × 1080
        /// when it is unknown), as the engine reads it at start (scripts/run_windows.py).</summary>
        public static (int W, int H) OutputSize(string value)
        {
            if (value == "auto")
            {
                var hw = Hardware.Detect();
                return hw.ScreenWidth >= 640 && hw.ScreenHeight >= 360
                    ? (hw.ScreenWidth & ~1, hw.ScreenHeight & ~1)
                    : (1920, 1080);
            }
            var parts = value.Split('x');
            return parts.Length == 2 && int.TryParse(parts[0], out var w) && int.TryParse(parts[1], out var h)
                ? (w, h)
                : (1920, 1080);
        }

        // Chromatic aberration, depth of field, motion blur and the game's own anti-aliasing start off:
        // the picture is clearer without them, and the upscalers anti-alias (players' advice).
        public static readonly Effect[] Effects =
        {
            new("effect_chromatic_aberration", L.T("Chromatic aberration"), false),
            new("effect_dof", L.T("Depth of field"), false),
            new("effect_motion_blur", L.T("Motion blur"), false),
            new("effect_ssao", L.T("Ambient occlusion (SSAO)"), true),
            new("effect_game_aa", L.T("Game's own anti-aliasing"), false),
            new("effect_dynamic_shadows", L.T("Shadows from dynamic lights"), true),
            new("effect_ssr", L.T("Screen-space reflections"), false, L.T("Not in the original game")),
            new("effect_gtao", L.T("Better ambient occlusion (GTAO)") + L.T(" (experimental)"), false, L.T("Replaces the game's SSAO shader; needs Ambient occlusion on")),
            new("effect_modern_ssr", L.T("Improved reflections"), false, L.T("Needs Screen-space reflections on")),
            new("effect_pom_cavity", L.T("Deeper parallax surfaces"), false),
            new("effect_contact_shadows", L.T("Contact shadows"), false),
            new("effect_pcss", L.T("Soft sun shadows (PCSS)"), false),
            new("effect_gi", L.T("Indirect lighting (GI)"), false, L.T("Heavy")),
            new("effect_volumetric_light", L.T("Volumetric sunlight"), false, L.T("Heavy")),
            new("effect_point_fog", L.T("Fog around lamps"), false, L.T("Heavy")),
            new("effect_point_shadows", L.T("Point light shadows") + L.T(" (experimental)"), false, L.T("Heavy")),
            new("skip_intro", L.T("Skip intro videos"), false),
            new("debug_camera", L.T("Free camera"), false, L.T("Toggle with Cross + L3 / Space + Z")),
            new("debug_menu", L.T("Debug menu"), false,
                L.T("Left touchpad / Tab. Needs the adhoc folder from Nexus mod #253 (adhoc/font) in the game's dvdroot_ps4 or as a mod; without it the patch is not applied")),
        };

        public static readonly Choice[] ShadowResolution =
        {
            new(L.T("Game's own"), "0"),
            new("4096", "1", L.T("Needs more video memory")),
            new("8192", "2", L.T("Needs much more video memory")),
        };

        public static readonly Choice[] LocalShadowResolution =
        {
            new(L.T("Game's own"), "0"),
            new(L.T("High"), "1", L.T("Needs more video memory")),
            new(L.T("Very high"), "2", L.T("Needs much more video memory")),
        };

        public static readonly Choice[] MotionBlurQuality =
        {
            new(L.T("Game's own"), "0"),
            new(L.T("High"), "1"),
        };

        public static readonly Choice[] CameraTuning =
        {
            new(L.T("Original (the game's camera)"), "original", L.T("As the game has it, at any frame rate")),
        };

        public static readonly Choice[] ModelLod =
        {
            new(L.T("As in the game"), "0"),
            new(L.T("Highest (-2)"), "-2"),
            new(L.T("Lower (1)"), "1"),
            new(L.T("Lowest (2)"), "2"),
        };

        public static readonly Choice[] FpsModes =
        {
            new("60 FPS", "60"),
            new("90 FPS", "90", L.T("Includes the sprint fix for high frame rates")),
            new("120 FPS", "120", L.T("For 120 Hz screens")),
            new("144 FPS", "144", L.T("For 144 Hz screens")),
            new(L.T("Uncapped"), "uncap", L.T("No frame-rate limit: the game's timing follows its frame rate. It can freeze at the end of a boss intro (Vicar Amelia's)")),
            new(L.T("30 FPS (as on PS4)"), "30"),
        };

        /// <summary>The DLSS model (BB_DLSS_PRESET): NVIDIA's render presets.</summary>
        public static readonly Choice[] DlssModels =
        {
            new(L.T("Auto (per quality)"), "auto", L.T("NVIDIA's DLSS 4.5 pairing: K for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance")),
            new(L.T("Transformer (preset K)"), "k", L.T("The newer model: steadier thin and moving objects (a weapon on the back, railings)")),
            new(L.T("Transformer 2 (preset M)"), "m", L.T("DLSS 4.5's model for Performance: sharper, a little heavier")),
            new(L.T("Transformer 2 (preset L)"), "l", L.T("DLSS 4.5's model for Ultra Performance")),
            new(L.T("Transformer (preset J)"), "j", L.T("Like K, slightly less ghosting and a little more flicker")),
            new(L.T("CNN (preset E)"), "e", L.T("The older model: a little faster on RTX 20 and 30 cards, softer and more ghosting in motion")),
            new(L.T("CNN (preset F)"), "f", L.T("The older model for Ultra Performance and DLAA")),
            new(L.T("The DLL's default"), "default", L.T("Whatever the included DLSS version picks for each quality mode")),
        };

        /// <summary>What opens the port's in-game settings menu (BB_OVERLAY_KEYS).</summary>
        public static readonly Choice[] OverlayKeys =
        {
            new(L.T("Insert or L3 + R3"), "both", L.T("The keyboard's Insert key, or both sticks pressed on a controller")),
            new(L.T("Insert only"), "insert", L.T("Pressing both sticks in a fight no longer opens it")),
            new(L.T("Off"), "off", L.T("The menu never opens; settings stay in the launcher and the game's PC menus")),
        };

        /// <summary>The port's in-game settings menu (Insert / L3+R3).</summary>
        public static readonly Choice[] MenuLanguages =
        {
            new("简体中文", "zh_CN"),
            new("English", "en"),
            new("Español", "es"),
            new("Português (Brasil)", "pt_BR"),
            new("Français", "fr"),
            new("Deutsch", "de"),
            new("Italiano", "it"),
            new("Polski", "pl"),
            new("Türkçe", "tr"),
            new("Русский", "ru"),
        };

        public static readonly Choice[] PresentModes =
        {
            new(L.T("Mailbox"), "Mailbox", L.T("Low latency without tearing")),
            new(L.T("FIFO (VSync)"), "Fifo", L.T("Classic vertical sync")),
            new(L.T("FIFO Relaxed"), "FifoRelaxed", L.T("VSync that tears instead of stuttering when a frame is late")),
            new(L.T("Immediate"), "Immediate", L.T("Lowest latency; may tear")),
        };

        public static readonly Choice[] DrawPipe =
        {
            new(L.T("Auto"), "", L.T("On with 8 or more CPU threads")),
            new(L.T("On"), "1"),
            new(L.T("Off"), "0", L.T("More stable, but slower")),
        };

        public static readonly Choice[] GameLanguages =
        {
            new("English", "1"),
            new(L.T("Russian"), "8"),
            new(L.T("Japanese"), "0"),
            new(L.T("French"), "2"),
            new(L.T("Spanish"), "3"),
            new(L.T("German"), "4"),
            new(L.T("Italian"), "5"),
            new("Português (Brasil)", "17"),
        };

        public static readonly Choice[] HdrPaperWhite =
        {
            new(L.T("100 nits"), "100"), new(L.T("150 nits"), "150"), new(L.T("200 nits (default)"), "200"),
            new(L.T("250 nits"), "250"), new(L.T("300 nits"), "300"), new(L.T("400 nits"), "400"),
        };

        public static readonly Choice[] HdrPeak =
        {
            new(L.T("400 nits"), "400"), new(L.T("600 nits"), "600"), new(L.T("800 nits"), "800"), new(L.T("1000 nits (default)"), "1000"),
            new(L.T("1500 nits"), "1500"), new(L.T("2000 nits"), "2000"), new(L.T("4000 nits"), "4000"),
        };

        /// <summary>button_prompts: the game's button icons as keys while the keyboard or mouse is in use, always, or never.</summary>
        public static readonly Choice[] ButtonPrompts =
        {
            new(L.T("Automatic (default)"), "auto"), new(L.T("Keyboard"), "keyboard"), new(L.T("Controller"), "controller"),
        };

        /// <summary>pad_icons: the game's controller icons as they are (PlayStation's) or as an Xbox controller's buttons; automatic follows the controller in use.</summary>
        public static readonly Choice[] DiscordStatus =
        {
            new(L.T("Off"), "off"), new("Lantern Engine", "lantern"), new(L.T("Custom"), "custom"),
        };

        public static readonly Choice[] PadIcons =
        {
            new(L.T("Automatic (default)"), "auto"), new("PlayStation", "playstation"), new("Xbox", "xbox"),
        };

        public static readonly Choice[] HdrMode =
        {
            new(L.T("Native (default)"), "native"), new(L.T("Expanded"), "expand"),
        };

        public static readonly Choice[] HdrBoost =
        {
            new(L.T("Off"), "0"), new(L.T("Low (default)"), "0.25"), new(L.T("Medium"), "0.5"), new(L.T("High"), "1"),
        };

        public static readonly Choice[] HdrUiBrightness =
        {
            new(L.T("Dimmer"), "0.75"), new(L.T("Paper white (default)"), "1"), new(L.T("Brighter"), "1.25"), new(L.T("Much brighter"), "1.5"),
        };

        public static readonly Choice[] HdrUiColor =
        {
            new(L.T("Exact (sRGB)"), "0"), new(L.T("In between"), "0.5"), new(L.T("Vivid (default)"), "1"),
        };

        public static readonly Choice[] OnlineServers =
        {
            new(L.T("The Hunter's Dream"), "huntersdream"), new("SuperMedo", SuperMedo.Id),
        };

        public static readonly Choice[] NamePlates =
        {
            new(L.T("Name only"), "name"), new(L.T("Role and name"), "role"),
        };

        public static readonly Choice[] SeamlessConnections =
        {
            new(L.T("Server"), "server"), new(L.T("Direct P2P"), "direct"),
        };

        public static readonly Choice[] SeamlessRoles =
        {
            new(L.T("Join"), "guest"), new(L.T("Host"), "host"),
        };

        public static readonly Choice[] SignTimeout =
        {
            new(L.T("30 seconds (default)"), "30"), new(L.T("60 seconds"), "60"), new(L.T("The game's own (180 seconds)"), "0"),
        };

        public static readonly Choice[] LiveResolution =
        {
            new(L.T("Auto (by GPU)"), "auto"),
            new(L.T("Off (faster)"), "0"),
            new(L.T("On"), "1"),
        };

        public static readonly Choice[] Readbacks =
        {
            new(L.T("Relaxed"), "", L.T("Default")),
            new(L.T("Off"), "0"),
            new(L.T("Precise"), "2"),
        };

        public static readonly Choice[] SoundOutput =
        {
            new("WASAPI", "", L.T("Default")),
            new("DirectSound", "directsound"),
            new(L.T("DirectSound, larger buffer"), "directsound_large"),
        };

        public static readonly Choice[] LaunchMethods =
        {
            new("Lantern Engine", "windows", L.T("Prepares mods and patches, then starts the engine (out\\bb-probe.exe)")),
            new(L.T("Custom command"), "custom", L.T("Runs a program of your choice with the settings as environment variables")),
        };

        public static int IndexOf(IReadOnlyList<Choice> choices, string value)
        {
            for (var i = 0; i < choices.Count; i++)
                if (choices[i].Value == value)
                    return i;
            // A number written another way (the engine saves "1.00" for "1"): the same choice.
            if (double.TryParse(value, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var number))
                for (var i = 0; i < choices.Count; i++)
                    if (double.TryParse(choices[i].Value, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var option) &&
                        Math.Abs(option - number) < 1e-6)
                        return i;
            return 0;
        }
    }
}
