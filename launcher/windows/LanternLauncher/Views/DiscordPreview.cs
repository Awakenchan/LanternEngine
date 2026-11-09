using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Immutable;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>How the Discord status will look on a profile ("Playing" card), drawn from the same
    /// <see cref="DiscordPresence.Look"/> the launcher sends.</summary>
    public sealed class DiscordPreview : Border
    {
        private static readonly IBrush Card = new ImmutableSolidColorBrush(Color.FromRgb(0x23, 0x24, 0x28));
        private static readonly IBrush Muted = new ImmutableSolidColorBrush(Color.FromRgb(0xB5, 0xBA, 0xC1));
        private static readonly IBrush Bright = new ImmutableSolidColorBrush(Color.FromRgb(0xF2, 0xF3, 0xF5));
        private static readonly IBrush Green = new ImmutableSolidColorBrush(Color.FromRgb(0x3B, 0xA5, 0x5C));
        private static readonly IBrush ButtonFill = new ImmutableSolidColorBrush(Color.FromRgb(0x4E, 0x50, 0x58));
        private static readonly Bitmap? Lantern = LoadLantern();
        private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(10) };

        private readonly Image _picture = new() { Width = 72, Height = 72, Stretch = Stretch.UniformToFill };
        private readonly Image _badge = new() { Width = 24, Height = 24, Stretch = Stretch.UniformToFill };
        private readonly Border _badgeFrame;
        private readonly TextBlock _details = Line(Bright, 13), _state = Line(Bright, 13), _timer = Line(Green, 12);
        private readonly TextBlock _name = new() { Foreground = Bright, FontWeight = FontWeight.Bold, FontSize = 15 };
        private readonly StackPanel _buttons = new() { Margin = new Thickness(0, 10, 0, 0) };

        public DiscordPreview()
        {
            Background = Card;
            CornerRadius = new CornerRadius(8);
            Padding = new Thickness(14);
            Margin = new Thickness(0, 6, 0, 10);
            MaxWidth = 420;
            HorizontalAlignment = HorizontalAlignment.Left;
            _badgeFrame = new Border
            {
                Width = 28, Height = 28, CornerRadius = new CornerRadius(14), Background = Card,
                HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom,
                Margin = new Thickness(0, 0, -6, -6),
                Child = new Border { Width = 24, Height = 24, CornerRadius = new CornerRadius(12), ClipToBounds = true, Child = _badge },
            };
            var picture = new Grid { Width = 72, Height = 72, VerticalAlignment = VerticalAlignment.Top };
            picture.Children.Add(new Border { CornerRadius = new CornerRadius(8), ClipToBounds = true, Child = _picture });
            picture.Children.Add(_badgeFrame);
            var text = new StackPanel { Margin = new Thickness(14, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            text.Children.Add(_name);
            text.Children.Add(_details);
            text.Children.Add(_state);
            text.Children.Add(_timer);
            var row = new DockPanel();
            DockPanel.SetDock(picture, Dock.Left);
            row.Children.Add(picture);
            row.Children.Add(text);
            var body = new StackPanel();
            body.Children.Add(new TextBlock { Text = L.T("Playing"), Foreground = Muted, FontSize = 12, FontWeight = FontWeight.SemiBold, Margin = new Thickness(0, 0, 0, 8) });
            body.Children.Add(row);
            body.Children.Add(_buttons);
            Child = body;
        }

        public void Show(DiscordPresence.Look? look)
        {
            IsVisible = look != null;
            if (look == null) return;
            _name.Text = look.Name;
            if (look.Image == "logo") _picture.Source = Lantern;
            else FromLink(_picture, look.Image);
            _picture.SetTip(look.ImageText);
            _badgeFrame.IsVisible = look.Badge.Length > 0;
            FromLink(_badge, look.Badge);
            _badge.SetTip(look.BadgeText);
            Set(_details, look.Details);
            Set(_state, look.State);
            _timer.Text = "▶ 1:16";
            _timer.IsVisible = look.Timer;
            _buttons.Children.Clear();
            _buttons.IsVisible = look.Buttons.Count > 0;
            foreach (var (label, url) in look.Buttons)
            {
                var button = new Border
                {
                    Background = ButtonFill, CornerRadius = new CornerRadius(4), Padding = new Thickness(0, 6, 0, 6),
                    Margin = new Thickness(0, 0, 0, 6),
                    Child = new TextBlock { Text = label, Foreground = Bright, FontSize = 13, HorizontalAlignment = HorizontalAlignment.Center },
                };
                button.SetTip(url);
                _buttons.Children.Add(button);
            }
        }

        private static void Set(TextBlock block, string text)
        {
            block.Text = text;
            block.IsVisible = text.Length >= 2;
        }

        private static TextBlock Line(IBrush brush, double size) =>
            new() { Foreground = brush, FontSize = size, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 2, 0, 0) };

        /// <summary>A picture from the web, downloaded in the background; the card fills in when it arrives.</summary>
        private static async void FromLink(Image image, string url)
        {
            image.Source = null;
            image.Tag = url;
            if (url.Length == 0 || !Uri.TryCreate(url, UriKind.Absolute, out var uri) || uri.Scheme is not ("http" or "https")) return;
            try
            {
                var bytes = await Http.GetByteArrayAsync(uri);
                var bitmap = await Task.Run(() => new Bitmap(new MemoryStream(bytes)));
                if (Equals(image.Tag, url)) image.Source = bitmap; // still the picture asked for
            }
            catch (Exception)
            {
                // Not a picture, or not reachable: the preview stays empty there, as Discord would.
            }
        }

        private static Bitmap? LoadLantern()
        {
            try
            {
                using var stream = AssetLoader.Open(new Uri("avares://LanternEngine/Assets/discord-logo.png"));
                return new Bitmap(stream);
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
