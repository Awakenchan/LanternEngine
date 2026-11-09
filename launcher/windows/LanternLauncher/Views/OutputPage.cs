using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Threading;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The game's own output (run.py and bb-probe), live while it runs.</summary>
    public sealed class OutputPage : LauncherPage
    {
        private readonly TextBox _output;
        private readonly ToggleSwitch _follow;

        public OutputPage() : base(L.T("Game output"), L.T("What run.py and the game print. Copy or export it when reporting a problem."))
        {
            var bar = new Grid { Margin = new Thickness(0, 18, 0, 8), ColumnDefinitions = new ColumnDefinitions("*,Auto") };

            _follow = new ToggleSwitch { IsChecked = true, VerticalAlignment = VerticalAlignment.Center };
            bar.Children.Add(Row(_follow, new TextBlock { Text = L.T("Follow new output"), Margin = new Thickness(10, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center }));

            var buttons = Row(
                SmallButton(L.T("Copy"), SymbolRegular.Copy24, (_, _) => Copy()),
                SmallButton(L.T("Export…"), SymbolRegular.Save24, (_, _) => Export()),
                SmallButton(L.T("Clear"), SymbolRegular.Broom24, (_, _) => Ws.ClearOutput()));
            Grid.SetColumn(buttons, 1);
            bar.Children.Add(buttons);
            Body.Children.Add(bar);

            SettingToggle(SymbolRegular.WindowConsole20, L.T("Show this page when the game starts"), null, "switch_to_output");

            _output = new TextBox { Height = 520, Margin = new Thickness(0, 4, 0, 0) };
            _output.Classes.Add("CodeBox");
            Body.Children.Add(_output);

            Ws.GameOutput += batch =>
            {
                if (!IsShown || MainWindow.Instance?.WindowState == WindowState.Minimized)
                {
                    _stale = true;
                    _pending.Clear();
                    return;
                }
                _pending.AddRange(batch);
            };
            Ws.OutputCleared += () =>
            {
                _output.Text = "";
                _lines = 0;
                _pending.Clear();
            };
            _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
            _timer.Tick += (_, _) => Tick();
            AttachedToVisualTree += (_, _) =>
            {
                if (_stale) Rebuild();
                _timer.Start();
            };
            DetachedFromVisualTree += (_, _) => _timer.Stop();
            Rebuild();
        }

        private const int Kept = 2000;
        private readonly List<string> _pending = new();
        private readonly DispatcherTimer _timer;
        private DateTime _flushed;
        private int _lines;
        private bool _stale;

        private void Tick()
        {
            if (!IsShown || MainWindow.Instance?.WindowState == WindowState.Minimized) return;
            if (_stale)
            {
                Rebuild();
                return;
            }
            if (_pending.Count == 0) return;
            var wait = MainWindow.Instance?.IsActive == true ? 250 : 2000;
            if ((DateTime.UtcNow - _flushed).TotalMilliseconds < wait) return;
            _flushed = DateTime.UtcNow;
            _lines += _pending.Count;
            if (_lines > Kept + 500)
            {
                Rebuild();
                return;
            }
            _output.Text += string.Join("\n", _pending) + "\n";
            _pending.Clear();
            if (_follow.IsChecked == true)
                ScrollToEnd();
        }

        private void ScrollToEnd() =>
            Dispatcher.UIThread.Post(() => _output.CaretIndex = (_output.Text ?? "").Length, DispatcherPriority.Background);

        private void Rebuild()
        {
            var all = Ws.OutputLines;
            var from = Math.Max(0, all.Count - Kept);
            _output.Text = all.Count > 0 ? string.Join("\n", all.GetRange(from, all.Count - from)) + "\n" : "";
            _lines = all.Count - from;
            _pending.Clear();
            _stale = false;
            _flushed = DateTime.UtcNow;
            if (_follow.IsChecked == true)
                ScrollToEnd();
        }

        public override void OnShown()
        {
            if (_stale) Rebuild();
            else if (_follow.IsChecked == true)
                ScrollToEnd();
        }

        private void Copy()
        {
            if (Ws.OutputLines.Count == 0) return;
            if (ClipboardText.Set(string.Join("\n", Ws.OutputLines) + "\n"))
                Ws.Log(L.T("Game output copied."), LogLevel.Good);
            else
                Ws.Log(L.T("Could not copy: another program is using the clipboard. Try again, or use Export."), LogLevel.Warn);
        }

        private void Export()
        {
            var file = Ui.SaveFile(L.T("Export the game output"), $"lantern-{DateTime.Now:yyyyMMdd_HHmmss}.log",
                L.T("Log files|*.log|Text files|*.txt|All files|*.*"));
            if (file == null) return;
            try
            {
                // The whole session (logs\last_session.log), not the last lines this page keeps: a
                // report trimmed to its end lost the start and what came before a freeze.
                if (File.Exists(GameRunner.SessionLog))
                {
                    using var from = new FileStream(GameRunner.SessionLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    using var to = File.Create(file);
                    from.CopyTo(to);
                }
                else
                    File.WriteAllText(file, string.Join("\n", Ws.OutputLines) + "\n");
                Ws.Log(L.T("Game output saved: ") + file, LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not save the output: ") + e.Message, LogLevel.Bad);
            }
        }
    }
}
