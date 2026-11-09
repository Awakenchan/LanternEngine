using System.Buffers.Binary;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Discord status while the game runs ("Playing Lantern Engine"), through the Discord app's own
    /// local connection (the named pipe discord-ipc-N: an 8-byte header, opcode and length, then
    /// JSON). Nothing when Discord is not running; it is looked for again every 20 s.
    /// discord_status: "off" (default), "lantern", or "custom": the player's own lines, picture
    /// (an uploaded asset name or an https link), hover texts, small badge, timer and up to two
    /// buttons. {version} in a line is the launcher's version.
    /// </summary>
    public static class DiscordPresence
    {
        private const string LanternApp = "1561836572364505089";
        private const string LogoLink = "https://awakenchan.github.io/LanternEngine/lantern-logo.png";
        /// <summary>A second app named "Bloodborne"; empty until it is registered.</summary>
        private const string BloodborneApp = "";

        /// <summary>The name in bold is the Discord app's own: one app per name. A player's own
        /// app (any name they gave it on discord.com/developers) by its Application ID.</summary>
        public static Choice[] Names => string.IsNullOrEmpty(BloodborneApp)
            ? new[] { new Choice("Lantern Engine", "lantern"), new Choice("My own Discord app", "own") }
            : new[] { new Choice("Lantern Engine", "lantern"), new Choice("Bloodborne", "bloodborne"), new Choice("My own Discord app", "own") };

        /// <summary>What the status shows; the launcher's preview draws the same.</summary>
        public sealed record Look(string AppId, string Name, string Details, string State, string Image, string ImageText, string Badge,
            string BadgeText, bool Timer, IReadOnlyList<(string Label, string Url)> Buttons);

        /// <summary>Pictures uploaded to the Lantern Engine app (Rich Presence art assets).</summary>
        public static readonly Choice[] Pictures = { new("Lantern", "logo"), new("Your own link", "url") };

        public static string Version => Updater.CurrentLabel;

        /// <summary>The look for the settings; null when the status is off.</summary>
        public static Look? LookOf(LauncherSettings s, LiveGameInfo? example = null)
        {
            var mode = s.Str("discord_status");
            if (mode.Length == 0 || mode == "off") return null;
            var game = LiveGame.Current ?? example;
            bool place = s.Bool("discord_game_place"), stats = s.Bool("discord_game_stats");
            if (mode != "custom")
                return new Look(LanternApp, "Lantern Engine", (place ? Doing(game) : null) ?? "Hunting in Yharnam", (stats ? Progress(game) : null) ?? Version, "logo",
                    "Lantern Engine", "", "", true, Array.Empty<(string, string)>());
            string Fill(string text) => FillIn(text, game, place, stats);
            string Text(string key, string fallback) => Fill(s.Str(key).Trim() is { Length: > 0 } v ? v : fallback);
            var picture = s.Str("discord_picture") == "url" && IsLink(s.Str("discord_image_url")) ? s.Str("discord_image_url").Trim() : "logo";
            var badge = IsLink(s.Str("discord_badge_url")) ? s.Str("discord_badge_url").Trim() : "";
            var buttons = new List<(string, string)>();
            foreach (var n in new[] { 1, 2 })
            {
                var label = s.Str($"discord_button{n}_label").Trim();
                var url = s.Str($"discord_button{n}_url").Trim();
                if (label.Length > 0 && IsLink(url)) buttons.Add((Cut(label, 32), url));
            }
            var (app, name) = s.Str("discord_app") switch
            {
                "bloodborne" when BloodborneApp.Length > 0 => (BloodborneApp, "Bloodborne"),
                "own" when IsAppId(s.Str("discord_app_id")) => (s.Str("discord_app_id").Trim(),
                    s.Str("discord_app_name").Trim() is { Length: > 0 } own ? own : "Your app"),
                _ => (LanternApp, "Lantern Engine"),
            };
            // Another app has none of the Lantern Engine app's uploaded pictures: a link or nothing.
            if (picture == "logo" && app != LanternApp) picture = "";
            return new Look(app, name, Text("discord_details", (place ? Doing(game) : null) ?? "Hunting in Yharnam"), Text("discord_state", (stats ? Progress(game) : null) ?? ""), picture,
                Text("discord_image_text", "Lantern Engine"), badge, Fill(s.Str("discord_badge_text").Trim()),
                s.Bool("discord_timer"), buttons);
        }

        public static bool IsAppId(string text) => text.Trim().Length is >= 15 and <= 22 && text.Trim().All(char.IsDigit);

        public static bool IsLink(string text) =>
            Uri.TryCreate(text.Trim(), UriKind.Absolute, out var uri) && uri.Scheme == Uri.UriSchemeHttps;

        private static string? Doing(LiveGameInfo? game) =>
            game == null ? null
            : game.BossName.Length > 0 ? $"Fighting {game.BossName}"
            : game.Area.Length > 0 ? $"In {game.Area}"
            : null;

        private static string? Progress(LiveGameInfo? game) =>
            game == null ? null : $"Level {game.Level} \u2022 {game.Echoes:N0} Blood Echoes \u2022 Insight {game.Insight}";

        private static string FillIn(string text, LiveGameInfo? game, bool place, bool stats)
        {
            var where = place ? game : null;
            var mine = stats ? game : null;
            return text.Replace("{version}", Version, StringComparison.OrdinalIgnoreCase)
                .Replace("{area}", where?.Area ?? "", StringComparison.OrdinalIgnoreCase)
                .Replace("{boss}", where?.BossName ?? "", StringComparison.OrdinalIgnoreCase)
                .Replace("{level}", mine?.Level.ToString() ?? "", StringComparison.OrdinalIgnoreCase)
                .Replace("{echoes}", mine?.Echoes.ToString("N0") ?? "", StringComparison.OrdinalIgnoreCase)
                .Replace("{insight}", mine?.Insight.ToString() ?? "", StringComparison.OrdinalIgnoreCase);
        }

        private static string Cut(string text, int max) => text.Length > max ? text[..max] : text;

        private static JsonObject ToActivity(Look look, DateTime? startedUtc)
        {
            var assets = new JsonObject();
            if (look.Image.Length > 0) assets["large_image"] = look.Image == "logo" ? LogoLink : look.Image;
            if (look.ImageText.Length >= 2) assets["large_text"] = Cut(look.ImageText, 128);
            if (look.Badge.Length > 0)
            {
                assets["small_image"] = look.Badge;
                if (look.BadgeText.Length >= 2) assets["small_text"] = Cut(look.BadgeText, 128);
            }
            var activity = new JsonObject { ["assets"] = assets };
            // Discord wants each line 2 to 128 characters long, or leaves it out.
            if (look.Details.Length >= 2) activity["details"] = Cut(look.Details, 128);
            if (look.State.Length >= 2) activity["state"] = Cut(look.State, 128);
            if (look.Timer)
                activity["timestamps"] = new JsonObject { ["start"] = new DateTimeOffset(startedUtc ?? DateTime.UtcNow).ToUnixTimeSeconds() };
            if (look.Buttons.Count > 0)
            {
                var buttons = new JsonArray();
                foreach (var (label, url) in look.Buttons) buttons.Add((JsonNode)new JsonObject { ["label"] = label, ["url"] = url });
                activity["buttons"] = buttons;
            }
            return activity;
        }

        private static readonly object Gate = new();
#if AVALONIA
        private static Stream? _pipe; // a named pipe on Windows, a Unix socket elsewhere
#else
        private static NamedPipeClientStream? _pipe;
#endif
        private static string _pipeApp = "";  // the app the pipe signed in as
        private static string _wantedApp = LanternApp;
        private static JsonObject? _wanted;   // the activity to show; null: none
        private static bool _dirty;
        private static CancellationTokenSource? _loop;

        /// <summary>The game started or stopped, or the settings changed.</summary>
        private static LauncherSettings? _settings;
        private static bool _running;
        private static DateTime? _startedUtc;

        static DiscordPresence()
        {
            LiveGame.Changed += () =>
            {
                LauncherSettings? s;
                lock (Gate) s = _settings;
                if (s != null && _running) Update(s, true, _startedUtc);
            };
        }

        public static void Update(LauncherSettings s, bool gameRunning, DateTime? startedUtc)
        {
            lock (Gate)
            {
                _settings = s;
                _running = gameRunning;
                _startedUtc = startedUtc;
            }
            var look = gameRunning ? LookOf(s) : null;
            lock (Gate)
            {
                _wanted = look == null ? null : ToActivity(look, startedUtc);
                if (look != null) _wantedApp = look.AppId;
                _dirty = true;
                if (_loop == null && (_wanted != null || _pipe != null))
                {
                    _loop = new CancellationTokenSource();
                    var token = _loop.Token;
                    _ = Task.Run(() => Loop(token));
                }
            }
        }

        /// <summary>Clears the status (the launcher is closing).</summary>
        public static void Stop()
        {
            lock (Gate)
            {
                _loop?.Cancel();
                _loop = null;
                try
                {
                    if (_pipe != null) Send(_pipe, 1, Activity(null));
                }
                catch (Exception) { }
                _pipe?.Dispose();
                _pipe = null;
            }
        }

        private static async Task Loop(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    JsonObject? wanted;
                    bool dirty;
                    lock (Gate)
                    {
                        wanted = _wanted;
                        dirty = _dirty;
                        _dirty = false;
                    }
                    string app;
                    lock (Gate) app = _wantedApp;
                    if (_pipe != null && wanted != null && app != _pipeApp)
                    {
                        // Another app (another name): clear the old one's status and sign in again.
                        try { Send(_pipe, 1, Activity(null)); Receive(_pipe); } catch (Exception) { }
                        _pipe.Dispose();
                        _pipe = null;
                    }
                    if (_pipe == null && wanted != null)
                    {
                        _pipe = Connect(app);
                        _pipeApp = app;
                        dirty = _pipe != null;
                    }
                    if (_pipe != null && dirty)
                    {
                        Send(_pipe, 1, Activity(wanted));
                        Receive(_pipe); // the answer, so the pipe never fills up
                    }
                    if (_pipe != null && wanted == null)
                    {
                        // Nothing to show: let Discord go until the next game.
                        _pipe.Dispose();
                        _pipe = null;
                        lock (Gate)
                        {
                            if (_wanted == null)
                            {
                                _loop = null;
                                return;
                            }
                        }
                    }
                }
                catch (Exception)
                {
                    // Discord closed or restarted: connect again later, with the status set again.
                    _pipe?.Dispose();
                    _pipe = null;
                    lock (Gate) _dirty = true;
                }
                try { await Task.Delay(_pipe == null ? 20_000 : 2_000, token); } catch (TaskCanceledException) { return; }
            }
        }

#if AVALONIA
        private static Stream? Connect(string app)
        {
            if (!OperatingSystem.IsWindows()) return ConnectSocket(app);
            return ConnectPipe(app);
        }

        /// <summary>Linux: discord-ipc-0..9 in the runtime folder (the Flatpak and Snap Discords one
        /// folder below it).</summary>
        private static Stream? ConnectSocket(string app)
        {
            var runtime = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR") is { Length: > 0 } dir ? dir : Path.GetTempPath();
            foreach (var folder in new[] { runtime, Path.Combine(runtime, "app", "com.discordapp.Discord"), Path.Combine(runtime, "snap.discord") })
                for (var i = 0; i < 10; ++i)
                {
                    var path = Path.Combine(folder, $"discord-ipc-{i}");
                    if (!File.Exists(path)) continue;
                    var socket = new System.Net.Sockets.Socket(System.Net.Sockets.AddressFamily.Unix, System.Net.Sockets.SocketType.Stream,
                        System.Net.Sockets.ProtocolType.Unspecified);
                    try
                    {
                        socket.Connect(new System.Net.Sockets.UnixDomainSocketEndPoint(path));
                        var stream = new System.Net.Sockets.NetworkStream(socket, ownsSocket: true) { ReadTimeout = 3000 };
                        Send(stream, 0, new JsonObject { ["v"] = 1, ["client_id"] = app });
                        Receive(stream); // READY
                        return stream;
                    }
                    catch (Exception)
                    {
                        socket.Dispose();
                    }
                }
            return null;
        }

        private static NamedPipeClientStream? ConnectPipe(string app)
#else
        private static NamedPipeClientStream? Connect(string app)
#endif
        {
            for (var i = 0; i < 10; ++i)
            {
                var pipe = new NamedPipeClientStream(".", $"discord-ipc-{i}", PipeDirection.InOut, PipeOptions.None);
                try
                {
                    pipe.Connect(300);
                    Send(pipe, 0, new JsonObject { ["v"] = 1, ["client_id"] = app });
                    Receive(pipe); // READY
                    return pipe;
                }
                catch (Exception)
                {
                    pipe.Dispose();
                }
            }
            return null;
        }

        private static JsonObject Activity(JsonObject? activity) => new()
        {
            ["cmd"] = "SET_ACTIVITY",
            ["args"] = new JsonObject
            {
                ["pid"] = Environment.ProcessId,
                ["activity"] = activity?.DeepClone(),
            },
            ["nonce"] = Guid.NewGuid().ToString(),
        };

        private static void Send(Stream pipe, int opcode, JsonObject body)
        {
            var json = Encoding.UTF8.GetBytes(body.ToJsonString());
            var frame = new byte[8 + json.Length];
            BinaryPrimitives.WriteInt32LittleEndian(frame.AsSpan(0, 4), opcode);
            BinaryPrimitives.WriteInt32LittleEndian(frame.AsSpan(4, 4), json.Length);
            json.CopyTo(frame, 8);
            pipe.Write(frame);
            pipe.Flush();
        }

        private static void Receive(Stream pipe)
        {
            var header = new byte[8];
            pipe.ReadExactly(header);
            var length = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(4, 4));
            if (length < 0 || length > 1 << 20) throw new IOException("bad Discord frame");
            var body = new byte[length];
            pipe.ReadExactly(body);
        }
    }
}
