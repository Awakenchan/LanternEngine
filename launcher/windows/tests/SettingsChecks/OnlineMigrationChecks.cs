// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json.Nodes;
using BloodborneLauncher.Core;

static class OnlineMigrationChecks
{
    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    public static void Run(string root)
    {
        const string superMedo = "# Lantern Engine settings\nupscaler=fsr\nonline_enabled=1\nonline_server=supermedo\n" +
            "online_host=game.supermedo.com\nonline_port=31313\nonline_webapi=http://game.supermedo.com:31315/\n" +
            "online_game_api=https://game.supermedo.com/\nonline_thd_account=\nonline_pool=\nonline_username=Hunter\n" +
            "online_upnp=1\nonline_remember=1\nonline_website=https://example.invalid\n# online_host=kept comment\n" +
            "online_p2p_port=3658\nseamless_enabled=0\n";
        const string migrated = "# Lantern Engine settings\nupscaler=fsr\nonline_enabled=1\nonline_pool=\n" +
            "# online_host=kept comment\nonline_p2p_port=3658\nseamless_enabled=0\n";
        Require(OnlineSettings.MigrateIni(superMedo) == migrated, "A saved SuperMedo choice and the shadNet keys leave lantern.ini, the rest stays");
        Require(OnlineSettings.MigrateIni(migrated) is null, "A migrated lantern.ini is left alone");
        Require(OnlineSettings.MigrateIni("online_game_api=srv.shadps4.net:31315\nonline_enabled=1") == "online_enabled=1",
            "The old public shadNet game API goes too");
        Require(OnlineSettings.MigrateIni("online_game_api=https://game.example/\nonline_enabled=1") is null,
            "Another game server stays");
        Require(OnlineSettings.MigrateIni(superMedo.Replace("\n", "\r\n")) == migrated.Replace("\n", "\r\n"), "Windows line ends are kept");
        Require(OnlineSettings.MigrateIni("upscaler=fsr\r\nonline_upnp=1\nonline_enabled=1\r\nonline_host=x") == "upscaler=fsr\r\nonline_enabled=1\r\n",
            "Each kept line keeps its own line end");
        Require(OnlineSettings.MigrateIni("; online_host=x\n;online_upnp=1\n  ; online_game_api=https://game.supermedo.com/\n") is null,
            "Lines starting with ';' are comments, not keys");

        var folder = Path.Combine(root, "online-migration");
        Directory.CreateDirectory(folder);
        var ini = Path.Combine(folder, "lantern.ini");
        Require(OnlineSettings.MigrateIniFile(ini).State == OnlineSettings.IniMigration.Absent, "A missing lantern.ini is absent, not written");
        Require(!File.Exists(ini), "Nothing is created for a missing lantern.ini");

        File.WriteAllText(ini, superMedo);
        using (new FileStream(ini, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var held = OnlineSettings.MigrateIniFile(ini);
            Require(held.State == OnlineSettings.IniMigration.Unreadable && held.Detail.Length > 0, "A lantern.ini that cannot be read is reported unreadable");
        }
        Require(File.ReadAllText(ini) == superMedo, "An unreadable lantern.ini is not written over");

        var result = OnlineSettings.MigrateIniFile(ini);
        Require(result.State == OnlineSettings.IniMigration.Migrated, "A readable SuperMedo lantern.ini is migrated: " + result.Detail);
        Require(File.ReadAllText(ini) == migrated, "The migrated lantern.ini holds the kept lines");
        Require(File.ReadAllText(ini + ".bak") == superMedo, "The previous lantern.ini is kept as .bak");
        Require(!Directory.EnumerateFiles(folder, "*.tmp").Any(), "No temporary file is left");
        Require(OnlineSettings.MigrateIniFile(ini).State == OnlineSettings.IniMigration.Unchanged, "A second start changes nothing");

        File.WriteAllText(ini, superMedo);
        File.SetAttributes(ini, FileAttributes.ReadOnly);
        try
        {
            var refused = OnlineSettings.MigrateIniFile(ini);
            Require(refused.State == OnlineSettings.IniMigration.WriteFailed, "A lantern.ini that cannot be replaced is reported: " + refused.State);
            Require(File.ReadAllText(ini) == superMedo, "A failed replace leaves the old lantern.ini");
            Require(!Directory.EnumerateFiles(folder, "*.tmp").Any(), "A failed replace removes its temporary file");
        }
        finally
        {
            File.SetAttributes(ini, FileAttributes.Normal);
        }

        var settings = (JsonObject)JsonNode.Parse("""{ "online_password": "secret", "hd_name": "Hunter" }""")!;
        Require(OnlineSettings.RetireSettings(settings), "The stored shadNet password is removed");
        Require(!settings.ContainsKey("online_password") && settings["hd_name"]?.GetValue<string>() == "Hunter",
            "Only the shadNet password leaves settings.json");
        Require(!OnlineSettings.RetireSettings(settings), "Removing it again changes nothing");
        var marked = (JsonObject)JsonNode.Parse("""{ "online_password": "secret", "online_shadnet_retired": true }""")!;
        Require(!OnlineSettings.RetireSettings(marked) && marked.ContainsKey("online_password"),
            "With the marker set settings.json is not migrated again");

        var onceIni = Path.Combine(folder, "once.ini");
        var first = new LauncherSettings();
        File.WriteAllText(onceIni, superMedo);
        using (new FileStream(onceIni, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var held = OnlineSettings.MigrateIniOnce(first, onceIni);
            Require(held.State == OnlineSettings.IniMigration.Unreadable && !first.Bool(OnlineSettings.MigratedMarker),
                "An unreadable lantern.ini leaves the marker unset, so the next start tries again");
        }
        var pass = OnlineSettings.MigrateIniOnce(first, onceIni);
        Require(pass.State == OnlineSettings.IniMigration.Migrated && first.Bool(OnlineSettings.MigratedMarker),
            "The first start migrates lantern.ini and sets the marker: " + pass.State);
        Require(File.ReadAllText(onceIni) == migrated, "The first pass leaves the kept lines");
        File.WriteAllText(onceIni, superMedo);
        Require(OnlineSettings.MigrateIniOnce(first, onceIni).State == OnlineSettings.IniMigration.AlreadyDone,
            "With the marker set lantern.ini is not scanned again");
        Require(File.ReadAllText(onceIni) == superMedo, "The marked pass writes nothing");

        var fresh = new LauncherSettings();
        Require(OnlineSettings.MigrateIniOnce(fresh, Path.Combine(folder, "missing.ini")).State == OnlineSettings.IniMigration.Absent &&
                fresh.Bool(OnlineSettings.MigratedMarker), "A missing lantern.ini settles the migration");
        var tidy = new LauncherSettings();
        File.WriteAllText(onceIni, migrated);
        Require(OnlineSettings.MigrateIniOnce(tidy, onceIni).State == OnlineSettings.IniMigration.Unchanged &&
                tidy.Bool(OnlineSettings.MigratedMarker), "A lantern.ini without shadNet keys settles the migration");

        var hunter = new LauncherSettings();
        hunter.Set("hd_token", "token");
        hunter.Set("hd_name", "Hunter");
        var foreign = new IniFile();
        foreign.Set("online_game_api", "https://game.other.example/");
        OnlineSettings.ChoosePool(foreign, hunter);
        Require(foreign.Get("online_pool") == OnlineSettings.Pool && foreign.Get("online_game_api") == "",
            "The Hunter's Dream pool clears a game API on another host");
        var clean = new IniFile();
        OnlineSettings.ChoosePool(clean, hunter);
        Require(clean.Get("online_pool") == OnlineSettings.Pool && clean.Raw("online_game_api") is null,
            "Choosing the pool adds no game API key");
        var unlinked = new IniFile();
        unlinked.Set("online_game_api", "https://game.other.example/");
        OnlineSettings.ChoosePool(unlinked, new LauncherSettings());
        Require(unlinked.Get("online_pool") == "" && unlinked.Get("online_game_api") == "",
            "Without a Hunter's Dream link the pool is empty and the game API is cleared as before");
    }
}
