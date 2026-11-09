using Avalonia;
using Avalonia.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>Upscaling and the game's own effects (patches applied at start).</summary>
    public sealed class GraphicsPage : LauncherPage
    {
        private readonly TextBlock _upscalerHint, _dlssHint;
        private TextBlock? _motionBlurHint;
        private readonly SettingRow _presetCard, _renderScaleCard, _sharpenCard, _sharpnessCard, _mipBiasCard;
        private readonly Control _enginePreset;
        private readonly Dictionary<string, ToggleSwitch> _features = new();
        private bool _syncing;

        public GraphicsPage() : base(L.T("Graphics"),
            L.T("Stored in lantern.ini; the upscaler can also be changed in game (Insert or L3 + R3)."))
        {
            Add(Card(SymbolRegular.Gauge24, L.T("Best settings for this PC"), Hardware.Detect().Summary,
                SmallButton(L.T("Recommend…"), SymbolRegular.Sparkle24, (_, _) => MainWindow.Instance!.OfferBestSettings(firstRun: false)), out _));

            Group(L.T("Upscaler"));
            // FSR 4.1.1 runs through fsr4vk (in the package) or an fsr4_411 set built from the player's
            // own AMD DLL: offered when either is there. Without both the setting becomes FSR 4, the
            // model it would fall back to (#22).
            var fsr411 = Fsr4VkFound() || Directory.Exists(Fsr411Dir());
            if (Ini.Get("upscaler") == "fsr") Ini.Set("upscaler", "fsr4");
            if (!fsr411 && Ini.Get("upscaler") == "fsr411") Ini.Set("upscaler", "fsr4");
            IniCombo(SymbolRegular.Sparkle24, L.T("Upscaler"), null,
                fsr411 ? Choices.Upscalers : Choices.Upscalers.Where(c => c.Value != "fsr411").ToArray(), "upscaler", changed: UpdateUpscaler);
            _upscalerHint = (TextBlock)((SettingRow)Body.Children[^1]).Tag!;

            IniCombo(SymbolRegular.ZoomIn24, L.T("Quality preset"), L.T("How far below the output resolution the game renders"),
                Choices.Presets, "preset", changed: UpdateUpscaler);
            _presetCard = (SettingRow)Body.Children[^1];
            SliderRow(SymbolRegular.SlideSize24, L.T("Render scale (Custom preset)"),
                L.T("Output size divided by this is the render size: 1.00 renders at the output, 1.30 a little below, 3.00 like Ultra Performance. FSR 4 and DLSS use the nearest preset's model"),
                1.0, 3.0, 0.05, () => ParseDouble(Ini.Get("render_scale", "1.30"), 1.3), v => Ini.Set("render_scale", Fmt(v)), format: "x0.00", valueWidth: 56);
            _renderScaleCard = (SettingRow)Body.Children[^1];

            IniToggle(SymbolRegular.Eyedropper24, L.T("Sharpening (RCAS)"), L.T("Works with every option above, TAA included: turn it up if the picture looks soft"), "sharpen");
            _sharpenCard = (SettingRow)Body.Children[^1];
            SliderRow(SymbolRegular.SlideSize24, L.T("Sharpening strength"), L.T("Up to 1: the upscaler's own sharpening. Above 1: an extra pass"), 0, 2, 0.05,
                () => ParseDouble(Ini.Get("sharpness"), 0.5), v => Ini.Set("sharpness", Fmt(v)));
            _sharpnessCard = (SettingRow)Body.Children[^1];
            SliderRow(SymbolRegular.Image24, L.T("Texture mip bias"),
                L.T("Added to the texture detail the upscaler asks for: below 0 sharper textures (more shimmer), above 0 softer. 0 is the upscaler's own"),
                -2, 1, 0.05, () => ParseDouble(Ini.Get("mip_bias", "0"), 0), v => Ini.Set("mip_bias", Fmt(v)), format: "+0.00;-0.00;0.00", valueWidth: 56);
            _mipBiasCard = (SettingRow)Body.Children[^1];

            IniCombo(SymbolRegular.FastForward24, L.T("Frame generation"),
                L.T("Shows an extra frame between the game's own (2x smoother motion). Adds a little input lag. Off by default."),
                Choices.FrameGeneration, "frame_gen");
            IniToggle(SymbolRegular.Timer24, L.T("Frame generation: game at half refresh rate"),
                L.T("Every generated frame reaches the screen evenly (60 + 60 at 120 Hz). Off: the game runs faster, but the screen skips some generated frames"), "frame_gen_cap");

            IniCombo(SymbolRegular.BrainCircuit24, L.T("DLSS model"),
                L.T("Which NVIDIA model DLSS runs. Applied on the next launch"), Choices.DlssModels, "dlss_preset");
            ToggleRow(SymbolRegular.BrightnessHigh24, L.T("DLSS auto exposure"),
                L.T("DLSS works out each frame's exposure itself, for the game's finished frame too. Can calm ghosting in dark scenes"),
                () => Ini.Get("dlss_auto_exposure", "0") == "1", v => Ini.Set("dlss_auto_exposure", v ? "1" : "0"));
            ToggleRow(SymbolRegular.Image24, L.T("DLSS sharper textures"),
                L.T("One more mip level of texture detail under DLSS, as NVIDIA's guide asks. Off: the render-to-output ratio only"),
                () => Ini.Get("dlss_mip_bias", "1") != "0", v => Ini.Set("dlss_mip_bias", v ? "1" : "0"));
            IniToggle(SymbolRegular.Flash24, L.T("AMD Anti-Lag 2"),
                L.T("Lower input lag on AMD drivers that offer it; does nothing on other GPUs. Turns itself off until the next start if the driver holds the game too long"),
                "anti_lag");
            IniToggle(SymbolRegular.ArrowMove24, L.T("Object motion vectors"), L.T("Less ghosting on characters; costs about 10% FPS"), "object_motion");
            IniToggle(SymbolRegular.DataUsage24, L.T("Show FPS"), null, "show_fps");
            IniToggle(SymbolRegular.Wrench24, L.T("Note while shaders build"),
                L.T("\"Building shaders\" in a corner while the game builds shaders for a place it shows for the first time. Also in game: Insert or L3 + R3"),
                "compile_note");
            // The FPS counter's look (live in game too: Insert or L3 + R3 › Other).
            IniCombo(SymbolRegular.PositionToFront24, L.T("FPS counter position"), null, new Choice[]
            {
                new(L.T("Top right"), "0"), new(L.T("Top left"), "1"), new(L.T("Bottom right"), "2"), new(L.T("Bottom left"), "3"), new(L.T("Top centre"), "4"),
            }, "fps_position");
            IniSlider(SymbolRegular.TextFontSize24, L.T("FPS counter size"), 0.75, 2.0, "fps_scale");
            IniCombo(SymbolRegular.Color24, L.T("FPS counter text colour"), L.T("Any colour in game: Insert or L3 + R3 › Other"), new Choice[]
            {
                new(L.T("White"), "#FFFFFF"), new(L.T("Yellow"), "#FFD54A"), new(L.T("Green"), "#7CFC8A"), new(L.T("Cyan"), "#6FE3FF"),
                new(L.T("Red"), "#FF5A5A"), new(L.T("Orange"), "#FFA040"), new(L.T("Pink"), "#FF8AD8"), new(L.T("Black"), "#000000"),
            }, "fps_text_color");
            IniSlider(SymbolRegular.Eye24, L.T("FPS counter text opacity"), 0.1, 1.0, "fps_text_opacity", fallback: 1.0);
            IniCombo(SymbolRegular.ColorBackground24, L.T("FPS counter background"), null, new Choice[]
            {
                new(L.T("Dark grey"), "#0F0F0F"), new(L.T("Black"), "#000000"), new(L.T("Dark red"), "#3A0A0A"), new(L.T("Dark blue"), "#0A1A3A"), new(L.T("White"), "#FFFFFF"),
            }, "fps_bg_color");
            IniSlider(SymbolRegular.Blur24, L.T("FPS counter background opacity"), 0.0, 1.0, "fps_bg_opacity", fallback: 0.5);
            IniToggle(SymbolRegular.BorderAll24, L.T("FPS counter border"), null, "fps_border");
            // What the counter shows (on unless turned off), folded under the switch.
            var fpsParts = Advanced(L.T("What the FPS counter shows"));
            foreach (var (icon, key, title) in new[]
                     {
                         (SymbolRegular.TopSpeed24, "show_fps_rate", L.T("Frame rate")),
                         (SymbolRegular.Timer24, "show_fps_frame_time", L.T("Frame time")),
                         (SymbolRegular.ResizeLarge24, "show_fps_upscaler", L.T("Upscaler")),
                         (SymbolRegular.DeveloperBoard24, "show_fps_cpu", L.T("CPU usage")),
                         (SymbolRegular.Gauge24, "show_fps_gpu", L.T("GPU usage")),
                     })
            {
                // CPU and GPU use are off unless chosen (the engine's defaults, bbport_settings.h).
                var fallback = key is "show_fps_cpu" or "show_fps_gpu" ? "0" : "1";
                ToggleRow(icon, title, L.T("Shown in the FPS counter"), () => Ini.Get(key, fallback) == "1", v => Ini.Set(key, v ? "1" : "0"), fpsParts);
                fpsParts.Children[^1].Margin = new Thickness(24, 0, 0, 0);
            }

            Add(Card(SymbolRegular.DeveloperBoard24, "DLSS", "", null, out _dlssHint));

            // The engine's picture and camera controls (bbport_settings.cpp clamps the same ranges).
            Group(L.T("Picture"));
            var picture = new StackPanel();
            IniToggle(SymbolRegular.Color24, L.T("Picture tuning"), L.T("Turns the settings below on, live. Also in the game's System menu (PC Graphics, PC Effects, PC Colour) and the Insert menu"), "graphics_controls");
            IniToggle(SymbolRegular.CircleHalfFill24, L.T("Vignette"), null, "graphics_vignette", picture);
            IniSlider(SymbolRegular.DarkTheme24, L.T("Ambient occlusion strength"), 0, 3, "graphics_ao_strength", picture);
            IniSlider(SymbolRegular.WeatherSunny24, L.T("Shadow distance"), 0.5, 4, "graphics_shadow_scale", picture);
            IniSlider(SymbolRegular.Lightbulb24, L.T("Bloom"), 0, 3, "graphics_bloom", picture);
            IniSlider(SymbolRegular.Color24, L.T("Saturation"), 0, 3, "graphics_saturation", picture);
            IniSlider(SymbolRegular.BrightnessHigh24, L.T("Brightness"), 0, 3, "graphics_brightness", picture);
            IniSlider(SymbolRegular.DarkTheme24, L.T("Contrast"), 0.8, 1.2, "graphics_contrast", picture, step: 0.01);
            IniSlider(SymbolRegular.Color24, L.T("Hue"), -180, 180, "graphics_hue", picture, step: 1, fallback: 0);
            IniSlider(SymbolRegular.WeatherFog24, L.T("Fog"), 0, 3, "graphics_fog", picture);
            Body.Children.Add(picture);

            Group(L.T("Menus"));
            _ui4k = ToggleRow(SymbolRegular.ZoomIn24, L.T("4K menus"),
                L.T("Menus, icons and item pictures upscaled 4x on your PC from your own game files. Nothing is downloaded but the upscaler"),
                () => UiUpscale.Ready && Ini.Get("ui_4k", "1") != "0", v =>
                {
                    Ini.Set("ui_4k", v ? "1" : "0");
                    if (v && !UiUpscale.Ready) _ = BuildUi4kAsync();
                });
            _ui4kButton = SmallButton(L.T("Build"), SymbolRegular.ArrowDownload24, async (_, _) => await BuildUi4kAsync());
            _ui4kRemove = SmallButton(L.T("Remove"), SymbolRegular.Delete24, (_, _) => RemoveUi4k());
            var ui4kButtons = new StackPanel { Orientation = Avalonia.Layout.Orientation.Horizontal };
            ui4kButtons.Children.Add(_ui4kButton);
            ui4kButtons.Children.Add(_ui4kRemove);
            Add(Card(SymbolRegular.Image24, L.T("4K menu textures"), null, ui4kButtons, out _ui4kHint));
            _ui4kBar = new ProgressBar { Height = 4, Minimum = 0, Maximum = 1, Margin = new Thickness(24, 0, 24, 8), IsVisible = false };
            Body.Children.Add(_ui4kBar);
            Body.Children.Add(Text(L.F($"Upscaled with {UiUpscale.Credit}, downloaded once from its official release ({UiUpscale.Project}). It takes a while and uses the GPU; about 3.5 GB on disk."), "Faint"));
            ShowUi4k();

            Group(L.T("Camera"));
            IniToggle(SymbolRegular.Camera24, L.T("Camera tuning"), L.T("Turns the three settings below on"), "camera_controls");
            IniSlider(SymbolRegular.ZoomOut24, L.T("Field of view"), 1, 1.5, "camera_fov_scale");
            IniSlider(SymbolRegular.ArrowMaximize24, L.T("Distance"), 0.5, 1.5, "camera_distance_scale");
            IniSlider(SymbolRegular.ArrowBidirectionalUpDown24, L.T("Height"), 0.5, 1.5, "camera_height_scale");

            // Engine features from 0.9.4, each with a switch of its own.
            Group(L.T("Experimental"));
            // Engine preset: New or Legacy (0.9.33) turns the newer features below on or off at
            // once; a feature changed by hand shows Custom.
            _enginePreset = ComboRow(SymbolRegular.Options24, L.T("Engine preset"),
                L.T("New is the current engine. Legacy runs it the way 0.9.33 did: try it if the game crashes or draws something wrong"),
                EnginePreset.Choices, () => EnginePreset.Current(S), v => EnginePreset.Apply(S, v), changed: SyncFeatures);
            var exp = Advanced(L.T("Experimental engine features"));
            SettingToggle(SymbolRegular.Code24, L.T("Legacy shader compiler"),
                L.T("Off by default: the native compiler reads constant data through the GPU's constant cache. On compiles the game's shaders the way 0.9.33 did: try it if something draws wrong. Switching recompiles every shader once"),
                "legacy_shaders", exp);
            SettingToggle(SymbolRegular.Broom24, L.T("VRAM cleanup"),
                L.T("Frees video memory held by textures the GPU drew, so long sessions don't fill up VRAM and slow down. Recommended: on"),
                "exp_vram_cleanup", exp);
            SettingToggle(SymbolRegular.Timer24, L.T("Precise frame pacing"),
                L.T("A high-resolution timer and even spacing between frames: steadier frame times and less VRR flicker. Uses a little CPU. On by default"),
                "exp_frame_pacing", exp);
            Feature(SymbolRegular.Storage24, L.T("Use more VRAM for textures"),
                L.T("Keeps textures in video memory up to 80% of the card instead of about 40%, so they aren't thrown out and loaded again (the cause of most stutters on 8 GB cards)"),
                "exp_vram_budget", exp);
            SettingToggle(SymbolRegular.TopSpeed24, L.T("Graphics threads first"),
                L.T("Runs the engine's graphics threads at a higher CPU priority than the game's other threads, so a frame isn't held up behind them. On by default"),
                "exp_thread_priority", exp);
            SettingToggle(SymbolRegular.Sleep24, L.T("Shorter sleep spin"),
                L.T("The game's many short sleeps busy-wait only as long as the timer needs (about 0.2–0.4 ms instead of 0.6 ms), leaving that CPU time to the graphics threads. On by default"),
                "exp_sleep_spin", exp);
            SettingToggle(SymbolRegular.FlashFlow24, L.T("Background shader compiling"),
                L.T("New shaders compile on helper threads instead of freezing the frame: much shorter hitches when you reach a new area. On by default"),
                "exp_async_pipelines", exp);
            SettingToggle(SymbolRegular.EyeOff24, L.T("Occlusion culling"),
                L.T("Off by default. The game skips objects hidden behind walls: higher FPS in dense areas, but objects and floors can appear late or flicker"),
                "exp_occlusion", exp);
            Feature(SymbolRegular.FastForward24, L.T("Faster draw thread"),
                L.T("Remembers texture and sampler lookups and hands the end of each frame to the draw thread: higher FPS in busy areas. New: turn it off if the game hangs or crashes"),
                "exp_fast_draws", exp);
            Feature(SymbolRegular.Shapes24, L.T("No shader freezes"),
                L.T("While a new shader compiles, the objects that need it appear a moment later instead of the game freezing. New: turn it off if something stays missing"),
                "exp_compile_skip", exp);
            SettingToggle(SymbolRegular.ArrowForward24, L.T("Fast loading"),
                L.T("Loading screens run at 30 FPS: fewer frames leave the CPU to the loading work itself. Not before cutscenes. On by default"),
                "exp_fast_loading", exp);
            SettingToggle(SymbolRegular.Flash24, L.T("Fewer memory write checks"),
                L.T("Memory the game rewrites before almost every use is no longer watched for changes, saving up to about 2 ms a frame in busy areas. On by default"),
                "exp_hot_pages", exp);
            SettingToggle(SymbolRegular.Storage24, L.T("PC memory model"),
                L.T("The GPU reads the game's memory where it is instead of copies, so far less memory is watched and copied: more of the CPU and GPU used. On by default; turn it off if textures look wrong or the game stutters"),
                "exp_guest_in_place", exp);
            SettingToggle(SymbolRegular.Code24, L.T("Native draw calls"),
                L.T("The game's draw calls reach the engine directly instead of as console GPU commands it decodes again. On by default; turn it off if something stops drawing"),
                "exp_native_calls", exp);
            SettingToggle(SymbolRegular.ShieldCheckmark24, L.T("Effect trail crash fix"),
                L.T("For crashes while drawing effect trails on newer Intel CPUs (Witch of Hemwick, for example): stops the crash, and that kind of trail is not drawn. Off by default"),
                "exp_sfx_workaround", exp);
            SettingToggle(SymbolRegular.Camera24, L.T("Camera easing for high FPS"),
                L.T("With the FPS++ follow camera only: its eases converted for 60 FPS and above. Off by default"),
                "exp_camera_easing", exp);
            SettingToggle(SymbolRegular.Run24, L.T("Sharper motion (FSR 4)"),
                L.T("Uses FSR 4's balanced model at Performance and Ultra Performance: fewer trails behind moving characters, a little more GPU time"),
                "fsr4_balanced", exp);

            Group(L.T("Game effects"));
            Body.Children.Add(Text(L.T("Patches to the game, applied when it starts."), "Faint"));
            IniCombo(SymbolRegular.Cube24, L.T("Model detail"), null, Choices.ModelLod, "model_lod");
            IniCombo(SymbolRegular.Camera24, L.T("Follow camera"), L.T("Original keeps the game's camera distance, angle and follow speed at 60 FPS and above"), Choices.CameraTuning, "camera_tuning");
            foreach (var effect in Choices.Effects)
            {
                var motionBlur = effect.Key == "effect_motion_blur";
                IniToggle(EffectIcon(effect.Key), effect.Title, effect.Hint, effect.Key, changed: motionBlur ? UpdateUpscaler : null);
                if (motionBlur) _motionBlurHint = ((SettingRow)Body.Children[^1]).Hint;
                if (effect.Key == "effect_gtao")
                    IniCombo(SymbolRegular.Color24, L.T("Ambient occlusion quality"),
                        L.T("Regular GTAO is unchanged. High traces three visibility-bitmask GTAO directions at the full internal render resolution (follows DLSS/FSR scaling) and applies a depth/normal-aware denoiser for sharper, more stable contact shadows. Higher GPU cost; needs Better ambient occlusion (GTAO) on. Requires restarting Bloodborne."),
                        Choices.AoQualities, "graphics_ao_quality");
            }

            Group(L.T("Lighting"));
            IniCombo(SymbolRegular.Lightbulb24, L.T("Sun shadow resolution"), L.T("Applied when the game starts"), Choices.ShadowResolution, "graphics_shadow_resolution");
            IniCombo(SymbolRegular.Lightbulb24, L.T("Lamp shadow resolution"), L.T("Applied when the game starts"), Choices.LocalShadowResolution, "graphics_local_shadow_resolution");
            IniCombo(SymbolRegular.Lightbulb24, L.T("Motion blur quality"), L.T("Applied when the game starts"), Choices.MotionBlurQuality, "graphics_motion_blur_quality");
            IniSlider(SymbolRegular.Lightbulb24, L.T("Indirect lighting strength"), 0, 12, "graphics_gi_strength", fallback: 6);
            IniSlider(SymbolRegular.Lightbulb24, L.T("Reflection strength"), 0, 3, "graphics_ssr_intensity");
            IniSlider(SymbolRegular.Lightbulb24, L.T("Volumetric sunlight strength"), 0, 3, "graphics_volumetric_intensity");
            IniSlider(SymbolRegular.Lightbulb24, L.T("Lamp fog strength"), 0, 3, "graphics_point_fog_intensity");

            Ws.PortChanged += UpdateUpscaler;
        }

        private readonly ToggleSwitch _ui4k;
        private readonly AppButton _ui4kButton, _ui4kRemove;
        private readonly TextBlock _ui4kHint;
        private readonly ProgressBar _ui4kBar;
        private CancellationTokenSource? _ui4kBuild;

        private void ShowUi4k(string? status = null)
        {
            var building = _ui4kBuild != null;
            _ui4kButton.Text = building ? L.T("Cancel") : UiUpscale.Ready ? L.T("Rebuild") : L.T("Build");
            _ui4kRemove.IsVisible = !building && (UiUpscale.Ready || Directory.Exists(UiUpscale.PackDir));
            _ui4kBar.IsVisible = building;
            _ui4k.IsChecked = UiUpscale.Ready && Ini.Get("ui_4k", "1") != "0";
            SetHint(_ui4kHint, status ?? (UiUpscale.Ready
                ? L.F($"Ready ({UiUpscale.PackBytes() / 1073741824.0:0.0} GB). Turn 4K menus off above to use the game's own")
                : L.T("Not built yet. Build makes them from your game files")));
        }

        private async Task BuildUi4kAsync()
        {
            if (_ui4kBuild is { } running)
            {
                running.Cancel();
                return;
            }
            if (Ws.Game.Running)
            {
                Prompt.Tell(L.T("4K menus"), L.T("Close the game first: the upscaler needs the GPU."));
                return;
            }
            _ui4kBuild = new CancellationTokenSource();
            ShowUi4k(L.T("Starting…"));
            var progress = new Progress<UiUpscale.Step>(step =>
            {
                _ui4kBar.Value = step.Fraction;
                SetHint(_ui4kHint, step.Text);
            });
            try
            {
                await UiUpscale.BuildAsync(GameEnvironment.GameDir(S), progress, _ui4kBuild.Token);
                Ini.Set("ui_4k", "1");
                Ws.SaveSoon();
                Ws.Log(L.T("4K menus are ready: they show the next time the game starts"), LogLevel.Good);
            }
            catch (OperationCanceledException)
            {
                Ws.Log(L.T("4K menus: stopped. Build again to continue where it stopped"), LogLevel.Step);
            }
            catch (Exception e)
            {
                Ws.Log(L.F($"4K menus: {e.Message}"), LogLevel.Warn);
                Prompt.Tell(L.T("4K menus"), e.Message);
            }
            finally
            {
                _ui4kBuild.Dispose();
                _ui4kBuild = null;
                ShowUi4k();
            }
        }

        private void RemoveUi4k()
        {
            try
            {
                UiUpscale.Remove();
                Ini.Set("ui_4k", "0");
                Ws.SaveSoon();
            }
            catch (IOException e)
            {
                Prompt.Tell(L.T("4K menus"), e.Message);
            }
            ShowUi4k();
        }

        private void IniSlider(SymbolRegular icon, string title, double min, double max, string key, Panel? into = null,
            double step = 0.05, double fallback = 1.0) =>
            SliderRow(icon, title, null, min, max, step, () => ParseDouble(Ini.Get(key, Fmt(fallback)), fallback), v => Ini.Set(key, Fmt(v)), into: into);

        /// <summary>A switch the engine preset sets: changing it by hand shows the preset it now matches.</summary>
        private void Feature(SymbolRegular icon, string title, string hint, string key, Panel into) =>
            _features[key] = SettingToggle(icon, title, hint, key, into, changed: SyncPreset);

        /// <summary>The preset was picked: its switches show their new values.</summary>
        private void SyncFeatures()
        {
            foreach (var (key, toggle) in _features)
                toggle.IsChecked = S.Bool(key);
            SyncPreset();
        }

        private void SyncPreset()
        {
            if (_syncing) return;
            _syncing = true;
            var index = Choices.IndexOf(EnginePreset.Choices, EnginePreset.Current(S));
            if (_enginePreset is Segmented segmented) segmented.Select(index, notify: false);
            else if (_enginePreset is ComboBox box) box.SelectedIndex = index;
            _syncing = false;
        }

        private static SymbolRegular EffectIcon(string key) => key switch
        {
            "skip_intro" => SymbolRegular.FastForward24,
            "debug_camera" => SymbolRegular.Camera24,
            "debug_menu" => SymbolRegular.Bug24,
            _ => SymbolRegular.Color24,
        };

        public override void OnShown() => UpdateUpscaler();

        private static bool DlssFound() =>
            Paths.DlssRuntime != null; // the engine has DLSS built in; NVIDIA's runtime sits beside it

        private void UpdateUpscaler()
        {
            var value = Ini.Get("upscaler");
            string? hint = value switch
            {
                "fsr4" => Directory.Exists(Path.Combine(Paths.EngineDir, "fsr4_shaders"))
                    ? L.T("Assets found. GPUs without the needed shader features fall back to FSR 3.1")
                    : L.T("fsr4_shaders is missing from the port folder: the game falls back to FSR 3.1"),
                "fsr411" when Fsr4VkFound() =>
                    L.T("Through fsr4vk (included, experimental): runs on any GPU with the needed Vulkan features. Prefer FSR 4 if it looks wrong"),
                "fsr411" => Fsr411Problem() is { } problem
                    ? L.F($"{problem}: the game falls back to FSR 4")
                    : L.T("Assets for the chosen resolution and preset found"),
                "dlss" => DlssFound()
                    ? L.T("Needs an NVIDIA GeForce RTX GPU; others fall back to FSR 3.1") +
                      (Ini.Get("preset", "0") == "0" ? L.T(". With Native AA this is DLAA: DLSS as anti-aliasing at full resolution") : "")
                    : L.T("DLSS files are missing: the game falls back to FSR 3.1"),
                "taa" => L.T("Anti-aliasing at the output resolution, without an FSR model. Softer than DLAA or FSR Native AA: add sharpening below"),
                _ => null,
            };
            SetHint(_upscalerHint, hint);
            _presetCard.IsEnabled = value is not ("taa" or "off");
            _renderScaleCard.IsVisible = Ini.Get("preset", "0") == "5";
            _renderScaleCard.IsEnabled = value is not ("taa" or "off");
            _sharpenCard.IsEnabled = value != "off";
            _sharpnessCard.IsEnabled = value != "off";
            _mipBiasCard.IsEnabled = value != "off";

            // An upscaler takes the finished frame at outputs other than 1920x1080 and can smear the
            // game's motion blur; More > Engine can leave the blur out there (BB_UPSCALER_NO_BLUR).
            var (w, h) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            if (_motionBlurHint != null)
                SetHint(_motionBlurHint, value != "off" && (w, h) != (1920, 1080) && Ini.Flag("effect_motion_blur")
                    ? L.T("With an upscaler at an output other than 1920x1080 the blur can smear. More > Engine > No motion blur smear with upscalers leaves it out there")
                    : null);

            // Linux: libnvidia-ngx-dlss.so.<version>, DLSS Super Resolution only (Streamline's frame
            // generation is Windows only).
            var ngx = Paths.DlssRuntime;
            SetHint(_dlssHint, ngx != null
                ? L.F($"NVIDIA DLSS {Paths.DlssVersion(ngx)} is included ({Path.Combine("out", Path.GetFileName(ngx))}). Choose it as the upscaler above")
                : L.F($"{Path.Combine("out", OperatingSystem.IsWindows() ? "nvngx_dlss.dll" : "libnvidia-ngx-dlss.so")} is missing from the engine folder"));
        }

        /// <summary>The fsr4vk provider the package ships (FSR 4.1.1 on any GPU, vk_fsr4vk.cpp).</summary>
        private static bool Fsr4VkFound() =>
            File.Exists(Path.Combine(Paths.EngineDir, "fsr4vk", "amd_fidelityfx_upscaler_vk.dll"));

        private static string Fsr411Dir()
        {
            var env = Environment.GetEnvironmentVariable("BB_FSR411_DIR");
            if (!string.IsNullOrEmpty(env)) return env;
            var local = Path.Combine(Paths.EngineDir, "fsr4_411");
            return Directory.Exists(local) ? local : Path.Combine(Paths.DataDir, "fsr4_411");
        }

        /// <summary>The FSR 4.1.1 model files the chosen output and preset need (fsr4_411\).</summary>
        private static string? Fsr411Problem()
        {
            var (w, h) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            var model = (w > 1920 || h > 1080 ? "t2160" : "t1080") + (Ini.Get("preset", "4") == "4" ? "_m1" : "_m0");
            var passes = new List<string> { "spd", "prepass", "pass0_post" };
            for (var i = 1; i <= 12; i++)
            {
                passes.Add($"pass{i}");
                passes.Add($"pass{i}_post");
            }
            passes.Add("postpass");
            passes.Add("rcas");

            var dir = Path.Combine(Fsr411Dir(), model);
            foreach (var name in passes)
            {
                var file = new FileInfo(Path.Combine(dir, name + ".spv"));
                if (!file.Exists || file.Length < 20 || file.Length % 4 != 0)
                    return L.F($"Missing or damaged fsr4_411\\{model}\\{name}.spv");
            }
            var init = new FileInfo(Path.Combine(dir, "initializer.bin"));
            return init.Exists && init.Length == 131072 ? null : L.F($"Missing or damaged fsr4_411\\{model}\\initializer.bin");
        }
    }
}
