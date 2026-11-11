using System.Net.Http;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Online play with The Hunter's Dream (the engine's scripts/online.py). Its account has no
    /// password: this PC is linked to it with a device code the player approves on the website
    /// (signed in with Discord), as bbhost does it (droogie/bbhost src/net/account.cpp), and the
    /// server hands back a token, kept encrypted in the launcher settings.
    /// </summary>
    public static class HuntersDream
    {
        /// <summary>BB_ONLINE_PAUSED=1 keeps the game offline (scripts/online.py, ONLINE_PAUSED).</summary>
        public static bool OnlinePaused { get; } = Environment.GetEnvironmentVariable("BB_ONLINE_PAUSED") == "1";

        public const string Server = "https://thehuntersdream.com";

        private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(30) };

        public sealed record Link(string DeviceCode, string UserCode, string Page, int Interval, int ExpiresIn);

        /// <summary>"BloodBorneButPc on PC-NAME": what the website's link page shows before approving.</summary>
        private static string DeviceName()
        {
            var host = Environment.MachineName;
            return host.Length == 0 ? "BloodBorneButPc" : "BloodBorneButPc on " + host[..Math.Min(48, host.Length)];
        }

        private static async Task<JsonObject> Call(string path, JsonObject body, CancellationToken cancel)
        {
            using var content = new StringContent(body.ToJsonString(), System.Text.Encoding.UTF8, "application/json");
            using var response = await Http.PostAsync(Server + path, content, cancel);
            var text = await response.Content.ReadAsStringAsync(cancel);
            JsonObject? reply = null;
            try { reply = JsonNode.Parse(text) as JsonObject; } catch (Exception) { }
            if (reply?["ResKind"] is null)
                throw new InvalidOperationException(L.F($"The Hunter's Dream answered HTTP {(int)response.StatusCode} without an account reply"));
            if (reply["ResKind"]!.GetValue<int>() != 0)
                throw new InvalidOperationException(reply["Message"]?.GetValue<string>() ?? "refused");
            return reply;
        }

        /// <summary>Starts a device link: the player opens <see cref="Link.Page"/> and enters the code.</summary>
        public static async Task<Link> StartLinkAsync(CancellationToken cancel)
        {
            var reply = await Call("/auth/device/start", new JsonObject { ["DeviceName"] = DeviceName() }, cancel);
            var link = new Link(reply["DeviceCode"]?.GetValue<string>() ?? "", reply["UserCode"]?.GetValue<string>() ?? "",
                reply["VerificationUri"]?.GetValue<string>() ?? Server + "/link",
                Math.Max(1, reply["Interval"]?.GetValue<int>() ?? 5), reply["ExpiresIn"]?.GetValue<int>() ?? 600);
            if (link.DeviceCode.Length == 0 || link.UserCode.Length == 0) throw new InvalidOperationException(L.T("no code in the reply"));
            return link;
        }

        /// <summary>Waits until the link is approved there. Returns (online name, token).</summary>
        public static async Task<(string Name, string Token)> WaitLinkAsync(Link link, CancellationToken cancel)
        {
            var interval = link.Interval;
            var until = DateTime.UtcNow.AddSeconds(link.ExpiresIn);
            while (DateTime.UtcNow < until)
            {
                await Task.Delay(TimeSpan.FromSeconds(interval), cancel);
                var reply = await Call("/auth/device/poll", new JsonObject { ["DeviceCode"] = link.DeviceCode }, cancel);
                switch (reply["Status"]?.GetValue<string>())
                {
                    case "approved":
                        var token = reply["Token"]?.GetValue<string>() ?? "";
                        var name = reply["OnlineId"]?.GetValue<string>() ?? "";
                        if (token.Length == 0 || name.Length == 0) throw new InvalidOperationException(L.T("no token in the reply"));
                        return (name, token);
                    case "slow_down":
                        interval += 5;
                        break;
                    case "denied":
                        throw new InvalidOperationException(L.T("the link was denied on the website"));
                    case "expired":
                        throw new InvalidOperationException(L.T("the code expired: start again"));
                }
            }
            throw new InvalidOperationException(L.T("the code expired: start again"));
        }

        /// <summary>An account held by this PC, without Discord: the name becomes the online name.
        /// Returns (name, token, recovery code); the code is shown once and must be written down.</summary>
        public static async Task<(string Name, string Token, string RecoveryCode)> CreateAccountAsync(string name, CancellationToken cancel)
        {
            var reply = await Call("/auth/account/create", new JsonObject { ["Name"] = name, ["DeviceName"] = DeviceName() }, cancel);
            return Account(reply);
        }

        /// <summary>A signed-out PC or a lost token: the name and its recovery code give a new token and
        /// a new recovery code; every other token of the account stops working.</summary>
        public static async Task<(string Name, string Token, string RecoveryCode)> RecoverAsync(string name, string code, CancellationToken cancel)
        {
            var reply = await Call("/auth/recover", new JsonObject { ["Name"] = name, ["RecoveryCode"] = code }, cancel);
            return Account(reply);
        }

        private static (string Name, string Token, string RecoveryCode) Account(JsonObject reply)
        {
            var token = reply["Token"]?.GetValue<string>() ?? "";
            var name = reply["OnlineId"]?.GetValue<string>() ?? "";
            if (token.Length == 0 || name.Length == 0) throw new InvalidOperationException(L.T("no token in the reply"));
            return (name, token, reply["RecoveryCode"]?.GetValue<string>() ?? "");
        }

        public static async Task<OnlineStatus.SignInState> WhoAmIAsync(string token, CancellationToken cancel)
        {
            try
            {
                using var content = new StringContent(new JsonObject { ["Token"] = token }.ToJsonString(), System.Text.Encoding.UTF8, "application/json");
                using var response = await Http.PostAsync(Server + "/auth/whoami", content, cancel);
                return OnlineStatus.ClassifyWhoAmI(await response.Content.ReadAsStringAsync(cancel));
            }
            catch (Exception) when (!cancel.IsCancellationRequested)
            {
                return new OnlineStatus.SignInState(OnlineStatus.SignIn.Unknown, "");
            }
        }

        /// <summary>Tells the server to forget the token (best effort).</summary>
        public static async Task LogoutAsync(string token)
        {
            try { await Call("/auth/logout", new JsonObject { ["Token"] = token }, CancellationToken.None); }
            catch (Exception) { }
        }
    }
}
