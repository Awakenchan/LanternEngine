using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// What a bug report needs after a crash: versions, the PC, the settings that matter and the
    /// end of the session log. The user's own folder is replaced by %USERPROFILE%.
    /// </summary>
    public static class CrashReport
    {
        private static readonly string[] Experimental =
            { "exp_vram_cleanup", "exp_frame_pacing", "exp_vram_budget", "exp_thread_priority", "exp_sleep_spin", "exp_async_pipelines", "exp_occlusion", "exp_fast_draws", "exp_compile_skip", "exp_fast_loading", "exp_hot_pages", "exp_sfx_workaround", "exp_camera_easing", "exp_sharp_ui" };

        /// <summary>What stopped the game, from the end of the session log (exit code 23 is the
        /// engine stopping itself, for several reasons).</summary>
        public static string Explain(int code)
        {
            var tail = Tail(GameRunner.SessionLog, 120).ToList();
            var why = Cause(tail, EngineCode(tail, code));
            var memory = LowMemory(tail);
            if (memory != null) why += " " + memory;
            // OptiScaler hooks the submissions and runs DLSS itself: a device lost with it loaded
            // says so (the engine runs a compatibility mode for it, Engine › OptiScaler compatibility).
            if (tail.Any(DeviceLost) && OptiScalerVersion() is { } opti)
            {
                // Its own frame generation on Vulkan runs through a D3D12 device it creates on the
                // first upscaled frames (the engine prints FGOutput from OptiScaler.ini).
                var fg = OptiScalerFrameGeneration();
                why += " " + (fg != null
                    ? L.F($"OptiScaler {opti} was loaded with its own frame generation on ({fg}); on Vulkan it runs through a D3D12 bridge that loses the GPU. Set Frame Generation to none in OptiScaler's menu (FGOutput=nofg in OptiScaler.ini) and use the launcher's frame generation instead.")
                    : L.F($"OptiScaler {opti} was loaded: it hooks the game's GPU submissions and runs DLSS itself. Keep Engine › OptiScaler compatibility on; if the crash comes back, take OptiScaler's files out of engine\\out to rule it out."));
            }
            return why;
        }

        /// <summary>A lost GPU in the log: the engine's report or Vulkan's ErrorDeviceLost (from a present).</summary>
        private static bool DeviceLost(string line) =>
            line.Contains("DEVICE_LOST", StringComparison.Ordinal) ||
            line.Contains("device lost", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("ErrorDeviceLost", StringComparison.Ordinal);

        /// <summary>OptiScaler's frame generation output from the engine's line
        /// ("OptiScaler: its frame generation is on (FGOutput=dlssg in OptiScaler.ini): ..."); null when off.</summary>
        private static string? OptiScalerFrameGeneration()
        {
            const string start = "OptiScaler: its frame generation is on (FGOutput=";
            var line = Tail(GameRunner.SessionLog, int.MaxValue).FirstOrDefault(l => l.StartsWith(start, StringComparison.Ordinal));
            return line?[start.Length..].Split(' ')[0];
        }

        /// <summary>OptiScaler's version from its own start line ("OptiScaler v0.9.5-pre4 (8dac650) ... loaded")
        /// or the engine's ("OptiScaler: v0.9.5.0 loaded as winmm.dll; ..."); null when it was not loaded.</summary>
        private static string? OptiScalerVersion()
        {
            foreach (var line in Tail(GameRunner.SessionLog, int.MaxValue))
            {
                var at = line.IndexOf("OptiScaler v", StringComparison.Ordinal);
                if (at >= 0 && line.Contains(" loaded", StringComparison.Ordinal))
                    return line[(at + 11)..].Split(' ')[0];
                if (line.StartsWith("OptiScaler: ", StringComparison.Ordinal) && line.Contains(" loaded as ", StringComparison.Ordinal))
                    return line[12..].StartsWith('v') ? line[12..].Split(' ')[0] : "";
            }
            return null;
        }

        private static string Cause(List<string> tail, long code)
        {
            bool Has(string text) => tail.Any(l => l.Contains(text, StringComparison.OrdinalIgnoreCase));
            if (OnlineStatus.RoomFull(tail) is { } full) return full;
            // The game's memory pool could not be created at start: Windows counts all of it against
            // RAM plus page file (error 1450: not enough left).
            if (Has("creating the memory pool section") || Has("mapping the memory pool"))
                return L.T("Windows could not give the game its memory at start (about 10 GB for a scene above 1080p, else 6 GB, ") +
                       L.T("RAM plus page file). A game left running in the background holds it: end bb-probe.exe in Task Manager ") +
                       L.T("(Details) or restart the PC. Otherwise set the page file to System managed size (Windows Settings › System › About › ") +
                       L.T("Advanced system settings › Performance Settings › Advanced › Virtual memory › Change), close other ") +
                       L.T("programs, or turn Live resolution On (Display) or choose a lower upscaler preset: a scene up to 1080p needs about 4 GB less.");
            // The game's first files missing from /app0: an incomplete game folder or a mod's.
            if (Has("gxshader.shaderbnd.dcx) -> not found") || Has("systex.tpf.dcx) -> not found"))
                return L.T("The game's own files are missing (dvdroot_ps4\\shader, dvdroot_ps4\\other): the game folder is incomplete. ") +
                       L.T("Turn Mods off and start again; if it still crashes, dump or extract the game again (base game plus the 1.09 update).");
            // The game's own panic on a file it could not load (#19): it then crashes on purpose.
            if (Has("FileTransferTask.cpp"))
                return L.T("The game could not load one of its files and stopped itself (FileTransferTask). A damaged or incomplete game ") +
                       L.T("folder does this: dump or extract the game again (base game plus the 1.09 update), keep it out of OneDrive, and ") +
                       L.T("turn Mods off to rule them out. A \"Runtime: reading ... failed\" line in the log names the file.");
            if (Has("Out of order flip IRQ"))
                return L.T("A frame reached the screen out of order (a timing race in the engine; Precise frame pacing makes it likelier).");
            // The game's memory pool full: the game doesn't check the failed allocation and crashes on
            // it a moment later ("writing address 0").
            if (Has("the game's memory pool is full"))
                return L.T("The game ran out of its memory pool: a patch from Mods → Third-party patches enlarges the game's memory ") +
                       L.T("(a resolution patch above 1080p, for example). Turn it off and choose the resolution in Display → Output resolution instead.");
            // Out of video memory first: a device lost often follows it in the same log.
            if (Has("ErrorOutOfDeviceMemory") || Has("ErrorOutOfHostMemory"))
                return L.T("The graphics card ran out of video memory. A lower output resolution or upscaler preset (Graphics), ") +
                       L.T("fewer texture or model mods, and closing other programs (browsers, recorders) leave it more room; ") +
                       L.T("keep \"Use more VRAM for textures\" off on cards with 4-6 GB.");
            if (tail.Any(DeviceLost))
                return L.T("The graphics driver stopped responding: the GPU hung or lost the game's memory.");
            // The engine's own message: "GPU [Debug] <Critical> file:line Function: text", maybe
            // followed by a detail line ("data_fmt = 7") before the STOP line.
            var critical = tail.FindLastIndex(l => l.Contains("<Critical>", StringComparison.Ordinal));
            if (critical >= 0)
            {
                var message = tail[critical][(tail[critical].IndexOf("<Critical>", StringComparison.Ordinal) + 10)..].Trim();
                var next = critical + 1 < tail.Count ? tail[critical + 1].Trim() : "";
                if (next.Length > 0 && !next.StartsWith("STOP:") && !next.Contains("exited with code")) message += " (" + next + ")";
                return L.T("The engine stopped on an internal check: ") + message;
            }
            // "Guest fault 0xc0000005 at guest+0x1234, ..." / "Host fault 0xc0000005 at bb-probe.exe+0x1234, ...".
            var fault = tail.LastOrDefault(l => FaultLine.IsMatch(l)) is { } f ? FaultLine.Match(f) : null;
            // The game's effect ribbon writers (image 0x28ce7b0-0x28cf580): their stack locals lost on
            // newer Intel CPUs (scripts/patches.py SFX_WORKAROUND), seen at the Witch of Hemwick.
            if (fault != null && fault.Groups[1].Value == "Guest" &&
                fault.Groups[3].Value.StartsWith("guest+0x", StringComparison.Ordinal) &&
                ulong.TryParse(fault.Groups[3].Value[8..], System.Globalization.NumberStyles.HexNumber, null, out var at) &&
                at is >= 0x28ce7b0 and < 0x28cf580)
                return $"The game crashed drawing an effect trail ({fault.Groups[3].Value}), a known crash on newer Intel CPUs. " +
                       "Turn on Graphics › Experimental › Effect trail crash fix: it stops this crash, " +
                       "and that kind of effect trail is no longer drawn.";
            if (fault != null)
                return fault.Groups[1].Value == "Guest"
                    ? L.F($"The game's own code crashed ({fault.Groups[3].Value}), usually a rare timing race in the game.")
                    : L.F($"The engine crashed in {fault.Groups[3].Value} ({Name(Convert.ToUInt32(fault.Groups[2].Value, 16))}).");
            if (tail.LastOrDefault(l => l.StartsWith("Online configuration:", StringComparison.Ordinal) ||
                                        l.StartsWith("bb-start:", StringComparison.Ordinal) ||
                                        l.StartsWith("prepare failed:", StringComparison.Ordinal)) is { } start)
                return L.T("The game did not start: ") + start[(start.IndexOf(':') + 1)..].Trim();
            return code switch
            {
                23 => L.T("The engine stopped itself on an error (exit code 23); the report has the details."),
                24 => L.T("The graphics card stopped answering for a minute, so the engine ended the game (exit code 24)."),
                0xC0000005 => L.T("The engine crashed on a bad memory access (0xC0000005)."),
                0xC0000409 => L.T("The engine stopped on a failed safety check (0xC0000409)."),
                0xC00000FD => L.T("The engine ran out of stack (0xC00000FD)."),
                > 0xFFFF => L.F($"The game closed unexpectedly (0x{code:X8})."),
                _ => L.F($"The game closed unexpectedly (exit code {code}).")
            };
        }

        private static string Name(uint code) => code switch
        {
            0xC0000005 => L.T("bad memory access"),
            0xC000001D => L.T("illegal instruction"),
            0xC0000094 => L.T("division by zero"),
            _ => $"0x{code:X8}"
        };

        private static readonly Regex FaultLine = new(@"^(Host|Guest) fault 0x([0-9a-fA-F]{8}) at ([^,\s]+)", RegexOptions.Compiled);
        private static readonly Regex ExitLine = new(@"bb-probe exited with code (-?\d+)", RegexOptions.Compiled);
        private static readonly Regex MemoryLine = new(
            @"^Memory: game (\d+) MiB in RAM \((\d+) MiB committed, peak (\d+) MiB\); PC (\d+) of (\d+) MiB free, (\d+) MiB commit left",
            RegexOptions.Compiled);

        /// <summary>The engine's own exit code: run_windows.py before 0.9.9 passed Windows crash
        /// codes (0xC0000005) on as -1.</summary>
        private static long EngineCode(List<string> tail, int code)
        {
            var line = tail.LastOrDefault(l => ExitLine.IsMatch(l));
            var engine = line != null && long.TryParse(ExitLine.Match(line).Groups[1].Value, out var c) ? c : code;
            return engine < 0 ? engine & 0xFFFFFFFF : engine;
        }

        /// <summary>A sentence when the last memory line shows Windows nearly out of RAM or commit.</summary>
        private static string? LowMemory(List<string> tail)
        {
            var line = tail.LastOrDefault(l => l.StartsWith("Memory: game ", StringComparison.Ordinal));
            if (line == null || MemoryLine.Match(line) is not { Success: true } m) return null;
            long free = long.Parse(m.Groups[4].Value), total = long.Parse(m.Groups[5].Value), commit = long.Parse(m.Groups[6].Value);
            return free * 100 < total * 4 || commit < 1024
                ? L.F($"Windows was nearly out of memory ({free} MiB of {total} MiB free, {commit} MiB commit left): closing other programs or a larger page file helps.")
                : null;
        }

        /// <summary>"RAM: 5210 MiB at the start, 5390 MiB at the end, peak 5502 MiB; PC ..." from
        /// the once-a-minute memory lines (engines from 0.9.9 on).</summary>
        private static string? RamTrend()
        {
            string? first = null, last = null;
            foreach (var line in Tail(GameRunner.SessionLog, int.MaxValue))
                if (line.StartsWith("Memory: game ", StringComparison.Ordinal) && MemoryLine.IsMatch(line))
                {
                    first ??= line;
                    last = line;
                }
            if (first == null || last == null) return null;
            Match a = MemoryLine.Match(first), b = MemoryLine.Match(last);
            return L.F($"RAM: game {a.Groups[1].Value} MiB at the start, {b.Groups[1].Value} MiB at the end ") +
                   L.F($"({a.Groups[2].Value} → {b.Groups[2].Value} MiB committed, peak {b.Groups[3].Value} MiB); ") +
                   L.F($"PC {b.Groups[4].Value} of {b.Groups[5].Value} MiB free, {b.Groups[6].Value} MiB commit left");
        }

        public static string Build(LauncherSettings s, IniFile ini, int code, int logLines = 150)
        {
            var hw = Hardware.Detect();
            var text = new StringBuilder();
            text.AppendLine($"Lantern Engine {Updater.CurrentLabel}, engine {Updater.LocalEngine() ?? "unknown"}");
            var tail = Tail(GameRunner.SessionLog, 120).ToList();
            text.AppendLine(L.F($"Exit code 0x{EngineCode(tail, code):X}: {Explain(code)}"));
            text.AppendLine(L.F($"{hw.Summary}, {hw.RamBytes >> 30} GB RAM, Windows {Environment.OSVersion.Version}"));
            if (RamTrend() is { } ram) text.AppendLine(ram);
            text.AppendLine(L.F($"Upscaler {ini.Get("upscaler")}, preset {ini.Get("preset")}, output {ini.Get("output_res")}, ") +
                            L.F($"FPS {s.Str("fps_mode")}, live resolution {ini.Get("live_resolution")}, mods {(s.Bool("mods_enabled") ? "on" : "off")}"));
            text.AppendLine(L.T("Experimental: ") + string.Join(", ", Experimental.Select(k => $"{k[4..].Replace('_', ' ')} {(s.Bool(k) ? "on" : "off")}")));
            text.AppendLine();
            text.AppendLine(L.F($"Last {logLines} lines of logs\\last_session.log (repeats folded):"));
            text.AppendLine("```");
            // The game logs some lines hundreds of times (every autosave): folded, so the lines
            // before them still fit.
            var lines = new List<(string Line, int Count)>();
            foreach (var line in Tail(GameRunner.SessionLog, 2000))
                if (lines.Count > 0 && lines[^1].Line == line) lines[^1] = (line, lines[^1].Count + 1);
                else lines.Add((line, 1));
            foreach (var (line, count) in lines.Skip(Math.Max(0, lines.Count - logLines)))
                text.AppendLine(count > 1 ? L.F($"{line}  (x{count})") : line);
            text.AppendLine("```");
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return home.Length > 3 ? text.ToString().Replace(home, "%USERPROFILE%", StringComparison.OrdinalIgnoreCase) : text.ToString();
        }

        private static IEnumerable<string> Tail(string path, int count)
        {
            try
            {
                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var reader = new StreamReader(stream);
                var lines = new Queue<string>();
                while (reader.ReadLine() is { } line)
                {
                    lines.Enqueue(line);
                    if (lines.Count > count) lines.Dequeue();
                }
                return lines;
            }
            catch (Exception e)
            {
                return new[] { L.T("(the session log could not be read: ") + e.Message + ")" };
            }
        }
    }
}
