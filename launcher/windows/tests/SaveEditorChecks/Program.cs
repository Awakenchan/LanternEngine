// SPDX-License-Identifier: GPL-3.0-or-later
using BloodborneLauncher.Core;

return args.Length > 0 && args[0] == "edit" ? Edit(args[1..]) : Check(args);

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static void Name(byte[] bytes, int at)
{
    var text = "Fixture";
    for (var i = 0; i < text.Length; i++) bytes[at + i * 2] = (byte)text[i];
}

static byte[] Fixture()
{
    var bytes = new byte[SaveSlot.FileSize];
    void W(int at, uint value) => BitConverter.TryWriteBytes(bytes.AsSpan(at, 4), value);
    const int table = 0x54;
    var at = table;
    W(at, 0xC0800000); W(at + 4, 0x8000C544); at += 0x28;
    W(at, 0x80800001); W(at + 4, 8000000); W(at + 8, 250); W(at + 16, 1);
    for (var slot = 0; slot < 5; slot++) W(at + 20 + slot * 8, 0x80000000);
    at += 0x3C;
    for (var slot = 2; slot < SaveSlot.TableSlots; slot++) { W(at + 4, 0xFFFFFFFF); at += 8; }
    var player = at;
    W(player + 0x80, 155);
    W(player + 0x84, 12345);
    W(player + 0x74, 40);
    foreach (var (offset, value) in new[] { (0x30, 40u), (0x38, 42u), (0x48, 40u), (0x50, 20u), (0x58, 40u), (0x60, 23u) }) W(player + offset, value);
    Name(bytes, player + 0x98);
    W(player + 0x25C, 2);
    var inventory = player + 0x260;
    W(inventory, 0x80800001); W(inventory + 4, 8000000); W(inventory + 8, 1); W(inventory + 12, 0x7C830040);
    W(inventory + 16, 0xB00003E8); W(inventory + 20, 0x400003E8); W(inventory + 24, 7); W(inventory + 28, 0x02710041);
    var keys = inventory + SaveSlot.InventoryCapacity * 16;
    W(keys, 0);
    var face = keys + 4 + SaveSlot.KeyCapacity * 16 + 64;
    bytes[face] = (byte)'F'; bytes[face + 1] = (byte)'A'; bytes[face + 2] = (byte)'C'; bytes[face + 3] = (byte)'E';
    var storage = face + 0xF0;
    W(storage, 1);
    W(storage + 4, 0xB00003E8); W(storage + 8, 0x400003E8); W(storage + 12, 300); W(storage + 16, 0x02710040);
    var size = (uint)(storage + 4 + SaveSlot.StorageCapacity * 16 + 0x94 - table);
    var header = new uint[]
    {
        0x41, 0x18010000, 5000, table, size + 241, table, size, table + size + 158, 0,
        table + size, 150, table + size + 150, 8, table + size + 241, 150001,
    };
    for (var i = 0; i < header.Length; i++) W(i * 4, header[i]);
    var pool = header[13];
    W(15 * 4, pool + 150001);
    W(16 * 4, 100);
    W(17 * 4, pool + 150001 + 100);
    W(18 * 4, 180);
    W(19 * 4, pool + 150001 + 180);
    W(20 * 4, 0x20004);
    bytes[pool + 150001 + 180 + 0x20003] = 1;
    bytes[pool + 150001 + 180 + 0x20004 + 0x1026] = 0xAB;
    bytes[pool + 150001 + 180 + 0x20004 + 0x1027] = 0xCD;
    return bytes;
}

static int Check(string[] saves)
{
    var fixture = Fixture();
    var slot = SaveSlot.Parse(fixture);
    Require(slot.ToBytes().AsSpan().SequenceEqual(fixture), "Round trip without edits must give the same bytes");
    Require(slot.Name == "Fixture" && slot.Level == 155 && slot.Echoes == 12345 && slot.Insight == 40, "Fields of the fixture");
    Require(slot.Items.Count == 3, "Fixture items");

    Require(SaveStore.Title("Hunter", "test", true) == "Hunter (test)", "A hunter title names its profile when profiles are shown");
    Require(SaveStore.Title("Hunter", "Main", false) == "Hunter", "A hunter title is the name alone with one profile");
    Require(SaveStore.Title("", "test", true) == "? (test)" && SaveStore.Title("", "Main", false) == "?", "A nameless hunter shows a question mark");
    Require(SaveStore.Title("Hunter", "", true) == "Hunter", "A hunter title skips an empty profile");
    Require(SaveStore.Title("Hunter", "test", true) != SaveStore.Title("Hunter", "Main", true), "Same names in two profiles read differently");

    SaveCharacter H(string profile, int slot, string name, int level) => new(profile, slot, name, level, "", DateTime.MinValue, 0, 0);
    var titles = SaveStore.Titles(new[] { H("test", 0, "Hunter", 22), H("Main", 0, "Hunter", 10), H("Main", 1, "Hunter", 155) }, true);
    Require(titles.SequenceEqual(new[] { "Hunter (test)", "Hunter (Main, level 10)", "Hunter (Main, level 155)" }), "Only clashing titles get the level: " + string.Join(" | ", titles));
    titles = SaveStore.Titles(new[] { H("Main", 0, "Hunter", 10), H("Main", 1, "Hunter", 155) }, false);
    Require(titles.SequenceEqual(new[] { "Hunter (level 10)", "Hunter (level 155)" }), "Clashing titles with one profile get the level: " + string.Join(" | ", titles));
    titles = SaveStore.Titles(new[] { H("Main", 0, "Hunter", 10), H("Main", 1, "Eileen", 10) }, false);
    Require(titles.SequenceEqual(new[] { "Hunter", "Eileen" }), "Unique titles stay as they are: " + string.Join(" | ", titles));
    titles = SaveStore.Titles(new[] { H("Main", 0, "Hunter", 10), H("Main", 3, "Hunter", 10), H("Main", 1, "Hunter", 155) }, false);
    Require(titles.SequenceEqual(new[] { "Hunter (level 10, slot 1)", "Hunter (level 10, slot 4)", "Hunter (level 155)" }), "Same name and level fall back to the slot: " + string.Join(" | ", titles));
    titles = SaveStore.Titles(new[] { H("Main", 0, "hunter", 10), H("Main", 1, "Hunter", 155) }, false);
    Require(titles.SequenceEqual(new[] { "hunter (level 10)", "Hunter (level 155)" }), "Titles differing only in case clash: " + string.Join(" | ", titles));
    Require(SaveStore.Titles(Array.Empty<SaveCharacter>(), true).Count == 0, "No hunters give no titles");

    Require(SaveSlot.FlagBlock(9456) == 45 && SaveSlot.FlagBlock(12401800) == 169 && SaveSlot.FlagBlock(2410) == 10, "Flag blocks");
    Require(SaveSlot.FlagBlock(72410100) == 119 + 5 && SaveSlot.FlagBlock(11100000) == null, "Flag areas");
    var flagged = SaveSlot.Parse(fixture);
    flagged.SetFlag(12401800, true);
    Require(flagged.Flag(12401800) && !flagged.Flag(12401801), "A flag sets one bit");
    flagged.SetFlag(12401800, false);
    Require(flagged.ToBytes().AsSpan().SequenceEqual(fixture), "Clearing a flag restores the bytes");

    var edited = SaveSlot.Parse(fixture);
    edited.SetStat(SaveStat.Vitality, 50);
    Require(edited.Stat(SaveStat.Vitality) == 50 && edited.Level == 165, "A stat moves the level with it");
    edited.SetStat(SaveStat.Vitality, 40);
    Require(edited.ToBytes().AsSpan().SequenceEqual(fixture), "Undoing a stat restores the bytes");
    Throws(() => edited.SetStat(SaveStat.Vitality, 100), "Stats stop at 99");
    Throws(() => edited.SetEchoes(-1), "Echoes can't be negative");
    Throws(() => edited.SetInsight(100), "Insight stops at 99");

    var goods = SaveSlot.Parse(fixture);
    goods.AddGoods(1000, 3, 10000, false);
    Require(goods.Items.Single(i => i.List == SaveList.Inventory && i.Id == 1000).Quantity == 10, "Giving a held item adds to its stack");
    goods.AddGoods(4114, 1, 680060, true);
    var key = goods.Items.Single(i => i.List == SaveList.KeyItems);
    Require(key.Handle == 0xB0001012 && key.ItemId == 0x40001012 && key.Packed == (680060u << 12), "A key item's record");
    goods.AddGoods(1200, 2, 40010, false);
    var added = goods.Items.Single(i => i.Id == 1200);
    Require(added.Position == 2 && added.Packed == ((40010u << 12) | 0x42), "A new item takes the next free place");
    Require(SaveSlot.Parse(goods.ToBytes()).Items.Count == 5, "Given items survive a new parse");

    var gear = SaveSlot.Parse(fixture);
    var moved = gear.FlagPool;
    gear.SetFlag(12401800, true);
    gear.AddWeapon(7000300, 200, 510000 - 300, new[] { 1u, 1u, 0x80000000, 0x80000000, 0x80000000 });
    gear.AddArmor(230000, 0, 100000);
    Require(gear.FlagPool == moved + 2 * 0x34, "The flag pool moves with the item table");
    Require(gear.Flag(12401800), "Flags survive the move");
    var again = SaveSlot.Parse(gear.ToBytes());
    Require(again.Items.Count(i => i.List == SaveList.Inventory) == 4, "Given gear is in the inventory");
    var weapon = again.Items.Single(i => i.Id == 7000300);
    var armor = again.Items.Single(i => i.Category == 1);
    Require(weapon.Handle == 0x80800002 && armor.Handle == 0x90800003, "Gear handles take the next free table index, past the gem's");
    Require(again.Gear.Count == SaveSlot.TableSlots, "The table keeps its size");
    Require(again.Name == "Fixture" && again.Level == 155, "Player data moved whole");
    var many = SaveSlot.Parse(fixture);
    var firstGoods = many.Items.First(i => i.Category == 4);
    for (var i = 0; i < 12; i++) many.AddWeapon(7000300, 200, 510000 - 300, new[] { 1u, 1u, 0x80000000, 0x80000000, 0x80000000 });
    many.AddArmor(230000, 0, 100000);
    many.AddWeapon(7000300, 200, 510000 - 300, new[] { 1u, 1u, 0x80000000, 0x80000000, 0x80000000 });
    many.SetQuantity(firstGoods, firstGoods.Quantity + 1);
    var manyAgain = SaveSlot.Parse(many.ToBytes());
    var givenWeapons = manyAgain.Items.Where(i => i.Id == 7000300 && i.Category == 0).ToList();
    Require(givenWeapons.Count == 13, "Many of one weapon can be given in one go");
    Require(manyAgain.Items.Where(i => i.List == SaveList.Inventory).Select(i => i.Handle).Distinct().Count() == manyAgain.Items.Count(i => i.List == SaveList.Inventory), "Given gear never shares a handle");
    Require(manyAgain.Items.Where(i => i.Category <= 1).All(i => manyAgain.Gear.Any(g => g.Handle == i.Handle && g.ItemId == i.ItemId)), "Every given item has its own table entry");
    Require(manyAgain.Items.First(i => i.Position == firstGoods.Position && i.List == firstGoods.List).Quantity == firstGoods.Quantity + 1, "A quantity edit still finds its item after gear moved the lists");
    var grown = gear.ToBytes();
    Require(grown[again.LogicalEnd - 1] == 0xAB, "The last byte of the save moves with it");
    Require(grown[again.LogicalEnd + 2 * 0x34 - 2 * 0x34 + 1] == fixture[again.LogicalEnd + 1], "What lies past the save stays where it is");

    Throws(() => SaveSlot.Parse(new byte[100]), "A short file is refused");
    var broken = (byte[])fixture.Clone();
    broken[0] = 0x42;
    Throws(() => SaveSlot.Parse(broken), "An unknown version is refused");
    broken = (byte[])fixture.Clone();
    broken[0x18] ^= 1;
    Throws(() => SaveSlot.Parse(broken), "A broken section table is refused");
    broken = (byte[])fixture.Clone();
    BitConverter.TryWriteBytes(broken.AsSpan(0x54 + 0x28 + 0x3C, 4), 0x12345678u);
    Throws(() => SaveSlot.Parse(broken), "An unknown item entry is refused");

    var summary = new byte[SaveSlotSummary.FileSize];
    Name(summary, 0x1096 + 0x192);
    var levelled = SaveSlotSummary.WithLevel(summary, 1, "Fixture", 200);
    Require(levelled != null && SaveSlotSummary.Level(levelled, 1) == 200, "The summary takes the new level");
    Require(SaveSlotSummary.WithLevel(summary, 1, "Other", 200) == null, "A summary of another name is left alone");

    StoreChecks(fixture);

    foreach (var boss in SaveWorld.Bosses)
        foreach (var flag in boss.FightFlags.Concat(boss.DefeatOn).Concat(boss.DefeatOff).Concat(boss.LockFlags).Append(boss.Defeated))
            Require(SaveSlot.FlagBlock(flag) != null, $"{boss.Name}: flag {flag} has no place in the pool");
    foreach (var lamp in SaveWorld.Lamps)
        Require(SaveSlot.FlagBlock(lamp.Flag) != null, $"Lamp {lamp.Flag} has no place in the pool");

    Require(SaveWorld.Bosses.Count(SaveWorld.IsDlcBoss) == 5, "The Old Hunters has five bosses here");
    Require(SaveWorld.Lamps.Count(l => SaveWorld.IsDlcLamp(l.Flag)) == 10, "The Old Hunters has ten lamps");
    Require(SaveWorld.IsDlcWeapon(31000000) && SaveWorld.IsDlcWeapon(19100000) && !SaveWorld.IsDlcWeapon(19000000) && !SaveWorld.IsDlcWeapon(8101000), "Weapons of The Old Hunters");
    Require(SaveWorld.IsDlcArmor(371000) && SaveWorld.IsDlcArmor(41000) && !SaveWorld.IsDlcArmor(71000) && !SaveWorld.IsDlcArmor(920000), "Armor of The Old Hunters");

    var real = 0;
    foreach (var path in saves)
        foreach (var file in Directory.Exists(path) ? Directory.GetFiles(path, "userdata*", SearchOption.AllDirectories) : new[] { path })
        {
            var bytes = File.ReadAllBytes(file);
            if (bytes.Length != SaveSlot.FileSize || SaveSlot.IsEmptySlot(bytes)) continue;
            var parsed = SaveSlot.Parse(bytes);
            Require(parsed.ToBytes().AsSpan().SequenceEqual(bytes), $"{file}: round trip");
            real++;
            var sum = parsed.Stat(SaveStat.Vitality) + parsed.Stat(SaveStat.Endurance) + parsed.Stat(SaveStat.Strength) + parsed.Stat(SaveStat.Skill) + parsed.Stat(SaveStat.Bloodtinge) + parsed.Stat(SaveStat.Arcane);
            Require(sum - 50 == parsed.Level, $"{file}: level is the stats' sum less 50 ({sum} vs {parsed.Level})");
            Require(parsed.LogicalEnd <= SaveSlot.FileSize, $"{file}: logical end");
        }
    if (saves.Length > 0) Console.WriteLine($"{real} real save(s) round-tripped");
    Console.WriteLine("Save editor checks passed");
    return 0;
}

static void StoreChecks(byte[] fixture)
{
    var root = Path.Combine(Path.GetTempPath(), "lantern-save-editor-" + Guid.NewGuid().ToString("N"));
    try
    {
        var folder = Path.Combine(root, "savedata", "1", "CUSA00207", "SPRJ0005");
        Directory.CreateDirectory(folder);
        var file = Path.Combine(folder, "userdata0001");
        var summaryPath = Path.Combine(folder, "userdata0010");
        File.WriteAllBytes(file, fixture);
        var summary = new byte[SaveSlotSummary.FileSize];
        Name(summary, 0x1096 + 0x192);
        File.WriteAllBytes(summaryPath, summary);
        var place = new SavePlace("Test", Path.Combine(root, "savedata"), folder, Path.Combine(root, "backups"));
        var character = SaveStore.Characters(place).Single();
        Require(character.Slot == 1 && character.Name == "Fixture", "The character is listed");
        Require(character.BossesDefeated == 0, "A fresh character has no bosses defeated");
        var older = character with { Profile = "Main", Saved = new DateTime(2026, 1, 1) };
        var newer = character with { Profile = "Main", Saved = new DateTime(2026, 6, 1) };
        var other = character with { Profile = "Other", Saved = new DateTime(2026, 9, 1) };
        Require(SaveStore.DefaultCharacter(new[] { older, newer, other }, "Main") == 1, "The current profile's latest save opens first");
        Require(SaveStore.DefaultCharacter(new[] { older, other }, "Missing") == 1, "With no match the latest save opens first");
        Require(SaveStore.DefaultCharacter(Array.Empty<SaveCharacter>(), "Main") == -1, "No characters, nothing opens");

        var edit = SaveSlot.Parse(fixture);
        edit.SetStat(SaveStat.Arcane, 30);
        edit.AddGoods(1200, 2, 40010, false);
        if (SaveStore.GameProcessRunning())
        {
            Throws(() => SaveStore.Commit(place, character, fixture, edit), "A running game refuses the write");
            Require(File.ReadAllBytes(file).AsSpan().SequenceEqual(fixture), "A refused write changes nothing");
            Console.WriteLine("Store write checks skipped: the game is running");
            return;
        }
        var made = SaveStore.Commit(place, character, fixture, edit);
        Require(Directory.Exists(made), "A write leaves a backup");
        Require(File.ReadAllBytes(Path.Combine(made, "savedata", "1", "CUSA00207", "SPRJ0005", "userdata0001")).AsSpan().SequenceEqual(fixture), "The backup holds the old bytes");
        var now = File.ReadAllBytes(file);
        Require(now.AsSpan().SequenceEqual(edit.ToBytes()), "The save holds the edit");
        Require(SaveSlotSummary.Level(File.ReadAllBytes(summaryPath), 1) == edit.Level, "The title menu's level follows");
        Throws(() => SaveStore.Commit(place, character, fixture, edit), "A save changed since it was opened is refused");
        Require(File.ReadAllBytes(file).AsSpan().SequenceEqual(now), "A refused write changes nothing");

        var first = SaveStore.Backups(place).Single();
        SaveStore.Revert(place, first);
        Require(File.ReadAllBytes(file).AsSpan().SequenceEqual(fixture), "Revert puts the old bytes back");
        Require(SaveSlotSummary.Level(File.ReadAllBytes(summaryPath), 1) == 0, "Revert puts the summary back");
        Require(SaveStore.Backups(place).Count == 2, "Revert backs up what it replaces");

        var empty = Path.Combine(place.Backups, "2020-01-01_00-00-00");
        Directory.CreateDirectory(Path.Combine(empty, "savedata"));
        Require(SaveStore.Backups(place).All(b => b.Dir != empty), "An empty backup is not listed");
        Throws(() => SaveStore.Revert(place, new SaveEditBackup(empty, DateTime.Now)), "An empty backup is refused");
        Require(File.ReadAllBytes(file).AsSpan().SequenceEqual(fixture), "A refused revert changes nothing");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static void Throws(Action action, string message)
{
    try
    {
        action();
    }
    catch (Exception)
    {
        return;
    }
    throw new InvalidOperationException(message);
}

static int Edit(string[] args)
{
    var input = args[0];
    var output = args[1];
    var gameDir = args[2];
    var slot = SaveSlot.Parse(File.ReadAllBytes(input));
    var catalog = SaveCatalog.TryLoad(gameDir, "en", out var problem) ?? throw new InvalidOperationException(problem);
    foreach (var command in args[3..])
    {
        var parts = command.Split(':');
        switch (parts[0])
        {
            case "echoes": slot.SetEchoes(int.Parse(parts[1])); break;
            case "insight": slot.SetInsight(int.Parse(parts[1])); break;
            case "stat": slot.SetStat(Enum.Parse<SaveStat>(parts[1]), int.Parse(parts[2])); break;
            case "goods":
                var goods = catalog.GoodsById[int.Parse(parts[1])];
                slot.AddGoods(goods.Id, int.Parse(parts[2]), goods.Sort, goods.KeyItem);
                break;
            case "allgoods":
                foreach (var g in catalog.Goods) slot.AddGoods(g.Id, 1, g.Sort, g.KeyItem);
                break;
            case "weapon":
                var weapon = catalog.Weapons.Single(w => w.Id == int.Parse(parts[1]));
                slot.AddWeapon(weapon.Id, weapon.Durability, weapon.Sort, weapon.GemSlots);
                break;
            case "armor":
                var armor = catalog.Armor.Single(a => a.Id == int.Parse(parts[1]));
                slot.AddArmor(armor.Id, armor.Durability, armor.Sort);
                break;
            case "qty":
                var list = Enum.Parse<SaveList>(parts[1]);
                slot.SetQuantity(slot.Items.Single(i => i.List == list && i.Id == int.Parse(parts[2])), int.Parse(parts[3]));
                break;
            case "defeat": SaveWorld.MarkDefeated(slot, SaveWorld.Bosses.Single(b => b.Defeated == int.Parse(parts[1]))); break;
            case "fight": SaveWorld.FightAgain(slot, SaveWorld.Bosses.Single(b => b.Defeated == int.Parse(parts[1]))); break;
            case "lock": SaveWorld.LockAgain(slot, SaveWorld.Bosses.Single(b => b.Defeated == int.Parse(parts[1]))); break;
            case "lamp": slot.SetFlag(int.Parse(parts[1]), parts[2] == "1"); break;
            case "list":
                Console.OutputEncoding = System.Text.Encoding.UTF8;
                Console.WriteLine($"goods {catalog.Goods.Count} ({catalog.Goods.Count(g => g.Dlc)} DLC) weapons {catalog.Weapons.Count} ({catalog.Weapons.Count(w => w.Dlc)} DLC) armor {catalog.Armor.Count} ({catalog.Armor.Count(a => a.Dlc)} DLC) lamps {catalog.Lamps.Count} ({catalog.Lamps.Count(l => l.Dlc)} DLC)");
                Console.WriteLine(string.Join(", ", catalog.Goods.Where(g => g.Dlc).Select(g => g.Name)));
                Console.WriteLine(string.Join(", ", catalog.Armor.Where(a => a.Dlc).Select(a => a.Name)));
                foreach (var l in catalog.Lamps) Console.WriteLine($"lamp {l.Flag} {l.Area}: {l.Name}");
                foreach (var i in slot.Items.Take(int.Parse(parts[1]))) Console.WriteLine($"{i.List} {catalog.ItemName(i.ItemId)} x{i.Quantity}");
                break;
            default: throw new ArgumentException(command);
        }
    }
    File.WriteAllBytes(output, slot.ToBytes());
    Console.WriteLine($"{slot.Name} level {slot.Level}, {slot.Echoes} echoes, {slot.Insight} insight, room for {slot.GearRoom} more gear");
    return 0;
}
