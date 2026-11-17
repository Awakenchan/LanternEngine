// SPDX-License-Identifier: GPL-3.0-or-later
using BloodborneLauncher.Core;

static class ExtraEnvChecks
{
    private const string Owner = "BB_GUEST_IN_PLACE=1 LANTERN_RING_CONSTANTS=1 LANTERN_JOB_LOCK=1 LANTERN_JOB_NODE_CACHE=1 LANTERN_MAIN_JOBS_BARRIER=1";

    public static void Run(Action<bool, string> require)
    {
        var s = new LauncherSettings();
        s.Set("env:BB_BACKGROUND_FPS", "60");
        require(GameEnvironment.Build(s).GetValueOrDefault("BB_BACKGROUND_FPS") == "60", "Without Extra variables the Engine page switch is passed");
        s.Set("extra_env", Owner + " BB_BACKGROUND_FPS=0");
        var env = GameEnvironment.Build(s);
        foreach (var item in Owner.Split(' '))
        {
            var at = item.IndexOf('=');
            require(env.GetValueOrDefault(item[..at]) == item[(at + 1)..], "Extra variables reach the game: " + item);
        }
        require(env.GetValueOrDefault("BB_BACKGROUND_FPS") == "0", "Extra variables win over an Engine page switch");
        require(env.GetValueOrDefault("LANTERN_EXTRA_VARIABLES") ==
                "BB_GUEST_IN_PLACE,LANTERN_RING_CONSTANTS,LANTERN_JOB_LOCK,LANTERN_JOB_NODE_CACHE,LANTERN_MAIN_JOBS_BARRIER,BB_BACKGROUND_FPS",
            "The game is told which variables came from Extra variables, so the package file leaves them alone");
        s.Set("extra_env", "");
        require(GameEnvironment.Build(s).GetValueOrDefault("LANTERN_EXTRA_VARIABLES") == "", "No Extra variables: the list is empty, not inherited");
    }
}
