using BloodborneLauncher.Core;
using System.Globalization;

var root = Path.Combine(AppContext.BaseDirectory, "lantern-backup-checks-" + Guid.NewGuid());
Directory.CreateDirectory(root);
try
{
    var s = new LauncherSettings { Root = root };
    var source = SaveBackups.SaveData(s);
    Directory.CreateDirectory(source);
    var file = Path.Combine(source, "userdata0000");
    File.WriteAllText(file, "freshly completed save");
    var recent = SaveBackups.CreateDuringPlay(s);
    Check(recent != null, "a completed recent write must not wait ten more minutes");
    Check(SaveBackups.CreateDuringPlay(s) == null, "unchanged saves are not duplicated");
    File.Delete(Path.Combine(recent!, "fingerprint.txt")); // native snapshots need content-based deduplication
    Check(SaveBackups.Create(s) == null, "unchanged native snapshots are not duplicated before Play");
    string Named(string name) => SaveBackups.CreateNamed(s, name);
    var named = Named("Before Gascoigne");
    Check(SaveBackups.List(s).Any(b => b.Dir == named && b.Label == "Before Gascoigne"), "named label survives listing");
    Check(Named("Before Gascoigne") != named, "same-second named copies never overwrite");
    foreach (var invalid in new[] { "", "../outside", "a/b", "a\\b", new string('x', 81) })
    {
        try { Named(invalid); throw new Exception("invalid name was accepted: " + invalid); }
        catch (ArgumentException) { }
    }
    var backups = SaveBackups.Folder(s);
    Directory.Move(recent!, Path.Combine(backups, DateTime.Now.AddMinutes(-5).ToString("yyyy-MM-dd_HH-mm-ss", CultureInfo.InvariantCulture) + "_play"));
    for (var i = 0; i < SaveBackups.PlayKeep + 3; i++)
    {
        var old = Path.Combine(backups, DateTime.Now.AddDays(-1).AddMinutes(i).ToString("yyyy-MM-dd_HH-mm-ss", CultureInfo.InvariantCulture) + "_play");
        Directory.CreateDirectory(Path.Combine(old, "savedata"));
        File.WriteAllText(Path.Combine(old, "savedata", "userdata0000"), "old");
    }
    File.WriteAllText(file, "changed save");
    File.SetLastWriteTimeUtc(file, DateTime.UtcNow.AddMinutes(-1));
    SaveBackups.CreateDuringPlay(s);
    Check(Directory.Exists(named), "named copy survives automatic pruning");
    Check(SaveBackups.List(s).Count(b => b.MadeDuringPlay && b.Label == null) <= SaveBackups.PlayKeep, "play retention enforced");
    Check(!Directory.GetDirectories(backups).Any(d => d.EndsWith(".copying")), "no unfinished copies remain");
    var selected = SaveBackups.List(s).Single(b => b.Dir == named);
    using (var live = new FileStream(Path.Combine(backups, ".session.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
    {
        try { SaveBackups.Restore(s, selected); throw new Exception("restored during a direct game session"); }
        catch (IOException) { }
    }
    using (var busy = new FileStream(Path.Combine(backups, ".backup.lock"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
    {
        try { Named("During a transaction"); throw new Exception("copied an active save transaction"); }
        catch (IOException) { }
    }
    Check(!SaveBackups.List(s).Any(b => b.Label == "During a transaction"), "busy snapshots are not published");
    Directory.CreateDirectory(Path.Combine(source, "write.lantern-work"));
    Check(SaveBackups.CreateDuringPlay(s) == null, "incomplete write is deferred");
    Directory.Delete(Path.Combine(source, "write.lantern-work"));
    SaveBackups.Restore(s, selected);
    Check(File.ReadAllText(file) == "freshly completed save", "named backup restores original content");
    void Delete(SaveBackups.Backup backup) => SaveBackups.Delete(s, backup);
    var other = SaveBackups.List(s).First(b => b.Dir != named);
    using (var busy = new FileStream(Path.Combine(backups, ".backup.lock"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
    {
        try { Delete(selected); throw new Exception("deleted while a snapshot writer held the profile lock"); }
        catch (IOException) { }
    }
    Check(Directory.Exists(named), "busy deletion leaves the snapshot intact");
    try { Delete(selected with { SafetyCopy = true }); throw new Exception("accepted a local path marked as a vault copy"); }
    catch (ArgumentException) { }
    var notes = Path.Combine(backups, "notes");
    Directory.CreateDirectory(notes);
    File.WriteAllText(Path.Combine(notes, "keep.txt"), "not a save backup");
    try { Delete(selected with { Dir = notes }); throw new Exception("accepted an unrelated folder"); }
    catch (ArgumentException) { }
    Check(File.Exists(Path.Combine(notes, "keep.txt")), "unrecognized folders are preserved");
    foreach (var outside in new[] { source, root, Path.Combine(root, "launcher-backups", "Other") })
    {
        Directory.CreateDirectory(outside);
        try { Delete(selected with { Dir = outside }); throw new Exception("accepted a path outside the profile's backups"); }
        catch (ArgumentException) { }
    }
    Delete(selected);
    Check(!Directory.Exists(named), "explicit deletion removes a named snapshot");
    Check(Directory.Exists(other.Dir), "deleting a snapshot preserves other backups");
    Check(File.ReadAllText(file) == "freshly completed save", "deleting a snapshot preserves live saves");
    Delete(other);
    Check(!Directory.Exists(other.Dir), "automatic backups can also be explicitly deleted");
    Delete(selected); // a concurrent refresh or prior deletion is harmless
    File.WriteAllText(file, "unique progress for mirrored-deletion regression");
    var mirrored = Named("Delete with safety mirror");
    var mirrorBackup = SaveBackups.List(s).Single(b => b.Dir == mirrored);
    var vault = SaveGuard.ProfileCopies(root, "Main");
    var mirrorCopy = Path.Combine(vault, "2020-01-01_00-00-00");
    SaveBackups.Copy(Path.Combine(mirrored, "savedata"), Path.Combine(mirrorCopy, "savedata"));
    File.Copy(Path.Combine(mirrored, "fingerprint.txt"), Path.Combine(mirrorCopy, "fingerprint.txt"));
    Check(!SaveBackups.ListWithSafetyCopies(backups, vault).Any(b => b.Dir == mirrorCopy), "safety mirror is initially hidden behind its regular backup");
    File.Delete(Path.Combine(mirrored, "fingerprint.txt")); // native snapshots may not have this metadata
    var unrelatedCopy = Path.Combine(vault, "2019-01-01_00-00-00");
    Directory.CreateDirectory(Path.Combine(unrelatedCopy, "savedata"));
    File.WriteAllText(Path.Combine(unrelatedCopy, "savedata", "userdata0000"), "unrelated recovery state");
    // A safety copy can have stale metadata after a concurrent game write during its creation.
    File.Copy(Path.Combine(mirrorCopy, "fingerprint.txt"), Path.Combine(unrelatedCopy, "fingerprint.txt"));
    Check(SaveBackups.ListWithSafetyCopies(backups, vault).Any(b => b.Dir == unrelatedCopy), "distinct recovery contents are visible even with matching metadata");
    var otherProfileCopy = Path.Combine(SaveGuard.ProfileCopies(root, "Other"), "2019-01-01_00-00-00");
    Directory.CreateDirectory(Path.Combine(otherProfileCopy, "savedata"));
    File.WriteAllText(Path.Combine(otherProfileCopy, "savedata", "userdata0000"), "another player's progress");
    using (var vaultBusy = new FileStream(Path.Combine(vault, ".vault.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
    {
        try { Delete(mirrorBackup); throw new Exception("deleted during a vault write"); }
        catch (IOException) { }
    }
    Check(Directory.Exists(mirrored) && Directory.Exists(mirrorCopy), "vault lock conflict preserves both copies");
    try { Delete(mirrorBackup with { Dir = otherProfileCopy, SafetyCopy = true }); throw new Exception("deleted another profile's safety copy"); }
    catch (ArgumentException) { }
    Delete(mirrorBackup);
    Check(!Directory.Exists(mirrorCopy), "deleting a backup also removes its matching hidden safety mirror");
    Check(!SaveBackups.ListWithSafetyCopies(backups, vault).Any(b => b.Dir == mirrorCopy), "deleted backup must not reappear as Safety copy");
    Check(Directory.Exists(unrelatedCopy) && Directory.Exists(otherProfileCopy), "unrelated safety copies and other profiles are preserved");
    var safetyOnly = Path.Combine(vault, "2020-01-02_00-00-00");
    Directory.CreateDirectory(Path.Combine(safetyOnly, "savedata"));
    File.WriteAllText(Path.Combine(safetyOnly, "savedata", "userdata0000"), "a safety-only copy");
    File.WriteAllText(Path.Combine(safetyOnly, "fingerprint.txt"), SaveBackups.Fingerprint(Path.Combine(safetyOnly, "savedata")));
    var safetyRow = SaveBackups.ListWithSafetyCopies(backups, vault).Single(b => b.Dir == safetyOnly);
    Check(safetyRow.SafetyCopy, "safety-only copy is listed as such");
    Delete(safetyRow);
    Check(!Directory.Exists(safetyOnly), "safety-only backups can be explicitly deleted");
    Check(File.ReadAllText(file) == "unique progress for mirrored-deletion regression", "safety deletion preserves current progress");
    Console.WriteLine("PASS: mirrored backups do not reappear; safety-only backups support explicit deletion");
    Console.WriteLine("PASS: explicit named/automatic deletion, profile boundaries, concurrent writer exclusion and live-save preservation");
    Console.WriteLine("PASS: recent saves, named snapshots, validation, collision safety, retention and restore");
    var captureRoot = Path.Combine(root, "captured-profile");
    s.Root = captureRoot;
    var capturedSource = SaveBackups.SaveData(s);
    var capturedFolder = SaveBackups.Folder(s);
    Directory.CreateDirectory(capturedSource);
    File.WriteAllText(Path.Combine(capturedSource, "userdata0000"), "captured profile progress");
    s.Root = Path.Combine(root, "settings-reloaded-profile");
    var captured = SaveBackups.Create(capturedSource, capturedFolder);
    Check(captured != null && Path.GetDirectoryName(captured) == capturedFolder, "settings reload cannot redirect a queued backup");
    Check(!Directory.Exists(SaveBackups.Folder(s)), "queued backup does not write or prune the reloaded profile");
    Console.WriteLine("PASS: preplay backup remains bound to its captured profile across settings reload");
}
finally { Directory.Delete(root, true); }

static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }

namespace BloodborneLauncher.Core
{
    public sealed class LauncherSettings { public string Root { get; set; } = ""; }
    public static class GameEnvironment
    {
        public static string UserDir(LauncherSettings s) => s.Root;
        public static string SavesRoot(LauncherSettings s) => s.Root;
    }
    public static class PlayerProfiles { public static string Current(LauncherSettings s) => "Main"; }
    public static class SaveGuard { public static string ProfileCopies(string root, string profile) => Path.Combine(root, "vault", profile); }
    public static class L { public static string T(string text) => text; }
}
