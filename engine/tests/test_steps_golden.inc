// SPDX-License-Identifier: GPL-3.0-or-later
R"golden(lantern-diagnosis 1
session: pid 12345, started 2026-11-13T21:04:11
[attention]
STEP DIFFERED (cannot turn off live): draw-tokens at frame 1: Step differs: draw-tokens (cannot turn off live): frame 1 draw 1843221 area 0 thread bb:DrawRec#1; vs 6c62a79f ps a0e88f56 key 9f13c0a2e4b1d377; packet re-encode guest 0+0: new 0 0, old 0 0; extra 0. Next start, copy this into Extra variables: LANTERN_STEPS_OFF=gx-own-buffers,draw-tokens,vertex-memo
STEP DIFFERED (cannot turn off live): vertex-memo at frame 1: Step differs: vertex-memo (cannot turn off live): frame 1 draw 1843221 area 0 thread bb:DrawRec#1; vs 6c62a79f ps a0e88f56 key 9f13c0a2e4b1d377; vertex buffer guest 0x10dbb62000+0x8000: new 0x2b1c0 0x1000, old 0x2b1c0 0; extra 0. Next start, copy this into Extra variables: LANTERN_STEPS_OFF=gx-own-buffers,draw-tokens,vertex-memo
[build]
engine_id: 8adc288f
os: Windows
[session]
ended: exit
minutes: 47.3
frames: 302112
areas: 6
fps: 141.2 average
[switches]
BB_FRAME_STATS=1
LANTERN_STEPS_OFF=gx-own-buffers
LANTERN_VERTEX_MEMO=1
BB_ROOM_PASSWORD=<secret>
[steps]
name | switch | state | used | covered/total | fell back (by reason) | checked | differ | skipped | inconclusive | check ms
direct-draw-calls | BB_NATIVE_CALLS=0 | off | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
draw-tokens | LANTERN_DRAW_TOKENS=1 | on (registry only) | 0 | 0/0 | none | 1 | 1 | 0 | 0 | 0.0
INSTRUMENT SUSPECT draw-tokens: every check differs
native-shaders | BB_NATIVE_SHADERS=1 | on (registry only) | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
native-shader-compiler | LANTERN_LIFTER=1 | on (registry only) | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
native-tables | BB_NATIVE_TABLES=0 | off (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
ring-constants | LANTERN_RING_CONSTANTS=1 | on (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
vertex-memo | LANTERN_VERTEX_MEMO=1 | on | 1 | 1/1 | not clean 1 | 1 | 1 | 0 | 0 | 0.0
INSTRUMENT SUSPECT vertex-memo: every check differs
pipeline-memo | LANTERN_PIPELINE_MEMO=0 | off | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
prepared-pipeline | LANTERN_PREPARED_PIPELINE=1 | on (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
fresh-flats | LANTERN_FRESH_FLATS=1 | on (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
key-hash | LANTERN_KEY_HASH=1 | on (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
gx-textures | LANTERN_GX_TEXTURES=0 | off (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
gx-own-buffers | LANTERN_GX_OWN_BUFFERS=0 | off | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
gx-binds | LANTERN_GX_BINDS=1 | on (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
gx-constants | LANTERN_GX_CONSTANTS=0 | off (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
gx-records | LANTERN_GX_RECORDS=0 | off (registry only) | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
gx-native | LANTERN_GX_NATIVE=0 | off (registry only) | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
read-filter | LANTERN_READ_FILTER=1 | on (registry only) | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
frame-start-pacing | LANTERN_FRAME_START_PACING=1 | on (registry only) | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
toggles | BB_TOGGLE_FILE=(not set) | on (registry only) | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
dynamic-state-skip | LANTERN_DYNAMIC_STATE_SKIP=0 | off | 0 | 0/0 | none | 0 | 0 | 0 | 0 | 0.0
job-lock | LANTERN_JOB_LOCK=1 | on | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
job-node-cache | LANTERN_JOB_NODE_CACHE=1 | on | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
main-jobs-barrier | LANTERN_MAIN_JOBS_BARRIER=1 | on | 0 | 0/0 | none | 0 unverified | 0 | 0 | 0 | 0.0
[first-differences]
Step differs: draw-tokens (cannot turn off live): frame 1 draw 1843221 area 0 thread bb:DrawRec#1; vs 6c62a79f ps a0e88f56 key 9f13c0a2e4b1d377; packet re-encode guest 0+0: new 0 0, old 0 0; extra 0. Next start, copy this into Extra variables: LANTERN_STEPS_OFF=gx-own-buffers,draw-tokens,vertex-memo
Step differs: vertex-memo (cannot turn off live): frame 1 draw 1843221 area 0 thread bb:DrawRec#1; vs 6c62a79f ps a0e88f56 key 9f13c0a2e4b1d377; vertex buffer guest 0x10dbb62000+0x8000: new 0x2b1c0 0x1000, old 0x2b1c0 0; extra 0. Next start, copy this into Extra variables: LANTERN_STEPS_OFF=gx-own-buffers,vertex-memo
[unverified]
native-shaders | on | offline: bb-shader-lab --check-cache on captured shaders (BB_SHADER_CAPTURE)
native-shader-compiler | on | offline: spirv_diff against the old recompiler on captured shaders
gx-native | off | compare: LANTERN_GX_RECORDS=2 checks the binds, constants and targets the native draws use
read-filter | on | log: read faults and write faults/s in the Frame stats line; no in-game comparison
frame-start-pacing | on | log: Frame pacing+ line
toggles | on | none: state only
job-lock | on | offline: main-jobs-image-test runs the game's own Lock and Unlock against this one on one word under 8 threads, and main-jobs-test's mutants
job-node-cache | on | offline: main-jobs-image-test pops the game's own ProcessOne and Enqueue with every heap block counted back once, and main-jobs-test's mutants
main-jobs-barrier | on | offline: main-jobs-barrier-test (the step against the game's step on a threaded job system, mutants)
[warnings]
none
[vulkan]
none
[triage]
not written at a clean exit
[threads]
)golden"
