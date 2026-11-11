# Save backups

Automatic backups copy progress that the game has written to disk. They do not force an
in-game save. Before-play copies remain launcher-owned; the engine owns the ten-minute
schedule while playing, including direct launches without the launcher window open.

## Automatic copies

The engine uses monotonic elapsed time. Once a backup is due, busy or inconsistent copies
retry every five seconds rather than waiting another ten minutes. Writable save mounts,
save-memory writes and deletion share the profile backup lock with launcher operations.
Only a completed, verified snapshot is renamed into the visible backup list. Unchanged
progress is not copied again, including after an engine restart.

The newest ten before-play copies and six during-play copies are kept. Backups remain in
`<saves root>/launcher-backups/<profile>/`, with `savedata` inside each timestamped folder.
Direct starts infer Main or `profiles/<name>` from the effective user directory. The
launcher supplies `BB_SAVE_BACKUP_DIR` for the selected profile explicitly.

The launcher persists its automatic-backup switch as `save_backups=1` or `0` in
`lantern.ini`, so direct engine launches use the same preference. `BB_SAVE_BACKUPS`
provides the fallback when that key is absent.

## Named copies and deletion

Both launchers offer **Saves → Create named save…**. Names allow 1–80 letters, numbers,
spaces, hyphens or underscores. Reusing a name never overwrites an earlier snapshot.
Named copies are excluded from automatic retention cleanup and can be restored through
the same list as automatic backups. Their labels are stored alongside the savedata.

**Delete** requires confirmation and removes only the selected profile's backup plus
identical hidden recovery-vault copies. It checks file contents before deleting a vault
mirror; matching timestamp/size fingerprints alone are insufficient. Distinct recovery
copies, other local snapshots and live savedata are preserved. Safety-only rows also
offer Delete. Later game saves or automatic protection can create new snapshots.

An engine session lock prevents restoration while the game is running, including direct
launches. Starting another engine or starting during a restore against the same profile
is rejected before save configuration can modify any files.

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
mkdir "$fixture/runtime"
out/gpu/save-backup-boundaries-test.exe "$fixture/runtime"
```

The fixture commands write test data. Use a new temporary directory, never a player's
user directory, and remove that fixture after the test executables have exited. Checks
cover scheduling/retry, transaction and cross-session protection, retention, named-save
validation/collisions, profile isolation across settings reload, deletion and vault mirrors.
