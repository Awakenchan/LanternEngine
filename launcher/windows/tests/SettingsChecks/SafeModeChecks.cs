// SPDX-License-Identifier: GPL-3.0-or-later
using BloodborneLauncher.Core;

static class SafeModeChecks
{
    public static void Run(Action<bool, string> require)
    {
        var env = new Dictionary<string, string>(StringComparer.Ordinal);
        GameEnvironment.ApplySafeMode(env);
        foreach (var name in new[] { "LANTERN_RING_CONSTANTS", "LANTERN_JOB_LOCK", "LANTERN_JOB_NODE_CACHE", "LANTERN_MAIN_JOBS_BARRIER", "LANTERN_RT_CAMERA_MAP", "LANTERN_RT_KEEP_PREVIOUS", "LANTERN_RT_NEW_BUDGET", "LANTERN_RT_POOL", "LANTERN_RT_EVICT_HOLD", "LANTERN_RT_RIGID", "LANTERN_RT_CUTOUT", "LANTERN_RT_CUTOUT_LAMPS", "LANTERN_RT_BOUNCE_MOTION", "LANTERN_RT_SETUP_WORKER", "LANTERN_RT_PERSISTENT_SCENE", "LANTERN_RT_GAME_LIGHTS" })
            require(env.GetValueOrDefault(name) == "0", $"Safe mode turns {name} off");
        require(env.Count == GameEnvironment.SafeModeOff.Length, "Safe mode sets only the speed and ray tracing switches");

        var source = File.ReadAllText(FindEnvironment());
        var at = source.IndexOf("if (!s.Bool(\"safe_mode_next\"))\r\n                foreach (var (variable, value) in EngineSwitches.Changed(s))", StringComparison.Ordinal);
        if (at < 0)
            at = source.IndexOf("if (!s.Bool(\"safe_mode_next\"))\n                foreach (var (variable, value) in EngineSwitches.Changed(s))", StringComparison.Ordinal);
        var call = source.IndexOf("ApplySafeMode(env);", StringComparison.Ordinal);
        var extra = source.IndexOf("foreach (var (variable, value) in ExtraVariables(s))", StringComparison.Ordinal);
        require(at >= 0 && call > at && call < extra, "Safe mode turns the switches off before the extra variables, which still win");
    }

    private static string FindEnvironment()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var path = Path.Combine(dir.FullName, "launcher", "BloodborneLauncher", "Core", "GameEnvironment.cs");
            if (File.Exists(path)) return path;
        }
        throw new FileNotFoundException("GameEnvironment.cs");
    }
}
