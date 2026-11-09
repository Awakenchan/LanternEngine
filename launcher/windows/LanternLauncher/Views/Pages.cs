// SPDX-License-Identifier: GPL-3.0-or-later
namespace BloodborneLauncher.Views
{
    public static class Pages
    {
        public static LauncherPage Create(Type type) => type.Name switch
        {
            nameof(PlayPage) => new PlayPage(),
            nameof(SavesPage) => new SavesPage(),
            nameof(DisplayPage) => new DisplayPage(),
            nameof(GraphicsPage) => new GraphicsPage(),
            nameof(ControlsPage) => new ControlsPage(),
            nameof(LooksPage) => new LooksPage(),
            nameof(TrophiesPage) => new TrophiesPage(),
            nameof(OnlinePage) => new OnlinePage(),
            nameof(SettingsPage) => new SettingsPage(),
            nameof(MorePage) => new MorePage(),
            nameof(ModsPage) => new ModsPage(),
            nameof(CheatsPage) => new CheatsPage(),
            nameof(SaveEditorPage) => new SaveEditorPage(),
            nameof(EnginePage) => new EnginePage(),
            nameof(OutputPage) => new OutputPage(),
            _ => throw new ArgumentException(type.Name),
        };
    }
}
