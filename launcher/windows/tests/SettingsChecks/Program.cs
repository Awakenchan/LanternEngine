// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json.Nodes;
using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static JsonObject Obj(string json) => (JsonObject)JsonNode.Parse(json)!;

static string Str(JsonObject o, string key) => o[key]?.GetValue<string>() ?? "";

var root = Path.Combine(Path.GetTempPath(), "lantern-settings-checks-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
try
{
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
    Require(!File.Exists(finisher + ".lock"), "The lock file is gone after a save");

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

    Console.WriteLine("Settings and saves folder checks passed");
    return 0;
}
finally
{
    try { Directory.Delete(root, recursive: true); } catch (IOException) { }
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
