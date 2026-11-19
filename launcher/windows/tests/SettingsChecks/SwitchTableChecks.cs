// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text.RegularExpressions;
using BloodborneLauncher.Core;

static class SwitchTableChecks
{
    private sealed record Row(string Name, string Kind, string Default, long Min, long Max, bool Launcher);

    private static readonly Regex RowPattern = new(
        @"^X\(\w+, ""(?<name>[A-Z0-9_]+)"", (?<kind>\w+), ""(?<def>[^""]*)"", (?<min>-?\d+), (?<max>-?\d+), \w+, \w+, (?<flags>[^)]*)\)$",
        RegexOptions.CultureInvariant);

    private static string FindTable()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "engine", "gpu", "shim", "lantern_switches.inc");
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException("engine/gpu/shim/lantern_switches.inc not found above " + AppContext.BaseDirectory);
    }

    private static Dictionary<string, Row> ReadTable(string path)
    {
        var rows = new Dictionary<string, Row>(StringComparer.Ordinal);
        foreach (var raw in File.ReadAllLines(path))
        {
            var line = raw.Trim();
            if (!line.StartsWith("X(", StringComparison.Ordinal)) continue;
            var m = RowPattern.Match(line);
            if (!m.Success) throw new InvalidOperationException("Switch table line not understood: " + line);
            rows.Add(m.Groups["name"].Value, new Row(m.Groups["name"].Value, m.Groups["kind"].Value, m.Groups["def"].Value,
                long.Parse(m.Groups["min"].Value, CultureInfo.InvariantCulture),
                long.Parse(m.Groups["max"].Value, CultureInfo.InvariantCulture),
                m.Groups["flags"].Value.Contains("LANTERN_SW_LAUNCHER", StringComparison.Ordinal)));
        }
        return rows;
    }

    private static bool TableOn(Row row) => row.Default.Length > 0 && row.Default != "0";

    public static void Run(Action<bool, string> require)
    {
        var table = ReadTable(FindTable());
        require(table.Count > 0, "The switch table has rows");
        foreach (var item in EngineSwitches.All)
        {
            require(table.TryGetValue(item.Env, out var row), item.Env + " is on the Engine page but not in lantern_switches.inc");
            row = table[item.Env];
            require(row.Launcher, item.Env + " is on the Engine page but its row is not marked LANTERN_SW_LAUNCHER");
            switch (item.Kind)
            {
                case SwitchKind.Toggle:
                    require((item.Default == item.On) == TableOn(row),
                        item.Env + ": the launcher's default and the engine table's default disagree");
                    if (row.Kind is "Bool" or "Mode")
                        require(item.On == "1" && (item.Off == "0" || item.Off == ""), item.Env + ": a 0/1 switch writes 1 and 0");
                    break;
                case SwitchKind.Number:
                    require(item.Min == row.Min && item.Max == row.Max, item.Env + ": the launcher's range and the table's range disagree");
                    break;
                case SwitchKind.Choice:
                    require(item.Default == row.Default, item.Env + ": the launcher's default and the table's default disagree");
                    foreach (var option in item.Options.Where(o => o.Value.Length > 0))
                        require(long.TryParse(option.Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) &&
                                v >= row.Min && v <= row.Max, item.Env + ": choice " + option.Value + " is outside the table's range");
                    break;
                case SwitchKind.Text:
                    require(row.Kind == "Text", item.Env + ": a text box needs a Text row");
                    break;
            }
        }
        foreach (var row in table.Values.Where(r => r.Launcher))
            require(EngineSwitches.All.Any(x => x.Env == row.Name), row.Name + " is marked for the launcher but is not on the Engine page");
    }
}
