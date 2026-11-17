// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json.Nodes;
using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

SwitchTableChecks.Run(Require);
GcMarksChecks.Run(Require);
SafeModeChecks.Run(Require);
ExtraEnvChecks.Run(Require);

Require(Looks.SnapToOne(0.996) == 1 && Looks.SnapToOne(1.004) == 1 && Looks.SnapToOne(1) == 1, "Fog within 0.005 of 1 is exactly 1");
Require(Looks.SnapToOne(0.99) == 0.99 && Looks.SnapToOne(1.01) == 1.01 && Looks.SnapToOne(0.5) == 0.5, "Fog further from 1 keeps its value");
SeamlessPlacementChecks.Run(Require);

static JsonObject Obj(string json) => (JsonObject)JsonNode.Parse(json)!;

static string Str(JsonObject o, string key) => o[key]?.GetValue<string>() ?? "";

static bool Throws<T>(Action action) where T : Exception
{
    try
    {
        action();
        return false;
    }
    catch (T)
    {
        return true;
    }
}

if (args.Length == 3 && args[0] == "--ini-stress")
    return IniStress(args[1], int.Parse(args[2], System.Globalization.CultureInfo.InvariantCulture));

var root = Path.Combine(Path.GetTempPath(), "lantern-settings-checks-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
    IniChecks(Path.Combine(root, "ini"));
    PadSplitChecks();
    ProfileChecks(Path.Combine(root, "data"));
    SettingsJsonRefusals(Path.Combine(root, "refusals"));

    var file = Path.Combine(root, "config", "settings.json");
    Directory.CreateDirectory(Path.GetDirectoryName(file)!);
    File.WriteAllText(file, """{ "user_dir": "", "playtime": 10, "other_launcher_key": "kept" }""");

    Instance Open()
    {
        var defaults = Obj("""{ "user_dir": "", "playtime": 0, "ui_theme": "default" }""");
        var (disk, stamp) = SettingsFile.Read(file);
        foreach (var (key, value) in disk!) defaults[key] = value?.DeepClone();
        return new Instance((JsonObject)defaults.DeepClone(), defaults, stamp);
    }

    var play = Open();
    var main = Open();

    main.Current["user_dir"] = @"D:\Lantern\user";
    main.Save(file);
    var afterImport = SettingsFile.Read(file).Data!;
    Require(Str(afterImport, "user_dir") == @"D:\Lantern\user", "The import's saves folder must reach the file");
    Require(afterImport["ui_theme"]?.GetValue<string>() == "default", "A default missing from the file is written");
    Require(Str(afterImport, "other_launcher_key") == "kept", "Unknown keys stay");

    play.Current["playtime"] = 25;
    play.Save(file);
    var afterPlay = SettingsFile.Read(file).Data!;
    Require(Str(afterPlay, "user_dir") == @"D:\Lantern\user", "--play must not write back the user_dir it read before the import");
    Require(afterPlay["playtime"]!.GetValue<int>() == 25, "--play's own change must be written");
    Require(Str(play.Current, "user_dir") == @"D:\Lantern\user", "A save takes the other process's changes in");

    var stale = Open();
    Require(SettingsFile.StampOf(file) == stale.Stamp, "An unchanged file keeps its stamp");
    main.Current["ui_theme"] = "dark";
    main.Save(file);
    Require(SettingsFile.StampOf(file) != stale.Stamp, "Another process's save changes the stamp");
    stale.Current["playtime"] = 40;
    stale.Reload(file);
    Require(Str(stale.Current, "ui_theme") == "dark", "A reload must pick up keys changed on disk");
    Require(stale.Current["playtime"]!.GetValue<int>() == 40, "A reload must keep this process's unsaved change");
    stale.Save(file);
    var afterStale = SettingsFile.Read(file).Data!;
    Require(Str(afterStale, "ui_theme") == "dark" && afterStale["playtime"]!.GetValue<int>() == 40, "Reloaded and unsaved changes are both kept");

    var remover = Open();
    remover.Current.Remove("other_launcher_key");
    var keeper = Open();
    keeper.Current["user_dir"] = @"E:\Saves";
    keeper.Save(file);
    remover.Save(file);
    var afterRemove = SettingsFile.Read(file).Data!;
    Require(!afterRemove.ContainsKey("other_launcher_key"), "A removed key is removed from the file");
    Require(Str(afterRemove, "user_dir") == @"E:\Saves", "A removal does not undo another process's change");

    File.WriteAllText(file, """{ "user_dir": "", "player_profile": "", "playtime": 0 }""");
    var a = Open2(file);
    var b = Open2(file);
    a.Current["user_dir"] = @"E:\Saves";
    a.Current["player_profile"] = "Second";
    a.Save(file);
    b.Current["playtime"] = 77;
    b.Save(file);
    var fresh = Open2(file);
    Require(Str(fresh.Current, "user_dir") == @"E:\Saves" && Str(fresh.Current, "player_profile") == "Second" && fresh.Current["playtime"]!.GetValue<int>() == 77,
        "Two instances on one file: a fresh load has the saves folder, the profile and the playtime");

    var first = Open2(file);
    var second = Open2(file);
    first.Current["playtime"] = 100;
    second.Current["playtime"] = 200;
    first.Save(file);
    second.Save(file);
    Require(Open2(file).Current["playtime"]!.GetValue<int>() == 200, "Both change the same key: the later save wins");
    first.Current["playtime"] = 300;
    first.Save(file);
    Require(Open2(file).Current["playtime"]!.GetValue<int>() == 300, "The later save wins the other way round too");

    File.WriteAllText(file, """{ "exp_occlusion": true, "ui_theme": "light" }""");
    var switched = OpenWithSwitch(file);
    Require(switched.Current["exp_occlusion"]!.GetValue<bool>() == false && switched.Current["occlusion_off_99"]!.GetValue<bool>(), "The one-time switch is applied on load");
    var other = Open2(file);
    other.Current["ui_theme"] = "dark";
    other.Save(file);
    switched.Reload(file);
    Require(switched.Current["exp_occlusion"]!.GetValue<bool>() == false && Str(switched.Current, "ui_theme") == "dark", "A reload keeps the one-time switch and takes the other change in");
    switched.Save(file);
    var afterSwitch = SettingsFile.Read(file).Data!;
    Require(afterSwitch["exp_occlusion"]!.GetValue<bool>() == false && afterSwitch["occlusion_off_99"]!.GetValue<bool>() && Str(afterSwitch, "ui_theme") == "dark",
        "The merge writes the one-time switch and its marker, and keeps the other process's change");
    var later = OpenWithSwitch(file);
    later.Current["exp_occlusion"] = true;
    later.Save(file);
    Require(OpenWithSwitch(file).Current["exp_occlusion"]!.GetValue<bool>(), "After the marker the player's own choice stays");

    File.WriteAllText(file, """{ "user_dir": "D:\\Good", "playtime": 5 }""");
    var good = Open2(file);
    good.Save(file);
    Require(File.Exists(SettingsFile.BackupOf(file)), "A save keeps a copy of the file it wrote");
    File.WriteAllText(file, """{ "user_dir": "D:\\Go""");
    var halfRead = SettingsFile.Read(file).Data;
    Require(halfRead != null && Str(halfRead, "user_dir") == @"D:\Good" && halfRead["playtime"]!.GetValue<int>() == 5, "A half-written file falls back to the last good copy, not the defaults");
    Require(File.Exists(SettingsFile.BrokenOf(file)), "The broken file is kept beside it");
    var afterBroken = Open2(file);
    afterBroken.Current["playtime"] = 6;
    afterBroken.Save(file);
    var repaired = SettingsFile.Read(file).Data!;
    Require(Str(repaired, "user_dir") == @"D:\Good" && repaired["playtime"]!.GetValue<int>() == 6, "Saving over a broken file keeps the settings of the last good copy");

    var finisher = Path.Combine(root, "late", "settings.json");
    Directory.CreateDirectory(Path.GetDirectoryName(finisher)!);
    File.WriteAllText(finisher, """{ "user_dir": "D:\\Gam""");
    var writer = Task.Run(() => { Thread.Sleep(60); File.WriteAllText(finisher, """{ "user_dir": "D:\\Game" }"""); });
    var late = SettingsFile.Read(finisher).Data;
    writer.Wait();
    Require(late != null && Str(late, "user_dir") == @"D:\Game", "A file another launcher is still writing is read again until it is whole");

    File.WriteAllText(finisher, "{ broken");
    File.Delete(SettingsFile.BackupOf(finisher));
    var noCopy = Open2(finisher);
    noCopy.Current["playtime"] = 1;
    noCopy.Save(finisher);
    Require(SettingsFile.Read(finisher).Data!["playtime"]!.GetValue<int>() == 1, "A broken file without a copy is rewritten");
    Require(File.ReadAllText(SettingsFile.BrokenOf(finisher)) == "{ broken", "and what was there is kept");
    Require(File.Exists(finisher + ".lock"), "The lock file stays after a save, so every launcher locks the same file");

    var packageA = Path.Combine(root, "package-a", "user");
    var packageB = Path.Combine(root, "package-b", "user");
    Directory.CreateDirectory(packageA);
    Directory.CreateDirectory(packageB);
    var saveDir = Path.Combine(packageA, "savedata", "1", "CUSA00207", "SPRJ0005");
    Directory.CreateDirectory(saveDir);
    File.WriteAllText(Path.Combine(saveDir, "userdata0000"), "x");
    Require(SavesFolder.HasSaves(packageA) && !SavesFolder.HasSaves(packageB), "Saves are found where they are");
    Require(SavesFolder.Moved("", "", packageA) == null, "The first start asks nothing");
    Require(SavesFolder.Moved("", packageA, packageA + Path.DirectorySeparatorChar) == null, "The same folder asks nothing");
    Require(SavesFolder.Moved("", packageA, packageB) == SavesFolder.Full(packageA), "Another copy's start with an empty saves folder asks, naming the last folder");
    Require(SavesFolder.Moved(@"E:\Chosen", packageA, packageB) == null, "A chosen saves folder asks nothing");
    Require(SavesFolder.Moved("", Path.Combine(root, "gone", "user"), packageB) == null, "A last folder that is gone asks nothing");
    var bSave = Path.Combine(packageB, "savedata", "1", "CUSA00207", "SPRJ0005");
    Directory.CreateDirectory(bSave);
    File.WriteAllText(Path.Combine(bSave, "userdata0000"), "x");
    Require(SavesFolder.Moved("", packageA, packageB) == SavesFolder.Full(packageA), "It asks even when this copy's folder has a hunter");

    var shared = Path.Combine(root, "shared", "settings.json");
    Directory.CreateDirectory(Path.GetDirectoryName(shared)!);
    File.WriteAllText(shared, """{ "user_dir": "" }""");
    var copyA = Open2(shared);
    copyA.Current[SavesFolder.LastRootKey] = SavesFolder.Full(packageA);
    copyA.Save(shared);
    var copyB = Open2(shared);
    Require(SavesFolder.Moved(Str(copyB.Current, "user_dir"), Str(copyB.Current, SavesFolder.LastRootKey), packageB) == SavesFolder.Full(packageA),
        "The last start's folder written by one copy makes the other copy ask");
    Require(Str(copyB.Current, "user_dir") == "", "Nothing pins the saves folder by itself");

    Require(Path.IsPathFullyQualified(SavesFolder.Absolute(Path.Combine("relative", "user"))), "A relative saves folder becomes absolute");
    Require(SavesFolder.Absolute("") == "", "No folder stays empty");

    Require(!SavesFolder.InUse(false, () => false), "No game: saves can change");
    Require(SavesFolder.InUse(true, () => false), "The launcher's own game holds the saves");
    Require(SavesFolder.InUse(false, () => true), "A game started by Bloodborne.exe, Steam or an in-game restart holds the saves");
    var asked = false;
    Require(SavesFolder.InUse(true, () => { asked = true; return false; }) && !asked, "The launcher's own game needs no process search");

    var profileOnly = Path.Combine(root, "profiles-only", "user");
    var profileSaves = Path.Combine(profileOnly, "profiles", "Second", "savedata", "1", "CUSA00207", "SPRJ0005");
    Directory.CreateDirectory(profileSaves);
    File.WriteAllText(Path.Combine(profileSaves, "userdata0010"), "x");
    Require(SavesFolder.HasSaves(profileOnly), "A profile's saves count");
    if (OperatingSystem.IsWindows())
        Require(SavesFolder.Same(packageA.ToUpperInvariant(), packageA), "Windows paths compare without case");

    var engine = new LauncherSettings();
    foreach (var key in EngineFeatures.Keys)
    {
        Require(LauncherSettings.Default(key) is JsonValue flag && flag.TryGetValue<bool>(out _), "Every engine feature has an on/off default: " + key);
        engine.Set(key, !EngineFeatures.Default(key));
    }
    Require(EngineFeatures.Keys.Any(EngineFeatures.Default) && EngineFeatures.Keys.Any(k => !EngineFeatures.Default(k)), "Engine features are on and off by default");
    Require(EngineFeatures.Changed(engine).Count() == EngineFeatures.Keys.Length, "Every flipped engine feature counts as changed");
    Require(EnginePreset.Current(engine) != EnginePreset.New, "Flipped features leave the New preset");
    EngineFeatures.Reset(engine);
    foreach (var key in EngineFeatures.Keys)
        Require(engine.Bool(key) == EngineFeatures.Default(key), "Reset brings the engine feature back to its default: " + key);
    Require(!EngineFeatures.Changed(engine).Any(), "Nothing is changed after a reset");
    Require(EnginePreset.Current(engine) == EnginePreset.New, "A reset puts the engine preset back to New");
    EngineFeatures.Reset(engine);
    Require(!EngineFeatures.Changed(engine).Any(), "A second reset changes nothing");

    var source = new DirectoryInfo(AppContext.BaseDirectory);
    while (source != null && !File.Exists(Path.Combine(source.FullName, "LanternLauncher", "Views", "EnginePage.cs")))
        source = source.Parent;
    Require(source != null, "The launcher sources are found");
    foreach (var page in new[] { Path.Combine("LanternLauncher", "Views", "EnginePage.cs"), Path.Combine("BloodborneLauncher", "Views", "EnginePage.cs") })
    {
        var keys = System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(Path.Combine(source!.FullName, page)), @"""(\w+)"", exp\)")
            .Select(m => m.Groups[1].Value).Order(StringComparer.Ordinal);
        Require(keys.SequenceEqual(EngineFeatures.Keys.Order(StringComparer.Ordinal)), "The engine features a reset covers are the ones " + page + " shows");
    }

    OnlineMigrationChecks.Run(root);
    Console.WriteLine("Settings and saves folder checks passed");
    return 0;
}
finally
{
    try { Directory.Delete(root, recursive: true); } catch (IOException) { }
}

static int Leftovers(string folder) => Directory.GetFiles(folder, "*.tmp").Length;

static void PadSplitChecks()
{
    static ControlDef Row(string name) => InputMap.Controls.Single(c => c.Name == name);
    Require(PadMap.InputFor(Row("dodge")) == "dodge" && PadMap.InputFor(Row("backstep")) == "backstep" && PadMap.InputFor(Row("sprint")) == "sprint",
        "The Dodge, Backstep and Sprint rows have controller inputs of their own");
    Require(PadMap.InputFor(Row("roll")) == "circle", "Dodge / sprint stays the game's Circle");
    Require(PadMap.IsSplit("dodge") && PadMap.IsSplit("backstep") && PadMap.IsSplit("sprint") && !PadMap.IsSplit("circle"), "Only the three rows are split inputs");

    var untouched = new IniFile();
    Require(PadMap.Current(untouched).Count == 0, "An untouched controller layout has nothing off its default");
    Require(PadMap.Main(untouched, "dodge") == "" && PadMap.Main(untouched, "backstep") == "" && PadMap.Main(untouched, "sprint") == "" && PadMap.Main(untouched, "circle") == "b",
        "The split inputs start unbound and Circle stays on B");

    var ini = new IniFile();
    PadMap.Assign(ini, "sprint", "leftshoulder");
    Require(ini.Raw("pad.sprint") == "leftshoulder", "Binding Sprint writes pad.sprint");
    Require(ini.Raw("pad.l1") == "", "The button Sprint took leaves L1");
    Require(ini.Raw("pad.circle") == null && PadMap.Main(ini, "circle") == "b", "Binding Sprint leaves Circle alone");
    Require(ini.Raw("pad.dodge") == null && ini.Raw("pad.backstep") == null, "Binding Sprint leaves Dodge and Backstep unbound");
    PadMap.Assign(ini, "circle", "leftshoulder");
    Require(ini.Raw("pad.circle") == "leftshoulder" && ini.Raw("pad.sprint") == "b", "A button another input uses swaps over, split inputs too");

    var dodge = new IniFile();
    PadMap.Assign(dodge, "dodge", "b");
    Require(dodge.Raw("pad.dodge") == "b" && dodge.Raw("pad.circle") == "", "B on Dodge takes it off Circle");
    PadMap.Assign(dodge, "dodge", "");
    Require(dodge.Raw("pad.dodge") == null && PadMap.Main(dodge, "dodge") == "", "Unbinding Dodge removes pad.dodge");
    PadMap.Assign(dodge, "backstep", "paddle1");
    PadMap.ResetAll(dodge);
    Require(new[] { "dodge", "backstep", "sprint", "circle" }.All(i => dodge.Raw("pad." + i) == null), "Reset all writes none of the split inputs");
    PadMap.Reset(ini, "sprint");
    Require(ini.Raw("pad.sprint") == null, "Resetting Sprint removes pad.sprint");

    var profile = new ControlProfiles.Profile("Pad split", "", "", ControlProfiles.DefaultBindings(),
        Pad: new Dictionary<string, string> { ["sprint"] = "rightstick", ["bogus"] = "a" });
    var parsed = ControlProfiles.Parse(ControlProfiles.ToJson(profile));
    Require(parsed.Profile.Pad is { } pad && pad.GetValueOrDefault("sprint") == "rightstick" && !pad.ContainsKey("bogus"), "A profile keeps pad.sprint and skips an unknown input");
    Require(parsed.Notes.Any(n => n.Contains("bogus")), "The skipped input is noted");
    var applied = new IniFile();
    PadMap.Apply(applied, parsed.Profile.Pad);
    Require(applied.Raw("pad.sprint") == "rightstick" && applied.Raw("pad.dodge") == null, "Applying the profile writes pad.sprint only");
    Console.WriteLine("Controller split rows checks passed");
}

static void IniChecks(string folder)
{
    Directory.CreateDirectory(folder);
    var file = Path.Combine(folder, "lantern.ini");
    const string original = "# mine\nupscaler=fsr3\nlauncher_only=kept\n";
    File.WriteAllText(file, original);

    var ini = new IniFile(file);
    ini.Load();
    Require(ini.LastRead == TextState.Read && ini.Get("upscaler") == "fsr3", "The ini at the given path is read");
    ini.Set("preset", "2");
    using (new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.None))
    {
        Require(Throws<SettingsUnreadableException>(() => ini.Save()), "A save over a file that cannot be read tells the user");
        Require(Throws<SettingsUnreadableException>(() => ini.Save()), "A second refused save is reported too");
    }
    Require(File.ReadAllText(file) == original, "A file that cannot be read during a save keeps its content");
    Require(Leftovers(folder) == 0, "A refused save leaves no temporary file");
    Require(ini.Get("preset") == "2", "A refused save keeps the change in the launcher");

    Require(ini.Save() == SaveOutcome.Written, "Once readable, the save goes through");
    var saved = File.ReadAllText(file);
    Require(saved.Contains("\npreset=2\n") && saved.Contains("\nlauncher_only=kept\n") && saved.StartsWith("# mine\n"), "The kept change is written with the other keys and the comment");
    Require(File.ReadAllText(SettingsText.BackupOf(file)) == original, "The previous content is kept as lantern.ini.bak");
    Require(!saved.StartsWith('\uFEFF') && File.ReadAllBytes(file)[0] == (byte)'#', "The file is UTF-8 without a byte order mark");
    Require(Leftovers(folder) == 0, "A save leaves no temporary file");

    var unreadable = new IniFile(file);
    using (new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.None))
        unreadable.Load();
    Require(unreadable.LastRead == TextState.Unreadable, "A file held by another program loads as unreadable, not as empty");
    unreadable.Set("sharpen", "0");
    Require(unreadable.Save() == SaveOutcome.Written, "A save after an unreadable load reads the file again");
    var merged = new IniFile(file);
    merged.Load();
    Require(merged.Get("preset") == "2" && merged.Get("sharpen") == "0" && merged.Get("launcher_only") == "kept", "Defaults from the unreadable load do not replace what the file holds");

    var linked = Path.Combine(folder, "linked.ini");
    var otherName = Path.Combine(folder, "linked-other-name.ini");
    File.WriteAllText(linked, "a=1\n");
    if (Links.Make(otherName, linked))
    {
        var replaced = new IniFile(linked);
        replaced.Load();
        replaced.Set("a", "2");
        Require(replaced.Save() == SaveOutcome.Written, "The linked file is saved");
        Require(File.ReadAllText(otherName) == "a=1\n", "A save replaces lantern.ini instead of writing into it");
        Require(File.ReadAllText(linked).Contains("a=2\n"), "The saved file has the new value");
    }
    else
    {
        Console.WriteLine("SKIPPED: replace check (no hard links here)");
    }

    var fgFile = Path.Combine(folder, "frame-gen.ini");
    File.WriteAllText(fgFile, "frame_gen=fsr3\nupscaler=fsr\n");
    var launcherIni = new IniFile(fgFile);
    launcherIni.Load();
    Require(!launcherIni.ReloadIfChanged(), "An ini nobody else wrote is not reloaded");
    launcherIni.Set("frame_gen", "fsr");
    launcherIni.Save();
    Require(File.ReadAllText(fgFile).Contains("frame_gen=fsr\n"), "The launcher's frame generation choice is in lantern.ini before the game starts");
    Require(!launcherIni.ReloadIfChanged(), "The launcher's own save is not taken for an outside change");
    var engineStart = new IniFile(fgFile);
    engineStart.Load();
    Require(engineStart.Get("frame_gen") == "fsr", "The game reads frame_gen=fsr at its first start");
    File.WriteAllText(fgFile, File.ReadAllText(fgFile).Replace("upscaler=fsr\n", "upscaler=dlss\n"));
    Require(launcherIni.ReloadIfChanged() && launcherIni.Get("upscaler") == "dlss" && launcherIni.Get("frame_gen") == "fsr",
        "A game write of another key keeps frame_gen=fsr");
    File.WriteAllText(fgFile, File.ReadAllText(fgFile).Replace("frame_gen=fsr\n", "frame_gen=off\n"));
    Require(launcherIni.ReloadIfChanged() && launcherIni.Get("frame_gen") == "off", "A choice made in game shows in the launcher");
    launcherIni.Set("frame_gen", "fsr");
    File.WriteAllText(fgFile, File.ReadAllText(fgFile).Replace("upscaler=dlss\n", "upscaler=fsr\n"));
    launcherIni.ReloadIfChanged();
    Require(launcherIni.Get("frame_gen") == "fsr" && launcherIni.Get("upscaler") == "fsr", "A reload keeps the launcher's unsaved choice");
    launcherIni.Save();
    Require(File.ReadAllText(fgFile).Contains("frame_gen=fsr\n"), "The kept choice is saved over the game's value");

    var absent = new IniFile(Path.Combine(folder, "absent", "lantern.ini"));
    absent.Load();
    Require(absent.LastRead == TextState.Absent, "A missing file loads as absent");

    const int rounds = 150;
    var a = new IniFile(file);
    var b = new IniFile(file);
    a.Load();
    b.Load();
    void Writer(IniFile writer, string name)
    {
        for (var i = 0; i < rounds; i++)
        {
            writer.Set($"{name}_{i}", i.ToString(System.Globalization.CultureInfo.InvariantCulture));
            writer.Save();
        }
    }
    var first = new Thread(() => Writer(a, "a"));
    var second = new Thread(() => Writer(b, "b"));
    first.Start();
    second.Start();
    first.Join();
    second.Join();
    var both = new IniFile(file);
    both.Load();
    for (var i = 0; i < rounds; i++)
        Require(both.Raw($"a_{i}") != null && both.Raw($"b_{i}") != null, $"Two writers in a loop never lose a key (round {i})");
    Require(Leftovers(folder) == 0, "Two writers leave no temporary file");
}

static void ProfileChecks(string folder)
{
    Directory.CreateDirectory(folder);
    Environment.SetEnvironmentVariable("BB_DATA_DIR", folder);
    try
    {
        var mods = Mods.ProfilePath;
        var modsRoot = Path.Combine(folder, "mods");
        var entries = new[] { new Mods.Entry("alpha", true) };
        const string modsText = "{ \"order\": [\"alpha\"], \"disabled\": [\"alpha\"] }\n";
        File.WriteAllText(mods, modsText);
        using (new FileStream(mods, FileMode.Open, FileAccess.Read, FileShare.None))
            Mods.Load(modsRoot);
        Require(Throws<SettingsUnreadableException>(() => Mods.Save(entries)), "mods.json is not written after an unreadable load");
        Require(Throws<SettingsUnreadableException>(() => Mods.Save(entries)), "Every refused mods.json save is reported");
        Require(File.ReadAllText(mods) == modsText, "mods.json keeps its content after an unreadable load");
        Mods.Load(modsRoot);
        Require(Mods.Save(entries) == SaveOutcome.Written, "After a readable load mods.json is written");
        Require(File.ReadAllText(SettingsText.BackupOf(mods)) == modsText, "The previous mods.json is kept as mods.json.bak");
        File.WriteAllText(mods, "{ broken");
        Mods.Load(modsRoot);
        Require(File.ReadAllText(Mods.BrokenOf(mods)) == "{ broken", "A broken mods.json is kept as mods.json.broken");
        Require(Mods.Save(entries) == SaveOutcome.Written, "A broken mods.json with its copy kept is written again");

        var patches = Patches.ProfilePath;
        const string patchesText = "{ \"enabled\": [\"a.xml/One\"], \"disabled\": [\"a.xml/Two\"] }\n";
        File.WriteAllText(patches, patchesText);
        var one = new Patches.Patch("a.xml/One", "One", "a.xml", "", "", false);
        var two = new Patches.Patch("a.xml/Two", "Two", "a.xml", "", "", true);
        using (new FileStream(patches, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var unread = Patches.LoadProfile();
            Require(!Patches.IsOn(one, unread) && Patches.IsOn(two, unread), "An unreadable patches.json shows the files' defaults");
        }
        Require(Throws<SettingsUnreadableException>(() => Patches.Save(new[] { (one, false), (two, true) })), "patches.json is not written after an unreadable load");
        Require(File.ReadAllText(patches) == patchesText, "The player's patch choices stay after an unreadable load");
        var profile = Patches.LoadProfile();
        Require(Patches.IsOn(one, profile) && !Patches.IsOn(two, profile), "A readable load has the player's choices");
        Require(Patches.Save(new[] { (one, true), (two, true) }) == SaveOutcome.Written, "After a readable load patches.json is written");
        var saved = Patches.LoadProfile();
        Require(Patches.IsOn(one, saved) && Patches.IsOn(two, saved), "The new choices are saved");
        File.WriteAllText(patches, "[1, 2]");
        var broken = Patches.LoadProfile();
        Require(!Patches.IsOn(one, broken) && File.ReadAllText(Mods.BrokenOf(patches)) == "[1, 2]", "A patches.json that is not an object is kept as patches.json.broken");
        Require(Leftovers(folder) == 0, "Profile saves leave no temporary file");
    }
    finally
    {
        Environment.SetEnvironmentVariable("BB_DATA_DIR", null);
    }
}

static void SettingsJsonRefusals(string folder)
{
    Directory.CreateDirectory(folder);
    var file = Path.Combine(folder, "settings.json");
    const string original = "{ \"playtime\": 3 }";
    File.WriteAllText(file, original);
    var (disk, stamp) = SettingsFile.Read(file);
    var (data, baseline) = SettingsFile.Layer(Obj("""{ "playtime": 0 }"""), disk);
    data["playtime"] = 4;
    using (new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.None))
    {
        Require(SettingsFile.ReadWithState(file).State == TextState.Unreadable, "A held settings.json reads as unreadable");
        Require(Throws<SettingsUnreadableException>(() => SettingsFile.Save(file, baseline, data)), "settings.json is not written while it cannot be read");
    }
    Require(File.ReadAllText(file) == original, "settings.json keeps its content");
    using (new FileStream(file + ".lock", FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
        Require(Throws<SettingsBusyException>(() => SettingsFile.Save(file, baseline, data)), "A settings.json lock held by another launcher refuses the save");
    Require(File.ReadAllText(file) == original, "settings.json keeps its content while the lock is held");
    SettingsFile.Save(file, baseline, data);
    Require(SettingsFile.Read(file).Data!["playtime"]!.GetValue<int>() == 4 && stamp != SettingsFile.StampOf(file), "Once free, the save goes through");
    Require(File.ReadAllText(SettingsFile.BackupOf(file)) == original, "The previous settings.json is kept as settings.json.bak");
}

static int IniStress(string engineWriter, int cycles)
{
    var folder = Path.Combine(Path.GetTempPath(), "lantern-ini-stress-" + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(folder);
    var file = Path.Combine(folder, "lantern.ini");
    File.WriteAllText(file, "# stress\nlauncher_only=kept\n");
    var start = new System.Diagnostics.ProcessStartInfo(engineWriter) { UseShellExecute = false, RedirectStandardOutput = true };
    start.ArgumentList.Add("--writer");
    start.ArgumentList.Add(file);
    start.ArgumentList.Add(cycles.ToString(System.Globalization.CultureInfo.InvariantCulture));

    long readerReads = 0, readerAbsent = 0, readerFailures = 0, readerLost = 0;
    int loadFailures = 0, refusals = 0, busy = 0, replaceFailures = 0, lostKeys = 0, lostUpdates = 0;
    var stop = false;
    var reader = new Thread(() =>
    {
        while (!Volatile.Read(ref stop))
        {
            var read = SettingsText.Read(file);
            Interlocked.Increment(ref readerReads);
            if (read.State == TextState.Absent)
                Interlocked.Increment(ref readerAbsent);
            else if (read.State == TextState.Unreadable)
                Interlocked.Increment(ref readerFailures);
            else if (!read.Text.Contains("\nlauncher_only=kept\n") || !read.Text.Contains("\nupscaler="))
                Interlocked.Increment(ref readerLost);
        }
    });

    var clock = System.Diagnostics.Stopwatch.StartNew();
    using var engine = System.Diagnostics.Process.Start(start)!;
    var engineOutput = engine.StandardOutput.ReadToEndAsync();
    var ini = new IniFile(file);
    ini.Load();
    ini.Set("launcher_seq", "0");
    while (true)
    {
        try
        {
            ini.Save();
            break;
        }
        catch (SettingsBusyException)
        {
            busy++;
        }
    }
    reader.Start();
    var lastSeq = 0L;
    string? lastShown = null;
    var engineSeen = false;
    for (var i = 1; i <= cycles; i++)
    {
        ini.Load();
        if (ini.LastRead != TextState.Read)
        {
            loadFailures++;
        }
        else
        {
            var engineKey = ini.Raw("menu_language") != null;
            if (ini.Raw("launcher_only") != "kept" || (engineSeen && !engineKey))
                lostKeys++;
            engineSeen |= engineKey;
            if (long.TryParse(ini.Raw("launcher_seq"), out var seq) && seq < lastSeq)
                lostUpdates++;
            if (lastShown != null && ini.Raw("show_fps") != lastShown)
                lostUpdates++;
        }
        var shown = (i % 2).ToString(System.Globalization.CultureInfo.InvariantCulture);
        ini.Set("launcher_seq", i.ToString(System.Globalization.CultureInfo.InvariantCulture));
        ini.Set("show_fps", shown);
        try
        {
            ini.Save();
            lastSeq = i;
            lastShown = shown;
        }
        catch (SettingsUnreadableException)
        {
            refusals++;
        }
        catch (SettingsBusyException)
        {
            busy++;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            replaceFailures++;
        }
    }
    engine.WaitForExit();
    Volatile.Write(ref stop, true);
    reader.Join();
    var final = new IniFile(file);
    final.Load();
    if (final.Raw("launcher_only") != "kept" || final.Raw("menu_language") == null || final.Raw("launcher_seq") == null)
        lostKeys++;
    var leftovers = Leftovers(folder);
    Console.WriteLine($"launcher cycles={cycles} load_unreadable={loadFailures} save_refused={refusals} busy={busy} replace_failed={replaceFailures} lost_keys={lostKeys} lost_updates={lostUpdates} leftovers={leftovers}");
    Console.WriteLine($"launcher reader reads={readerReads} absent={readerAbsent} unreadable={readerFailures} lost_keys={readerLost}");
    Console.Write(engineOutput.Result);
    Console.WriteLine($"seconds={clock.Elapsed.TotalSeconds:F0} engine_exit={engine.ExitCode}");
    try { Directory.Delete(folder, recursive: true); } catch (IOException) { }
    return lostKeys == 0 && lostUpdates == 0 && readerLost == 0 && readerAbsent == 0 && readerFailures == 0 && leftovers == 0 && engine.ExitCode == 0 ? 0 : 1;
}

static Instance OpenWithSwitch(string file)
{
    var (disk, stamp) = SettingsFile.Read(file);
    var defaults = new Dictionary<string, JsonNode?> { ["exp_occlusion"] = true, ["ui_theme"] = "default" };
    var (data, baseline) = SettingsFile.Layer(defaults, disk);
    if (disk != null && !disk.ContainsKey("occlusion_off_99"))
        data["exp_occlusion"] = false;
    data["occlusion_off_99"] = true;
    return new Instance(data, baseline, stamp);
}

static Instance Open2(string file)
{
    var (disk, stamp) = SettingsFile.Read(file);
    var (data, baseline) = SettingsFile.Layer(Obj("""{ "playtime": 0 }"""), disk);
    return new Instance(data, baseline, stamp);
}

sealed class Instance
{
    public JsonObject Current;
    public JsonObject Baseline;
    public SettingsFile.Stamp Stamp;

    public Instance(JsonObject current, JsonObject baseline, SettingsFile.Stamp stamp)
    {
        Current = current;
        Baseline = baseline;
        Stamp = stamp;
    }

    public void Save(string file)
    {
        var (merged, stamp) = SettingsFile.Save(file, Baseline, Current);
        Baseline = (JsonObject)merged.DeepClone();
        Current = merged;
        Stamp = stamp;
    }

    public void Reload(string file)
    {
        var (disk, stamp) = SettingsFile.Read(file);
        (Current, Baseline) = SettingsFile.Refresh(disk!, Baseline, Current);
        Stamp = stamp;
    }
}

static class Links
{
    [System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateHardLink(string newName, string existing, IntPtr security);

    [System.Runtime.InteropServices.DllImport("libc", SetLastError = true)]
    private static extern int link(string existing, string newName);

    public static bool Make(string newName, string existing) =>
        OperatingSystem.IsWindows() ? CreateHardLink(newName, existing, IntPtr.Zero) : link(existing, newName) == 0;
}
