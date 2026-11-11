using System.IO;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>Upscaling and the game's own effects (patches applied at start).</summary>
    public sealed class GraphicsPage : LauncherPage
    {
        private readonly TextBlock _upscalerHint, _dlssHint, _frameGenHint;
        private TextBlock? _motionBlurHint;
        private readonly SettingRow _presetCard, _renderScaleCard, _sharpenCard, _sharpnessCard, _mipBiasCard;
        private readonly SettingRow _frameGenFramesCard, _frameGenDynamicCard, _reflexCard, _frameGenCapCard;

        public GraphicsPage() : base(L.T("Graphics"),
            L.T("Stored in lantern.ini; the upscaler can also be changed in game (Insert or L3 + R3)."))
        {
            Add(Card(SymbolRegular.Gauge24, L.T("Best settings for this PC"), Hardware.Detect().Summary,
                SmallButton(L.T("Recommend…"), SymbolRegular.Sparkle24, (_, _) => MainWindow.Instance!.OfferBestSettings(firstRun: false)), out _));

            Group(L.T("Upscaler"));
            var fsr411 = Fsr4VkFound();
            if (Ini.Get("upscaler") is "fsr4" || (!fsr411 && Ini.Get("upscaler") == "fsr411")) Ini.Set("upscaler", "fsr");
            IniCombo(SymbolRegular.Sparkle24, L.T("Upscaler"), null,
                fsr411 ? Choices.Upscalers : Choices.Upscalers.Where(c => c.Value != "fsr411").ToArray(), "upscaler", changed: UpdateUpscaler);
            _upscalerHint = (TextBlock)((SettingRow)Body.Children[^1]).Tag;

            IniCombo(SymbolRegular.ZoomIn24, L.T("Quality preset"), L.T("How far below the output resolution the game renders"),
                Choices.Presets, "preset", changed: UpdateUpscaler);
            _presetCard = (SettingRow)Body.Children[^1];
            // The Custom preset's ratio (render_scale), the engine's PresetScale: 1.0 renders at the
            // output (DLAA with DLSS), 3.0 is Ultra Performance.
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
            IniToggle(SymbolRegular.ArrowMove24, L.T("Object motion vectors"), L.T("Less ghosting on characters; costs about 10% FPS"), "object_motion");

            var dlssMore = Advanced(L.T("DLSS options"));
            IniCombo(SymbolRegular.BrainCircuit24, L.T("DLSS model"),
                L.T("Which NVIDIA model DLSS runs. Also in game (Insert or L3 + R3), where it changes at once"), Choices.DlssModels, "dlss_preset", dlssMore);
            ToggleRow(SymbolRegular.BrightnessHigh24, L.T("DLSS auto exposure"),
                L.T("DLSS works out each frame's exposure itself, for the game's finished frame too. Can calm ghosting in dark scenes"),
                () => Ini.Get("dlss_auto_exposure", "0") == "1", v => Ini.Set("dlss_auto_exposure", v ? "1" : "0"), dlssMore);
            ToggleRow(SymbolRegular.Image24, L.T("DLSS sharper textures"),
                L.T("One more mip level of texture detail under DLSS, as NVIDIA's guide asks. Off: the render-to-output ratio only"),
                () => Ini.Get("dlss_mip_bias", "1") != "0", v => Ini.Set("dlss_mip_bias", v ? "1" : "0"), dlssMore);
            Add(Card(SymbolRegular.DeveloperBoard24, "DLSS", "", null, out _dlssHint), dlssMore);

            Group(L.T("Frame generation"));
            IniCombo(SymbolRegular.FastForward24, L.T("Frame generation"),
                L.T("Extra frames between the game's own: smoother motion, a little more input lag. Needs an upscaler. Off by default"),
                Choices.FrameGeneration, "frame_gen", changed: UpdateUpscaler);
            _frameGenHint = (TextBlock)((SettingRow)Body.Children[^1]).Tag;
            IniCombo(SymbolRegular.Layer24, L.T("DLSS generated frames"),
                L.T("Frames shown per frame the game renders. More than 2x needs an RTX 50 GPU; the engine keeps to what the GPU allows"),
                Choices.FrameGenFrames, "frame_gen_frames");
            _frameGenFramesCard = (SettingRow)Body.Children[^1];
            IniToggle(SymbolRegular.TopSpeed24, L.T("DLSS dynamic frame generation"),
                L.T("RTX 50: the driver picks 2x, 3x or 4x moment by moment to reach your refresh rate, up to the generated frames above. Fewer generated frames when the game is fast: less lag, fewer artifacts. Also in game, with a target FPS"), "frame_gen_dynamic");
            _frameGenDynamicCard = (SettingRow)Body.Children[^1];
            IniCombo(SymbolRegular.Flash24, L.T("NVIDIA Reflex"),
                L.T("Keeps the GPU's queue short for lower input lag. Always on while DLSS Frame Generation runs"),
                Choices.Reflex, "reflex");
            _reflexCard = (SettingRow)Body.Children[^1];
            IniToggle(SymbolRegular.Flash24, L.T("AMD Anti-Lag 2"),
                L.T("Lower input lag on AMD drivers that offer it; does nothing on other GPUs. Turns itself off until the next start if the driver holds the game too long"),
                "anti_lag");
            IniToggle(SymbolRegular.Timer24, L.T("Frame generation: even pacing (Uncapped)"),
                L.T("With the Uncapped frame rate: the game at the refresh rate divided by the frames shown per game frame, so every generated frame reaches the screen. The fixed rates (30 to 144) are left as they are"), "frame_gen_even_pacing");
            _frameGenCapCard = (SettingRow)Body.Children[^1];

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
            var ui4kButtons = new StackPanel { Orientation = Orientation.Horizontal };
            ui4kButtons.Children.Add(_ui4kButton);
            ui4kButtons.Children.Add(_ui4kRemove);
            Add(Card(SymbolRegular.Image24, L.T("4K menu textures"), null, ui4kButtons, out _ui4kHint));
            _ui4kBar = new ProgressBar { Height = 4, Minimum = 0, Maximum = 1, Margin = new System.Windows.Thickness(24, 0, 24, 8), Visibility = System.Windows.Visibility.Collapsed };
            Body.Children.Add(_ui4kBar);
            Body.Children.Add(Text(L.F($"Upscaled with {UiUpscale.Credit}, downloaded once from its official release ({UiUpscale.Project}). It takes a while and uses the GPU; about 3.5 GB on disk."), "Faint"));
            ShowUi4k();

            Group(L.T("Camera"));
            IniToggle(SymbolRegular.Camera24, L.T("Camera tuning"), L.T("Turns the three settings below on"), "camera_controls");
            IniSlider(SymbolRegular.ZoomOut24, L.T("Field of view"), 1, 1.5, "camera_fov_scale");
            IniSlider(SymbolRegular.ArrowMaximize24, L.T("Distance"), 0.5, 1.5, "camera_distance_scale");
            IniSlider(SymbolRegular.ArrowBidirectionalUpDown24, L.T("Height"), 0.5, 1.5, "camera_height_scale");

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
        private readonly Wpf.Ui.Controls.Button _ui4kButton, _ui4kRemove;
        private readonly TextBlock _ui4kHint;
        private readonly ProgressBar _ui4kBar;
        private CancellationTokenSource? _ui4kBuild;

        private void ShowUi4k(string? status = null)
        {
            var building = _ui4kBuild != null;
            _ui4kButton.Content = building ? L.T("Cancel") : UiUpscale.Ready ? L.T("Rebuild") : L.T("Build");
            _ui4kRemove.Visibility = !building && (UiUpscale.Ready || Directory.Exists(UiUpscale.PackDir))
                ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
            _ui4kBar.Visibility = building ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
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

        /// <summary>An RTX 40 or 50 card (desktop or laptop): the GPUs NVIDIA's frame generation runs on.</summary>
        private static bool DlssFrameGenGpu() =>
            Hardware.Detect() is { Rtx: true } hardware &&
            System.Text.RegularExpressions.Regex.IsMatch(hardware.Gpu, @"RTX\s*(PRO\s*)?[45]0\d\d", System.Text.RegularExpressions.RegexOptions.IgnoreCase);

        /// <summary>NVIDIA Streamline's runtime beside the engine (tools/fetch_streamline.sh): DLSS Frame Generation.</summary>
        private static bool StreamlineFound() =>
            File.Exists(Path.Combine(Paths.BinariesDir, "sl.interposer.dll")) && File.Exists(Path.Combine(Paths.BinariesDir, "sl.dlss_g.dll"));

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
                "fsr" when !FsrSdkFound() =>
                    L.T("fsr_sdk is missing from the engine folder: the game falls back to FSR 3.1"),
                "fsr" when Amd() =>
                    L.T("AMD FSR SDK 2.4: FSR 4.1.2 on Radeon RX 6000 and newer and on supported Ryzen APUs, FSR 3.1.6 on older Radeons. The SDK picks the model"),
                "fsr" =>
                    L.T("AMD FSR SDK 2.4: FSR 3.1.6 on this GPU (AMD's FSR 4 runs on Radeon only). FSR 4.1.1 (fsr4vk, experimental) is the other choice here"),
                "fsr411" =>
                    L.T("Through fsr4vk (included, experimental): FSR 4.1.1 on NVIDIA and Intel GPUs with the needed Vulkan features. Choose FSR if it looks wrong"),
                "dlss" => DlssFound()
                    ? L.T("Needs an NVIDIA GeForce RTX GPU; others fall back to FSR 3.1") +
                      (Ini.Get("preset", "0") == "0" ? L.T(". With Native AA this is DLAA: DLSS as anti-aliasing at full resolution") : "")
                    : L.T("DLSS files are missing: the game falls back to FSR 3.1"),
                "xess" => XessFound()
                    ? L.T("Intel XeSS Super Resolution: the XMX model on Intel Arc, the DP4a model on other GPUs. GPUs it does not support fall back to FSR 3.1")
                    : L.T("xess (libxess.dll) is missing from the engine folder: the game falls back to FSR 3.1"),
                "taa" => L.T("Anti-aliasing at the output resolution, without an FSR model. Softer than DLAA or FSR Native AA: add sharpening below"),
                _ => null,
            };
            SetHint(_upscalerHint, hint);
            _presetCard.IsEnabled = value is not ("taa" or "off");
            _renderScaleCard.Visibility = Ini.Get("preset", "0") == "5" ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
            _renderScaleCard.IsEnabled = value is not ("taa" or "off");
            _sharpenCard.IsEnabled = value != "off";
            _sharpnessCard.IsEnabled = value != "off";
            _mipBiasCard.IsEnabled = value != "off";

            // Frame generation: DLSS's own rows only with DLSS chosen.
            var frameGen = Ini.Get("frame_gen", "off");
            _frameGenFramesCard.IsEnabled = frameGen == "dlss";
            _frameGenDynamicCard.IsEnabled = frameGen == "dlss";
            _reflexCard.IsEnabled = frameGen == "dlss";
            _frameGenCapCard.IsEnabled = frameGen != "off";
            SetHint(_frameGenHint, frameGen switch
            {
                "dlss" when !StreamlineFound() => L.T("NVIDIA Streamline (sl.interposer.dll, sl.dlss_g.dll) is missing from the engine folder: the game uses no frame generation"),
                "dlss" when !DlssFrameGenGpu() => L.T("Needs an NVIDIA RTX 40 or 50 GPU with Hardware-accelerated GPU scheduling on; other GPUs: choose FSR 3.1"),
                "dlss" => L.T("RTX 40: 2x; RTX 50: up to 6x. Loaded when the game starts; in game you can switch it off and on (Insert or L3 + R3)"),
                "fsr3" => L.T("Any GPU. Not in menus, movies or with HDR output"),
                _ => null,
            });
            if (frameGen != "off" && value == "off")
                SetHint(_frameGenHint, L.T("Frame generation needs an upscaler (or TAA): choose one above"));

            // An upscaler takes the finished frame at outputs other than 1920x1080 and can smear the
            // game's motion blur; More > Engine can leave the blur out there (BB_UPSCALER_NO_BLUR).
            var (w, h) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            if (_motionBlurHint != null)
                SetHint(_motionBlurHint, value != "off" && (w, h) != (1920, 1080) && Ini.Flag("effect_motion_blur")
                    ? L.T("With an upscaler at an output other than 1920x1080 the blur can smear. More > Engine > No motion blur smear with upscalers leaves it out there")
                    : null);

            var ngx = Paths.DlssRuntime;
            SetHint(_dlssHint, ngx != null
                ? L.F($"NVIDIA DLSS {Paths.DlssVersion(ngx)} is included ({Path.Combine("out", Path.GetFileName(ngx))}). Choose it as the upscaler above")
                : L.F($"{Path.Combine("out", "nvngx_dlss.dll")} is missing from the engine folder"));
        }

        /// <summary>The fsr4vk provider the package ships (FSR 4.1.1 on any GPU, vk_fsr4vk.cpp).</summary>
        private static bool Fsr4VkFound() =>
            File.Exists(Path.Combine(Paths.EngineDir, "fsr4vk", "amd_fidelityfx_upscaler_vk.dll"));

        /// <summary>Intel's XeSS-SR library (tools/fetch_xess.sh, vk_xess.cpp).</summary>
        private static bool XessFound() =>
            File.Exists(Path.Combine(Paths.EngineDir, "xess", "libxess.dll"));

        private static bool FsrSdkFound() =>
            File.Exists(Path.Combine(Paths.EngineDir, "fsr_sdk", "amd_fidelityfx_loader_vk.dll")) &&
            File.Exists(Path.Combine(Paths.EngineDir, "fsr_sdk", "amd_fidelityfx_upscaler_vk.dll"));

        private static bool Amd() => Hardware.Detect().Vendor == "AMD";
    }
}
