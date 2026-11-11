using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.LogicalTree;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    public sealed class CheatsPage : LauncherPage
    {
        private static readonly Choice[] Modes =
        {
            new(L.T("Off"), "off"),
            new(L.T("Infinite"), "infinite", L.T("Kept full at the normal carry limit, even if you have none. Items put into storage while it is on come back")),
            new(L.T("Custom amount"), "custom", L.T("Refilled to the amount below whenever it drops")),
        };

        private const int FirstEngineWithCheats = 5;

        private readonly InfoBar _engine;
        private readonly InfoBar _online;

        public CheatsPage() : base(L.T("Cheats"),
            L.T("Set here for the next start. While playing, change them in the in-game menu (Insert or L3 + R3): they apply live. Turning a cheat off puts the game's values back; items you already have stay."))
        {
            _engine = new InfoBar
            {
                IsClosable = false,
                Severity = InfoBarSeverity.Warning,
                Margin = new Thickness(0, 14, 0, 0),
                Title = L.T("Not in this engine yet"),
                Message = L.T("Your engine doesn't apply cheats yet: they arrive with the next engine update. What you pick here is saved and starts working then."),
            };
            Body.Children.Add(_engine);
            _online = new InfoBar
            {
                IsClosable = false,
                Severity = InfoBarSeverity.Informational,
                Margin = new Thickness(0, 14, 0, 0),
                Title = L.T("Off while Play online or seamless co-op is on"),
                Message = L.T("Play online or seamless co-op is on: every cheat stays off in online and seamless co-op sessions, so other players never meet them. ") +
                          L.T("Turn both off and they work again; what you pick here is kept."),
            };
            Body.Children.Add(_online);

            Group(L.T("Items"));
            Cheat(SymbolRegular.Drop24, L.T("Blood Vials"), "cheat_vials", "cheat_vials_amount");
            Cheat(SymbolRegular.Flash24, L.T("Quicksilver Bullets"), "cheat_bullets", "cheat_bullets_amount");
            IniToggle(SymbolRegular.Beaker24, L.T("Infinite consumables"),
                L.T("Molotovs, throwing knives, pebbles, antidotes, papers and the like are not used up (not Blood Vials or Bullets, which are above, and not insight items)"),
                "cheat_consumables");

            Group(L.T("Character"));
            IniToggle(SymbolRegular.Run24, L.T("Infinite stamina"), L.T("Your stamina stays full"), "cheat_stamina");
            IniToggle(SymbolRegular.EyeOff24, L.T("Stealth"), L.T("Enemies don't notice you and your steps make no noise"), "cheat_stealth");
            IniToggle(SymbolRegular.Shield24, L.T("No weapon wear"), L.T("Equipped weapons are kept at full durability, so no repairs"), "cheat_no_durability");
            ComboRow(SymbolRegular.Target24, L.T("Firearm range"), L.T("How far firearm bullets travel (pistols, rifles, blunderbuss, cannons)"),
                new[] { new Choice(L.T("Normal"), "off"), new Choice("150%", "150"), new Choice("200%", "200"), new Choice("300%", "300") },
                () => Ini.Get("cheat_gun_range", "off"), v => Ini.Set("cheat_gun_range", v));

            Group(L.T("Blood Echoes"));
            IniToggle(SymbolRegular.Heart24, L.T("Keep echoes on death"), L.T("Dying never drops your Blood Echoes, so there is nothing to pick back up"), "cheat_keep_echoes");
            ComboRow(SymbolRegular.ArrowTrendingLines24, L.T("Echoes from kills"), L.T("Enemies already loaded give the new amount after you rest at a lamp or reload"),
                new[] { new Choice(L.T("Normal"), "1"), new Choice("x2", "2"), new Choice("x5", "5"), new Choice("x10", "10") },
                () => Ini.Get("cheat_echo_multiplier", "1"), v => Ini.Set("cheat_echo_multiplier", v));
            Add(Card(SymbolRegular.Add24, L.T("Add echoes or insight"), L.T("In the in-game menu (Insert or L3 + R3): +1,000, +10,000, +100,000 echoes and +1 insight"), null, out _));
        }

        private void Cheat(SymbolRegular icon, string title, string key, string amountKey)
        {
            Slider amount = null!;
            string Mode() => Ini.Get(key, "off") switch
            {
                "infinite" => "infinite",
                var v when int.TryParse(v, out var n) && n > 0 => "custom",
                _ => "off",
            };
            int Amount() => Math.Clamp(int.TryParse(Ini.Get(key, ""), out var n) && n > 0 ? n : S.Int(amountKey), 1, 99);

            ComboRow(icon, title, null, Modes, Mode, v =>
            {
                Ini.Set(key, v == "custom" ? Amount().ToString(CultureInfo.InvariantCulture) : v);
                ShowAmount(amount, v == "custom");
                Ws.Log($"{title}: {(v == "custom" ? L.F($"kept at {Amount()}") : v)}.", LogLevel.Step);
            });
            amount = SliderRow(SymbolRegular.NumberSymbol24, L.T("Amount"), L.F($"How many {title} to keep"), 1, 99, 1, () => Amount(), v =>
            {
                S.Set(amountKey, (int)v);
                if (Mode() == "custom") Ini.Set(key, ((int)v).ToString(CultureInfo.InvariantCulture));
            }, "0");
            ShowAmount(amount, Mode() == "custom");
        }

        private static void ShowAmount(Slider amount, bool show)
        {
            ILogical? at = amount;
            while (at != null && at is not SettingRow) at = at.GetLogicalParent();
            ((Control?)at ?? amount).IsVisible = show;
        }

        public static bool Supported => Updater.LocalEngine() is { } engine &&
                                        !(EngineBuild(engine) is { } build && build < FirstEngineWithCheats);

        private static int? EngineBuild(string engine)
        {
            var at = engine.LastIndexOf("-fps", StringComparison.Ordinal);
            return at >= 0 && int.TryParse(engine.AsSpan(at + 4), NumberStyles.None, CultureInfo.InvariantCulture, out var build)
                ? build : null;
        }

        public override void OnShown()
        {
            _engine.IsOpen = !Supported;
            _online.IsOpen = Ini.Get("seamless_enabled") == "1" || (Ini.Get("online_enabled") == "1" && !HuntersDream.OnlinePaused);
        }
    }
}
