# bbhost: the Hunter's Dream online backend

Sources:

- https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771 for the backend in
  `gpu/bbnet/thd/*`.
- https://github.com/droogie/bbhost/tree/3f9c4c05e447c8447aee60b6a4223a875187bd39 (bbhost 0.2.31,
  PR #217) for NpSignaling: `gpu/bbnet/vendor/bbhost/np_signaling.cpp` is that commit's
  `src/hle/np_signaling.cpp`, unchanged, built through the adapter headers in
  `gpu/bbnet/vendor/bbhost_shim`. The Matching2 hooks, the P2P port's signaling path and the NpId
  compare in `gpu/bbnet/thd` follow the same commit. `tools/vendor/bbhost-signaling.sh` fetches the
  file at the pinned commit and checks its hash. The file is the same in the v0.2.31 release tag
  (https://github.com/droogie/bbhost/tree/99f75c1ad28a648560a57f3ef0aab4ac2505926c).
- https://github.com/droogie/bbhost/commit/f990415 (in v0.2.31): sceNpScoreSanitizeComment gives the
  character name back, cut at 255 bytes on a UTF-8 character start, as
  `gpu/bbnet/vendor/shadps4/core/libraries/np/np_score/np_score.cpp` now does.
  On The Hunter's Dream the same copy is `score_sanitize` in `gpu/bbnet/thd/thd_np.cpp`.
- The same tag's `src/hle/http.cpp` (a 401 with ResKind on the game's own requests marks the sign-in
  refused): `gpu/bbnet/vendor/shadps4/core/libraries/network/http.cpp` calls `bbthd_game_reply` for a
  401 while The Hunter's Dream is on (`bbthd_enabled()`), and `gpu/bbnet/thd/thd_session.cpp` reads
  it the same way; offline and in seamless rooms it does nothing.
- The same tag's `src/engine/matching_region.cpp` (Matching Regions starts at Worldwide):
  `MatchingWorldwide` in `gpu/shim/engine/online_fixes.cpp`, on The Hunter's Dream only.

The online backend for The Hunter's Dream adapts bbhost's: `gpu/bbnet/thd/*`, the play-log
redirect in `scripts/online.py` (after bbhost's `src/hle/http.cpp`), and the launcher's sign-in
(`launcher/BloodborneLauncher/Core/HuntersDream.cs`). It is kept as it is so the service's
servers see the client they expect.

These portions are GPL-3.0-or-later. Copyright remains with the bbhost contributors, and
Lantern Engine's additional terms (`NOTICE`) do not apply to them. Builds containing these
portions are distributed under GPL-3.0-or-later (see `GPL-3.0.txt`).
