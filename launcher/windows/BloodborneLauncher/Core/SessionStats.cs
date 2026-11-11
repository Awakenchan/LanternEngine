using System.Globalization;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// One play session from the engine's output: how long it ran, the frame rate (from the
    /// "Frame stats:" lines, printed with Frame statistics on) and the stutters ("Stall:" lines).
    /// Playtime adds up per player profile in the settings (playtime.&lt;profile&gt;, seconds).
    /// </summary>
    public sealed class SessionStats
    {
        private static readonly Regex FrameStats = new(@"^Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms", RegexOptions.Compiled);
        private static readonly Regex Stall = new(@"^Stall: ([\d.]+) ms frame", RegexOptions.Compiled);
        private static readonly Regex Memory = new(@"^Memory: game (\d+) MiB in RAM", RegexOptions.Compiled);
        // The Hunter's Dream answered the account's token with HTTP 401 (thd_session.cpp MarkRefused, once
        // a session): "THD: the server refused this PC's sign-in as <name> (<reason>): sign in again ...".
        private const string RefusedText = "THD: the server refused this PC's sign-in as ";
        private static readonly Regex Refused = new(@"refused this PC's sign-in as (.*?) \((.*)\): sign in again", RegexOptions.Compiled);

        private readonly List<double> _fps = new();

        public DateTime Started { get; } = DateTime.Now;
        public TimeSpan Length { get; private set; }
        public double WorstFrameMs { get; private set; }
        public int Stutters { get; private set; }
        public double AverageFps => _fps.Count > 0 ? _fps.Average() : 0;
        public double LowestFps => _fps.Count > 0 ? _fps.Min() : 0;
        public bool HasFrameStats => _fps.Count > 0;
        /// <summary>The game's RAM in MiB, first and last of the once-a-minute lines (0: none).</summary>
        public long RamStartMiB { get; private set; }
        public long RamEndMiB { get; private set; }
        /// <summary>The Hunter's Dream refused this PC's sign-in during the session: the account name and
        /// the server's reason ("HTTP 401" when it gave none). Null when it did not.</summary>
        public (string Name, string Reason)? SignInRefused { get; private set; }
        public string? RoomFull { get; private set; }

        public void Observe(string line)
        {
            if (line.StartsWith("Frame stats: ", StringComparison.Ordinal) && FrameStats.Match(line) is { Success: true } m)
            {
                _fps.Add(Parse(m.Groups[1].Value));
                WorstFrameMs = Math.Max(WorstFrameMs, Parse(m.Groups[2].Value));
            }
            else if (line.StartsWith("Memory: game ", StringComparison.Ordinal) && Memory.Match(line) is { Success: true } r)
            {
                RamEndMiB = long.Parse(r.Groups[1].Value, CultureInfo.InvariantCulture);
                if (RamStartMiB == 0) RamStartMiB = RamEndMiB;
            }
            else if (line.StartsWith("Stall: ", StringComparison.Ordinal) && Stall.Match(line) is { Success: true } s)
            {
                Stutters++;
                WorstFrameMs = Math.Max(WorstFrameMs, Parse(s.Groups[1].Value));
            }
            else if (line.Contains("This room is full", StringComparison.Ordinal) && OnlineStatus.RoomFull(line) is { } full)
            {
                RoomFull = full;
            }
            // Anywhere in the line, should a wrapper ever put something before the engine's "THD: ".
            else if (line.Contains(RefusedText, StringComparison.Ordinal))
            {
                var said = Refused.Match(line);
                var reason = said.Success ? said.Groups[2].Value.Trim() : "";
                SignInRefused = (said.Success ? said.Groups[1].Value : "", reason.Length > 0 ? reason : "HTTP 401");
            }
        }

        public void Finish() => Length = DateTime.Now - Started;

        /// <summary>"1 h 12 min · 112 FPS average (lowest 98) · worst frame 107 ms · 3 stutters".</summary>
        public string Summary =>
            Duration(Length) + (HasFrameStats
                ? L.F($" · {AverageFps:0} FPS average (lowest {LowestFps:0}) · worst frame {WorstFrameMs:0} ms · ") +
                  (Stutters == 1 ? L.T("1 stutter") : L.F($"{Stutters} stutters"))
                : " · turn on Frame statistics (Settings) for FPS and stutters") +
            (RamEndMiB > 0 ? L.F($" · RAM {RamStartMiB / 1024.0:0.0} → {RamEndMiB / 1024.0:0.0} GB") : "");

        public static string Duration(TimeSpan time) =>
            time.TotalHours >= 1 ? L.F($"{(int)time.TotalHours} h {time.Minutes} min") :
            time.TotalMinutes >= 1 ? L.F($"{(int)time.TotalMinutes} min") : L.F($"{Math.Max(0, (int)time.TotalSeconds)} s");

        public static TimeSpan Playtime(LauncherSettings s, string profile) =>
            TimeSpan.FromSeconds(Math.Max(0, s.Int(Key(profile))));

        public static void AddPlaytime(LauncherSettings s, string profile, TimeSpan time) =>
            s.Set(Key(profile), (int)Math.Min(int.MaxValue, Playtime(s, profile).TotalSeconds + time.TotalSeconds));

        private static string Key(string profile) => "playtime." + profile.ToLowerInvariant();

        private static double Parse(string value) => double.Parse(value, CultureInfo.InvariantCulture);
    }
}
