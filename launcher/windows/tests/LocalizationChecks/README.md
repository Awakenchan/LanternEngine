# Localization checks

On Windows with the .NET 8 Desktop SDK, run from the repository root:

```powershell
dotnet run --project launcher/tests/LocalizationChecks -- en
dotnet run --project launcher/tests/LocalizationChecks -- zh-CN
```

The argument is any code from `Localization.Languages` (`en`, `de`, `es`, `fr`,
`it`, `pl`, `pt-BR`, `tr`, `ru`, `uk`, `ja`, `ko`, `zh-CN`, `zh-TW`). Each
language runs in a fresh process so the launcher's static choice lists are
initialized in that language. Every run also checks every language file,
whichever language it selects: the language list and the embedded
`Localization/*.json` files agree, each file parses, holds no string that
`zh-CN.json` (the reference for the code's strings) lacks, and keeps its
format placeholders and file-dialog filter patterns. The PASS line shows each
file's coverage; strings a file lacks fall back to English. The checks also
cover English defaults and fallback, unknown codes, menu codes,
keyboard/controller identifiers, and control-profile round trips. They do not
load or save user settings, launch the game, or contact update servers.

To check only game-language choices (including PS4 language 17, Brazilian Portuguese),
the native display name and the in-memory settings value, independently of catalog coverage:

```powershell
dotnet run --project launcher/windows/tests/LocalizationChecks -- en --game-languages-only
dotnet run --project launcher/windows/tests/LocalizationChecks -- pt-BR --game-languages-only
```

## Adding a language

Add `launcher/BloodborneLauncher/Localization/<code>.json` (English text to
translation, the keys of `zh-CN.json`) and one `(code, native name)` entry to
`Localization.Languages` in `Core/Localization.cs`. Both launchers embed every
file in that folder and show the list in **Settings > Appearance > Launcher
language**.

For visual review, select the language there and restart. Check both layouts,
the window buttons, settings search, dialogs, and **Display > Language >
In-game settings menu language**. The game menu uses `zh_CN` for Chinese; its
font and build steps are documented in `engine/docs/history/bbport-README.md`.
