using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;

namespace BloodborneLauncher.Core
{
    /// <summary>The online connection test, all on The Hunter's Dream: the game server (messages,
    /// bloodstains, ghosts), the session service co-op signs in to, and the STUN/relay server
    /// summons go through (the engine's thd backend, scripts/online.py).</summary>
    public static class Online
    {
        public sealed record Result(string Name, string Address, bool Ok, string Detail);

        private const string Host = "thehuntersdream.com";
        private const int SessionPort = 18671, StunPort = 3478;

        /// <summary>The game server as scripts/online.py uses it: empty is The Hunter's Dream, a
        /// bare name gets https://, and the old default (the public shadNet Web API, which has no
        /// game server) becomes The Hunter's Dream.</summary>
        public static string GameServer(string text)
        {
            text = text.Trim().TrimEnd('/');
            if (text.Length == 0) return LauncherSettings.OnlineCommunity;
            if (!text.Contains("://")) text = "https://" + text;
            return Uri.TryCreate(text, UriKind.Absolute, out var uri) && uri.Host == "srv.shadps4.net"
                ? LauncherSettings.OnlineCommunity
                : text;
        }

        public static async Task<List<Result>> CheckAsync(string gameServer)
        {
            var results = new List<Result> { await CheckGameServerAsync(gameServer) };
            var session = $"{Host}:{SessionPort}";
            try
            {
                using var tcp = new TcpClient();
                using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                await tcp.ConnectAsync(Host, SessionPort, cancel.Token);
                results.Add(new Result("Co-op sessions", session, true, ""));
            }
            catch (Exception e)
            {
                results.Add(new Result("Co-op sessions", session, false, e.GetBaseException().Message));
            }
            results.Add(await CheckStunAsync());
            return results;
        }

        /// <summary>A STUN binding request (RFC 5389) to the relay summons use: an answer means UDP
        /// gets through, with the address other players see.</summary>
        private static async Task<Result> CheckStunAsync()
        {
            var address = L.F($"{Host}:{StunPort} (UDP)");
            try
            {
                var ip = (await Dns.GetHostAddressesAsync(Host)).First(a => a.AddressFamily == AddressFamily.InterNetwork);
                using var udp = new UdpClient(0);
                var request = new byte[20];
                request[1] = 0x01; // Binding Request, no attributes
                request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;
                Random.Shared.NextBytes(request.AsSpan(8, 12));
                var server = new IPEndPoint(ip, StunPort);
                for (var attempt = 0; attempt < 3; ++attempt)
                {
                    await udp.SendAsync(request, request.Length, server);
                    using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(2));
                    try
                    {
                        var b = (await udp.ReceiveAsync(cancel.Token)).Buffer;
                        if (b.Length < 20 || b[0] != 0x01 || b[1] != 0x01 || !b.AsSpan(8, 12).SequenceEqual(request.AsSpan(8, 12)))
                            continue;
                        // The mapped address, IPv4: XOR-MAPPED-ADDRESS (0x0020, or 0x8020 from older
                        // servers, XORed with the magic cookie) or plain MAPPED-ADDRESS (0x0001).
                        for (var at = 20; at + 12 <= b.Length; at += 4 + ((b[at + 2] << 8 | b[at + 3]) + 3) / 4 * 4)
                        {
                            var type = b[at] << 8 | b[at + 1];
                            if (type is not (0x0020 or 0x8020 or 0x0001) || b[at + 5] != 0x01) continue;
                            var xor = type != 0x0001;
                            var port = (b[at + 6] << 8 | b[at + 7]) ^ (xor ? 0x2112 : 0);
                            var ipBytes = new byte[4];
                            for (var k = 0; k < 4; ++k) ipBytes[k] = (byte)(b[at + 8 + k] ^ (xor ? request[4 + k] : 0));
                            return new Result("Co-op relay", address, true, L.F($"the server sees you at {new IPAddress(ipBytes)}:{port}"));
                        }
                        return new Result("Co-op relay", address, true, "");
                    }
                    catch (OperationCanceledException)
                    {
                    }
                }
                return new Result("Co-op relay", address, false, "no answer (a firewall or VPN may block UDP)");
            }
            catch (Exception e)
            {
                return new Result("Co-op relay", address, false, e.GetBaseException().Message);
            }
        }

        /// <summary>The game's first request: its server information (ss.info, base64 XML that
        /// lists the game APIs). Without a port, the game's own (20443) is used.</summary>
        private static async Task<Result> CheckGameServerAsync(string gameServer)
        {
            try
            {
                var uri = new UriBuilder(gameServer);
                if (new Uri(gameServer).IsDefaultPort) uri.Port = 20443;
                uri.Path = "/bb-eu/ss.info";
                using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(7) };
                var body = await http.GetStringAsync(uri.Uri);
                var xml = Encoding.UTF8.GetString(Convert.FromBase64String(body.Trim()));
                var ok = xml.Contains("<api_", StringComparison.Ordinal);
                return new Result(L.T("Game server"), gameServer, ok, ok ? "" : L.T("not a Bloodborne game server"));
            }
            catch (FormatException)
            {
                return new Result(L.T("Game server"), gameServer, false, L.T("not a Bloodborne game server"));
            }
            catch (Exception e)
            {
                return new Result(L.T("Game server"), gameServer, false, e.GetBaseException().Message);
            }
        }
    }
}
