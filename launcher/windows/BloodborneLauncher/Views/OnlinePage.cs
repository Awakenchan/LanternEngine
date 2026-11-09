using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;

namespace BloodborneLauncher.Views
{
    public sealed class OnlinePage : LauncherPage
    {
        private readonly ToggleSwitch _online;
        private readonly StackPanel _results = new();
        private readonly Button _test;

        public OnlinePage() : base(L.T("Online"),
            L.T("1. Turn on Play online. 2. Connect your Hunter's Dream account below (with Discord, or without it). ") +
            L.T("3. In the game, choose online at the title screen: The Hunter's Dream's welcome message means you are online. ") +
            L.T("Everyone needs game version 1.09."))
        {
            SuperMedo.Apply(Ini, S);

            if (HuntersDream.OnlinePaused)
                Body.Children.Add(Text(L.T("Online play is paused in this version. The game starts offline whatever is set here; ") +
                                       L.T("your settings are kept for when it returns."), "Faint"));

            Group(L.T("Play"));
            _online = ToggleRow(SymbolRegular.Globe24, L.T("Play online"), L.T("Off: the game stays offline"),
                () => Ini.Get("online_enabled") == "1", v => Ini.Set("online_enabled", v ? "1" : "0"));

            Group(L.T("Seamless co-op"));
            Body.Children.Add(Text(L.T("Seamless co-op is still being worked on: expect bugs, and report them in #issues on our Discord."), "Faint"));
            ToggleRow(SymbolRegular.PeopleTeam24, L.T("Seamless co-op"),
                L.T("Play the campaign together in your own encrypted room: no Hunter's Dream, no ports to open"),
                () => Ini.Get("seamless_enabled") == "1", v =>
                {
                    Ini.Set("seamless_enabled", v ? "1" : "0");
                    if (!v || Ini.Get("online_enabled") == "1") return;
                    Ini.Set("online_enabled", "1");
                    _online.IsChecked = true;
                });
            _role = ComboRow(SymbolRegular.Crown24, L.T("Role"), L.T("One player hosts, the others join the host's world"),
                Choices.SeamlessRoles, () => Ini.Get("seamless_role", "guest"), v => { Ini.Set("seamless_role", v); ShowRoom(); });
            _connection = ComboRow(SymbolRegular.PlugConnected24, L.T("Connection"),
                L.T("Server: Lantern's co-op server, nothing to set up. Direct P2P: no server, guests connect straight to the host's PC for the lowest latency; they need to reach it over the same network, a VPN such as Tailscale or Radmin, or a forwarded port"),
                Choices.SeamlessConnections, () => Ini.Get("seamless_direct") == "1" ? "direct" : "server",
                v => { Ini.Set("seamless_direct", v == "direct" ? "1" : "0"); ShowRoom(); });
            _directAddress = RowOf(TextRow(SymbolRegular.Globe24, L.T("Your address for guests"),
                L.T("What guests connect to, written into your invite code: your VPN or public address. Empty: your address on this network"),
                L.T("This network"), () => Ini.Get("seamless_direct_address"), v => Ini.Set("seamless_direct_address", v.Trim()), width: 220));
            _directPort = RowOf(TextRow(SymbolRegular.Router24, L.T("Direct port"),
                L.T("TCP port of your direct room. Forward this one if guests come over the internet, and allow Lantern Engine if Windows asks"),
                DirectPort, () => Ini.Get("seamless_direct_port"), v => Ini.Set("seamless_direct_port", v.Trim()), width: 120));
            _directHost = RowOf(TextRow(SymbolRegular.Globe24, L.T("Host's address"),
                L.T("The host's address, or address:port. The invite code fills this in"),
                L.T("From the invite code"), () => Ini.Get("seamless_direct_host"), v => Ini.Set("seamless_direct_host", v.Trim()), width: 220));
            IniText(SymbolRegular.Person24, L.T("Plate name"), L.T("Your name on the plate above your hunter and in the party list (up to 16 characters, spaces and symbols allowed)"), "Hunter", "seamless_player", 220);
            IniText(SymbolRegular.Crown24, L.T("Host title"),
                L.T("The line above the host's name on plates in your game. Empty: only the name. The game's own is Beckoner"),
                L.T("Name only"), "plate_host_title", 220);
            IniText(SymbolRegular.Tag24, L.T("Guest title"),
                L.T("The line above each guest's name on plates in your game. Empty: only the name. The game's own is Cooperator"),
                L.T("Name only"), "plate_guest_title", 220);
            var invite = new StackPanel { Orientation = Orientation.Horizontal };
            _copy = SmallButton(L.T("Copy invite code"), SymbolRegular.Copy24, (_, _) => CopyInvite());
            invite.Children.Add(_copy);
            _paste = SmallButton(L.T("Paste invite code"), SymbolRegular.ClipboardPaste24, (_, _) => PasteInvite());
            invite.Children.Add(_paste);
            Add(Card(SymbolRegular.Link24, L.T("Invite code"), null, invite, out _roomHint));
            _newRoom = Add(Card(SymbolRegular.ArrowSync24, L.T("New room"),
                L.T("Makes a new room with a new invite code; the old code stops working"),
                SmallButton(L.T("New room"), SymbolRegular.ArrowSync24, (_, _) => NewRoom()), out _));
            _openRoom = RowOf(IniToggle(SymbolRegular.PeopleCommunity24, L.T("Open my room to visitors"),
                L.T("While on, your game gives your room's invite code to Lantern's co-op server, and the Lantern event team can see it in Discord and join for surprise events, so your room is not private from them. Everyone in the room sees a line when a visitor joins, and you can Kick them. Nobody is moved: guests keep control of their own summons. Off or leaving takes the room off the list at once; New room retires a code they already saw"),
                "coop_open_room"));
            EnsureHostRoom();
            ShowRoom();

            Group(L.T("Server"));
            ComboRow(SymbolRegular.Server24, L.T("Server"), L.T("Messages, bloodstains, ghosts, co-op and invasions"),
                Choices.OnlineServers, () => SuperMedo.Selected(Ini) ? SuperMedo.Id : "huntersdream", v =>
                {
                    Ini.Set("online_server", v == SuperMedo.Id ? SuperMedo.Id : "");
                    SuperMedo.Apply(Ini, S);
                    Ws.SaveNow();
                    ShowServer();
                });
            var hdFrom = Body.Children.Count;
            Add(Card(SymbolRegular.Globe24, L.T("The Hunter's Dream"), L.T("Players online, the map and your hunter's profile"),
                SmallButton(L.T("Open website"), SymbolRegular.Open24, (_, _) =>
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(LauncherSettings.OnlineCommunity) { UseShellExecute = true })),
                out _));

            Group(L.T("The Hunter's Dream account"));
            _hdButton = SmallButton(L.T("Connect"), SymbolRegular.Person24, async (_, _) => await LinkAsync());
            Add(Card(SymbolRegular.PersonAccounts24, L.T("Hunter's Dream account"), null, _hdButton, out _hdHint));
            ShowLink();
            for (var i = hdFrom; i < Body.Children.Count; i++) _hdOnly.Add(Body.Children[i]);

            var smFrom = Body.Children.Count;
            Group(L.T("SuperMedo account"));
            Body.Children.Add(Text(L.T("The name you registered on SuperMedo (up to 16 characters), not an email address. ") +
                                   L.T("The password is kept encrypted for your Windows user, not in bbport.ini."), "Faint"));
            Add(Card(SymbolRegular.PersonAdd24, L.T("No SuperMedo account yet?"), L.T("Make a free one on its website, then enter it below"),
                SmallButton(L.T("Create account"), SymbolRegular.Open24, (_, _) =>
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(SuperMedo.Register) { UseShellExecute = true })),
                out _));
            IniText(SymbolRegular.Person24, L.T("Account name"), null, "", "online_username", 220);
            var password = new Wpf.Ui.Controls.PasswordBox { Width = 220, Password = S.OnlinePassword };
            password.PasswordChanged += (_, _) =>
            {
                S.OnlinePassword = password.Password;
                Ws.SaveSoon();
            };
            Add(Card(SymbolRegular.Password24, L.T("Password"), null, password, out _));
            for (var i = smFrom; i < Body.Children.Count; i++) _smOnly.Add(Body.Children[i]);

            Group(L.T("Connection"));
            IniText(SymbolRegular.Router24, L.T("Local P2P port"), L.T("Empty: 3658, the PS4's"), "3658", "online_p2p_port", 120);
            ComboRow(SymbolRegular.Timer24, L.T("Summon sign timeout"), L.T("How long a sign you touched waits for its host before giving up"),
                Choices.SignTimeout, () => Ini.Get("online_sign_timeout", "30"), v => Ini.Set("online_sign_timeout", v));
            _test = SmallButton(L.T("Test"), SymbolRegular.PlayCircle24, async (_, _) => await TestAsync());
            Add(Card(SymbolRegular.Pulse24, L.T("Test the connection"), L.T("Checks The Hunter's Dream: game server, co-op sessions and the relay summons use"), _test, out _));
            Body.Children.Add(_results);
            ShowServer();
            Loaded += (_, _) => ShowServer();
        }

        private readonly List<UIElement> _hdOnly = new();
        private readonly List<UIElement> _smOnly = new();

        private void ShowServer()
        {
            var medo = SuperMedo.Selected(Ini);
            foreach (var element in _hdOnly) element.Visibility = medo ? Visibility.Collapsed : Visibility.Visible;
            foreach (var element in _smOnly) element.Visibility = medo ? Visibility.Visible : Visibility.Collapsed;
            foreach (var frame in Body.Children.OfType<Border>())
            {
                if (frame.Child is Panel box && box.Children.Count > 0)
                    frame.Visibility = box.Children.Cast<UIElement>().Any(c => c.Visibility == Visibility.Visible)
                        ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        private readonly System.Windows.Controls.TextBlock _roomHint;
        private readonly FrameworkElement _role;
        private readonly Button _copy;
        private readonly Button _paste;
        private readonly SettingRow _newRoom;
        private readonly SettingRow _openRoom;
        private readonly FrameworkElement _connection;
        private readonly SettingRow _directAddress;
        private readonly SettingRow _directPort;
        private readonly SettingRow _directHost;
        private const string InvitePrefix = "LANTERN-ROOM:";
        private const string VisitorPrefix = "LANTERN-VISIT:";

        private static string RandomText(int length)
        {
            const string letters = "abcdefghjkmnpqrstuvwxyz23456789";
            var bytes = System.Security.Cryptography.RandomNumberGenerator.GetBytes(length);
            return new string(bytes.Select(b => letters[b % letters.Length]).ToArray());
        }

        private const string DirectPort = "38765";

        private SettingRow RowOf(FrameworkElement _) => LastRow!;

        private void SelectConnection(int index)
        {
            if (_connection is Segmented segmented) segmented.Select(index, notify: false);
            else if (_connection is System.Windows.Controls.ComboBox box) box.SelectedIndex = index;
        }

        private static string NetworkAddress()
        {
            foreach (var card in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
            {
                if (card.OperationalStatus != System.Net.NetworkInformation.OperationalStatus.Up ||
                    card.NetworkInterfaceType == System.Net.NetworkInformation.NetworkInterfaceType.Loopback ||
                    card.GetIPProperties().GatewayAddresses.Count == 0)
                    continue;
                foreach (var entry in card.GetIPProperties().UnicastAddresses)
                {
                    if (entry.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork) return entry.Address.ToString();
                }
            }
            return "";
        }

        private void EnsureHostRoom()
        {
            if (Ini.Get("seamless_host_room").Length > 0 && S.SeamlessHostPassword.Length > 0) return;
            NewRoom(quiet: true);
        }

        private void NewRoom(bool quiet = false)
        {
            Ini.Set("seamless_host_room", RandomText(20));
            S.SeamlessHostPassword = RandomText(24);
            Ws.SaveNow();
            ShowRoom();
            if (!quiet) Ws.Log(L.T("New room made: copy the new invite code for your friends"), LogLevel.Good);
        }

        private void ShowRoom()
        {
            if (_roomHint is null) return;
            var host = Ini.Get("seamless_role", "guest") == "host";
            _copy.Visibility = host ? Visibility.Visible : Visibility.Collapsed;
            _paste.Visibility = host ? Visibility.Collapsed : Visibility.Visible;
            _newRoom.Visibility = host ? Visibility.Visible : Visibility.Collapsed;
            var direct = Ini.Get("seamless_direct") == "1";
            _openRoom.Visibility = host && !direct && Ini.Get("seamless_server").Length == 0 ? Visibility.Visible : Visibility.Collapsed;
            _directAddress.Visibility = host && direct ? Visibility.Visible : Visibility.Collapsed;
            _directPort.Visibility = host && direct ? Visibility.Visible : Visibility.Collapsed;
            _directHost.Visibility = !host && direct ? Visibility.Visible : Visibility.Collapsed;
            SetHint(_roomHint, host
                ? L.T("You host your own room: copy the code and send it to friends")
                : Ini.Get("seamless_room").Length > 0
                    ? L.T("Joining the room of the invite code you pasted")
                    : L.T("Paste the invite code the host sent you"));
        }

        private void CopyInvite()
        {
            EnsureHostRoom();
            var parts = new List<string> { Ini.Get("seamless_host_room"), S.SeamlessHostPassword };
            if (Ini.Get("seamless_direct") == "1")
            {
                var address = Ini.Get("seamless_direct_address").Trim();
                if (address.Length == 0) address = NetworkAddress();
                if (address.Length == 0)
                {
                    Prompt.Tell(L.T("Invite code"), L.T("No network address found on this PC. Type your VPN or public address under Direct connection."));
                    return;
                }
                var port = Ini.Get("seamless_direct_port").Trim();
                parts.Add("ws://" + address + ":" + (port.Length > 0 ? port : DirectPort));
            }
            else if (Ini.Get("seamless_server").Length > 0 && !Ini.Get("seamless_server").StartsWith("ws://", StringComparison.OrdinalIgnoreCase))
            {
                parts.Add(Ini.Get("seamless_server"));
            }
            var code = InvitePrefix + Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(string.Join('\n', parts)))
                .TrimEnd('=').Replace('+', '-').Replace('/', '_');
            Ini.Set("seamless_role", "host");
            if (_role is Segmented segmented) segmented.Select(1, notify: false);
            else if (_role is System.Windows.Controls.ComboBox box) box.SelectedIndex = 1;
            Ws.SaveNow();
            ShowRoom();
            if (ClipboardText.Set(code)) Ws.Log(L.T("Invite code copied: send it to your friends"), LogLevel.Good);
        }

        private void PasteInvite()
        {
            var text = System.Windows.Clipboard.ContainsText() ? System.Windows.Clipboard.GetText().Trim() : "";
            var visitorAt = text.IndexOf(VisitorPrefix, StringComparison.OrdinalIgnoreCase);
            var visitor = visitorAt >= 0;
            var start = visitor ? visitorAt + VisitorPrefix.Length : text.IndexOf(InvitePrefix, StringComparison.OrdinalIgnoreCase);
            if (!visitor && start >= 0) start += InvitePrefix.Length;
            string[] parts = Array.Empty<string>();
            if (start >= 0)
            {
                var body = new string(text[start..].TakeWhile(c => char.IsLetterOrDigit(c) || c == '-' || c == '_').ToArray())
                    .Replace('-', '+').Replace('_', '/');
                body = body.PadRight(body.Length + (4 - body.Length % 4) % 4, '=');
                try { parts = System.Text.Encoding.UTF8.GetString(Convert.FromBase64String(body)).Split('\n'); }
                catch (FormatException) { }
            }
            if (parts.Length < 2 || parts[0].Length == 0)
            {
                Prompt.Tell(L.T("Invite code"), L.T("The clipboard holds no invite code. Copy the whole code the host sent you, then paste again."));
                return;
            }
            if (parts[0] == Ini.Get("seamless_host_room"))
            {
                Prompt.Tell(L.T("Invite code"), L.T("That is your own room's code. Friends paste it on their PC to join you."));
                return;
            }
            Ini.Set("seamless_room", parts[0]);
            Ini.Set("seamless_visitor", visitor ? "1" : "0");
            S.SeamlessPassword = parts[1];
            var direct = parts.Length > 2 && parts[2].StartsWith("ws://", StringComparison.OrdinalIgnoreCase);
            Ini.Set("seamless_server", parts.Length > 2 && !direct ? parts[2] : "");
            Ini.Set("seamless_direct", direct ? "1" : "0");
            Ini.Set("seamless_direct_host", direct ? parts[2][5..] : "");
            SelectConnection(direct ? 1 : 0);
            Ini.Set("seamless_role", "guest");
            if (_role is Segmented segmented) segmented.Select(0, notify: false);
            else if (_role is System.Windows.Controls.ComboBox box) box.SelectedIndex = 0;
            Ws.SaveNow();
            ShowRoom();
            Ws.Log(visitor ? L.T("Visitor invite pasted: you join that room, and everyone there is told you are a visitor") : L.T("Invite code pasted: you join that room"), LogLevel.Good);
        }

        private readonly Button _hdButton;
        private readonly System.Windows.Controls.TextBlock _hdHint;
        private CancellationTokenSource? _linking;

        private void ShowLink()
        {
            var name = S.Str("hd_name");
            var linked = name.Length > 0 && S.HuntersDreamToken.Length > 0;
            var refused = linked ? S.HuntersDreamRefused : "";
            SetHint(_hdHint, !linked ? L.T("Not connected. Connect with Discord, or create an account here without it")
                : refused.Length > 0 ? L.F($"The Hunter's Dream refused the sign-in as {name} ({refused}). Sign in again: until then nobody can summon or invite you")
                : L.F($"Connected as {name}"));
            _hdHint.SetResourceReference(System.Windows.Controls.TextBlock.ForegroundProperty,
                refused.Length > 0 ? "WarningTextBrush" : "TextFillColorTertiaryBrush");
            if (_linking is null) _hdButton.Content = !linked ? L.T("Connect") : refused.Length > 0 ? L.T("Sign in again") : L.T("Disconnect");
        }

        private async Task AccountWithoutDiscordAsync(bool recover)
        {
            var fields = recover
                ? new[] { new Prompt.Field(L.T("Account name")), new Prompt.Field(L.T("Recovery code")) }
                : new[] { new Prompt.Field(L.T("Account name"), "", L.T("Your online name (letters, digits, - and _)")) };
            var answer = Prompt.Ask(recover ? L.T("Recover the account") : L.T("Create an account"),
                recover ? L.T("The name and the recovery code you wrote down. Other PCs signed in to this account are signed out.")
                        : L.T("An account held by this PC. You get a recovery code: write it down, it is the only way back in on another PC or after signing out."),
                fields);
            if (answer is null || answer[0].Trim().Length == 0) return;
            try
            {
                var (name, token, code) = recover
                    ? await HuntersDream.RecoverAsync(answer[0].Trim(), answer[1].Trim(), CancellationToken.None)
                    : await HuntersDream.CreateAccountAsync(answer[0].Trim(), CancellationToken.None);
                S.HuntersDreamToken = token;
                S.Set("hd_name", name);
                Ini.Set("online_pool", "huntersdream");
                Ws.SaveNow();
                Ws.Log(L.F($"The Hunter's Dream: signed in as {name}"), LogLevel.Good);
                if (code.Length > 0)
                {
                    ClipboardText.Set(code);
                    Prompt.Tell(L.T("Your recovery code"), L.F($"Signed in as {name}.\n\nRecovery code (copied to the clipboard):\n\n{code}\n\n") +
                        L.T("Write it down now: it is shown only once, and it is the only way back into this account."));
                }
            }
            catch (Exception e)
            {
                Prompt.Tell(recover ? L.T("Recovering failed") : L.T("Creating the account failed"), e.Message);
            }
            finally
            {
                ShowLink();
            }
        }

        private async Task LinkAsync()
        {
            if (_linking is { } running)
            {
                running.Cancel();
                return;
            }
            if (S.HuntersDreamToken is { Length: > 0 } token && S.HuntersDreamRefused.Length == 0)
            {
                await HuntersDream.LogoutAsync(token);
                S.HuntersDreamToken = "";
                S.Set("hd_name", "");
                Ini.Set("online_pool", "");
                Ws.SaveNow();
                Ws.Log(L.T("The Hunter's Dream: disconnected"), LogLevel.Info);
                ShowLink();
                return;
            }
            var how = Prompt.Choose(L.T("The Hunter's Dream account"),
                L.T("Sign in with Discord, or use an account held by this PC (a name and a recovery code, no Discord needed)."),
                L.T("Cancel"), "Discord", L.T("Without Discord…"), L.T("Recover with a code…"));
            if (how <= 0) return;
            if (how >= 2)
            {
                await AccountWithoutDiscordAsync(recover: how == 3);
                return;
            }
            _linking = new CancellationTokenSource();
            _hdButton.Content = L.T("Cancel");
            try
            {
                var link = await HuntersDream.StartLinkAsync(_linking.Token);
                ClipboardText.Set(link.UserCode);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(link.Page) { UseShellExecute = true });
                SetHint(_hdHint, L.F($"Enter the code {link.UserCode} on the page that opened (it is on the clipboard), then approve"));
                _hdHint.SetResourceReference(System.Windows.Controls.TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
                var (name, linked) = await HuntersDream.WaitLinkAsync(link, _linking.Token);
                S.HuntersDreamToken = linked;
                S.Set("hd_name", name);
                Ini.Set("online_pool", "huntersdream");
                Ws.SaveNow();
                Ws.Log(L.F($"The Hunter's Dream: connected as {name}"), LogLevel.Good);
            }
            catch (OperationCanceledException) { }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Connecting failed"), e.Message);
            }
            finally
            {
                _linking.Dispose();
                _linking = null;
                ShowLink();
            }
        }

        private void IniText(SymbolRegular icon, string title, string? hint, string placeholder, string key, double width = 300) =>
            TextRow(icon, title, hint, placeholder, () => Ini.Get(key), v => Ini.Set(key, v.Trim()), width: width);

        public override void OnShown()
        {
            _online.IsChecked = Ini.Get("online_enabled") == "1";
            if (_linking is null) ShowLink();
            ShowRoom();
        }

        private async Task TestAsync()
        {
            _results.Children.Clear();
            _test.IsEnabled = false;
            _test.Content = L.T("Testing…");
            try
            {
                var results = SuperMedo.Selected(Ini) ? await SuperMedo.CheckAsync() : await Online.CheckAsync(Online.GameServer(Ini.Get("online_game_api")));
                foreach (var r in results)
                {
                    var said = r.Ok ? (r.Detail.Length > 0 ? L.F($"Reachable: {r.Detail}") : L.T("Reachable")) : L.F($"Not reachable ({r.Detail})");
                    var card = Card(r.Ok ? SymbolRegular.CheckmarkCircle24 : SymbolRegular.ErrorCircle24,
                        $"{r.Name}: {r.Address}", said, null, out _);
                    card.ShowIcon = true;
                    card.Icon.SetResourceReference(ForegroundProperty, r.Ok ? "GoodTextBrush" : "DangerTextBrush");
                    SetHint(card.Hint, said);
                    card.Margin = new Thickness(24, 0, 0, 4);
                    _results.Children.Add(card);
                    Ws.Log($"{r.Name} {r.Address}: {(r.Ok ? "reachable" : "not reachable " + r.Detail)}", r.Ok ? LogLevel.Good : LogLevel.Warn);
                }
            }
            finally
            {
                _test.IsEnabled = true;
                _test.Content = L.T("Test");
            }
        }
    }
}
