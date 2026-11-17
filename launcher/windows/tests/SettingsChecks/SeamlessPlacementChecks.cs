// SPDX-License-Identifier: GPL-3.0-or-later
using BloodborneLauncher.Core;

static class SeamlessPlacementChecks
{
    private static string FindLauncher()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "launcher", "BloodborneLauncher", "Views", "OnlinePage.cs");
            if (File.Exists(candidate)) return Path.Combine(dir.FullName, "launcher");
        }
        throw new InvalidOperationException("launcher/BloodborneLauncher/Views/OnlinePage.cs not found above " + AppContext.BaseDirectory);
    }

    private static string GroupBody(string source, string group)
    {
        var start = source.IndexOf("Group(L.T(\"" + group + "\"));", StringComparison.Ordinal);
        if (start < 0) return "";
        var next = source.IndexOf("Group(L.T(\"", start + 1, StringComparison.Ordinal);
        return next < 0 ? source[start..] : source[start..next];
    }

    public static void Run(Action<bool, string> require)
    {
        var deaths = EngineSwitches.All.SingleOrDefault(x => x.Env == "BB_SEAMLESS_DEATHS");
        require(deaths != null, "BB_SEAMLESS_DEATHS is still a launcher switch");
        require(deaths!.Group == EngineSwitches.Seamless, "BB_SEAMLESS_DEATHS is in the Seamless co-op group");
        require(EngineSwitches.Seamless == "Seamless co-op", "The seamless switch group is named as the Online page's group");
        require(!EngineSwitches.Groups.Contains(EngineSwitches.Seamless), "The Seamless co-op group is not an Engine switches group");
        require(!EngineSwitches.Groups.Contains("Online"), "The Engine switches page has no Online group");
        require(!EngineSwitches.OnEnginePage.Contains(deaths), "BB_SEAMLESS_DEATHS is not on the Engine switches page");
        require(EngineSwitches.InGroup(EngineSwitches.Seamless).Contains(deaths), "BB_SEAMLESS_DEATHS is listed for the Seamless co-op group");
        require(deaths.Key == "env:BB_SEAMLESS_DEATHS" && deaths.Default == deaths.Off && deaths.On == "1",
            "BB_SEAMLESS_DEATHS keeps its setting key and its off default");
        require(deaths.Title == "Seamless co-op: spectate and party wipe", "BB_SEAMLESS_DEATHS keeps its title");
        foreach (var group in EngineSwitches.Groups)
            require(EngineSwitches.InGroup(group).Any(), "Engine switches group " + group + " is not empty");

        var s = new LauncherSettings();
        EngineSwitches.Set(s, deaths, deaths.On);
        require(EngineSwitches.Changed(s).Contains(("BB_SEAMLESS_DEATHS", "1")), "Turned on, BB_SEAMLESS_DEATHS is still passed to the game");
        EngineSwitches.ResetAll(s);
        require(EngineSwitches.Get(s, deaths) == deaths.On, "Resetting the Engine switches page leaves the Online page's switch alone");
        EngineSwitches.Set(s, deaths, deaths.Off);
        require(!EngineSwitches.Changed(s).Any(c => c.Name == "BB_SEAMLESS_DEATHS"), "Off, BB_SEAMLESS_DEATHS is not passed");

        var launcher = FindLauncher();
        foreach (var app in new[] { "BloodborneLauncher", "LanternLauncher" })
        {
            var online = File.ReadAllText(Path.Combine(launcher, app, "Views", "OnlinePage.cs"));
            var seamless = GroupBody(online, "Seamless co-op");
            var row = seamless.IndexOf("EngineSwitches.InGroup(EngineSwitches.Seamless)", StringComparison.Ordinal);
            require(row >= 0, app + ": the Online page's Seamless co-op group shows the seamless engine switches");
            require(row > seamless.IndexOf("\"coop_open_room\"", StringComparison.Ordinal) && seamless.IndexOf("\"coop_open_room\"", StringComparison.Ordinal) > 0,
                app + ": the seamless engine switches come after the other seamless rows");
            var engine = File.ReadAllText(Path.Combine(launcher, app, "Views", "EnginePage.cs"));
            require(!engine.Contains("EngineSwitches.Online", StringComparison.Ordinal) && !engine.Contains("Seamless", StringComparison.Ordinal),
                app + ": the Engine switches page has no Online or seamless group");
        }
    }
}
