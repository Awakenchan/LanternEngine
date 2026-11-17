// SPDX-License-Identifier: GPL-3.0-or-later
using Avalonia.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The game itself: its folder and check, the DLC, the Lantern extras inside the game, Steam,
    /// saves and the in-game menu's language. These were the Play page's rows; Play keeps the
    /// title screen and its button.
    /// </summary>
    public sealed class GamePage : LauncherPage
    {
        private readonly TextBlock _savesHint, _gameHint, _dlcHint;
        private readonly SymbolIcon _gameIcon;
        private readonly AppButton _resetDlc;

        public GamePage() : base(L.T("Game"), L.T("Where the game is, its add-ons and the Lantern extras inside it."))
        {
            // ==================== Game files ====================
            Group(L.T("Game files"));
            var gameCard = Card(SymbolRegular.Folder24, L.T("Game folder"), "", Row(
                SmallButton(L.T("Setup guide"), SymbolRegular.Lightbulb24, (_, _) => SetupGuide.Open()),
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) => ChooseGame()),
                IconButton(SymbolRegular.Open24, PlayPage.OpenTip, (_, _) => Paths.OpenFolder(GameEnvironment.GameDir(S)))), out _gameHint);
            _gameIcon = gameCard.Icon;
            gameCard.ShowIcon = true;
            Add(gameCard);

            // The Old Hunters on base-game dumps: the folder of the dumped add-ons (BB_DLC_DIR).
            _resetDlc = IconButton(SymbolRegular.Dismiss24, L.T("No DLC folder"), (_, _) =>
            {
                S.Set("dlc_dir", "");
                Ws.SaveSoon();
                Refresh();
            });
            ToggleRow(SymbolRegular.PuzzlePiece24, L.T("The Old Hunters (DLC)"),
                L.T("Off: the game starts without its DLC. The Game of the Year edition (CUSA03173) always has it"),
                () => S.Bool("dlc_enabled"), v =>
                {
                    S.Set("dlc_enabled", v);
                    Ws.SaveSoon();
                    Refresh();
                });
            Add(Card(SymbolRegular.FolderOpen24, L.T("DLC folder"),
                L.T("Only with a base-game dump: the folder of your dumped add-ons, one folder each (The Old Hunters: SPEXPANSIONDLC03). ") +
                L.T("Found on its own when it is beside the game folder. The Game of the Year edition (CUSA03173) has it already"),
                Row(
                    SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) => ChooseDlc()),
                    IconButton(SymbolRegular.Open24, PlayPage.OpenTip, (_, _) => Paths.OpenFolder(GameEnvironment.DlcDir(S))),
                    _resetDlc), out _dlcHint));

            // ==================== Lantern in the game ====================
            Group(L.T("Lantern in the game"));
            ToggleRow(SymbolRegular.TextBulletListSquare24, L.T("Lantern menus in the game"),
                L.T("The PC menus Lantern Engine adds to the game: the extra pages in System (PC graphics, controls, camera), the title ") +
                L.T("screen's entries and the Altar of Despair's rebirth. Off: only the game's own menus, as on PS4; the launcher and the ") +
                L.T("overlay (Insert or L3 + R3) still change every setting. Takes effect at the next start"),
                () => Ini.Get("pc_menus", "1") != "0", v => Ini.Set("pc_menus", v ? "1" : "0"));
            ToggleRow(SymbolRegular.PersonEdit24, L.T("Hunter's Dream Mirror"),
                L.T("The Dream's mirror opens the appearance editor (the unused \"Put on Disguise\"). Takes effect at the next start"),
                () => Ini.Get("change_appearance", "0") != "0", v => Ini.Set("change_appearance", v ? "1" : "0"));
            ToggleRow(SymbolRegular.ArrowCounterclockwise24, L.T("Rebirth at the Altar"),
                L.T("The Altar of Despair offers Rebirth: your stats go back to your origin's, the Blood Echoes are refunded, and it can be undone. Takes effect at the next start"),
                () => Ini.Get("rebirth", "0") != "0", v => Ini.Set("rebirth", v ? "1" : "0"));
            ToggleRow(SymbolRegular.Video24, L.T("PC Camera Controls"),
                L.T("Camera field of view, distance and height in the game's System menu. Takes effect at the next start"),
                () => Ini.Get("camera_controls", "0") != "0", v => Ini.Set("camera_controls", v ? "1" : "0"));
            ComboRow(SymbolRegular.LocalLanguage24, L.T("In-game menu language"), L.T("The port's settings menu (Insert or L3 + R3)"),
                Choices.MenuLanguages, () => Ini.Get("menu_language", "en"), v => Ini.Set("menu_language", v));

            // ==================== More ====================
            Group(L.T("More"));
            Add(Card(SymbolRegular.Save24, L.T("Saves and profiles"), L.T("Who is playing, their backups, and importing saves"),
                SmallButton(L.T("Manage"), SymbolRegular.FolderOpen24, (_, _) => MainWindow.Instance?.Show("saves")), out _savesHint));
            // Steam: the game as a non-Steam game (Core/SteamShortcut.cs).
            Add(Card(SymbolRegular.Games24, L.T("Add to Steam"),
                L.T("Bloodborne as a non-Steam game in your Steam library: Steam Input, the overlay and Big Picture work with it"),
                SmallButton(L.T("Add to Steam"), SymbolRegular.Add24, (_, _) => AddToSteam()), out _));

            Ws.PortChanged += Refresh;
            Ws.GameStateChanged += RefreshSaves;
        }

        public override void OnShown() => Refresh();

        /// <summary>Checks the game folder (eboot.bin, param.sfo, an update folder beside it, a DLC dump
        /// nearby) on a worker thread: on a slow or sleeping disk that took seconds. Play shows it too.</summary>
        internal static async Task<(GameStatus Status, string Dlc)> InspectAsync(string gameDir, string dlcChosen)
        {
            try
            {
                return await Task.Run(() => (GameCheck.Inspect(gameDir),
                    dlcChosen.Length > 0 ? Paths.Expand(dlcChosen) : GameEnvironment.FoundDlc(gameDir) ?? ""));
            }
            catch (Exception e)
            {
                return (new GameStatus(Health.Warning, L.T("The game folder could not be read: ") + e.Message, "", ""), "");
            }
        }

        /// <summary>The colour of a game check's state.</summary>
        internal static string HealthBrush(Health health) => health switch
        {
            Health.Good => "GoodTextBrush",
            Health.Warning => "WarningTextBrush",
            _ => "DangerTextBrush",
        };

        private int _refreshes;

        /// <summary>Shows what is known at once, then checks the game folder on a worker thread (a slow
        /// or sleeping disk must not freeze the launcher).</summary>
        private void Refresh()
        {
            var gameDir = GameEnvironment.GameDir(S);
            var dlcChosen = S.Str("dlc_dir");
            SetHint(_gameHint, string.IsNullOrEmpty(gameDir) ? L.T("Not chosen") : gameDir);
            _resetDlc.IsEnabled = dlcChosen.Length > 0;
            RefreshSaves();
            _ = CheckGameAsync(gameDir, dlcChosen);
        }

        private async Task CheckGameAsync(string gameDir, string dlcChosen)
        {
            var refresh = ++_refreshes;
            var (status, dlc) = await InspectAsync(gameDir, dlcChosen);
            if (refresh != _refreshes) return; // a newer check is on its way
            _gameIcon.Symbol = status.Health == Health.Good ? SymbolRegular.CheckmarkCircle24 : SymbolRegular.Warning24;
            _gameIcon.SetResourceReference(TextBlock.ForegroundProperty, HealthBrush(status.Health));
            _gameIcon.SetTip(status.Summary);
            SetHint(_gameHint, string.IsNullOrEmpty(gameDir) ? L.T("Not chosen") :
                status.Health == Health.Bad ? $"{gameDir}  ·  {status.Summary}" : $"{gameDir}  ·  {$"{status.TitleId} {status.Version}".Trim()}");
            // The long explanation is in the ⓘ: the line under the title says only what was found.
            SetHint(_dlcHint, dlc.Length > 0
                ? dlcChosen.Length > 0 ? dlc : L.F($"Found next to the game: {dlc}")
                : L.T("Not needed for the Game of the Year edition"));
        }

        private void ChooseGame()
        {
            var path = PickFolder(L.T("Game folder (the one with eboot.bin)"), GameEnvironment.GameDir(S));
            if (path == null) return;
            S.Set("game_dir", path);
            if (!Paths.IsPortDir(Paths.PortDir))
                Ws.ResolvePort(); // the port may sit beside the new game folder
            Ws.SaveSoon();
            Ws.Log(L.T("Game folder: ") + path, LogLevel.Step);
            Refresh();
            MainWindow.Instance?.ApplyBackdrop(); // the game's own artwork
        }

        private void ChooseDlc()
        {
            var path = PickFolder(L.T("DLC folder (your dumped add-ons, like SPEXPANSIONDLC03)"), GameEnvironment.DlcDir(S));
            if (path == null) return;
            S.Set("dlc_dir", path);
            Ws.SaveSoon();
            Ws.Log(L.T("DLC folder: ") + path, LogLevel.Step);
            Refresh();
        }

        /// <summary>Bloodborne as a non-Steam game (Core/SteamShortcut.cs).</summary>
        private static void AddToSteam()
        {
            // Windows: Bloodborne.exe, which starts the game with the launcher's settings and waits for
            // it. Elsewhere the launcher itself with --play, which does the same.
            var windows = OperatingSystem.IsWindows();
            var exe = windows ? Path.Combine(Paths.PortDir, "Bloodborne.exe") : Environment.ProcessPath ?? "";
            if (!File.Exists(exe))
            {
                Prompt.Tell(L.T("Add to Steam"), L.T("Bloodborne.exe is missing from the Lantern Engine folder: reinstall the package."));
                return;
            }
            if (SteamShortcut.SteamDir() is null)
            {
                Prompt.Tell(L.T("Add to Steam"), windows ? L.T("Steam is not installed for this Windows user.") : L.T("Steam is not installed for this user."));
                return;
            }
            while (SteamShortcut.SteamRunning())
            {
                if (Prompt.Choose(L.T("Close Steam first"),
                        L.T("Steam rewrites its list of non-Steam games when it closes, so it has to be closed while Bloodborne is added. ") +
                        L.T("Exit Steam (Steam › Exit, or right-click its tray icon › Exit), then press Try again."),
                        L.T("Cancel"), L.T("Try again")) <= 0)
                    return;
            }
            try
            {
                SteamShortcut.Add(exe, windows ? "" : "--play");
                Ws.Log(L.T("Added Bloodborne to Steam as a non-Steam game"), LogLevel.Good);
                Prompt.Tell(L.T("Added to Steam"),
                    L.T("Bloodborne is in your Steam library after Steam starts again. It starts the game with this launcher's settings; ") +
                    L.T("Steam shows it as running until the game ends."));
            }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Add to Steam"), e.Message);
            }
        }

        private void RefreshSaves()
        {
            var profile = PlayerProfiles.Current(S);
            var played = SessionStats.Playtime(S, profile);
            SetHint(_savesHint, played.TotalMinutes >= 1 ? L.F($"{profile}  ·  {SessionStats.Duration(played)} played") : profile);
        }
    }
}
