// SPDX-License-Identifier: GPL-3.0-or-later
using BloodborneLauncher.Core;

static class GcMarksChecks
{
    private const long GiB = 1L << 30;

    private static Dictionary<string, string> Env(bool budgetOn, long vram, bool integrated)
    {
        var env = new Dictionary<string, string>(StringComparer.Ordinal);
        if (!budgetOn) env["BB_GC_PRESSURE_PERCENT"] = "0";
        GameEnvironment.ApplyGcMarks(env, budgetOn, vram, integrated);
        return env;
    }

    public static void Run(Action<bool, string> require)
    {
        var six = Env(false, 6 * GiB, false);
        require(!six.ContainsKey("BB_GC_PRESSURE_PERCENT"), "VRAM budget off on a 6 GB discrete GPU must not send BB_GC_PRESSURE_PERCENT=0");
        require(six.GetValueOrDefault("BB_GC_PRESSURE_SECONDS") == "0", "VRAM budget off keeps BB_GC_PRESSURE_SECONDS=0");
        var eight = Env(false, 8 * GiB, false);
        require(!eight.ContainsKey("BB_GC_PRESSURE_PERCENT"), "VRAM budget off on an 8 GB discrete GPU must not send BB_GC_PRESSURE_PERCENT=0");
        var four = Env(false, 4 * GiB, false);
        require(!four.ContainsKey("BB_GC_PRESSURE_PERCENT"), "VRAM budget off on a 4 GB discrete GPU must not send BB_GC_PRESSURE_PERCENT=0");
        var sixteen = Env(false, 16 * GiB, false);
        require(sixteen.GetValueOrDefault("BB_GC_PRESSURE_PERCENT") == "0" && sixteen.GetValueOrDefault("BB_GC_PRESSURE_SECONDS") == "0",
            "VRAM budget off on a 16 GB GPU keeps the old marks");
        var unknown = Env(false, 0, false);
        require(unknown.GetValueOrDefault("BB_GC_PRESSURE_PERCENT") == "0", "An unknown VRAM size keeps the old launcher rule");
        var igpu = Env(false, 512L << 20, true);
        require(igpu.GetValueOrDefault("BB_GC_PRESSURE_PERCENT") == "0", "An integrated GPU keeps the old launcher rule");
        var on = Env(true, 6 * GiB, false);
        require(on.Count == 0, "VRAM budget on (the default) sends neither variable");
        var onBig = Env(true, 16 * GiB, false);
        require(onBig.Count == 0, "VRAM budget on (the default) sends neither variable on 16 GB");
        require(GameEnvironment.SmallDiscreteGpu(8 * GiB, false) && !GameEnvironment.SmallDiscreteGpu(8 * GiB + 1, false),
            "8 GB is the last small discrete size");

        var source = File.ReadAllText(FindEnvironment());
        require(source.Split("\"BB_GC_PRESSURE_PERCENT\"] = \"0\"").Length == 2,
            "Only ApplyGcMarks sets BB_GC_PRESSURE_PERCENT=0, safe mode included");
        require(source.Contains("ApplyGcMarks(env, s.Bool(\"exp_vram_budget\") && !s.Bool(\"safe_mode_next\"), vram, integrated);"),
            "Safe mode goes through the same VRAM rule as the budget switch");
    }

    private static string FindEnvironment()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "launcher", "BloodborneLauncher", "Core", "GameEnvironment.cs");
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException("launcher/BloodborneLauncher/Core/GameEnvironment.cs not found above " + AppContext.BaseDirectory);
    }
}
