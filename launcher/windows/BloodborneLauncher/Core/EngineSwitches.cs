using System.Globalization;

namespace BloodborneLauncher.Core
{
    public enum SwitchKind { Toggle, Number, Choice, Text }

    /// <summary>
    /// One BB_ environment variable the engine reads, as the Engine page shows it. Default is the
    /// value that means "the engine's default": the variable is then not passed at all, so a
    /// switch that is off by default is left unset rather than written as 0. Toggles write On or
    /// Off; numbers a value from Min to Max; choices one of Options' values; text what is typed
    /// ("" is only ever the default: the engine decides).
    /// </summary>
    public sealed record EngineSwitch(string Env, string Group, string Title, string Hint, SwitchKind Kind, string Default)
    {
        public string On { get; init; } = "1";
        public string Off { get; init; } = "0";
        public double Min { get; init; }
        public double Max { get; init; }
        public double Step { get; init; } = 1;
        public string Unit { get; init; } = "";
        public Choice[] Options { get; init; } = Array.Empty<Choice>();
        public string Placeholder { get; init; } = "";
        /// <summary>Passed even at its default (a launcher default that differs from the engine's).</summary>
        public bool AlwaysPass { get; init; }

        /// <summary>Its key in settings.json.</summary>
        public string Key => "env:" + Env;

        /// <summary>The slider's number format.</summary>
        public string Format => Step >= 1 ? "0" : Step >= 0.1 ? "0.0" : "0.00";
    }

    /// <summary>
    /// The engine's user-facing switches that no other launcher setting covers. Values live in
    /// settings.json as "env:BB_NAME"; GameEnvironment passes only those changed from the default,
    /// and Extra variables win over them. Each default is the engine's own (checked against the
    /// getenv sites in engine/src, engine/start and engine/gpu), so nothing changes until a switch
    /// is changed. Left out on purpose: what the launcher or bb-start sets itself (folders, online
    /// accounts, resolutions, frame rate), what a setting elsewhere already covers, what only the
    /// Linux build reads, and the internal logs, traces, A/B tests and tuning knobs for engine
    /// development (Extra variables still takes those).
    /// </summary>
    public static class EngineSwitches
    {
        public const string Graphics = "Graphics";
        public const string Performance = "Performance";
        public const string Memory = "Memory and VRAM";
        public const string Upscalers = "Upscalers";
        public const string Online = "Online";
        public const string Input = "Input";
        public const string Sound = "Sound";
        public const string Fixes = "Game fixes";
        public const string Diagnostics = "Diagnostics and logs";
        public const string DirectVulkan = "Direct rendering";

        public static readonly string[] Groups = { Graphics, Performance, Input, Online, Sound, Fixes };

        // ---------------------------------------------------------------- building the list

        private static EngineSwitch Toggle(string env, string group, string title, string hint, bool on, string onValue = "1", string offValue = "0") =>
            new(env, group, title, hint, SwitchKind.Toggle, on ? onValue : offValue) { On = onValue, Off = offValue };

        /// <summary>A switch that is off unless set: on writes onValue, off leaves it unset.</summary>
        private static EngineSwitch Flag(string env, string group, string title, string hint, string onValue = "1") =>
            Toggle(env, group, title, hint, on: false, onValue, offValue: "");

        /// <summary>A switch that is on unless it is 0.</summary>
        private static EngineSwitch OnUnless0(string env, string group, string title, string hint) =>
            Toggle(env, group, title, hint, on: true);

        private static EngineSwitch OffUnless1(string env, string group, string title, string hint) =>
            Toggle(env, group, title, hint, on: false);

        private static EngineSwitch Number(string env, string group, string title, string hint, double value, double min, double max,
            double step, string unit = "") =>
            new(env, group, title, hint, SwitchKind.Number, value.ToString(step >= 1 ? "0" : "0.###", CultureInfo.InvariantCulture))
            { Min = min, Max = max, Step = step, Unit = unit };

        private static EngineSwitch Pick(string env, string group, string title, string hint, string value, params Choice[] options) =>
            new(env, group, title, hint, SwitchKind.Choice, value) { Options = options };

        private static EngineSwitch Text(string env, string group, string title, string hint, string placeholder) =>
            new(env, group, title, hint, SwitchKind.Text, "") { Placeholder = placeholder };

        private static Choice[] Auto(string note, params string[] values) =>
            new[] { new Choice(L.T("Auto"), "", note) }.Concat(values.Select(v => new Choice(v, v))).ToArray();

        private static readonly Choice[] AutoOnOff = { new(L.T("Auto"), ""), new(L.T("On"), "1"), new(L.T("Off"), "0") };

        public static readonly string[] NativeSwitches =
            { "LANTERN_GX_DRAWS", "LANTERN_GX_TEXTURES", "LANTERN_GX_BUFFERS", "LANTERN_GX_RECORDS", "LANTERN_GX_NATIVE", "LANTERN_READ_FILTER", "LANTERN_LIFTER" };

        public static readonly IReadOnlyList<EngineSwitch> All = new[]
        {
            OffUnless1("LANTERN_GX_DRAWS", Performance, L.T("Native renderer: binds"), L.T("Lantern's own resource binds. Off by default while the native renderer is rebuilt: today it adds work to every draw")),
            OffUnless1("LANTERN_GX_TEXTURES", Performance, L.T("Native renderer: textures"), L.T("Images and depth targets taken from the game's own texture objects. Off by default while the native renderer is rebuilt: today it adds work to every draw")),
            OffUnless1("LANTERN_GX_BUFFERS", Performance, L.T("Native renderer: buffers"), L.T("The game's upload memory stays in place. Off by default while the native renderer is rebuilt: today it adds work to every draw")),
            OffUnless1("LANTERN_GX_RECORDS", Performance, L.T("Native renderer: pipelines"), L.T("Each draw's pipeline chosen from the game's own state. Off by default while the native renderer is rebuilt: today it adds work to every draw")),
            OffUnless1("LANTERN_GX_NATIVE", Performance, L.T("Native renderer: draws"), L.T("Bindings, render state, vertex and index buffers taken from the game's own objects instead of its command buffers. Needs Native renderer: pipelines. Off by default while the native renderer is rebuilt: today it adds work to every draw")),
            OnUnless0("LANTERN_READ_FILTER", Performance, L.T("Native renderer: read tracking"), L.T("Watches fewer memory reads. Turn off if something draws wrong or crashes")),
            OnUnless0("LANTERN_LIFTER", Performance, L.T("Native shader compiler"), L.T("Lantern's own shader compiler. Switching recompiles every shader once. Turn off if something draws wrong or crashes")),
            Pick("BB_GPU_ID", Graphics, L.T("Graphics card"),
                L.T("For laptops and PCs with two graphics cards where the game picks the wrong one. Auto takes the faster one"), "",
                new(L.T("Auto"), ""), new(L.T("First"), "0"), new(L.T("Second"), "1")),
            Pick("BB_ANISO", Graphics, L.T("Texture filtering"),
                L.T("Anisotropic filtering forced on the game's textures: ground and walls seen at an angle stay sharp. Lower ") +
                L.T("values cost a little less GPU time; Game's own leaves the game's filtering as it is"), "16",
                new("16x", "16"), new("8x", "8"), new("4x", "4"), new("2x", "2"), new(L.T("Game's own"), "0")),
            OnUnless0("BB_VOLUMETRIC_STEADY", Graphics, L.T("Steady sunlight color"),
                L.T("Keeps the sunlight fog's color steady: it ignores the game's default white light and fades to new colors, so it ") +
                L.T("does not flicker as the camera moves. Off: the original color, which can flip to white and back. Needs Volumetric ") +
                L.T("sunlight on")),
            Flag("BB_UPSCALER_NO_BLUR", Graphics, L.T("No motion blur smear with upscalers"),
                L.T("With an upscaler at an output other than 1920x1080 the game's motion blur is left out: the upscaler would ") +
                L.T("smear it over the whole picture. Off (default): the blur stays. The Motion blur setting is not changed")),
            OnUnless0("BB_POINT_FOG_STEADY", Graphics, L.T("Steady lamp fog"),
                L.T("Keeps each lamp's fog on after the game stops listing the lamp, with slow fades, so it does not flicker as the camera moves. ") +
                L.T("Can leave fog showing through a wall. Off: the original fog, from the first 64 lights the game lists. Needs Fog around lamps on")),

            Pick("BB_PIPELINE_THREADS", Performance, L.T("Shader compile threads"),
                L.T("How many threads compile new shaders in the background. More finish a new area's shaders sooner but take CPU ") +
                L.T("time from the game while they work"), "",
                Auto(L.T("A third of the CPU threads, at most 4"), "1", "2", "3", "4", "6", "8")),
            Pick("BB_FILE_PREFETCH", Performance, L.T("Area prefetch (hard disks)"),
                L.T("When the game starts loading a map area, a background thread reads that area's model files into the Windows file ") +
                L.T("cache in big sequential reads, so models and textures stop appearing late in front of you. Auto: on when the game ") +
                L.T("is installed on a hard disk (an SSD does not need it)"), "", AutoOnOff),
            Pick("BB_BACKGROUND_FPS", Performance, L.T("Frame rate in the background"),
                L.T("The frame-rate limit while the game window is in the background or minimized, to save power and heat. Online ") +
                L.T("play is never limited, so co-op and invasions keep their pace"), "30",
                new("10 FPS", "10"), new("15 FPS", "15"), new("30 FPS", "30"), new("60 FPS", "60"), new(L.T("No limit"), "0")),
            OnUnless0("LANTERN_FRAMERATE", Performance, L.T("Native frame rate"),
                L.T("Lantern Engine sets the game's frame rate itself: every per-frame step follows the time a frame really takes, ") +
                L.T("so physics, stamina and animations keep their real speed when a scene runs slower. Off: the older frame-rate ") +
                L.T("patches with fixed steps")),
            OnUnless0("LANTERN_FRAMERATE_FOLLOW", Performance, L.T("Steps follow the frame rate"),
                L.T("With the native frame rate: the game's steps take the frame rate it actually holds. Off: they stay at the ") +
                L.T("chosen frame rate, and the game slows down when it runs below it")),
            OnUnless0("LANTERN_MEMORY_RETRY_LIMIT", Performance, L.T("Memory retry limit"),
                L.T("With the PC memory model: game data the processor keeps rewriting stays in system memory after three tries ") +
                L.T("instead of moving back and forth for ever, which slowed the game down the longer you played. Off: no limit")),




            OffUnless1("BB_SEAMLESS_DEATHS", Online, L.T("Seamless co-op: spectate and party wipe"),
                L.T("Experimental, never run with two hunters. In a boss fight a hunter who dies watches the others through a spectator ") +
                L.T("camera, and a party wipe brings everyone back at the lamp without a loading screen. Off: a fallen hunter stays down ") +
                L.T("until the boss dies, and a party wipe sends everyone back to the lamp through a loading screen")),
            OffUnless1("LANTERN_CAMERA_TIMING", Input, L.T("Camera follows the frame time"),
                L.T("Above 30 FPS the game's camera turns, follows and eases once per frame, so with a controller it moves faster ") +
                L.T("and less evenly the higher the frame rate. On: those steps follow the frame time. Off: the game's own camera")),
            Text("BB_GAMEPAD", Input, L.T("Preferred gamepad"),
                L.T("Part of a controller's name (DualSense, Xbox, 8BitDo...) or its SDL GUID: that one is used when several are ") +
                L.T("connected. Empty: the pad you use takes over"), L.T("DualSense")),

            Flag("BB_AUDIO", Sound, L.T("Run without sound"),
                L.T("Starts without any sound device (silent). Only for finding out whether a sound driver is what keeps the game ") +
                L.T("from starting or hangs it"), onValue: "none"),

            OnUnless0("BB_RESPAWN_DELAY_FIX", Fixes, L.T("No minimum loading time after death"),
                L.T("Skips the game's 12-second minimum loading screen after dying. Off: the original delay")),
            OnUnless0("LANTERN_JOB_SLOTS", Fixes, L.T("Cutscene freeze fix"),
                L.T("A physics job that boss cutscenes start (Rom, Ludwig, the Queen) gives its memory slot back when it has to try again, ") +
                L.T("so the game no longer waits for ever on a black screen. Off: the game's own behaviour")),
            OnUnless0("LANTERN_GAME_STATUS", Fixes, L.T("Live Discord status"),
                L.T("The game tells the launcher the area, the boss, your level, Blood Echoes and Insight, so the Discord status can show them. ") +
                L.T("Off: the status shows only its own lines")),
            OnUnless0("BB_SOUND_HACK", Fixes, L.T("Weapon sound save fix"),
                L.T("Sets the flag in the game's save that some weapon and player sounds need (without it they never play), before ") +
                L.T("the game reads the save. Off leaves saves untouched")),
            OnUnless0("BB_OPTISCALER_COMPAT", Fixes, L.T("OptiScaler compatibility"),
                L.T("Only when OptiScaler is installed: DLSS calls wait for the GPU queue, and one thread sends the GPU work while its frame generation is on, ") +
                L.T("so OptiScaler's hooks don't crash. Costs a little FPS with its frame generation. Off: the engine runs as without OptiScaler")),
            OnUnless0("BB_EFFECT_FIXES", Fixes, L.T("Ebrietas effect fix"),
                L.T("Replaces Ebrietas' trail effect with the one Nexus mod 41 uses, made from your own game files: her rock attacks no longer pile up thousands of rocks ") +
                L.T("(FPS falling from 90 to 40, crashes before beta 8). Off: the game's own effect. A mod that changes these effect files wins")),
            Flag("BB_RED_ZONE_FUNCTIONS", Fixes, L.T("Intel effect crash fix (test)"),
                L.T("For crashes in effects on Intel 12th-14th gen CPUs (Ebrietas, Witch of Hemwick, One Reborn, the Cainhurst " +
                    "cutscene): the two effect ribbon writers keep their data where Windows can't overwrite it. Every effect stays " +
                    "drawn. Turn off the Effect trail crash fix with it, and post your log either way")),
            Flag("BB_INTEL_TONEMAP_FIX", Fixes, L.T("Intel black tonemap fix"),
                L.T("Adds the Intel Black Tonemap Fix patch: for Intel CPUs where the picture turns black or far too dark ") +
                L.T("(the DLC areas most)")),
            Flag("BB_SKIP_GAME_CHECK", Fixes, L.T("Skip the game files check"),
                L.T("Starts with game files that are not the supported version (base game plus the 1.09 update). Other editions ") +
                L.T("may crash, and the engine's patches and PC menus may not apply")),


            // bbport: BB_NATIVE_CALLS was here too (Direct draw calls, passed even at its default):
            // passed after Engine features > Native draw calls, its 0 turned that setting off for
            // everyone. That one setting is the switch now (GameEnvironment).
        };

        // ---------------------------------------------------------------- values

        /// <summary>The value the row shows: the saved one if it is valid, else the default.</summary>
        public static string Get(LauncherSettings s, EngineSwitch item) =>
            s.Str(item.Key) is { Length: > 0 } value && Valid(item, value) ? value : item.Default;

        /// <summary>Saves a value; the default (or anything not valid, an empty value too) is not
        /// saved, so the variable is left unset.</summary>
        public static void Set(LauncherSettings s, EngineSwitch item, string value)
        {
            if (item.Kind == SwitchKind.Text) value = value.Trim();
            if (value.Length == 0 || IsDefault(item, value) || !Valid(item, value)) s.Remove(item.Key);
            else s.Set(item.Key, value);
        }

        public static void ResetAll(LauncherSettings s)
        {
            foreach (var item in All)
                s.Remove(item.Key);
        }

        /// <summary>The variables to pass: every switch with a valid value other than its default.</summary>
        public static IEnumerable<(string Name, string Value)> Changed(LauncherSettings s)
        {
            foreach (var item in All)
                if (item.AlwaysPass)
                    yield return (item.Env, Get(s, item));
                else if (s.Str(item.Key) is { Length: > 0 } value && Valid(item, value) && !IsDefault(item, value))
                    yield return (item.Env, value);
        }

        public static double NumberOf(LauncherSettings s, EngineSwitch item) => Parse(Get(s, item)) ?? DefaultNumber(item);

        public static double DefaultNumber(EngineSwitch item) => Parse(item.Default) ?? item.Min;

        public static string Format(EngineSwitch item, double value) =>
            value.ToString(item.Step >= 1 ? "0" : "0.###", CultureInfo.InvariantCulture);

        /// <summary>The ⓘ text: the hint, then the variable and its default.</summary>
        public static string Describe(EngineSwitch item)
        {
            var value = item.Kind switch
            {
                SwitchKind.Toggle => item.Default == item.On ? "on" : "off",
                SwitchKind.Number => item.Default + (item.Unit.Length > 0 ? " " + item.Unit : ""),
                SwitchKind.Text => L.T("not set"),
                _ => item.Options.FirstOrDefault(o => o.Value == item.Default)?.Label ?? item.Default,
            };
            return L.F($"{item.Hint}\n{item.Env}, default: {value}. Applied when the game starts");
        }

        private static double? Parse(string text) =>
            double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : null;

        private static bool IsDefault(EngineSwitch item, string value) =>
            item.Kind == SwitchKind.Number && Parse(value) is { } number && Parse(item.Default) is { } fallback
                ? Math.Abs(number - fallback) < 1e-9
                : value == item.Default;

        private static bool Valid(EngineSwitch item, string value) => item.Kind switch
        {
            SwitchKind.Toggle => value.Length > 0 && (value == item.On || value == item.Off),
            SwitchKind.Number => Parse(value) is { } number && number >= item.Min - 1e-9 && number <= item.Max + 1e-9,
            SwitchKind.Text => value.Length is > 0 and <= 260 && !value.Any(char.IsControl),
            _ => value.Length > 0 && item.Options.Any(o => o.Value == value),
        };
    }
}
