// SPDX-License-Identifier: GPL-3.0-or-later
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;

namespace BloodborneLauncher.Views
{
    public static class WorkWindow
    {
        public static void Run(string title, string text, IReadOnlyList<(string Text, Action Work)> steps, bool showSteps)
        {
            if (steps.Count == 0) return;
            var bright = new SolidColorBrush(Color.FromRgb(0xEE, 0xF0, 0xF6));
            var status = new TextBlock { Margin = new Thickness(0, 10, 0, 0), Opacity = 0.8, Foreground = bright, IsVisible = showSteps };
            var bar = new ProgressBar { Height = 6, MinHeight = 6, Minimum = 0, Maximum = steps.Count, Margin = new Thickness(0, 14, 0, 0) };
            var done = false;
            var frame = new DispatcherFrame();
            var window = new Window
            {
                Title = "Lantern Engine",
                Width = 440,
                SizeToContent = SizeToContent.Height,
                CanResize = false,
                WindowStartupLocation = WindowStartupLocation.CenterScreen,
                Topmost = true,
                Background = new SolidColorBrush(Color.FromRgb(0x10, 0x12, 0x1A)),
                Content = new StackPanel
                {
                    Margin = new Thickness(22),
                    Children =
                    {
                        new TextBlock { Text = title, FontSize = 16, FontWeight = FontWeight.SemiBold, Foreground = bright },
                        new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0), Foreground = bright },
                        status,
                        bar,
                    },
                },
            };
            window.Closing += (_, e) => e.Cancel = !done;
            window.Closed += (_, _) => frame.Continue = false;
            window.Opened += async (_, _) =>
            {
                for (var i = 0; i < steps.Count; ++i)
                {
                    status.Text = steps[i].Text + "…";
                    var work = steps[i].Work;
                    try { await Task.Run(work); } catch (Exception) { }
                    bar.Value = i + 1;
                }
                status.Text = "Done.";
                await Task.Delay(showSteps ? 400 : 0);
                done = true;
                window.Close();
            };
            window.Show();
            Dispatcher.UIThread.PushFrame(frame);
        }
    }
}
