// SPDX-License-Identifier: GPL-3.0-or-later
namespace BloodborneLauncher.Core
{
    public sealed class ThdStart
    {
        public static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

        public static ThdStart Current { get; } = new();

        private readonly object _gate = new();
        private Task<OnlineStatus.SignInState>? _unused;
        private string _unusedToken = "";

        public void Checking(string token, Task<OnlineStatus.SignInState> check)
        {
            lock (_gate)
            {
                _unused = check;
                _unusedToken = token;
            }
        }

        public async Task<OnlineStatus.SignInState> ForPlayAsync(string token, Func<CancellationToken, Task<OnlineStatus.SignInState>> whoami, TimeSpan timeout)
        {
            Task<OnlineStatus.SignInState>? check;
            lock (_gate)
            {
                check = _unusedToken == token ? _unused : null;
                _unused = null;
                _unusedToken = "";
            }
            var unknown = new OnlineStatus.SignInState(OnlineStatus.SignIn.Unknown, "");
            using var cancel = new CancellationTokenSource();
            try { check ??= whoami(cancel.Token); }
            catch (Exception) { return unknown; }
            var done = await Task.WhenAny(check, Task.Delay(timeout, CancellationToken.None));
            if (done == check && check.Status == TaskStatus.RanToCompletion)
                return check.Result;
            cancel.Cancel();
            _ = check.ContinueWith(t => t.Exception, TaskContinuationOptions.OnlyOnFaulted);
            return unknown;
        }

        public static bool GoesOnline(Func<string, string, string> ini) =>
            ini("online_enabled", "") == "1" && ini("seamless_enabled", "") != "1" &&
            ini("online_pool", "") == "huntersdream" && ini("online_connected", "1") == "1";

        public static string Refusal(OnlineStatus.SignInState state, string refused) => state.State switch
        {
            OnlineStatus.SignIn.Valid => "",
            OnlineStatus.SignIn.Expired => state.Detail.Length > 0 ? state.Detail : "refused",
            _ => refused,
        };

        public static IEnumerable<(string Name, string Value)> Account(string token, string name, string refused)
        {
            var account = token.Length > 0 && name.Length > 0;
            if (account)
            {
                yield return ("BB_THD_TOKEN", token);
                yield return ("BB_THD_ONLINE_ID", name);
            }
            yield return ("BB_THD_REFUSED", account ? refused : "");
        }
    }
}
