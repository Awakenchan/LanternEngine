// SPDX-License-Identifier: GPL-3.0-or-later
using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static Dictionary<int, long> OldReader(string path)
{
    var read = new Dictionary<int, long>();
    foreach (var line in File.ReadAllLines(path))
    {
        var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length >= 2 && int.TryParse(parts[0], out var id) && long.TryParse(parts[1], out var when))
            if (!read.TryGetValue(id, out var seen) || when < seen)
                read[id] = when;
    }
    return read;
}

var dir = Path.Combine(Path.GetTempPath(), "lantern-trophy-checks-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(dir);
try
{
    Require(TrophyTracker.LogLine(14, 100, "own-save", "own") == "14 100 own-save own", "Full log line");
    Require(TrophyTracker.LogLine(14, 100, "unlocked", null) == "14 100 unlocked", "Line from an engine without a role");
    Require(TrophyTracker.LogLine(0, 100, null, null) == "0 100", "Platinum line stays two fields");

    var tracker = new TrophyTracker();
    tracker.SetUserDir(dir);
    Require(tracker.ObserveSave("Runtime: trophy 14 earned in this save as own").SequenceEqual(new[] { 14 }), "Own-save line with a role");
    Require(tracker.Observe("Runtime: trophy 16 unlocked as guest").SequenceEqual(new[] { 16 }), "Unlock line with a role");
    Require(tracker.Observe("Runtime: trophy 17 unlocked").SequenceEqual(new[] { 17 }), "Unlock line from an older engine");
    Require(tracker.ObserveSave("Runtime: trophy 18 earned in this save").SequenceEqual(new[] { 18 }), "Own-save line from an older engine");
    Require(tracker.Observe("Runtime: trophy 16 unlocked as host").Count == 0, "A known trophy is not recorded twice");

    var lines = File.ReadAllLines(tracker.LogPath);
    Require(lines.Length == 4, "Four lines written");
    Require(lines[0].EndsWith(" own-save own") && lines[0].StartsWith("14 "), "Own-save trophy carries its source and role");
    Require(lines[1].EndsWith(" unlocked guest") && lines[1].StartsWith("16 "), "Guest unlock carries its source and role");
    Require(lines[2].Split(' ').Length == 3 && lines[2].EndsWith(" unlocked"), "No role when the engine gave none");
    Require(lines[3].Split(' ').Length == 3 && lines[3].EndsWith(" own-save"), "Own-save without a role");

    var old = OldReader(tracker.LogPath);
    Require(old.Keys.OrderBy(k => k).SequenceEqual(new[] { 14, 16, 17, 18 }), "The old two-field reader still reads every id");
    Require(old.Values.All(v => v > 0), "The old reader still reads the time");

    File.AppendAllText(tracker.LogPath, "5 42\n");
    var fresh = new TrophyTracker();
    fresh.SetUserDir(dir);
    Require(fresh.Unlocked.Keys.OrderBy(k => k).SequenceEqual(new[] { 5, 14, 16, 17, 18 }), "A new tracker reads old and new lines alike");
    Require(fresh.Unlocked[5] == 42, "An old two-field line keeps its time");
}
finally
{
    Directory.Delete(dir, true);
}
Console.WriteLine("trophy checks: passed");
