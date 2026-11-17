// SPDX-License-Identifier: GPL-3.0-or-later
using System.Net;
using System.Net.Sockets;
using System.Text;
using BloodborneLauncher.Core;

var golden = args.FirstOrDefault(a => !a.StartsWith("--")) ??
    Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "requests.golden.txt");
var record = args.Contains("--record");

var listener = new TcpListener(IPAddress.Loopback, 0);
listener.Start();
var port = ((IPEndPoint)listener.LocalEndpoint).Port;
var seen = new List<string>();
var gate = new object();
var stop = new CancellationTokenSource();
var accepting = Task.Run(async () =>
{
    while (!stop.IsCancellationRequested)
    {
        TcpClient client;
        try { client = await listener.AcceptTcpClientAsync(stop.Token); }
        catch (OperationCanceledException) { return; }
        using (client)
        {
            var stream = client.GetStream();
            var head = new StringBuilder();
            var one = new byte[1];
            while (!head.ToString().EndsWith("\r\n\r\n") && await stream.ReadAsync(one, stop.Token) == 1)
                head.Append((char)one[0]);
            var first = head.ToString().Split("\r\n")[0];
            var target = first.Split(' ').Length >= 2 ? first.Split(' ')[0] + " " + first.Split(' ')[1] : first;
            lock (gate) seen.Add(target);
            var answer = Encoding.ASCII.GetBytes("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            await stream.WriteAsync(answer, stop.Token);
        }
    }
});

Environment.SetEnvironmentVariable("NO_PROXY", null);
Environment.SetEnvironmentVariable("HTTPS_PROXY", $"http://127.0.0.1:{port}");
Environment.SetEnvironmentVariable("HTTP_PROXY", $"http://127.0.0.1:{port}");

List<string> Phase()
{
    lock (gate)
    {
        var taken = seen.ToList();
        seen.Clear();
        return taken;
    }
}

var text = new StringBuilder();
void Write(string name, List<string> requests)
{
    text.Append(name).Append(": ").Append(requests.Count).Append('\n');
    foreach (var r in requests) text.Append("  ").Append(r).Append('\n');
}

var state = await HuntersDream.WhoAmIAsync("test-token-0123456789", CancellationToken.None);
Write("phase 1, launcher open with a token held: the Online page's sign-in check (HuntersDream.WhoAmIAsync, once per launcher run when the page is first shown)", Phase());

var failures = new List<string>();
void Require(bool ok, string what)
{
    if (!ok) failures.Add(what);
}
const string token = "test-token-0123456789";
var starts = new ThdStart();
Task<OnlineStatus.SignInState> Ask(CancellationToken cancel) => HuntersDream.WhoAmIAsync(token, cancel);
var played = await starts.ForPlayAsync(token, Ask, ThdStart.Timeout);
var firstPlay = Phase();
Require(firstPlay.Count == 1 && firstPlay[0] == "CONNECT thehuntersdream.com:443", "a Play without a check before it asks whoami once");
Require(played.State == OnlineStatus.SignIn.Unknown, "a server that cannot be reached reads as unknown at Play");
var page = HuntersDream.WhoAmIAsync(token, CancellationToken.None);
starts.Checking(token, page);
await page;
Phase();
played = await starts.ForPlayAsync(token, Ask, ThdStart.Timeout);
Require(Phase().Count == 0, "a Play right after the Online page's check reuses it and asks nothing");
played = await starts.ForPlayAsync(token, Ask, ThdStart.Timeout);
Require(Phase().Count == 1, "the next Play asks once again");
starts.Checking("an-older-token", Task.FromResult(new OnlineStatus.SignInState(OnlineStatus.SignIn.Valid, "TestHunter")));
played = await starts.ForPlayAsync(token, Ask, ThdStart.Timeout);
Require(Phase().Count == 1 && played.State == OnlineStatus.SignIn.Unknown, "a check for another token is not this start's check");
stop.Cancel();
listener.Stop();
try { await accepting; } catch (Exception) { }

var hung = new TaskCompletionSource<OnlineStatus.SignInState>();
var hungCancel = CancellationToken.None;
var late = await new ThdStart().ForPlayAsync(token, cancel =>
{
    hungCancel = cancel;
    return hung.Task;
}, TimeSpan.FromMilliseconds(200));
Require(late.State == OnlineStatus.SignIn.Unknown && hungCancel.IsCancellationRequested, "a check that does not answer in time reads as unknown and is cancelled");
var thrown = await new ThdStart().ForPlayAsync(token, _ => throw new HttpRequestException("down"), ThdStart.Timeout);
Require(thrown.State == OnlineStatus.SignIn.Unknown, "a check that fails at once reads as unknown");
var faulted = await new ThdStart().ForPlayAsync(token, _ => Task.FromException<OnlineStatus.SignInState>(new HttpRequestException("down")), ThdStart.Timeout);
Require(faulted.State == OnlineStatus.SignIn.Unknown, "a check that fails later reads as unknown");

var valid = new OnlineStatus.SignInState(OnlineStatus.SignIn.Valid, "TestHunter");
var unknown = new OnlineStatus.SignInState(OnlineStatus.SignIn.Unknown, "");
Require(ThdStart.Refusal(valid, "expired") == "", "a yes clears an earlier refusal");
Require(ThdStart.Refusal(OnlineStatus.ClassifyWhoAmI("{\"ResKind\":1,\"Message\":\"expired\"}"), "") == "expired", "an expired sign-in is a refusal with the server's words");
Require(ThdStart.Refusal(new OnlineStatus.SignInState(OnlineStatus.SignIn.Expired, ""), "") == "refused", "a refusal without words still counts");
Require(ThdStart.Refusal(unknown, "") == "", "a network failure does not refuse");
Require(ThdStart.Refusal(unknown, "expired") == "expired", "a network failure keeps an earlier refusal");

string Env(IEnumerable<(string Name, string Value)> list) => string.Join(" ", list.Select(v => v.Name + "=" + v.Value));
Require(Env(ThdStart.Account(token, "TestHunter", "")) == "BB_THD_TOKEN=" + token + " BB_THD_ONLINE_ID=TestHunter BB_THD_REFUSED=", "a signed-in start passes the account and clears an inherited refusal");
Require(Env(ThdStart.Account(token, "TestHunter", "expired")) == "BB_THD_TOKEN=" + token + " BB_THD_ONLINE_ID=TestHunter BB_THD_REFUSED=expired", "a refused start tells the engine, which then starts signed out");
Require(Env(ThdStart.Account("", "TestHunter", "expired")) == "BB_THD_REFUSED=" && Env(ThdStart.Account(token, "", "")) == "BB_THD_REFUSED=", "no account: no token passed, an inherited refusal cleared");

string Get(Dictionary<string, string> ini, string key, string fallback) => ini.TryGetValue(key, out var v) ? v : fallback;
var online = new Dictionary<string, string> { ["online_enabled"] = "1", ["online_pool"] = "huntersdream" };
Require(ThdStart.GoesOnline((k, f) => Get(online, k, f)), "online with The Hunter's Dream checks the sign-in at Play");
foreach (var (key, value) in new[] { ("online_enabled", "0"), ("online_pool", ""), ("online_connected", "0"), ("seamless_enabled", "1") })
{
    var other = new Dictionary<string, string>(online) { [key] = value };
    Require(!ThdStart.GoesOnline((k, f) => Get(other, k, f)), $"{key}={value}: the engine does not go to The Hunter's Dream, so no check");
}
foreach (var failure in failures)
    Console.WriteLine("FAIL " + failure);
if (failures.Count > 0)
    return 1;

if (state.State != OnlineStatus.SignIn.Unknown)
{
    Console.WriteLine("FAIL the refused sign-in check did not read as unknown");
    return 1;
}
if (record)
{
    File.WriteAllText(golden, text.ToString());
    Console.WriteLine("thd-request-checks: recorded " + golden);
    return 0;
}
var expected = File.Exists(golden) ? File.ReadAllText(golden).Replace("\r", "") : "";
if (expected != text.ToString())
{
    Console.WriteLine("FAIL the launcher's requests differ from " + golden);
    Console.Write(text.ToString());
    return 1;
}
Console.WriteLine("thd-request-checks: ok (requests golden; Play-time sign-in check: one whoami per start, shared with the Online page, 5 s, failures not refused; refused starts tell the engine)");
return 0;
