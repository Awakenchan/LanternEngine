// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.RegularExpressions;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Documents;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace BloodborneLauncher.Views
{
    public static partial class MarkdownText
    {
        [GeneratedRegex(@"\*\*(.+?)\*\*|`([^`]+)`|\[([^\]]+)\]\((https?://[^)\s]+)\)|(https?://[^\s)]+)")]
        private static partial Regex Inline();

        [GeneratedRegex(@"^(#{1,6})\s+(.*)$")]
        private static partial Regex Heading();

        [GeneratedRegex(@"^(\s*)([-*+]|\d+[.)])\s+(.*)$")]
        private static partial Regex Item();

        public static bool Looks(string text) =>
            text.Contains("**", StringComparison.Ordinal) || text.Contains("\n#", StringComparison.Ordinal) ||
            text.StartsWith('#') || text.Contains("](http", StringComparison.Ordinal);

        public static Control Build(string text, double fontSize = 13)
        {
            var panel = new StackPanel();
            var paragraph = new List<string>();

            void Flush()
            {
                if (paragraph.Count == 0) return;
                panel.Children.Add(Block(string.Join(" ", paragraph), fontSize, FontWeight.Normal, new Thickness(0, 0, 0, 8)));
                paragraph.Clear();
            }

            foreach (var raw in text.Replace("\r\n", "\n").Split('\n'))
            {
                var line = raw.TrimEnd();
                if (line.Trim().Length == 0)
                {
                    Flush();
                    continue;
                }
                if (Heading().Match(line) is { Success: true } heading)
                {
                    Flush();
                    var level = heading.Groups[1].Value.Length;
                    var size = level <= 1 ? fontSize + 4 : level == 2 ? fontSize + 2 : fontSize + 1;
                    var block = Block(heading.Groups[2].Value, size, FontWeight.SemiBold, new Thickness(0, panel.Children.Count == 0 ? 0 : 6, 0, 6));
                    block.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorPrimaryBrush");
                    panel.Children.Add(block);
                    continue;
                }
                if (Item().Match(line) is { Success: true } item)
                {
                    Flush();
                    var depth = Math.Min(3, item.Groups[1].Value.Replace("\t", "  ").Length / 2);
                    var marker = char.IsDigit(item.Groups[2].Value[0]) ? item.Groups[2].Value : "•";
                    var row = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), Margin = new Thickness(depth * 16, 0, 0, 4) };
                    var dot = new TextBlock { Text = marker, FontSize = fontSize, Margin = new Thickness(2, 0, 8, 0), VerticalAlignment = VerticalAlignment.Top };
                    dot.SetResourceReference(TextBlock.ForegroundProperty, "AccentTextFillColorPrimaryBrush");
                    var body = Block(item.Groups[3].Value, fontSize, FontWeight.Normal, new Thickness(0));
                    Grid.SetColumn(body, 1);
                    row.Children.Add(dot);
                    row.Children.Add(body);
                    panel.Children.Add(row);
                    continue;
                }
                if (line.TrimStart().StartsWith("> ", StringComparison.Ordinal))
                    line = line.TrimStart()[2..];
                paragraph.Add(line.Trim());
            }
            Flush();
            if (panel.Children.Count > 0 && panel.Children[^1] is Control last)
                last.Margin = new Thickness(last.Margin.Left, last.Margin.Top, last.Margin.Right, 0);
            return panel;
        }

        private static SelectableTextBlock Block(string text, double size, FontWeight weight, Thickness margin)
        {
            var block = new SelectableTextBlock { FontSize = size, FontWeight = weight, TextWrapping = TextWrapping.Wrap, Margin = margin };
            block.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorSecondaryBrush");
            var inlines = new InlineCollection();
            var at = 0;
            foreach (Match match in Inline().Matches(text))
            {
                if (match.Index > at) inlines.Add(new Run(text[at..match.Index]));
                if (match.Groups[1].Success)
                {
                    var bold = new Run(match.Groups[1].Value) { FontWeight = FontWeight.SemiBold };
                    bold.Foreground = LauncherTheme.Brush("TextFillColorPrimaryBrush");
                    inlines.Add(bold);
                }
                else if (match.Groups[2].Success)
                {
                    var code = new Run(match.Groups[2].Value);
                    if (Application.Current!.TryGetResource("MonoFont", Application.Current.ActualThemeVariant, out var mono) && mono is FontFamily family)
                        code.FontFamily = family;
                    code.Foreground = LauncherTheme.Brush("TextFillColorPrimaryBrush");
                    inlines.Add(code);
                }
                else
                {
                    var label = match.Groups[3].Success ? match.Groups[3].Value : match.Groups[5].Value;
                    var url = match.Groups[4].Success ? match.Groups[4].Value : match.Groups[5].Value;
                    inlines.Add(new InlineUIContainer(Link(label, url, size)) { BaselineAlignment = BaselineAlignment.TextBottom });
                }
                at = match.Index + match.Length;
            }
            if (at < text.Length) inlines.Add(new Run(text[at..]));
            block.Inlines = inlines;
            return block;
        }

        private static TextBlock Link(string label, string url, double size)
        {
            var link = new TextBlock
            {
                Text = label,
                FontSize = size,
                TextDecorations = TextDecorations.Underline,
                Cursor = new Cursor(StandardCursorType.Hand),
            };
            link.SetResourceReference(TextBlock.ForegroundProperty, "AccentTextFillColorPrimaryBrush");
            link.SetTip(url);
            link.PointerReleased += (_, e) =>
            {
                if (e.InitialPressMouseButton != MouseButton.Left) return;
                Ui.OpenLink(url);
                e.Handled = true;
            };
            return link;
        }
    }
}
