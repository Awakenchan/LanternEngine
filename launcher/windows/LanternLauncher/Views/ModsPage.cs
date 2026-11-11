using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>Mod folders with their load order, and third-party eboot patches.</summary>
    public sealed class ModsPage : LauncherPage
    {
        private readonly StackPanel _modList = new();
        private readonly StackPanel _patchList = new();
        private readonly TextBlock _modsHint, _patchesHint;
        private List<Mods.Entry> _mods = new();
        private List<(Patches.Patch Patch, bool On)> _patches = new();

        public ModsPage() : base(L.T("Mods & patches"),
            L.T("Extract each mod into its own folder (with dvdroot_ps4, or straight chr/, parts/ and so on). Applied when the game starts."))
        {
            Group(L.T("Mods"));
            SettingToggle(SymbolRegular.PuzzlePiece24, L.T("Load mods"), L.T("When two mods replace the same file, the one lower in the list wins"),
                "mods_enabled");
            Add(Card(SymbolRegular.Folder24, L.T("Mods folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) =>
                {
                    if (PickFolder(L.T("Mods folder"), GameEnvironment.ModsDir(S)) is { } path)
                    {
                        S.Set("mods_dir", path);
                        Ws.SaveSoon();
                        RefreshMods();
                    }
                }),
                IconButton(SymbolRegular.Open24, PlayPage.OpenTip, (_, _) => Paths.OpenFolder(GameEnvironment.ModsDir(S))),
                IconButton(SymbolRegular.ArrowClockwise24, L.T("Refresh the list"), (_, _) => RefreshMods())), out _modsHint));
            Body.Children.Add(_modList);

            Group(L.T("Third-party patches"));
            Body.Children.Add(Text(L.T("shadPS4-format XML patches for version 01.09 from the patches folder, and the port's built-in ") +
                                   L.T("ones (Bloodborne.xml, off until you switch them on; frame rate, effects and resolution have their own ") +
                                   L.T("settings). Put your own patches in a file of their own: Bloodborne.xml is replaced on every update."), "Faint"));
            Body.Children.Add(Text(L.T("Player No Dead, Player Stealth, Player Silent, Enemy Control and Restore Debug Menu are off while Play online or seamless co-op is on."), "Faint"));
            Add(Card(SymbolRegular.Folder24, L.T("Patches folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) =>
                {
                    if (PickFolder(L.T("Patches folder"), GameEnvironment.PatchesDir(S)) is { } path)
                    {
                        S.Set("patches_dir", path);
                        Ws.SaveSoon();
                        RefreshPatches();
                    }
                }),
                IconButton(SymbolRegular.Open24, PlayPage.OpenTip, (_, _) => Paths.OpenFolder(GameEnvironment.PatchesDir(S))),
                IconButton(SymbolRegular.ArrowClockwise24, L.T("Refresh the list"), (_, _) => RefreshPatches())), out _patchesHint));
            Body.Children.Add(_patchList);

            Ws.PortChanged += OnShown;
        }

        public override void OnShown()
        {
            RefreshMods();
            RefreshPatches();
        }

        private int _modReads, _patchReads;
        private string? _modsShown, _patchesShown;

        /// <summary>Reads the mods folder on a worker thread; the list is made again only when it changed.</summary>
        private async void RefreshMods()
        {
            var read = ++_modReads;
            var dir = GameEnvironment.ModsDir(S);
            List<Mods.Entry> mods;
            try { mods = await Task.Run(() => Mods.Load(dir)); }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not read the mods folder: ") + e.Message, LogLevel.Warn);
                return;
            }
            if (read != _modReads) return;
            _mods = mods;
            SetHint(_modsHint, _mods.Count == 0 ? L.F($"{dir} — no mods") : _mods.Count == 1 ? L.F($"{dir} — 1 mod") : L.F($"{dir} — {_mods.Count} mods"));
            var shown = string.Join("|", _mods.Select(m => $"{m.Name}={m.Enabled}"));
            if (shown == _modsShown) return;
            DrawMods();
        }

        private void DrawMods()
        {
            _modsShown = string.Join("|", _mods.Select(m => $"{m.Name}={m.Enabled}"));
            _modList.Children.Clear();
            for (var i = 0; i < _mods.Count; i++)
            {
                var index = i;
                var mod = _mods[i];
                var toggle = new ToggleSwitch { IsChecked = mod.Enabled, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 0, 0) };
                toggle.Click += (_, _) =>
                {
                    _mods[index] = mod with { Enabled = toggle.IsChecked == true };
                    SaveMods();
                };
                var up = IconButton(SymbolRegular.ArrowUp24, L.T("Load earlier"), (_, _) => Move(index, -1));
                var down = IconButton(SymbolRegular.ArrowDown24, L.T("Load later"), (_, _) => Move(index, 1));
                up.IsEnabled = i > 0;
                down.IsEnabled = i < _mods.Count - 1;

                var card = Card(SymbolRegular.Box24, mod.Name, L.F($"Load order {i + 1}"), Row(up, down, toggle), out _);
                card.Margin = new Thickness(24, 0, 0, 4);
                _modList.Children.Add(card);
            }
        }

        private void Move(int index, int direction)
        {
            var target = index + direction;
            if (target < 0 || target >= _mods.Count) return;
            (_mods[index], _mods[target]) = (_mods[target], _mods[index]);
            SaveMods();
            DrawMods();
        }

        private void SaveMods()
        {
            try
            {
                Mods.Save(_mods);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not save mods.json: ") + e.Message, LogLevel.Bad);
            }
        }

        /// <summary>Reads the patch files on a worker thread (every XML in the folder is parsed); the
        /// list is made again only when it changed.</summary>
        private async void RefreshPatches()
        {
            var read = ++_patchReads;
            var dir = GameEnvironment.PatchesDir(S);
            List<(Patches.Patch, bool)> patches;
            try
            {
                patches = await Task.Run(() =>
                {
                    var profile = Patches.LoadProfile();
                    return Patches.Discover(dir).Select(p => (p, Patches.IsOn(p, profile))).ToList();
                });
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not read the patches folder: ") + e.Message, LogLevel.Warn);
                return;
            }
            if (read != _patchReads) return;
            _patches = patches;
            SetHint(_patchesHint, _patches.Count == 0 ? L.F($"{dir} — no patches") : dir);
            var cheatsOff = Ini.Get("seamless_enabled") == "1" || (Ini.Get("online_enabled") == "1" && !HuntersDream.OnlinePaused);
            var shown = string.Join("\u0001", _patches.Select(p => $"{p.Patch.Key}|{p.Patch.Name}|{p.Patch.File}|{p.Patch.Author}|{p.Patch.Note}={p.On}")) + (cheatsOff ? "\u0002" : "");
            if (shown == _patchesShown) return;
            _patchesShown = shown;

            _patchList.Children.Clear();
            for (var i = 0; i < _patches.Count; i++)
            {
                var index = i;
                var (patch, on) = _patches[i];
                var toggle = new ToggleSwitch { IsChecked = on, VerticalAlignment = VerticalAlignment.Center };
                toggle.Click += (_, _) =>
                {
                    _patches[index] = (patch, toggle.IsChecked == true);
                    try
                    {
                        Patches.Save(_patches);
                    }
                    catch (Exception e)
                    {
                        Ws.Log(L.T("Could not save patches.json: ") + e.Message, LogLevel.Bad);
                    }
                };
                var source = patch.File + (patch.Author.Length > 0 ? L.F($" · by {patch.Author}") : "");
                var hint = patch.Note.Length > 0 ? patch.Note + "\n\n" + source : source;
                var card = Card(SymbolRegular.DocumentText24, patch.Name, hint, toggle, out var note);
                if (cheatsOff && OnlineStatus.CheatLike(patch.Key)) SetHint(note, OnlineStatus.CheatLikeNote);
                card.Margin = new Thickness(24, 0, 0, 4);
                if (patch.Note.Length > 0)
                    card.SetTip(SettingRow.Wrapped(patch.Note));
                _patchList.Children.Add(card);
            }
        }
    }
}
