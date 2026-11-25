# Save backups

Automatic backups copy progress that the game has written to disk. They do not force an
in-game save. The launcher makes before-play copies. The engine owns the ten-minute
schedule and also requests a backup when the supported game title menu is built. Both
engine triggers work with direct launches and share the same automatic-backup switch
and retention limit. The title trigger uses the existing verified PC menu hook.

## Automatic copies

The engine uses monotonic elapsed time. Once a backup is due, busy or inconsistent copies
retry every five seconds rather than waiting another ten minutes. A title-menu backup
can run before the ten-minute deadline and leaves that deadline in place. Writable
mounts, save-memory writes, metadata writes and deletion share the live profile lock
with launcher backups and recovery-vault copies. A completed copy is verified before
it appears in the backup list. Duplicate detection compares file contents.

The newest ten before-play copies and six during-play copies are kept. Backups remain in
`<saves root>/launcher-backups/<profile>/`, with `savedata` inside each timestamped folder.
Direct starts infer Main or `profiles/<name>` from the effective user directory. The
launcher supplies `BB_SAVE_BACKUP_DIR` for the selected profile explicitly.

The launcher persists its automatic-backup switch as `save_backups=1` or `0` in
`lantern.ini`, so direct engine launches use the same preference. `BB_SAVE_BACKUPS`
provides the fallback when that key is absent.

## Quit to Desktop

The PC System menu's **Quit to Desktop** uses Bloodborne's normal return-to-title
confirmation handler on a compatible 1.09 game. The engine waits for the title menu
and for the live save to be idle before closing. If automatic backups are enabled,
it also waits for the title backup. A timeout leaves the game running and records the
reason in the log. The game decides which position and state it records in its save.

## Named copies and deletion

Both launchers offer **Saves → Create named save…**. Names allow 1–80 letters, numbers,
spaces, hyphens or underscores. Reusing a name never overwrites an earlier snapshot.
Named copies are excluded from automatic retention cleanup and can be restored through
the same list as automatic backups. Their labels are stored alongside the savedata.

**Delete** requires confirmation and removes only the selected profile's backup plus
identical recovery-vault copies, including mirrors with different timestamps. It
checks file paths and contents before deleting a vault mirror. Distinct recovery
copies, other local snapshots and live savedata are preserved. Safety-only rows also
offer Delete. Later game saves or automatic protection can create new snapshots.

## Restore and recovery

The session lock is in the live profile folder. Changing the backup destination does
not allow a second engine or a restore to use the same live saves. Both launcher
restore paths hold this lock, the live write lock and the selected backup's lock
through publication.

A restore verifies the replacement in a staging directory and creates a distinct undo
copy when current progress exists. It then renames the current save directory aside
and publishes the replacement. A failed publication rolls the previous directory
back. If the process stops between the renames, engine startup or the next restore
recovers the previous directory before opening or replacing live saves.

## Checks

From the repository root:

```powershell
dotnet run --project launcher/windows/tests/BackupChecks/BackupChecks.csproj -c Release
```

After building the engine, from `engine/` in CLANG64:

```bash
ninja -C out/gpu save-snapshots-test save-backup-boundaries-test
out/gpu/save-snapshots-test.exe
fixture=$(mktemp -d "$PWD/out/save-backup-check-XXXXXX")
out/gpu/save-snapshots-test.exe --transactions "$fixture/user"
out/gpu/save-snapshots-test.exe --title-worker "$fixture/title"
BB_SAVE_BACKUPS=0 out/gpu/save-snapshots-test.exe --quit-without-backups "$fixture/quit"
python3 tests/test_save_sessions.py out/gpu/save-snapshots-test.exe
mkdir "$fixture/runtime"
out/gpu/save-backup-boundaries-test.exe "$fixture/runtime"
```

The fixture commands write test data. Use a new temporary directory, never a player's
user directory, and remove that fixture after the test executables have exited. Checks
cover scheduling, title requests during writes, cross-process session protection,
interrupted restores, same-second undo copies, vault coordination, retention and
byte-identical mirrors with different timestamps.
