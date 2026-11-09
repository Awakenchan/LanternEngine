using System.IO;
using Avalonia.Controls;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    public sealed class LooksPage : LauncherPage
    {
        private readonly StackPanel _back = new();
        private readonly StackPanel _saved = new();
        private readonly List<(Looks.Look Look, TextBlock Hint, string Text, AppButton Use)> _hints = new();

        public LooksPage() : base(L.T("Looks"), L.T("Lighting and picture settings: pick a preset, or share your own with others"))
        {
            Body.Children.Add(_back);
            Group(L.T("Presets"));
            foreach (var look in Looks.Presets)
            {
                var use = UseButton(look);
                AddLook(look, Body, use, Row(use));
            }
            Group(L.T("Saved looks"));
            Body.Children.Add(_saved);
            Group(L.T("Share"));
            Add(Card(SymbolRegular.Save24, L.T("Save my current picture"), L.T("Keeps your lighting and picture settings as a look you can export"),
                Row(SmallButton(L.T("Save"), SymbolRegular.Save24, (_, _) => SaveCurrent())), out _));
            Add(Card(SymbolRegular.ArrowImport24, L.T("Import a look"), L.T("Someone's code or file. Only lighting and picture values are read; nothing is applied until you press Use"),
                Row(SmallButton(L.T("Paste code"), SymbolRegular.ClipboardPaste24, (_, _) => ImportCode()),
                    SmallButton(L.T("Open file"), SymbolRegular.FolderOpen24, (_, _) => ImportFile())), out _));
            Refresh();
        }

        public override void OnShown() => Refresh();

        private AppButton UseButton(Looks.Look look) => SmallButton(L.T("Use"), SymbolRegular.Play24, (_, _) => Use(look));

        private void AddLook(Looks.Look look, Panel into, AppButton use, object control)
        {
            var row = Add(Card(SymbolRegular.Sparkle24, look.Name, null, control, out var hint), into);
            var text = look.BuiltIn ? look.Description
                : look.Author.Length > 0 ? L.F($"By {look.Author}") : L.T("Author not given");
            _hints.Add((look, hint, text, use));
            SetHint(hint, text);
        }

        private void Refresh()
        {
            _back.Children.Clear();
            if (Looks.HasPrevious)
                Add(Card(SymbolRegular.ArrowUndo24, L.T("Back to my settings"), L.T("Puts back the values you had before the first look you used"),
                    Row(SmallButton(L.T("Go back"), SymbolRegular.ArrowUndo24, (_, _) => GoBack())), out _), _back);
            _saved.Children.Clear();
            _hints.RemoveAll(h => !h.Look.BuiltIn);
            var saved = Looks.Saved();
            if (saved.Count == 0)
                _saved.Children.Add(Text(L.T("None yet. Save your current picture or import someone's look below."), "Faint"));
            foreach (var look in saved)
            {
                var use = UseButton(look);
                AddLook(look, _saved, use, Row(
                    use,
                    SmallButton(L.T("Copy code"), SymbolRegular.Copy24, (_, _) => CopyCode(look)),
                    SmallButton(L.T("Save as file"), SymbolRegular.Save24, (_, _) => Export(look)),
                    SmallButton(L.T("Delete"), SymbolRegular.Delete24, (_, _) => Delete(look))));
            }
            foreach (var (look, hint, text, use) in _hints)
            {
                var current = Looks.IsCurrent(look, Ini);
                SetHint(hint, current ? L.T("In use. ") + text : text);
                use.Text = current ? L.T("Selected") : L.T("Use");
                use.Icon = Icon(current ? SymbolRegular.Checkmark24 : SymbolRegular.Play24);
                use.Appearance = current ? ControlAppearance.Primary : ControlAppearance.Secondary;
                use.IsEnabled = !current;
            }
        }

        private bool GameClosed()
        {
            if (!Ws.Game.Running) return true;
            Prompt.Tell(L.T("Close the game first"), L.T("The game changes these settings too while it runs."));
            return false;
        }

        private void Use(Looks.Look look)
        {
            if (!GameClosed()) return;
            try
            {
                Looks.Apply(look, Ini);
                Ws.Log(L.F($"Look in use: {look.Name}. It takes effect the next time the game starts."), LogLevel.Good);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Prompt.Tell(L.T("Could not use the look"), e.Message);
                return;
            }
            MainWindow.Instance?.ReloadPages();
        }

        private void GoBack()
        {
            if (!GameClosed()) return;
            try
            {
                Looks.GoBack(Ini);
                Ws.Log(L.T("Your previous lighting and picture settings are back."), LogLevel.Good);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                Prompt.Tell(L.T("Could not use the look"), e.Message);
                return;
            }
            MainWindow.Instance?.ReloadPages();
        }

        private void SaveCurrent()
        {
            var values = Prompt.Ask(L.T("Save my current picture"), null,
                new Prompt.Field(L.T("Name"), "", L.T("My look")),
                new Prompt.Field(L.T("Your name (optional)")));
            if (values == null || values[0].Trim().Length == 0) return;
            Keep(Looks.FromCurrent(Ini, values[0], values[1]));
        }

        private void Keep(Looks.Look look)
        {
            if (Looks.IsSaved(look) && !Prompt.Confirm(L.T("Replace the look?"), L.F($"You already have a look called {look.Name}."), L.T("Replace")))
                return;
            try
            {
                Looks.Save(look);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Prompt.Tell(L.T("Could not save the look"), e.Message);
                return;
            }
            Refresh();
        }

        private void Offer(string text)
        {
            try
            {
                var look = Looks.Parse(text);
                var changed = Looks.Knobs.Count(k => Looks.Normal(k, Looks.ValueIn(look, k)) != Looks.Normal(k, Looks.Current(k, Ini)));
                if (Prompt.Confirm(L.T("Import a look"), L.F($"{look.Name}\n\nIt would change {changed} of your lighting and picture settings when you use it."), L.T("Import")))
                    Keep(look);
            }
            catch (FormatException e)
            {
                Prompt.Tell(L.T("Could not import"), e.Message);
            }
        }

        private void ImportCode()
        {
            var clipboard = Ui.ClipboardText();
            var values = Prompt.Ask(L.T("Import a look"), L.F($"Paste the code someone shared (it starts with {Looks.CodePrefix})."),
                new Prompt.Field(L.T("Code"), clipboard.Contains(Looks.CodePrefix, StringComparison.OrdinalIgnoreCase) ? clipboard.Trim() : "",
                    Looks.CodePrefix + "…", Multiline: true));
            if (values == null || values[0].Trim().Length == 0) return;
            Offer(values[0]);
        }

        private void ImportFile()
        {
            var file = Ui.OpenFile(L.T("Import a look"), L.T("Looks|*.json;*.txt|All files|*.*"));
            if (file == null) return;
            try
            {
                if (new FileInfo(file).Length > Looks.MaxBytes * 2)
                    throw new FormatException(L.T("That is far too large to be a look."));
                Offer(File.ReadAllText(file));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException)
            {
                Prompt.Tell(L.T("Could not import"), e.Message);
            }
        }

        private void CopyCode(Looks.Look look)
        {
            if (ClipboardText.Set(Looks.ToCode(look)))
                Ws.Log(L.T("Look code copied. Others use Looks › Import a look › Paste code."), LogLevel.Good);
            else
                Ws.Log(L.T("Could not copy: another program is using the clipboard. Try again, or use Save settings file."), LogLevel.Warn);
        }

        private void Export(Looks.Look look)
        {
            var file = Ui.SaveFile(L.T("Save as file"), "Lantern look.json", L.T("Looks|*.json"));
            if (file == null) return;
            try
            {
                File.WriteAllText(file, Looks.ToJson(look));
                Ws.Log(L.T("Look saved: ") + file, LogLevel.Good);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Prompt.Tell(L.T("Could not save the look"), e.Message);
            }
        }

        private void Delete(Looks.Look look)
        {
            if (!Prompt.Confirm(L.T("Delete the look?"), look.Name, L.T("Delete"))) return;
            try
            {
                Looks.Delete(look);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Prompt.Tell(L.T("Could not save the look"), e.Message);
                return;
            }
            Refresh();
        }
    }
}
