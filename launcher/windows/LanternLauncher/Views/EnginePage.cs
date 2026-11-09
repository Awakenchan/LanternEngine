using Avalonia.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The engine's user-facing environment switches as controls, built from Core/EngineSwitches.cs,
    /// so nobody has to type BB_ variables. A switch left at its default is not passed to the game
    /// at all. Every row goes in with Add, so the settings search finds it (by title, description
    /// or variable name).
    /// </summary>
    public sealed class EnginePage : LauncherPage
    {
        private readonly List<Action> _resets = new();
        private readonly TextBlock _changedHint;
        private readonly Control _enginePreset;
        private readonly Dictionary<string, ToggleSwitch> _features = new();
        private bool _syncing;

        public EnginePage() : base(L.T("Engine"), L.T("The engine preset, its features and every engine switch, in one place."))
        {
            var intro = Text(L.T("Each switch stays at the engine's default until you change it. Only changed ones are used."), "Faint");
            intro.SetTip(SettingRow.Wrapped(L.T("Only the switches you change are passed to the game, when it starts; a start in safe mode (after a crash) ") +
                                            L.T("leaves them out. Settings › Diagnostics › Extra variables is still there for anything not listed, and wins over these.")));
            Body.Children.Add(intro);
            Add(Card(SymbolRegular.ArrowReset24, L.T("Changed switches"), L.T("Every switch on this page that is not at its default"),
                SmallButton(L.T("Reset all"), SymbolRegular.ArrowUndo24, (_, _) => ResetAll()), out _changedHint));

            Group(L.T("Engine features"));
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

            foreach (var group in EngineSwitches.Groups.Where(g => EngineSwitches.All.Any(x => x.Group == g)))
            {
                Group(L.T(group));
                foreach (var item in EngineSwitches.All.Where(x => x.Group == group))
                    AddSwitch(item);
            }
            Group(L.T("About"));
            Body.Children.Add(Text(L.T("Lantern Engine by Awaken (github.com/Awakenchan/LanternEngine). Free software under the GNU GPL v3 or later ") +
                                   L.T("with additional terms: anything built on it must keep this credit. See NOTICE in the game's folder."), "Faint"));
            ShowChanged();
        }

        public override void OnShown() => ShowChanged();

        private void Feature(SymbolRegular icon, string title, string hint, string key, Panel into) =>
            _features[key] = SettingToggle(icon, title, hint, key, into, changed: SyncPreset);

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

        private static SymbolRegular GroupIcon(string group) => group switch
        {
            EngineSwitches.Graphics => SymbolRegular.Sparkle24,
            EngineSwitches.Performance => SymbolRegular.TopSpeed24,
            EngineSwitches.Memory => SymbolRegular.Storage24,
            EngineSwitches.Upscalers => SymbolRegular.ZoomIn24,
            EngineSwitches.Online => SymbolRegular.Globe24,
            EngineSwitches.Input => SymbolRegular.XboxController24,
            EngineSwitches.Sound => SymbolRegular.Speaker224,
            EngineSwitches.Fixes => SymbolRegular.ShieldCheckmark24,
            EngineSwitches.Diagnostics => SymbolRegular.TextBulletListSquare24,
            _ => SymbolRegular.Bug24,
        };

        /// <summary>One switch's row: a toggle, a slider, a choice or a text field, bound to its env: setting.</summary>
        private void AddSwitch(EngineSwitch item)
        {
            var icon = GroupIcon(item.Group);
            var info = EngineSwitches.Describe(item);
            switch (item.Kind)
            {
                case SwitchKind.Toggle:
                {
                    var toggle = ToggleRow(icon, item.Title, info, () => EngineSwitches.Get(S, item) == item.On,
                        v => EngineSwitches.Set(S, item, v ? item.On : item.Off), changed: ShowChanged);
                    _resets.Add(() => toggle.IsChecked = item.Default == item.On);
                    break;
                }
                case SwitchKind.Number:
                {
                    var format = item.Unit.Length > 0 ? $"{item.Format}' {item.Unit}'" : item.Format;
                    var slider = SliderRow(icon, item.Title, info, item.Min, item.Max, item.Step,
                        () => EngineSwitches.NumberOf(S, item),
                        v =>
                        {
                            EngineSwitches.Set(S, item, EngineSwitches.Format(item, v));
                            ShowChanged();
                        },
                        format, valueWidth: item.Unit.Length > 0 ? 86 : 52);
                    _resets.Add(() => slider.Value = EngineSwitches.DefaultNumber(item));
                    break;
                }
                case SwitchKind.Text:
                {
                    var box = TextRow(icon, item.Title, info, item.Placeholder, () => EngineSwitches.Get(S, item),
                        v =>
                        {
                            EngineSwitches.Set(S, item, v);
                            ShowChanged();
                        });
                    _resets.Add(() => box.Text = "");
                    break;
                }
                default:
                {
                    var control = ComboRow(icon, item.Title, info, item.Options, () => EngineSwitches.Get(S, item),
                        v => EngineSwitches.Set(S, item, v), changed: ShowChanged);
                    _resets.Add(() =>
                    {
                        var index = Choices.IndexOf(item.Options, item.Default);
                        if (control is Segmented segmented) segmented.Select(index, notify: true);
                        else if (control is ComboBox box) box.SelectedIndex = index;
                    });
                    break;
                }
            }
        }

        /// <summary>The summary row: what is passed to the game, and what Extra variables override.</summary>
        private void ShowChanged()
        {
            if (_changedHint == null) return; // rows built before the summary row exists
            var changed = EngineSwitches.Changed(S).ToList();
            var extra = GameEnvironment.ExtraVariables(S).Select(e => e.Name).ToHashSet(StringComparer.Ordinal);
            var overridden = changed.Where(c => extra.Contains(c.Name)).Select(c => c.Name).ToList();
            var text = changed.Count == 0
                ? L.T("None: the engine runs with its own defaults")
                : L.F($"{changed.Count} passed to the game: ") + string.Join(", ", changed.Select(c => $"{c.Name}={c.Value}"));
            if (overridden.Count > 0)
                text += L.F($"\nExtra variables also set {string.Join(", ", overridden)}: their value is used");
            SetHint(_changedHint, text);
        }

        private void ResetAll()
        {
            if (!EngineSwitches.Changed(S).Any()) return;
            if (!Prompt.Confirm(L.T("Reset engine switches"), L.T("Every switch on this page goes back to the engine's default."), L.T("Reset all"))) return;
            EngineSwitches.ResetAll(S);
            foreach (var reset in _resets)
                reset();
            Ws.SaveSoon();
            ShowChanged();
        }
    }
}
