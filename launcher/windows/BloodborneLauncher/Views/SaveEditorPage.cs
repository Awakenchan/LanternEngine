// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;
using TextBox = Wpf.Ui.Controls.TextBox;

namespace BloodborneLauncher.Views
{
    public sealed class SaveEditorPage : LauncherPage
    {
        private const int MaxRows = 60;

        private static readonly (string Key, string Label)[] Sections =
        {
            ("character", L.T("Character")),
            ("items", L.T("Items")),
            ("gear", L.T("Weapons & gear")),
            ("bosses", L.T("Bosses")),
            ("lamps", L.T("Lamps")),
            ("revert", L.T("Revert")),
        };

        private readonly InfoBar _notice;
        private readonly InfoBar _online;
        private readonly ComboBox _characters = new() { MinWidth = 280, MaxWidth = 460 };
        private readonly TextBlock _status = Text("", "Faint");
        private readonly Button _save;
        private readonly Button _discard;
        private readonly Segmented _tabs;
        private readonly StackPanel _section = new();
        private readonly Segmented _content;
        private int _contentFilter;
        private List<(SavePlace Place, SaveCharacter Character)> _found = new();
        private SaveEditorSession? _session;
        private SaveCatalog? _catalog;
        private string? _catalogFor;
        private string _search = "";
        private int _shown;
        private bool _filling;
        private string? _error;
        private readonly Dictionary<string, int> _given = new();

        public SaveEditorPage() : base(L.T("Save editor"), "")
        {
            _notice = new InfoBar { IsClosable = false, Severity = InfoBarSeverity.Warning, Margin = new Thickness(0, 10, 0, 0) };
            Body.Children.Add(_notice);
            _online = new InfoBar
            {
                IsClosable = false,
                IsOpen = true,
                Severity = InfoBarSeverity.Warning,
                Margin = new Thickness(0, 10, 0, 0),
                Title = L.T("Edited saves and playing with others"),
                Message = L.T("An edited hunter brings its items, levels and bosses into online and seamless co-op sessions, where other players meet it. ") +
                          L.T("Decide before you go online: keep edited hunters in their own profile and play online with an unedited one, or tell the people you play with."),
            };
            Body.Children.Add(_online);

            Group(L.T("Hunter"));
            _characters.SelectionChanged += (_, _) => { if (!_filling) Pick(_characters.SelectedIndex); };
            _save = SmallButton(L.T("Save changes"), SymbolRegular.Save24, (_, _) => Save(), ControlAppearance.Primary);
            _discard = SmallButton(L.T("Discard"), SymbolRegular.ArrowUndo24, (_, _) => Discard());
            Add(Card(SymbolRegular.Person24, L.T("Pick a hunter"), null, Row(_characters, _save, _discard), out _));
            _status.Margin = new Thickness(4, 6, 0, 0);
            Body.Children.Add(_status);

            _tabs = new Segmented(Sections.Select(s => new Choice(s.Label, s.Key)).ToArray()) { Margin = new Thickness(0, 12, 0, 6) };
            _tabs.Changed += i => { _shown = i; _search = ""; Draw(); };
            Body.Children.Add(_tabs);
            _content = new Segmented(new[] { new Choice(L.T("All content"), "all"), new Choice(L.T("Base game"), "base"), new Choice(L.T("The Old Hunters"), "dlc") }) { Margin = new Thickness(0, 0, 0, 6) };
            _content.Changed += i => { _contentFilter = i; Draw(); };
            _content.Select(0, notify: false);
            Body.Children.Add(_content);
            Body.Children.Add(_section);
            _tabs.Select(0, notify: false);
        }

        public override void OnShown()
        {
            FillCharacters();
            LoadCatalog();
            Refresh();
        }

        private bool GameRunning => Ws.Game.Running || SaveStore.GameProcessRunning();

        private void Refresh()
        {
            var problem = GameRunning
                ? L.T("Close the game to edit saves: the page only reads them while it runs.")
                : _catalog == null
                    ? L.T("Item names come from your game folder. Set the game folder on the Play page to give items and see their names.")
                    : null;
            _notice.Title = _error != null ? L.T("That did not work") : problem == null ? "" : L.T("Saves can't be changed yet");
            _notice.Message = _error ?? problem ?? "";
            _notice.Severity = _error != null ? InfoBarSeverity.Error : InfoBarSeverity.Warning;
            _notice.IsOpen = _error != null || problem != null;
            _save.IsEnabled = _session is { Changes: > 0 } && !GameRunning;
            _discard.IsEnabled = _session is { Changes: > 0 };
            _status.Text = _session == null
                ? L.T("No hunters found in your profiles yet.")
                : _session.Changes == 0
                    ? L.F($"{_session.Slot.Name}, level {_session.Slot.Level}. No unsaved changes. Every save first makes a backup you can go back to on the Revert tab.")
                    : L.F($"{_session.Slot.Name}, level {_session.Slot.Level}. {_session.Changes} unsaved change(s): nothing is written until you press Save changes.");
        }

        private void FillCharacters()
        {
            var before = _session?.Character;
            _found = SaveEditorSession.Places(S)
                .SelectMany(place =>
                {
                    try { return SaveStore.Characters(place).Select(character => (place, character)).ToList(); }
                    catch (IOException e) { Report(e); return new List<(SavePlace place, SaveCharacter character)>(); }
                })
                .OrderByDescending(f => f.character.Saved)
                .ToList();
            _filling = true;
            _characters.Items.Clear();
            foreach (var (_, character) in _found)
                _characters.Items.Add(SaveStore.Title(character.Name, character.Profile, true) + L.F($", level {character.Level}, {character.BossesDefeated} bosses, {character.PlayHours:0.#} h, saved {character.Saved:d} (slot {character.Slot + 1})"));
            var index = before == null ? -1 : _found.FindIndex(f => f.Character.File == before.File);
            _filling = false;
            if (index < 0) index = SaveStore.DefaultCharacter(_found.Select(f => f.Character).ToList(), PlayerProfiles.Current(S));
            _characters.SelectedIndex = index;
            if (index >= 0 && _session?.Character.File != _found[index].Character.File) Pick(index);
            if (index >= 0 && _session is { Changes: 0 })
            {
                try { _session.Reload(); } catch (Exception e) when (IsSaveError(e)) { Report(e); }
                Draw();
            }
            if (_found.Count == 0)
            {
                _session = null;
                Draw();
            }
        }

        private async void LoadCatalog()
        {
            var dir = GameEnvironment.GameDir(S);
            var key = dir + "|" + L.Language;
            if (_catalogFor == key) return;
            _catalogFor = key;
            var language = L.Language;
            var catalog = await Task.Run(() => SaveCatalog.TryLoad(dir, language, out _));
            if (_catalogFor != key) return;
            _catalog = catalog;
            Refresh();
            Draw();
        }

        private void Pick(int index)
        {
            if (index < 0 || index >= _found.Count) return;
            if (_session is { Changes: > 0 } && _session.Character.File != _found[index].Character.File &&
                !Prompt.Confirm(L.T("Drop unsaved changes?"), L.T("You have changes that were not saved. Open the other hunter and drop them?"), L.T("Drop them")))
            {
                _filling = true;
                _characters.SelectedIndex = _found.FindIndex(f => f.Character.File == _session.Character.File);
                _filling = false;
                return;
            }
            _error = null;
            _given.Clear();
            try
            {
                _session = SaveEditorSession.Open(_found[index].Place, _found[index].Character);
            }
            catch (Exception e) when (IsSaveError(e))
            {
                _session = null;
                Report(e);
            }
            Draw();
        }

        private void Save()
        {
            if (_session == null) return;
            _error = null;
            try
            {
                _session.Commit();
                _given.Clear();
                Ws.Log(L.F($"Save editor: {_session.Slot.Name} saved"), LogLevel.Info);
                FillCharacters();
            }
            catch (Exception e) when (IsSaveError(e))
            {
                Report(e);
            }
            Refresh();
            Draw();
        }

        private void Discard()
        {
            _session?.Discard();
            _error = null;
            _given.Clear();
            Refresh();
            Draw();
        }

        private void Edit(Action<SaveSlot> edit)
        {
            if (_session == null) return;
            _error = null;
            try
            {
                _session.Edit(edit);
            }
            catch (Exception e) when (e is InvalidOperationException or ArgumentException)
            {
                Report(e);
            }
            Refresh();
        }

        private void Report(Exception e)
        {
            _error = e.Message.Length > 0 ? e.Message : L.T("The save did not take that change.");
            _notice.Title = L.T("That did not work");
            _notice.Message = _error;
            _notice.Severity = InfoBarSeverity.Error;
            _notice.IsOpen = true;
        }

        private static bool IsSaveError(Exception e) =>
            e is System.IO.IOException or UnauthorizedAccessException or SaveFormatException or InvalidOperationException or ArgumentException;

        private void Draw()
        {
            _section.Children.Clear();
            _notice.Severity = InfoBarSeverity.Warning;
            var key = Sections[_shown].Key;
            _content.Visibility = key is "character" or "revert" ? Visibility.Collapsed : Visibility.Visible;
            Refresh();
            if (_session == null && Sections[_shown].Key != "revert") return;
            switch (Sections[_shown].Key)
            {
                case "character": DrawCharacter(); break;
                case "items": DrawItems(); break;
                case "gear": DrawGear(); break;
                case "bosses": DrawBosses(); break;
                case "lamps": DrawLamps(); break;
                case "revert": DrawRevert(); break;
            }
        }

        private TextBox Number(int value, int min, int max, Action<int> set)
        {
            var box = new TextBox { Text = value.ToString(CultureInfo.InvariantCulture), Width = 120, ClearButtonEnabled = false };
            box.LostFocus += (_, _) =>
            {
                if (int.TryParse(box.Text, NumberStyles.None, CultureInfo.InvariantCulture, out var typed) && typed >= min && typed <= max)
                {
                    if (typed != value)
                    {
                        value = typed;
                        set(typed);
                    }
                }
                box.Text = value.ToString(CultureInfo.InvariantCulture);
            };
            return box;
        }

        private void DrawCharacter()
        {
            var slot = _session!.Slot;
            Group(L.T("Attributes"), _section);
            foreach (var (stat, label) in new[]
            {
                (SaveStat.Vitality, L.T("Vitality")), (SaveStat.Endurance, L.T("Endurance")), (SaveStat.Strength, L.T("Strength")),
                (SaveStat.Skill, L.T("Skill")), (SaveStat.Bloodtinge, L.T("Bloodtinge")), (SaveStat.Arcane, L.T("Arcane")),
            })
            {
                var which = stat;
                Add(Card(SymbolRegular.Person24, label, null, Number(slot.Stat(stat), 1, SaveSlot.MaxStat, v => { Edit(s => s.SetStat(which, v)); }), out _), _section);
            }
            _section.Children.Add(Text(L.T("Your level follows the attributes. Health and stamina follow them when the save loads."), "Faint"));
            Group(L.T("Wealth"), _section);
            Add(Card(SymbolRegular.Drop24, L.T("Blood Echoes"), null, Number(slot.Echoes, 0, SaveSlot.MaxEchoes, v => Edit(s => s.SetEchoes(v))), out _), _section);
            Add(Card(SymbolRegular.Eye24, L.T("Insight"), null, Number(slot.Insight, 0, SaveSlot.MaxInsight, v => Edit(s => s.SetInsight(v))), out _), _section);
        }

        private string Name(uint item) => _catalog?.ItemName(item) ?? L.F($"Item {item & 0xFFFFFFF}");

        private TextBox SearchBox(Action redraw)
        {
            var box = new TextBox { PlaceholderText = L.T("Search by name"), Text = _search, Margin = new Thickness(0, 4, 0, 8) };
            box.TextChanged += (_, _) =>
            {
                _search = box.Text;
                redraw();
            };
            return box;
        }

        private bool Matches(string name) => _search.Length == 0 || name.Contains(_search, StringComparison.CurrentCultureIgnoreCase);

        private bool Shows(bool dlc) => _contentFilter == 0 || (_contentFilter == 2) == dlc;

        private string? GivenNote(string name) =>
            _given.TryGetValue(name, out var count) ? L.F($"{count} added. Press Save changes at the top to keep it.") : null;

        private static string Tagged(string? hint, bool dlc) =>
            !dlc ? hint ?? "" : string.IsNullOrEmpty(hint) ? L.T("The Old Hunters") : hint + "  ·  " + L.T("The Old Hunters");

        private void DrawItems()
        {
            var results = new StackPanel();
            _section.Children.Add(SearchBox(() => FillItems(results)));
            _section.Children.Add(results);
            FillItems(results);
        }

        private void FillItems(StackPanel into)
        {
            into.Children.Clear();
            var slot = _session!.Slot;
            Group(L.T("What you have"), into);
            var held = slot.Items
                .Where(i => i.Category == 4 && Shows(SaveWorld.IsDlcGoods(i.Id)))
                .Select(i => (Item: i, Name: Name(i.ItemId)))
                .Where(i => Matches(i.Name))
                .OrderBy(i => i.Name, StringComparer.CurrentCultureIgnoreCase)
                .ToList();
            foreach (var (item, name) in held.Take(MaxRows))
            {
                var stored = item.List == SaveList.Storage;
                var info = _catalog?.GoodsById.GetValueOrDefault(item.Id);
                var max = info == null ? item.Quantity : Math.Max(item.Quantity, stored ? info.MaxStored : info.MaxHeld);
                var place = item.List switch
                {
                    SaveList.Storage => L.T("In storage"),
                    SaveList.KeyItems => L.T("Key item"),
                    _ => L.T("Carried"),
                };
                var current = item;
                Add(Card(SymbolRegular.Box24, name, Tagged(place + "  " + L.F($"(up to {max})"), SaveWorld.IsDlcGoods(item.Id)),
                    Number(item.Quantity, 1, Math.Max(1, max), v => Edit(s => s.SetQuantity(current, v))), out _), into);
            }
            if (held.Count > MaxRows) into.Children.Add(Text(L.F($"Showing {MaxRows} of {held.Count}. Search to narrow it down."), "Faint"));

            Group(L.T("Give an item"), into);
            if (_catalog == null)
            {
                into.Children.Add(Text(L.T("Needs the game folder for the item list."), "Faint"));
                return;
            }
            var goods = _catalog.Goods.Where(g => Matches(g.Name) && Shows(g.Dlc)).ToList();
            foreach (var g in goods.Take(MaxRows))
            {
                var give = g;
                var amount = new TextBox { Text = "1", Width = 70, ClearButtonEnabled = false, IsEnabled = g.MaxHeld > 1 };
                var button = SmallButton(g.KeyItem ? L.T("Unlock") : L.T("Give"), SymbolRegular.Add24, (_, _) =>
                {
                    var count = int.TryParse(amount.Text, NumberStyles.None, CultureInfo.InvariantCulture, out var typed) ? Math.Clamp(typed, 1, give.MaxHeld) : 1;
                    var have = _session!.Slot.Items.FirstOrDefault(i => i.Id == give.Id && i.Category == 4 && i.List != SaveList.Storage);
                    if (have != null) count = Math.Max(0, Math.Min(count, give.MaxHeld - have.Quantity));
                    if (count > 0)
                    {
                        var before = _session.Changes;
                        Edit(s => s.AddGoods(give.Id, count, give.Sort, give.KeyItem));
                        if (_session.Changes > before) _given[give.Name] = _given.GetValueOrDefault(give.Name) + count;
                    }
                    else
                    {
                        Report(new InvalidOperationException(L.F($"You already carry the most you can of {give.Name}.")));
                    }
                    FillItems(into);
                });
                Add(Card(SymbolRegular.Add24, g.Name, Tagged(GivenNote(g.Name) ?? (g.KeyItem ? L.T("Key item") : L.F($"Carry up to {g.MaxHeld}")), g.Dlc), Row(amount, button), out _), into);
            }
            if (goods.Count > MaxRows) into.Children.Add(Text(L.F($"Showing {MaxRows} of {goods.Count}. Search to narrow it down."), "Faint"));
        }

        private void DrawGear()
        {
            var results = new StackPanel();
            _section.Children.Add(SearchBox(() => FillGear(results)));
            _section.Children.Add(results);
            FillGear(results);
        }

        private void FillGear(StackPanel into)
        {
            into.Children.Clear();
            var slot = _session!.Slot;
            Group(L.T("What you have"), into);
            var owned = slot.Items
                .Where(i => (i.Category == 0 && i.Id >= 2000000) || (i.Category == 1 && i.Id < 900000))
                .Where(i => Shows(i.Category == 0 ? SaveWorld.IsDlcWeapon(i.Id) : SaveWorld.IsDlcArmor(i.Id)))
                .Select(i => (Item: i, Name: Name(i.ItemId)))
                .Where(i => Matches(i.Name) && !i.Name.StartsWith("Bare Fists", StringComparison.Ordinal))
                .OrderBy(i => i.Name, StringComparer.CurrentCultureIgnoreCase)
                .ToList();
            foreach (var (item, name) in owned.Take(MaxRows))
                Add(Card(item.Category == 0 ? SymbolRegular.Wrench24 : SymbolRegular.Shield24, name,
                    Tagged(item.List == SaveList.Storage ? L.T("In storage") : L.T("Carried"), item.Category == 0 ? SaveWorld.IsDlcWeapon(item.Id) : SaveWorld.IsDlcArmor(item.Id)), null, out _), into);
            if (owned.Count > MaxRows) into.Children.Add(Text(L.F($"Showing {MaxRows} of {owned.Count}. Search to narrow it down."), "Faint"));

            Group(L.T("Give a weapon or armor"), into);
            if (_catalog == null)
            {
                into.Children.Add(Text(L.T("Needs the game folder for the gear list."), "Faint"));
                return;
            }
            var weapons = _catalog.Weapons.Where(w => Matches(w.Name) && Shows(w.Dlc)).Select(w => (w.Name, w.Dlc, Give: (Action)(() => Edit(s => s.AddWeapon(w.Id, w.Durability, w.Sort, w.GemSlots))), Icon: SymbolRegular.Wrench24));
            var armor = _catalog.Armor.Where(a => Matches(a.Name) && Shows(a.Dlc)).Select(a => (a.Name, a.Dlc, Give: (Action)(() => Edit(s => s.AddArmor(a.Id, a.Durability, a.Sort))), Icon: SymbolRegular.Shield24));
            var all = weapons.Concat(armor).OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
            foreach (var (name, dlc, give, icon) in all.Take(MaxRows))
            {
                var run = give;
                Add(Card(icon, name, GivenNote(name) is { } note ? Tagged(note, dlc) : dlc ? Tagged(null, true) : null, SmallButton(L.T("Give"), SymbolRegular.Add24, (_, _) =>
                {
                    var before = _session!.Changes;
                    run();
                    if (_session.Changes > before) _given[name] = _given.GetValueOrDefault(name) + 1;
                    FillGear(into);
                }), out _), into);
            }
            if (all.Count > MaxRows) into.Children.Add(Text(L.F($"Showing {MaxRows} of {all.Count}. Search to narrow it down."), "Faint"));
            into.Children.Add(Text(L.T("New gear is plain, with its normal gem slots empty. Blood gems and Caryll runes are shown in game as usual but can't be given here."), "Faint"));
        }

        private void DrawBosses()
        {
            var slot = _session!.Slot;
            Group(L.T("Bosses"), _section);
            foreach (var boss in SaveWorld.Bosses.Where(b => Shows(SaveWorld.IsDlcBoss(b))))
            {
                var which = boss;
                var defeated = SaveWorld.State(slot, boss) == BossState.Defeated;
                var mark = SmallButton(L.T("Mark defeated"), SymbolRegular.CheckmarkCircle24, (_, _) => { Edit(s => SaveWorld.MarkDefeated(s, which)); Draw(); });
                var fight = SmallButton(L.T("Fight again"), SymbolRegular.ArrowSync24, (_, _) => { Edit(s => SaveWorld.FightAgain(s, which)); Draw(); });
                var relock = SmallButton(L.T("Lock again"), SymbolRegular.Key24, (_, _) => { Edit(s => SaveWorld.LockAgain(s, which)); Draw(); });
                mark.IsEnabled = !defeated;
                fight.IsEnabled = defeated;
                Add(Card(SymbolRegular.Target24, _catalog?.NpcName(boss.NameId, boss.Name) ?? boss.Name,
                    Tagged(defeated ? L.T("Defeated") : L.T("Not defeated"), SaveWorld.IsDlcBoss(boss)), Row(mark, fight, relock), out _), _section);
            }
            _section.Children.Add(Text(L.T("Fight again brings the boss back and leaves the rest of the world as it is. Lock again also takes back its intro and fog door, as on a first visit. Mark defeated removes the boss, without giving its drops or echoes. Lamps stay lit."), "Faint"));
            _section.Children.Add(Text(L.T("Not offered: Chalice Dungeon bosses, Gehrman and the Moon Presence (they end the game), and rolling back world changes such as the Blood Moon or NPC stories."), "Faint"));
        }

        private void DrawLamps()
        {
            if (_catalog == null)
            {
                _section.Children.Add(Text(L.T("Needs the game folder for the lamp names."), "Faint"));
                return;
            }
            var slot = _session!.Slot;
            Group(L.T("Lamps"), _section);
            foreach (var lamp in _catalog.Lamps.Where(l => Shows(l.Dlc)))
            {
                var which = lamp;
                var toggle = new ToggleSwitch { IsChecked = slot.Flag(lamp.Flag) };
                toggle.Click += (_, _) => Edit(s => s.SetFlag(which.Flag, toggle.IsChecked == true));
                Add(Card(SymbolRegular.Lightbulb24, lamp.Name, Tagged(lamp.Area, lamp.Dlc), toggle, out _), _section);
            }
            _section.Children.Add(Text(L.T("A lit lamp lets you travel to it from the Hunter's Dream. Chalice Dungeon lamps are not offered."), "Faint"));
        }

        private void DrawRevert()
        {
            Group(L.T("Backups made by the editor"), _section);
            var place = _session?.Place ?? _found.Select(f => f.Place).FirstOrDefault();
            if (place == null) return;
            var backups = SaveStore.Backups(place);
            if (backups.Count == 0) _section.Children.Add(Text(L.T("None yet: the first one is made when you save an edit."), "Faint"));
            for (var i = 0; i < backups.Count; i++)
            {
                var backup = backups[i];
                var title = i == 0 ? L.F($"{backup.When:g} (latest)") : L.F($"{backup.When:g}");
                Add(Card(SymbolRegular.History24, title, L.F($"Profile {place.Profile}"), SmallButton(L.T("Revert to this"), SymbolRegular.ArrowUndo24, (_, _) => Revert(place, backup)), out _), _section);
            }
            _section.Children.Add(Text(L.T("Reverting puts every save of this profile back as it was, including anything you played since. What is there now is backed up first."), "Faint"));
        }

        private void Revert(SavePlace place, SaveEditBackup backup)
        {
            if (!Prompt.Confirm(L.T("Revert this profile's saves?"),
                    L.F($"All saves of profile {place.Profile} go back to {backup.When:g}. What is there now is backed up first."), L.T("Revert"))) return;
            try
            {
                SaveStore.Revert(place, backup);
                _session?.Reload();
                FillCharacters();
            }
            catch (Exception e) when (IsSaveError(e))
            {
                Report(e);
            }
            Refresh();
            Draw();
        }
    }
}
