using System.IO;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    /// <summary>The setup guide's steps that check something.</summary>
    public enum SetupStep { Folder, Version, Files, Dlc }

    /// <summary>One line of the setup check: what was found and, when it is not right, what to do.
    /// Optional lines are information (nothing needs doing).</summary>
    public sealed record SetupItem(SetupStep Step, Health Health, string Text, bool Optional = false);

    /// <summary>What the setup guide found in a game folder.</summary>
    public sealed class SetupReport
    {
        public string GameDir { get; init; } = "";
        public bool HasEboot { get; init; }
        /// <summary>The game one or two folders below the chosen one (the folder above it was chosen).</summary>
        public string? GameBelow { get; init; }
        public string Title { get; init; } = "?";
        /// <summary>The game folder's own version (param.sfo).</summary>
        public string Version { get; init; } = "?";
        /// <summary>An update beside the game (&lt;game&gt;-UPDATE or -patch), which the engine lays over it at start.</summary>
        public string? UpdateDir { get; init; }
        public string UpdateVersion { get; init; } = "?";
        /// <summary>The version the engine will see: the update's when there is one beside the game.</summary>
        public string Effective => UpdateDir != null && UpdateVersion != "?" ? UpdateVersion : Version;
        public List<SetupItem> Items { get; } = new();

        public Health Worst(SetupStep step) =>
            Items.Where(i => i.Step == step && !i.Optional).Select(i => i.Health).DefaultIfEmpty(Health.Good).Max();

        public Health Worst() => Items.Where(i => !i.Optional).Select(i => i.Health).DefaultIfEmpty(Health.Good).Max();
    }

    /// <summary>
    /// The setup guide's checks (Views/SetupGuide.cs shows them): the game folder, its edition and
    /// version, whether the files the engine needs are there, and The Old Hunters. bb-start checks the
    /// same things at Play (start/main.cpp, start/prepare.cpp CheckGame); this names them earlier, in
    /// plain words, with what to do. The eboot.bin image hash is left to bb-start.
    /// </summary>
    public static class GameSetup
    {
        /// <summary>Files bb-start needs before it starts the game (start/main.cpp "core").</summary>
        public static readonly string[] CoreFiles =
        {
            Path.Combine("dvdroot_ps4", "shader", "gxshader.shaderbnd.dcx"),
            Path.Combine("dvdroot_ps4", "other", "systex.tpf.dcx"),
        };

        /// <summary>CUSA00900 (US) with update 1.09 runs the same code as CUSA03173: every patch works.</summary>
        private static readonly string[] FullEditions = { "CUSA03173", "CUSA00900" };

        public static SetupReport Check(string gameDir, string dlcChosen, bool dlcEnabled)
        {
            var report = Inspect(gameDir);
            if (report.HasEboot)
            {
                CheckVersion(report);
                CheckFiles(report);
                CheckDlc(report, dlcChosen, dlcEnabled);
            }
            return report;
        }

        // ---------------------------------------------------------------- step 1: the folder

        private static SetupReport Inspect(string gameDir)
        {
            void Add(SetupReport r, Health health, string text) => r.Items.Add(new SetupItem(SetupStep.Folder, health, text));

            if (string.IsNullOrWhiteSpace(gameDir))
            {
                var none = new SetupReport();
                Add(none, Health.Bad, L.T("No game folder chosen yet."));
                return none;
            }
            gameDir = Path.TrimEndingDirectorySeparator(gameDir);
            if (!Directory.Exists(gameDir))
            {
                var gone = new SetupReport { GameDir = gameDir };
                Add(gone, Health.Bad, L.T("This folder does not exist any more: it was moved or renamed, or its drive is not plugged in. Choose it again."));
                return gone;
            }
            if (!File.Exists(Path.Combine(gameDir, "eboot.bin")))
            {
                var below = GameBelow(gameDir);
                var missing = new SetupReport { GameDir = gameDir, GameBelow = below };
                if (below != null)
                    Add(missing, Health.Bad, L.F($"The game is one level further in, in {Relative(gameDir, below)}. Press \"Use this folder\" to choose it."));
                else if (HasFiles(gameDir, "*.pkg"))
                    Add(missing, Health.Bad, L.T("This folder holds .pkg files. The engine cannot read those: it needs the unpacked game, a folder with eboot.bin, sce_sys and dvdroot_ps4. Unpack the .pkg first (with the tool you dumped it with, or shadPS4's package install), then choose the folder that comes out."));
                else
                    Add(missing, Health.Bad, L.T("There is no eboot.bin in this folder. Choose the folder that holds eboot.bin, sce_sys and dvdroot_ps4 (usually named CUSA03173)."));
                return missing;
            }

            var sfo = GameCheck.ReadSfo(Path.Combine(gameDir, "sce_sys", "param.sfo"));
            var update = GameCheck.UpdateFolder(gameDir);
            var updateOnly = UpdateOnly(gameDir, sfo);
            var report = new SetupReport
            {
                GameDir = gameDir,
                // The update on its own is not a game: the later steps wait for the game folder.
                HasEboot = !updateOnly,
                Title = sfo.GetValueOrDefault("TITLE_ID", "?"),
                Version = sfo.GetValueOrDefault("APP_VER", "?"),
                UpdateDir = update,
                UpdateVersion = update == null ? "?" : GameCheck.ReadSfo(Path.Combine(update, "sce_sys", "param.sfo")).GetValueOrDefault("APP_VER", "?"),
            };
            if (updateOnly)
                Add(report, Health.Bad, L.T("This is the update on its own, not the whole game (its sce_sys says it is a patch). Choose the base game folder; this update can then be copied into it in the next step."));
            else
                Add(report, Health.Good, L.T("eboot.bin found: this is a game folder."));
            return report;
        }

        /// <summary>The update on its own, chosen instead of the game: its param.sfo says "gp" (a patch),
        /// and it is named like one or lacks the game's files. A game with the update merged into it
        /// has the patch's param.sfo too, but all its files.</summary>
        private static bool UpdateOnly(string dir, IReadOnlyDictionary<string, string> sfo) =>
            sfo.GetValueOrDefault("CATEGORY", "") == "gp" &&
            (Regex.IsMatch(Path.GetFileName(Path.TrimEndingDirectorySeparator(dir)), "[-_ ](update|patch)", RegexOptions.IgnoreCase) ||
             !CoreFiles.All(f => File.Exists(Path.Combine(dir, f))));

        /// <summary>A game folder one or two levels below <paramref name="dir"/> (CUSA03173 inside the
        /// chosen folder, or CUSA03173\CUSA03173 after unzipping), the best one if there are several.</summary>
        public static string? GameBelow(string dir)
        {
            var found = new List<string>();
            foreach (var child in Children(dir).Take(300))
            {
                if (IsGame(child)) found.Add(child);
                else
                    foreach (var grandchild in Children(child).Take(50))
                        if (IsGame(grandchild)) found.Add(grandchild);
            }
            return Best(found);
        }

        // ---------------------------------------------------------------- step 2: edition and version

        private static void CheckVersion(SetupReport r)
        {
            void Add(Health health, string text) => r.Items.Add(new SetupItem(SetupStep.Version, health, text));

            if (r.Title == "?")
                Add(Health.Bad, L.T("sce_sys\\param.sfo is missing or unreadable, so the edition and version cannot be read. Copy the sce_sys folder again from your dump."));
            else if (!GameCheck.SupportedTitles.Contains(r.Title))
                Add(Health.Bad, L.F($"This is not Bloodborne ({r.Title}). Choose your Bloodborne folder."));
            else if (FullEditions.Contains(r.Title))
                Add(Health.Good, L.F($"Edition {r.Title}: every Lantern Engine feature works with it."));
            else
                Add(Health.Warning, L.F($"Edition {r.Title}: it starts, but the frame rate, resolution and engine patches are made for CUSA03173, so this edition plays at 30 FPS with the game's own settings. CUSA03173 is the recommended dump."));

            if (r.Title == "?") return;
            if (r.UpdateDir != null && r.UpdateVersion == GameCheck.SupportedVersion)
                Add(Health.Good, L.F($"Version 1.09, from the update beside the game ({Path.GetFileName(r.UpdateDir)}). The engine lays it over the game at every start, so it can stay as it is."));
            else if (r.Version == GameCheck.SupportedVersion)
                Add(Health.Good, L.T("Version 1.09: the update is already in the game folder."));
            else if (r.UpdateDir != null)
                Add(Health.Bad, L.F($"The update beside the game ({Path.GetFileName(r.UpdateDir)}) is version {r.UpdateVersion}, not 1.09. Dump the 1.09 update and use that one instead."));
            else
                Add(Health.Bad, L.F($"Version {Shown(r.Version)}: the 1.09 update is missing. The engine and its patches are made for 1.09, so it will not start without it. Dump the update from your PS4 as well, then press \"Choose update folder\"."));
        }

        /// <summary>Whether <paramref name="update"/> is the 1.09 update for the game <paramref name="title"/>:
        /// null when it is, else what is wrong.</summary>
        public static string? CheckUpdate(string update, string gameDir, string title)
        {
            if (string.Equals(Path.GetFullPath(update).TrimEnd('\\'), Path.GetFullPath(gameDir).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                return L.T("That is the game folder itself. Choose the folder of the dumped update (often named CUSA03173-patch or CUSA03173-UPDATE).");
            if (!File.Exists(Path.Combine(update, "eboot.bin")))
                return L.T("There is no eboot.bin in that folder. The update folder has its own eboot.bin and sce_sys, like the game.");
            var sfo = GameCheck.ReadSfo(Path.Combine(update, "sce_sys", "param.sfo"));
            var updateTitle = sfo.GetValueOrDefault("TITLE_ID", "?");
            var version = sfo.GetValueOrDefault("APP_VER", "?");
            if (updateTitle != "?" && title != "?" && updateTitle != title)
                return L.F($"That update is for {updateTitle}, but the game is {title}. An update only fits the edition it was made for.");
            if (version != GameCheck.SupportedVersion)
                return L.F($"That folder is version {Shown(version)}, not 1.09.");
            return null;
        }

        /// <summary>Copies the update into the game folder, replacing the older files (what the PS4
        /// does when it installs an update). The update folder stays as it is.</summary>
        public static void MergeUpdate(string update, string gameDir, IProgress<double> progress, CancellationToken cancel)
        {
            var files = Directory.GetFiles(update, "*", SearchOption.AllDirectories);
            var total = Math.Max(1L, files.Sum(f => new FileInfo(f).Length));
            long done = 0;
            var buffer = new byte[4 << 20];
            foreach (var file in files)
            {
                cancel.ThrowIfCancellationRequested();
                var target = Path.Combine(gameDir, Path.GetRelativePath(update, file));
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                // Through a temporary file: a copy stopped half way never leaves a cut eboot.bin.
                var part = target + ".lantern-part";
                using (var from = File.OpenRead(file))
                using (var to = File.Create(part))
                {
                    int read;
                    while ((read = from.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        cancel.ThrowIfCancellationRequested();
                        to.Write(buffer, 0, read);
                        done += read;
                        progress.Report((double)done / total);
                    }
                }
                File.Move(part, target, overwrite: true);
            }
        }

        // ---------------------------------------------------------------- step 3: the files

        private static void CheckFiles(SetupReport r)
        {
            void Add(Health health, string text) => r.Items.Add(new SetupItem(SetupStep.Files, health, text));
            var game = r.GameDir;
            string? Found(string relative) =>
                new[] { r.UpdateDir, game }.Where(d => d != null).Select(d => Path.Combine(d!, relative)).FirstOrDefault(File.Exists);

            var eboot = new FileInfo(Path.Combine(r.UpdateDir != null && File.Exists(Path.Combine(r.UpdateDir, "eboot.bin")) ? r.UpdateDir : game, "eboot.bin"));
            var ebootOk = false;
            try
            {
                if (eboot.Length == 0)
                    Add(Health.Bad, L.T("eboot.bin is empty (0 bytes). A copy stopped half way, or an antivirus emptied it. Copy it again from your dump, and if it happens again, add the game folder to your antivirus' exclusions."));
                else if (eboot.Length < 1 << 20)
                    Add(Health.Bad, L.F($"eboot.bin is far too small ({eboot.Length / 1024} KB). Copy it again from your dump."));
                else if (OnlineOnly(eboot.FullName))
                    Add(Health.Warning, L.T("The game files are kept online only (OneDrive). The game reads them all the time, so they must be on this PC: right-click the game folder › Always keep on this device, or move the game out of OneDrive."));
                else if (!LooksExecutable(eboot.FullName))
                    Add(Health.Bad, L.T("eboot.bin is not a PS4 game executable the engine can read (damaged, or not from a dump). Copy it again from your dump or the dumped 1.09 update."));
                else
                    ebootOk = true;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Add(Health.Bad, L.T("eboot.bin cannot be read: ") + e.Message);
            }

            var missing = CoreFiles.Append(Path.Combine("sce_module", "libc.prx")).Append(Path.Combine("sce_sys", "param.sfo"))
                .Where(f => Found(f) == null).ToList();
            if (missing.Count > 0)
                Add(Health.Bad, L.F($"The game folder is incomplete: {string.Join(", ", missing)} missing. Copy the game again from your dump, base game and update."));

            var (count, bytes) = Size(Path.Combine(game, "dvdroot_ps4"));
            if (count == 0)
                Add(Health.Bad, L.T("dvdroot_ps4 is missing or empty: it holds the whole game world. Copy the game again from your dump."));
            else if (bytes < 10L << 30)
                Add(Health.Warning, L.F($"dvdroot_ps4 holds only {bytes / (double)(1L << 30):0.0} GB in {count:N0} files: a full Bloodborne is much larger. The copy may have stopped early; copy it again if the game fails to load."));
            else if (missing.Count == 0 && ebootOk)
                Add(Health.Good, L.F($"All the files the engine needs are here (dvdroot_ps4: {count:N0} files, {bytes / (double)(1L << 30):0.0} GB)."));

            if (count > 0)
            {
                var files = GameFiles.Check(game, r.UpdateDir, TimeSpan.FromSeconds(20));
                if (files.Problems.Count > 0)
                {
                    var shown = string.Join("; ", files.Problems.Take(6).Select(p => p.File + " " + p.What));
                    var more = files.Problems.Count > 6 ? L.F($" and {files.Problems.Count - 6} more") : "";
                    Add(Health.Bad, L.F($"The game copy is damaged or incomplete: {shown}{more}. Areas that use these files load without textures, never finish loading, or crash. Copy the game again from your dump, base game and update."));
                }
                else if (!files.Complete)
                    Add(Health.Warning, L.F($"The game's files were only partly checked ({files.Checked:N0} packed files in 20 seconds; the drive is slow). None of those is damaged."));
            }

            var place = Place(game);
            if (place != null) Add(Health.Warning, place);
        }

        /// <summary>Why the game folder's place may cause trouble, or null.</summary>
        private static string? Place(string game)
        {
            var full = Path.GetFullPath(game);
            bool Under(string? root) => !string.IsNullOrEmpty(root) &&
                full.StartsWith(Path.TrimEndingDirectorySeparator(root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
            if (Under(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles)) ||
                Under(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86)))
                return L.T("The game is inside Program Files, which Windows protects: mods and the update copy can fail there. A folder such as D:\\Games\\CUSA03173 is better.");
            if (Under(Environment.GetEnvironmentVariable("OneDrive")) || Under(Environment.GetEnvironmentVariable("OneDriveConsumer")))
                return L.T("The game is inside OneDrive: it uploads the whole game and may keep files online only. A folder outside OneDrive is better.");
            if (full.StartsWith(@"\\", StringComparison.Ordinal))
                return L.T("The game is on a network folder: loading will be slow and may stutter. Copy it to a drive in this PC, an SSD if you can.");
            try
            {
                if (new DriveInfo(Path.GetPathRoot(full)!).DriveType == DriveType.Network)
                    return L.T("The game is on a network folder: loading will be slow and may stutter. Copy it to a drive in this PC, an SSD if you can.");
            }
            catch (Exception) { }
            return null;
        }

        /// <summary>A file kept only in the cloud (OneDrive's "online-only"): Windows downloads it when read.</summary>
        private static bool OnlineOnly(string path)
        {
            const FileAttributes recallOnOpen = (FileAttributes)0x40000, recallOnDataAccess = (FileAttributes)0x400000;
            var attributes = File.GetAttributes(path);
            return (attributes & (FileAttributes.Offline | recallOnOpen | recallOnDataAccess)) != 0;
        }

        /// <summary>A PS4 SELF or a plain ELF (start/elf.cpp ParseSelf).</summary>
        private static bool LooksExecutable(string path)
        {
            var head = new byte[4];
            using var file = File.OpenRead(path);
            if (file.Read(head, 0, 4) != 4) return false;
            return (head[0] == 0x4F && head[1] == 0x15 && head[2] == 0x3D && head[3] == 0x1D) ||
                   (head[0] == 0x7F && head[1] == (byte)'E' && head[2] == (byte)'L' && head[3] == (byte)'F');
        }

        private static (int Count, long Bytes) Size(string dir)
        {
            if (!Directory.Exists(dir)) return (0, 0);
            var count = 0;
            long bytes = 0;
            try
            {
                foreach (var file in new DirectoryInfo(dir).EnumerateFiles("*", new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true }))
                {
                    count++;
                    bytes += file.Length;
                }
            }
            catch (Exception) { }
            return (count, bytes);
        }

        // ---------------------------------------------------------------- step 4: The Old Hunters

        private static void CheckDlc(SetupReport r, string dlcChosen, bool dlcEnabled)
        {
            void Add(Health health, string text, bool optional = false) => r.Items.Add(new SetupItem(SetupStep.Dlc, health, text, optional));
            if (r.Title == "CUSA03173")
                Add(Health.Good, L.T("The Old Hunters is part of this edition (Game of the Year): nothing to add."), optional: true);
            else if (dlcChosen.Length > 0 && Directory.Exists(dlcChosen))
                Add(Health.Good, L.F($"Add-ons folder: {dlcChosen}"), optional: true);
            else if (dlcChosen.Length > 0)
                Add(Health.Warning, L.F($"The add-ons folder chosen before is gone: {dlcChosen}. Choose it again, or clear it."));
            else if (GameEnvironment.FoundDlc(r.GameDir) is { } found)
                Add(Health.Good, L.F($"Add-ons found next to the game: {found}"), optional: true);
            else
                Add(Health.Good, L.T("No add-ons found. Only needed to play The Old Hunters with this edition: dump the add-on from your PS4 too, then choose its folder here. You can skip this."), optional: true);
            if (!dlcEnabled)
                Add(Health.Good, L.T("The Old Hunters switch on the Play page is off: the game starts without it."), optional: true);
        }

        // ---------------------------------------------------------------- finding the game

        /// <summary>
        /// Looks for Bloodborne game folders on this PC: beside the launcher's package, in the folders
        /// shadPS4's settings name, in the usual user folders, and three levels below each drive's
        /// root. Each folder found is passed to <paramref name="found"/> as it is found; gives up after
        /// <paramref name="limit"/>. Returns them all, the best first.
        /// </summary>
        public static List<string> Search(TimeSpan limit, Action<string>? found, CancellationToken cancel)
        {
            var until = DateTime.UtcNow + limit;
            var games = new List<string>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            bool Over() => cancel.IsCancellationRequested || DateTime.UtcNow > until;

            void Walk(string dir, int depth)
            {
                if (Over() || !seen.Add(Path.TrimEndingDirectorySeparator(dir))) return;
                if (IsGame(dir))
                {
                    games.Add(dir);
                    found?.Invoke(dir);
                    return;
                }
                if (depth == 0) return;
                foreach (var child in Children(dir))
                {
                    if (Skipped(Path.GetFileName(child))) continue;
                    Walk(child, depth - 1);
                    if (Over()) return;
                }
            }

            foreach (var (place, depth) in Places())
            {
                if (Over()) break;
                if (Directory.Exists(place)) Walk(place, depth);
            }
            return Best(games) is { } best ? games.OrderByDescending(g => g == best).ThenBy(g => g).ToList() : games;
        }

        private static IEnumerable<(string Dir, int Depth)> Places()
        {
            if (Paths.PortDir.Length > 0 && Directory.GetParent(Paths.PortDir) is { } parent)
                yield return (parent.FullName, 2);
            foreach (var dir in ShadPs4Folders())
                yield return (dir, 2);
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            foreach (var dir in new[]
                     {
                         Path.Combine(home, "Games"), Path.Combine(home, "Downloads"),
                         Environment.GetFolderPath(Environment.SpecialFolder.Desktop),
                         Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                     })
                yield return (dir, 3);
            foreach (var drive in DriveInfo.GetDrives())
            {
                bool ready;
                try { ready = drive.IsReady && drive.DriveType is DriveType.Fixed or DriveType.Removable; }
                catch (Exception) { ready = false; }
                if (ready) yield return (drive.RootDirectory.FullName, 3);
            }
        }

        /// <summary>The game folders shadPS4 was given (its settings files name them): every quoted path
        /// in them that is a folder.</summary>
        private static IEnumerable<string> ShadPs4Folders()
        {
            var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            var files = new List<string>();
            foreach (var root in new[] { Path.Combine(appData, "shadPS4"), Path.Combine(appData, "shadPS4QtLauncher") })
                if (Directory.Exists(root))
                    files.AddRange(Directory.EnumerateFiles(root, "*.*", SearchOption.TopDirectoryOnly)
                        .Where(f => f.EndsWith(".toml", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".json", StringComparison.OrdinalIgnoreCase) ||
                                    f.EndsWith(".ini", StringComparison.OrdinalIgnoreCase)));
            var folders = new List<string>();
            foreach (var file in files)
            {
                try
                {
                    if (new FileInfo(file).Length > 1 << 20) continue;
                    foreach (Match m in Regex.Matches(File.ReadAllText(file), "[\"']([A-Za-z]:[\\\\/][^\"'\\r\\n]*)[\"']"))
                    {
                        var path = m.Groups[1].Value.Replace("\\\\", "\\").Replace('/', '\\');
                        if (Directory.Exists(path)) folders.Add(path);
                    }
                }
                catch (Exception) { }
            }
            return folders.Distinct(StringComparer.OrdinalIgnoreCase);
        }

        private static bool Skipped(string name) =>
            name.StartsWith('$') || name.StartsWith('.') ||
            name is "Windows" or "Program Files" or "Program Files (x86)" or "ProgramData" or "System Volume Information" or
                "AppData" or "node_modules" or "Recovery" or "PerfLogs" or "steamapps" or "WindowsApps";

        // ---------------------------------------------------------------- helpers

        /// <summary>A Bloodborne game folder (not its update on its own): eboot.bin and a param.sfo
        /// naming a Bloodborne title.</summary>
        private static bool IsGame(string dir)
        {
            try
            {
                if (!File.Exists(Path.Combine(dir, "eboot.bin"))) return false;
                var sfo = GameCheck.ReadSfo(Path.Combine(dir, "sce_sys", "param.sfo"));
                return GameCheck.SupportedTitles.Contains(sfo.GetValueOrDefault("TITLE_ID", "")) && !UpdateOnly(dir, sfo);
            }
            catch (Exception)
            {
                return false;
            }
        }

        /// <summary>The best of several game folders: CUSA03173 first, then version 1.09 (its own or an
        /// update beside it).</summary>
        private static string? Best(List<string> games) =>
            games.OrderByDescending(g => Score(g)).FirstOrDefault();

        private static int Score(string game)
        {
            var sfo = GameCheck.ReadSfo(Path.Combine(game, "sce_sys", "param.sfo"));
            var score = sfo.GetValueOrDefault("TITLE_ID", "") == "CUSA03173" ? 2 : 0;
            if (sfo.GetValueOrDefault("APP_VER", "") == GameCheck.SupportedVersion || GameCheck.UpdateFolder(game) != null) score++;
            return score;
        }

        private static IEnumerable<string> Children(string dir)
        {
            try { return Directory.EnumerateDirectories(dir).ToList(); }
            catch (Exception) { return Array.Empty<string>(); }
        }

        private static bool HasFiles(string dir, string pattern)
        {
            try { return Directory.EnumerateFiles(dir, pattern).Any(); }
            catch (Exception) { return false; }
        }

        private static string Relative(string from, string to) => Path.GetRelativePath(from, to);

        private static string Shown(string version) => version == "?" ? L.T("unknown") : version;
    }
}
