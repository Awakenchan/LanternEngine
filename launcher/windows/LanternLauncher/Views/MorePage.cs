using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>The less used pages behind one tab: mods, cheats, the engine's switches and the game's
    /// log (Online and Settings have tabs of their own).</summary>
    public sealed class MorePage : LauncherPage
    {
        // Cheats only once the engine applies them.
        private static readonly (string Key, string Label, Type Page)[] Sections = new (string, string, Type)[]
        {
            ("mods", L.T("Mods"), typeof(ModsPage)),
            ("cheats", L.T("Cheats"), typeof(CheatsPage)),
            ("saves-editor", L.T("Save editor"), typeof(SaveEditorPage)),
            ("engine", L.T("Engine"), typeof(EnginePage)),
            ("output", L.T("Log"), typeof(OutputPage)),
        }.Where(s => s.Item1 != "cheats" || CheatsPage.Supported).ToArray();

        private readonly Dictionary<Type, LauncherPage> _pages = new();
        private readonly ContentControl _frame = new() { Focusable = false, HorizontalContentAlignment = HorizontalAlignment.Stretch, VerticalContentAlignment = VerticalAlignment.Stretch };
        private readonly Segmented _tabs;
        private int _current = -1;

        public MorePage() : base(L.T("More"), "")
        {
            _tabs = new Segmented(Sections.Select(s => new Choice(s.Label, s.Key)).ToArray())
            {
                HorizontalAlignment = HorizontalAlignment.Center,
                Width = Sections.Length * 104,
                Margin = new Thickness(0, 6, 0, 2),
            };
            _tabs.Changed += Open;

            var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*") };
            root.Children.Add(_tabs);
            Grid.SetRow(_frame, 1);
            root.Children.Add(_frame);
            Content = root;
            Open(0);
        }

        public void ShowSection(string key)
        {
            var index = Array.FindIndex(Sections, s => s.Key == key);
            if (index >= 0) Open(index);
        }

        public IEnumerable<string> SectionKeys => Sections.Select(s => s.Key);

        /// <summary>The page of the section shown (Settings › Navigation keeps it when it switches).</summary>
        public Type? ShownPage => _current >= 0 ? Sections[_current].Page : null;

        /// <summary>Makes a section's page without showing it (the settings search, in the background).</summary>
        public void MakeSection(string key)
        {
            var index = Array.FindIndex(Sections, s => s.Key == key);
            if (index >= 0) PageOf(Sections[index].Page);
        }

        /// <summary>The sections whose page is made: the settings search looks through them as pages
        /// of their own (MakeSection makes the others).</summary>
        public IEnumerable<(string Key, string Label, LauncherPage Page)> MadeSectionPages() =>
            Sections.Where(s => _pages.ContainsKey(s.Page)).Select(s => (s.Key, s.Label, _pages[s.Page]));

        private LauncherPage PageOf(Type type)
        {
            if (!_pages.TryGetValue(type, out var page))
                _pages[type] = page = Pages.Create(type);
            return page;
        }

        private void Open(int index)
        {
            _tabs.Select(index, notify: false);
            var page = PageOf(Sections[index].Page);
            if (_current != index) _frame.Content = page;
            _current = index;
            page.OnShown();
        }

        public override void OnShown()
        {
            if (_current >= 0 && _pages.TryGetValue(Sections[_current].Page, out var page))
                page.OnShown();
        }
    }
}
