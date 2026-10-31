# Brazilian Portuguese game language

Select **Display > Language > Game language > Português (Brasil)** in either launcher.
The setting uses PS4 system-language value `17` through the existing `BB_LANGUAGE`
environment variable. On Linux a source launch can also use `BB_LANGUAGE=17 ./run.sh`.

The runtime selects the Brazilian files already present in the player's CUSA03173
1.09 dump: `msg/porbr`, `menu/porbr` and localized `sound/*_ptb.fsb` banks. There are no
game assets in this contribution, no audio conversion and no files copied into the dump.

The European game's fallback paths can name English, European Portuguese or another
supported language. Only localized game-file requests are redirected. General sound
banks, music, movie files and non-game mounts keep their paths. A missing Brazilian
file falls back to the originally requested file. The selected Brazilian asset still
resolves through the usual mod lookup and per-file mounts.

For `sprj_opening.mp4`, the runtime prefers a Portuguese-tagged audio stream. The
original CUSA03173 opening has no useful language tags: its layout is one video and
eight audio tracks, with Brazilian Portuguese last. Only that layout uses the positional
fallback. Other movies, unknown layouts and other game-language selections keep their
previous behavior. The file itself is never edited or repacked.

No region function, gameplay parameters or online patch selection is changed. This
does not certify connectivity to any particular server.

## Scope and compatibility

The code is shared by Windows and Linux, on every supported CPU and GPU. It changes
file selection and the opening's audio stream, not rendering or Vulkan calls. Selecting
another game language keeps the previous path. The launcher language and the port's
in-game settings-menu language remain independent.

Disable older regional translation mods before testing this native option. Such mods
may replace the fallback files and reorder the opening's audio tracks, so their movie
layout may no longer match the original. This contribution does not remove those mods
or restore game files they overwrote.

## Automated checks

- `engine/tests/test_game_language.c`, part of `engine/build.sh --test`: language
  folders, all localized voice suffixes, shared-bank exclusion, missing-file fallback,
  relative paths, `/hostapp`, non-game mounts, bounds, mod precedence and per-file mounts.
  Fixtures are synthetic and contain no game assets.
- `engine/tests/test_sys_av_player.c`: the original nine-stream opening, explicit
  Portuguese tags, shorter/unknown layouts, other movies, other game languages,
  automatic start, video-stream preservation and invalid stream indices.
- `launcher/windows/tests/LocalizationChecks`: the existing language values and the
  added value `17`, the native label under different launcher languages, and the
  in-memory settings value. A focused run is available independently of catalog coverage:

```powershell
dotnet run --project launcher/windows/tests/LocalizationChecks -- en --game-languages-only
dotnet run --project launcher/windows/tests/LocalizationChecks -- pt-BR --game-languages-only
```

## Manual verification before release

Use original CUSA03173 1.09 files with regional translation mods disabled. Confirm
Brazilian text on the title screen, menus, items and dialogue, NPC voices, and the
opening's Brazilian voice. Check DLC dialogue and boss music transitions, including
Ludwig. Restart with English and confirm that its original files and voices return.
Check online with the original region and Unlock Game Region disabled.

Automated fixtures do not replace these in-game checks or a Linux test boot.
