// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.h"
#include "sys/av_player.h"
#include "sys/sys.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST __attribute__((sysv_abi))
#define CODE(x) ((uint32_t)(x))
#define E_PARAMS 0x806A0001u
#define E_FAILED 0x806A0002u
#define E_NOT_SUPPORTED 0x806A0004u

static int failures;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

typedef struct {
    void *object;
    void *(GUEST *allocate)(void *, uint32_t, uint32_t);
    void (GUEST *deallocate)(void *, void *);
    void *(GUEST *allocate_texture)(void *, uint32_t, uint32_t);
    void (GUEST *deallocate_texture)(void *, void *);
    SysAvFileCalls file;
    void *event_object;
    void (GUEST *event)(void *, int32_t, int32_t, void *);
    int32_t debug_level;
    uint32_t base_priority;
    int32_t video_framebuffers;
    bool auto_start;
    uint8_t reserved[3];
    const char *default_language;
} InitData;

typedef struct {
    void *data;
    uint8_t reserved[4];
    uint64_t timestamp;
    struct { uint16_t channels; uint8_t reserved[2]; uint32_t sample_rate, size; char language[4]; } audio;
} AudioInfo;

typedef struct {
    void *data;
    uint8_t reserved[4];
    uint64_t timestamp;
    uint32_t width, height;
    float aspect;
    uint8_t language[4];
    uint32_t framerate, crop_left, crop_right, crop_top, crop_bottom, pitch;
    uint8_t luma, chroma, full_range;
    uint8_t rest[37];
} VideoInfo;
_Static_assert(sizeof(VideoInfo) == 104, "SceAvPlayerFrameInfoEx");

typedef void *(GUEST *InitFn)(InitData *);
typedef int32_t (GUEST *PostFn)(void *, const void *);
typedef int32_t (GUEST *AddFn)(void *, const char *);
typedef int32_t (GUEST *HandleFn)(void *);
typedef int32_t (GUEST *LoopFn)(void *, bool);
typedef bool (GUEST *ActiveFn)(void *);
typedef bool (GUEST *VideoFn)(void *, VideoInfo *);
typedef bool (GUEST *AudioFn)(void *, AudioInfo *);
typedef uint64_t (GUEST *TimeFn)(void *);
typedef int32_t (GUEST *InfoFn)(void *, uint32_t, SysAvStreamInfo *);
typedef int32_t (GUEST *EnableFn)(void *, uint32_t);

static InitFn av_init;
static PostFn av_post;
static AddFn av_add;
static HandleFn av_close, av_start, av_stop, av_pause, av_resume, av_count;
static LoopFn av_loop;
static ActiveFn av_active;
static VideoFn av_video;
static AudioFn av_audio;
static TimeFn av_time;
static InfoFn av_info;
static EnableFn av_enable;

static void *fn(const char *nid) {
    const SysExport *found = sys_find(nid, NULL);
    if (!found) {
        printf("FAIL: %s is missing\n", nid);
        exit(1);
    }
    return found->function;
}

static void bind(void) {
    av_init = (InitFn)fn("aS66RI0gGgo");
    av_post = (PostFn)fn("HD1YKVU26-M");
    av_add = (AddFn)fn("KMcEa+rHsIo");
    av_loop = (LoopFn)fn("OVths0xGfho");
    av_close = (HandleFn)fn("NkJwDzKmIlw");
    av_start = (HandleFn)fn("ET4Gr-Uu07s");
    av_stop = (HandleFn)fn("ZC17w3vB5Lo");
    av_active = (ActiveFn)fn("UbQoYawOsfY");
    av_pause = (HandleFn)fn("9y5v+fGN4Wk");
    av_resume = (HandleFn)fn("w5moABNwnRY");
    av_video = (VideoFn)fn("JdksQu8pNdQ");
    av_time = (TimeFn)fn("wwM99gjFf1Y");
    av_count = (HandleFn)fn("hdTyRzCXQeQ");
    av_info = (InfoFn)fn("d8FcbzfAdQw");
    av_enable = (EnableFn)fn("ODJK2sn9w4A");
    av_audio = (AudioFn)fn("Wnp1OVcrZgk");
}

static atomic_int live_memory, live_textures;
static atomic_int texture_failures;
static void *pause_during_start;
static int32_t pause_during_start_result;

static void *GUEST allocate(void *object, uint32_t alignment, uint32_t size) {
    (void)object;
    (void)alignment;
    atomic_fetch_add(&live_memory, 1);
    return malloc(size);
}

static void GUEST deallocate(void *object, void *memory) {
    (void)object;
    atomic_fetch_sub(&live_memory, 1);
    free(memory);
}

static void *GUEST allocate_texture(void *object, uint32_t alignment, uint32_t size) {
    (void)object;
    if (alignment != 0x100) atomic_fetch_add(&texture_failures, 1);
    if (pause_during_start) {
        void *player = pause_during_start;
        pause_during_start = NULL;
        pause_during_start_result = av_pause(player);
    }
    atomic_fetch_add(&live_textures, 1);
    return malloc(size);
}

static void GUEST deallocate_texture(void *object, void *memory) {
    (void)object;
    atomic_fetch_sub(&live_textures, 1);
    free(memory);
}

static atomic_int event_counts[64], warning_seen;

static void GUEST on_event(void *object, int32_t event, int32_t source, void *data) {
    (void)object;
    (void)source;
    if (event >= 0 && event < 64) atomic_fetch_add(&event_counts[event], 1);
    if (event == 0x20 && data) atomic_store(&warning_seen, *(int32_t *)data);
}

typedef struct {
    int kind;
    int number;
} FakePacket;

static struct {
    int streams_found;
    bool has_audio;
    int audio_tracks, tagged_audio, enabled_audio;
    bool untagged_audio;
    int video_frames, audio_frames;
    int fail_read_at, fail_send_at, fail_receive_at, fail_begin;
    int media_open, packets_live, decoders_begun;
    int started[2];
    uint32_t frame_ms;
    int spacing_us;
    int read_delay_ms, count_error;
    atomic_int wrong_sizes;
} fake;

typedef struct {
    int position[2];
    int sent[2], received[2];
    bool flushing[2];
    int reads, sends, receives;
    HostMutex lock;
} FakeMedia;

static SysAvMedia *fake_open(const char *path, const SysAvFileCalls *file) {
    (void)file;
    if (!strcmp(path, "/app0/missing.mp4")) return NULL;
    FakeMedia *media = calloc(1, sizeof(*media));
    host_mutex_init(&media->lock);
    ++fake.media_open;
    return (SysAvMedia *)media;
}

static void fake_close(SysAvMedia *media) {
    free(media);
    --fake.media_open;
}

static int fake_find(SysAvMedia *media) {
    (void)media;
    return fake.streams_found;
}

static int fake_count(SysAvMedia *media) {
    (void)media;
    if (fake.count_error) return -1;
    return fake.has_audio ? 1 + fake.audio_tracks : 1;
}

static int fake_info(SysAvMedia *media, uint32_t index, SysAvStreamInfo *info) {
    (void)media;
    if (index >= (uint32_t)(fake.has_audio ? 1 + fake.audio_tracks : 1)) return 0;
    memset(info, 0, sizeof(*info));
    info->type = index == 0 ? SYS_AV_VIDEO : SYS_AV_AUDIO;
    info->duration = (uint64_t)fake.video_frames * fake.frame_ms;
    if (index == SYS_AV_VIDEO) {
        info->details.video.width = 640;
        info->details.video.height = 368;
        memcpy(info->details.video.language, "eng", 3);
    } else {
        info->details.audio.channel_count = 2;
        info->details.audio.sample_rate = 48000;
        memcpy(info->details.audio.language, (int)index == fake.tagged_audio ? "por" : fake.untagged_audio ? "und" : "eng", 3);
    }
    return 1;
}

static int fake_enable(SysAvMedia *media, uint32_t index) {
    (void)media;
    if (index >= (uint32_t)(fake.has_audio ? 1 + fake.audio_tracks : 1)) return -1;
    if (index) fake.enabled_audio = (int)index;
    return index ? SYS_AV_AUDIO : SYS_AV_VIDEO;
}

static int fake_begin(SysAvMedia *media, uint32_t *video_bytes) {
    FakeMedia *m = (FakeMedia *)media;
    if (fake.fail_begin) return 0;
    memset(m->position, 0, sizeof(m->position));
    memset(m->sent, 0, sizeof(m->sent));
    memset(m->received, 0, sizeof(m->received));
    memset(m->flushing, 0, sizeof(m->flushing));
    m->reads = m->sends = m->receives = 0;
    *video_bytes = 4096;
    ++fake.decoders_begun;
    return 1;
}

static void fake_end(SysAvMedia *media) {
    (void)media;
}

static int fake_read(SysAvMedia *media, void **packet, int *kind) {
    FakeMedia *m = (FakeMedia *)media;
    if (fake.read_delay_ms) host_sleep_ns((uint64_t)fake.read_delay_ms * 1000000);
    if (fake.fail_read_at && ++m->reads == fake.fail_read_at) return -1;
    int want = !fake.has_audio || m->position[SYS_AV_VIDEO] <= m->position[SYS_AV_AUDIO] ? SYS_AV_VIDEO : SYS_AV_AUDIO;
    int limit[2] = {fake.video_frames, fake.audio_frames};
    if (m->position[want] >= limit[want]) want = !want;
    if (want == SYS_AV_AUDIO && !fake.has_audio) return 0;
    if (m->position[want] >= limit[want]) return 0;
    FakePacket *p = malloc(sizeof(*p));
    *p = (FakePacket){want, m->position[want]++};
    *packet = p;
    *kind = want;
    __atomic_add_fetch(&fake.packets_live, 1, __ATOMIC_RELAXED);
    return 1;
}

static int fake_rewind(SysAvMedia *media) {
    FakeMedia *m = (FakeMedia *)media;
    m->position[0] = m->position[1] = 0;
    return 0;
}

static void fake_free_packet(void *packet) {
    __atomic_sub_fetch(&fake.packets_live, 1, __ATOMIC_RELAXED);
    free(packet);
}

static int fake_send(SysAvMedia *media, int kind, void *packet) {
    FakeMedia *m = (FakeMedia *)media;
    if (fake.fail_send_at && kind == SYS_AV_VIDEO && ++m->sends == fake.fail_send_at) return -1;
    if (!packet) {
        m->flushing[kind] = true;
        return 0;
    }
    ++m->sent[kind];
    return 0;
}

static int fake_receive(SysAvMedia *media, int kind, void *buffer, uint32_t size, SysAvFrame *frame) {
    FakeMedia *m = (FakeMedia *)media;
    if (fake.fail_receive_at && kind == SYS_AV_VIDEO && ++m->receives == fake.fail_receive_at) return SYS_AV_ERROR;
    int held_back = kind == SYS_AV_VIDEO ? 2 : 0;
    if (m->received[kind] + held_back >= m->sent[kind] && !m->flushing[kind]) return SYS_AV_MORE;
    if (m->received[kind] >= m->sent[kind]) return m->flushing[kind] ? SYS_AV_END : SYS_AV_MORE;
    if (size != (kind == SYS_AV_VIDEO ? 4096u : 32768u)) atomic_fetch_add(&fake.wrong_sizes, 1);
    int number = m->received[kind]++;
    memset(buffer, kind * 16 + (number & 15), size);
    frame->timestamp = (uint64_t)(number % (kind == SYS_AV_VIDEO ? fake.video_frames : fake.audio_frames)) * fake.frame_ms;
    if (kind == SYS_AV_VIDEO) {
        frame->width = 640;
        frame->height = 368;
        frame->pitch = 640;
        frame->crop_bottom = 8;
    } else {
        frame->channel_count = 2;
        frame->sample_rate = 48000;
        frame->bytes = 256;
    }
    return SYS_AV_FRAME;
}

static uint64_t fake_duration(SysAvMedia *media) {
    (void)media;
    return (uint64_t)fake.video_frames * fake.frame_ms;
}

static const SysAvDecoder fake_decoder = {fake_open, fake_close, fake_find, fake_count, fake_info, fake_enable, fake_begin,
                                          fake_end, fake_read, fake_rewind, fake_free_packet, fake_send, fake_receive, fake_duration};

static void reset_fake(void) {
    memset(&fake, 0, sizeof(fake));
    fake.streams_found = 1;
    fake.has_audio = true;
    fake.audio_tracks = 1;
    fake.enabled_audio = -1;
    fake.video_frames = 30;
    fake.audio_frames = 40;
    fake.frame_ms = 1;
    for (int i = 0; i < 64; ++i) atomic_store(&event_counts[i], 0);
    atomic_store(&warning_seen, 0);
}

static InitData make_init(void) {
    InitData init;
    memset(&init, 0, sizeof(init));
    init.allocate = allocate;
    init.deallocate = deallocate;
    init.allocate_texture = allocate_texture;
    init.deallocate_texture = deallocate_texture;
    init.event = on_event;
    init.video_framebuffers = 4;
    return init;
}

static bool wait_until_event(int event, int count) {
    for (int i = 0; i < 4000; ++i) {
        if (atomic_load(&event_counts[event]) >= count) return true;
        host_sleep_ns(1000000);
    }
    return false;
}

static void drain(void *player, int *videos, int *audios) {
    uint64_t started = host_monotonic_ns();
    uint64_t last_video = 0;
    int v = 0, a = 0;
    while (av_active(player) && host_monotonic_ns() - started < UINT64_C(10000000000)) {
        VideoInfo video;
        AudioInfo audio;
        bool any = false;
        if (av_video(player, &video)) {
            EXPECT(*(unsigned char *)video.data == (unsigned char)(v & 15));
            EXPECT(video.width == 640 && video.pitch == 640 && video.crop_bottom == 8 && video.luma == 8 && video.chroma == 8);
            EXPECT(!v || video.timestamp >= last_video);
            last_video = video.timestamp;
            ++v;
            any = true;
        }
        if (av_audio(player, &audio)) {
            EXPECT(*(unsigned char *)audio.data == (unsigned char)(16 + (a & 15)));
            EXPECT(audio.audio.channels == 2 && audio.audio.sample_rate == 48000 && audio.audio.size == 256);
            ++a;
            any = true;
        }
        if (!any) host_yield();
    }
    *videos = v;
    *audios = a;
}

static void test_arguments(void) {
    InitData init = make_init();
    EXPECT(!av_init(NULL));
    InitData bad = init;
    bad.allocate = NULL;
    EXPECT(!av_init(&bad));
    bad = init;
    bad.deallocate_texture = NULL;
    EXPECT(!av_init(&bad));
    EXPECT(CODE(av_add(NULL, "x")) == E_PARAMS);
    EXPECT(CODE(av_close(NULL)) == E_PARAMS);
    EXPECT(CODE(av_start(NULL)) == E_PARAMS);
    EXPECT(CODE(av_stop(NULL)) == E_PARAMS);
    EXPECT(CODE(av_pause(NULL)) == E_PARAMS);
    EXPECT(CODE(av_resume(NULL)) == E_PARAMS);
    EXPECT(CODE(av_count(NULL)) == E_PARAMS);
    EXPECT(CODE(av_post(NULL, &init)) == E_PARAMS);
    EXPECT(CODE(av_loop(NULL, true)) == E_PARAMS);
    EXPECT(CODE(av_enable(NULL, 0)) == E_PARAMS);
    EXPECT(!av_active(NULL));
    VideoInfo video;
    AudioInfo audio;
    SysAvStreamInfo info;
    EXPECT(!av_video(NULL, &video) && !av_audio(NULL, &audio));
    EXPECT(CODE(av_info(NULL, 0, &info)) == E_PARAMS);

    void *player = av_init(&init);
    EXPECT(player != NULL);
    EXPECT(CODE(av_post(player, NULL)) == E_PARAMS);
    EXPECT(av_post(player, &init) == 0);
    EXPECT(!av_video(player, NULL) && !av_audio(player, NULL));
    EXPECT(CODE(av_info(player, 0, NULL)) == E_PARAMS);
    EXPECT(CODE(av_count(player)) == E_FAILED);
    EXPECT(CODE(av_info(player, 0, &info)) == E_FAILED);
    EXPECT(CODE(av_enable(player, 0)) == E_FAILED);
    EXPECT(CODE(av_start(player)) == E_FAILED);
    EXPECT(CODE(av_stop(player)) == E_FAILED);
    EXPECT(CODE(av_pause(player)) == E_FAILED);
    EXPECT(CODE(av_resume(player)) == E_FAILED);
    EXPECT(CODE(av_loop(player, true)) == E_FAILED);
    EXPECT(!av_active(player));
    EXPECT(av_time(player) == 0);
    EXPECT(!av_video(player, &video) && !av_audio(player, &audio));
    EXPECT(CODE(av_add(player, "")) == E_FAILED);
    EXPECT(CODE(av_add(player, "/x/live.M3U8")) == E_NOT_SUPPORTED);
    EXPECT(CODE(av_add(player, "http://a/b.m3u8?x=1.mp4")) == E_NOT_SUPPORTED);
    EXPECT(CODE(av_add(player, "/app0/missing.mp4")) == E_FAILED);
    EXPECT(av_close(player) == 0);
}

static void test_playback(void) {
    reset_fake();
    InitData init = make_init();
    void *player = av_init(&init);
    EXPECT(av_add(player, "/app0/movie.mp4") == 0);
    EXPECT(CODE(av_add(player, "/app0/second.mp4")) == E_FAILED);
    EXPECT(wait_until_event(2, 1));
    EXPECT(av_count(player) == 2);
    SysAvStreamInfo info;
    EXPECT(av_info(player, 0, &info) == 0 && info.type == 0 && info.details.video.width == 640 && info.duration == 30);
    EXPECT(av_info(player, 1, &info) == 0 && info.type == 1 && info.details.audio.sample_rate == 48000);
    EXPECT(CODE(av_info(player, 2, &info)) == E_FAILED);
    EXPECT(CODE(av_enable(player, 2)) == E_FAILED);
    EXPECT(CODE(av_pause(player)) == E_FAILED);
    EXPECT(av_enable(player, 0) == 0 && av_enable(player, 1) == 0);
    EXPECT(av_start(player) == 0);
    EXPECT(atomic_load(&event_counts[3]) == 1);
    EXPECT(atomic_load(&live_textures) == 4 && atomic_load(&live_memory) == 8);
    EXPECT(av_active(player));
    int videos = 0, audios = 0;
    drain(player, &videos, &audios);
    EXPECT(videos == 30);
    EXPECT(audios == 40);
    EXPECT(!av_active(player));
    EXPECT(wait_until_event(1, 1));
    EXPECT(av_time(player) == 30);
    EXPECT(CODE(av_stop(player)) == E_FAILED);
    EXPECT(av_pause(player) == 0);
    EXPECT(__atomic_load_n(&fake.packets_live, __ATOMIC_RELAXED) == 0);
    EXPECT(av_close(player) == 0);
    EXPECT(atomic_load(&live_textures) == 0 && atomic_load(&live_memory) == 0);
    EXPECT(fake.media_open == 0);
    EXPECT(atomic_load(&texture_failures) == 0);
    EXPECT(atomic_load(&fake.wrong_sizes) == 0);
}

static void test_stop_restart_and_pause(void) {
    reset_fake();
    fake.video_frames = 400;
    fake.audio_frames = 400;
    fake.frame_ms = 5;
    InitData init = make_init();
    void *player = av_init(&init);
    EXPECT(av_add(player, "/app0/movie.mp4") == 0);
    EXPECT(wait_until_event(2, 1));
    EXPECT(av_enable(player, 0) == 0 && av_enable(player, 1) == 0);
    EXPECT(av_start(player) == 0);
    VideoInfo video;
    AudioInfo audio;
    for (int i = 0; i < 200 && !av_audio(player, &audio); ++i) host_sleep_ns(1000000);
    host_sleep_ns(20000000);
    EXPECT(av_pause(player) == 0);
    EXPECT(atomic_load(&event_counts[4]) == 1);
    EXPECT(CODE(av_pause(player)) == E_FAILED);
    EXPECT(!av_video(player, &video) && !av_audio(player, &audio));
    uint64_t frozen = av_time(player);
    host_sleep_ns(20000000);
    EXPECT(av_time(player) == frozen);
    EXPECT(av_resume(player) == 0);
    EXPECT(CODE(av_resume(player)) == E_FAILED);
    bool flowing = false;
    for (int i = 0; i < 500 && !flowing; ++i) {
        flowing = av_audio(player, &audio);
        if (!flowing) host_sleep_ns(1000000);
    }
    EXPECT(flowing);
    EXPECT(av_stop(player) == 0);
    EXPECT(atomic_load(&event_counts[1]) == 1);
    EXPECT(CODE(av_stop(player)) == E_FAILED);
    EXPECT(!av_active(player));
    EXPECT(atomic_load(&live_textures) == 0 && atomic_load(&live_memory) == 0);
    EXPECT(__atomic_load_n(&fake.packets_live, __ATOMIC_RELAXED) == 0);
    EXPECT(av_start(player) == 0);
    EXPECT(fake.decoders_begun == 2);
    EXPECT(atomic_load(&live_textures) == 4);
    EXPECT(av_start(player) == 0);
    EXPECT(atomic_load(&event_counts[1]) == 2);
    EXPECT(atomic_load(&live_textures) == 4 && atomic_load(&live_memory) == 8);
    EXPECT(av_close(player) == 0);
    EXPECT(atomic_load(&live_textures) == 0 && atomic_load(&live_memory) == 0);
    EXPECT(__atomic_load_n(&fake.packets_live, __ATOMIC_RELAXED) == 0);
}

static void test_looping(void) {
    reset_fake();
    fake.has_audio = false;
    fake.video_frames = 40;
    InitData init = make_init();
    void *player = av_init(&init);
    EXPECT(av_add(player, "/app0/movie.mp4") == 0);
    EXPECT(wait_until_event(2, 1));
    EXPECT(av_enable(player, 0) == 0);
    EXPECT(av_loop(player, true) == 0);
    EXPECT(av_start(player) == 0);
    int frames = 0;
    VideoInfo video;
    uint64_t started = host_monotonic_ns();
    while (frames < 100 && host_monotonic_ns() - started < UINT64_C(10000000000)) {
        if (av_video(player, &video)) ++frames;
        else host_yield();
    }
    EXPECT(frames >= 100);
    EXPECT(wait_until_event(0x20, 1));
    EXPECT(CODE(atomic_load(&warning_seen)) == 0x806A00A1);
    EXPECT(av_close(player) == 0);
}

static void test_auto_start(void) {
    reset_fake();
    InitData init = make_init();
    init.auto_start = true;
    init.default_language = "eng";
    void *player = av_init(&init);
    EXPECT(av_add(player, "/app0/movie.mp4") == 0);
    EXPECT(wait_until_event(3, 1));
    EXPECT(atomic_load(&event_counts[2]) == 0);
    int videos = 0, audios = 0;
    drain(player, &videos, &audios);
    EXPECT(videos == 30 && audios == 40);
    EXPECT(av_close(player) == 0);

    reset_fake();
    init = make_init();
    init.event = NULL;
    player = av_init(&init);
    EXPECT(av_add(player, "/app0/movie.mp4") == 0);
    for (int i = 0; i < 2000 && !atomic_load(&live_textures); ++i) host_sleep_ns(1000000);
    EXPECT(atomic_load(&live_textures) == 4);
    EXPECT(av_close(player) == 0);
}

static void test_failures(void) {
    reset_fake();
    fake.streams_found = 0;
    InitData init = make_init();
    void *player = av_init(&init);
    EXPECT(av_add(player, "/app0/movie.mp4") == 0);
    EXPECT(wait_until_event(0x20, 1));
    EXPECT(CODE(atomic_load(&warning_seen)) == E_NOT_SUPPORTED);
    EXPECT(atomic_load(&event_counts[2]) == 0);
    EXPECT(av_close(player) == 0);

    reset_fake();
    player = av_init(&init);
    EXPECT(av_add(player, "/app0/movie.mp4") == 0);
    EXPECT(wait_until_event(2, 1));
    EXPECT(CODE(av_start(player)) == E_FAILED);
    EXPECT(av_enable(player, 0) == 0);
    EXPECT(CODE(av_start(player)) == E_FAILED);
    EXPECT(av_close(player) == 0);

    for (int mode = 0; mode < 4; ++mode) {
        reset_fake();
        fake.video_frames = 200;
        if (mode == 0) fake.fail_read_at = 5;
        if (mode == 1) fake.fail_send_at = 5;
        if (mode == 2) fake.fail_receive_at = 3;
        if (mode == 3) fake.fail_begin = 1;
        player = av_init(&init);
        EXPECT(av_add(player, "/app0/movie.mp4") == 0);
        EXPECT(wait_until_event(2, 1));
        EXPECT(av_enable(player, 0) == 0);
        int32_t started = av_start(player);
        EXPECT(mode == 3 ? CODE(started) == E_FAILED : started == 0);
        for (int i = 0; i < 2000 && av_active(player); ++i) host_sleep_ns(1000000);
        EXPECT(!av_active(player));
        EXPECT(av_close(player) == 0);
        EXPECT(atomic_load(&live_textures) == 0 && atomic_load(&live_memory) == 0);
        EXPECT(__atomic_load_n(&fake.packets_live, __ATOMIC_RELAXED) == 0);
    }
}

static void *open_ready(InitData *init) {
    void *player = av_init(init);
    EXPECT(av_add(player, "/app0/movie.mp4") == 0);
    EXPECT(wait_until_event(2, 1));
    return player;
}

static void test_brazilian_opening(void) {
#ifdef _WIN32
#define LANGUAGE(value) _putenv_s("BB_LANGUAGE", value)
#else
#define LANGUAGE(value) setenv("BB_LANGUAGE", value, 1)
#endif
    const char *previous = getenv("BB_LANGUAGE");
    char *saved = previous ? strdup(previous) : NULL;
    for (int mode = 0; mode < 7; ++mode) {
        reset_fake();
        LANGUAGE(mode == 0 ? "1" : "17");
        fake.audio_tracks = mode == 4 || mode == 6 ? 2 : 8;
        fake.untagged_audio = true;
        if (mode == 3) fake.tagged_audio = 3;
        if (mode == 6) fake.tagged_audio = 2;
        InitData init = make_init();
        init.auto_start = mode == 5;
        void *player = av_init(&init);
        EXPECT(av_add(player, mode == 2 ? "/app0/dvdroot_ps4/movie/other.mp4" :
                                        "/app0/dvdroot_ps4/movie/sprj_opening.mp4") == 0);
        EXPECT(wait_until_event(mode == 5 ? 3 : 2, 1));
        if (mode != 5) {
            EXPECT(av_enable(player, 0) == 0);
            EXPECT(av_enable(player, 1) == 0);
        }
        int wanted = mode == 0 || mode == 2 || mode == 4 ? 1 : mode == 3 ? 3 : mode == 6 ? 2 : 8;
        EXPECT(fake.enabled_audio == wanted);
        if (mode == 1) {
            /* A saved in-game Spanish voice choice must not select Spanish again. */
            EXPECT(av_enable(player, 6) == 0 && fake.enabled_audio == 8);
            EXPECT(av_enable(player, 8) == 0 && fake.enabled_audio == 8);
        }
        SysAvStreamInfo info;
        EXPECT(av_info(player, 1, &info) == 0 && info.type == SYS_AV_AUDIO);
        if (mode == 3 || mode == 6) EXPECT(!memcmp(info.details.audio.language, "por", 3));
        EXPECT(av_info(player, 0, &info) == 0 && info.type == SYS_AV_VIDEO);
        EXPECT(CODE(av_enable(player, 99)) == E_FAILED);
        EXPECT(av_close(player) == 0);
    }
    LANGUAGE(saved ? saved : "");
    free(saved);
#undef LANGUAGE
}

static void test_edges(void) {
    VideoInfo video;

    reset_fake();
    fake.has_audio = false;
    fake.frame_ms = 1000;
    InitData init = make_init();
    void *player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0 && av_start(player) == 0);
    host_sleep_ns(30000000);
    EXPECT(av_video(player, &video) && video.timestamp == 0);
    EXPECT(!av_video(player, &video));
    EXPECT(av_close(player) == 0);

    reset_fake();
    fake.has_audio = false;
    init.video_framebuffers = 1;
    player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0 && av_start(player) == 0);
    EXPECT(atomic_load(&live_textures) == 2);
    EXPECT(av_close(player) == 0);
    init.video_framebuffers = 99;
    reset_fake();
    fake.has_audio = false;
    player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0 && av_start(player) == 0);
    EXPECT(atomic_load(&live_textures) == 16);
    EXPECT(av_close(player) == 0);
    init = make_init();

    reset_fake();
    fake.has_audio = false;
    player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0 && av_start(player) == 0);
    host_sleep_ns(100000000);
    EXPECT(av_active(player) && av_time(player) == 30);
    EXPECT(av_close(player) == 0);

    reset_fake();
    fake.has_audio = false;
    fake.video_frames = 2;
    player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0 && av_start(player) == 0);
    host_sleep_ns(60000000);
    EXPECT(atomic_load(&event_counts[1]) == 0);
    int videos = 0, audios = 0;
    drain(player, &videos, &audios);
    EXPECT(videos == 2);
    EXPECT(wait_until_event(1, 1));
    host_sleep_ns(30000000);
    EXPECT(atomic_load(&event_counts[1]) == 1);
    EXPECT(av_close(player) == 0);

    reset_fake();
    player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0 && av_enable(player, 1) == 0);
    EXPECT(av_start(player) == 0 && av_stop(player) == 0);
    EXPECT(atomic_load(&event_counts[1]) == 1);
    EXPECT(CODE(av_pause(player)) == E_FAILED);
    EXPECT(av_start(player) == 0);
    drain(player, &videos, &audios);
    EXPECT(videos == 30 && audios == 40);
    EXPECT(wait_until_event(1, 2));
    EXPECT(av_close(player) == 0);

    reset_fake();
    fake.has_audio = false;
    fake.read_delay_ms = 30;
    player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0 && av_start(player) == 0);
    EXPECT(wait_until_event(5, 1));
    EXPECT(av_pause(player) == 0);
    EXPECT(av_resume(player) == 0);
    EXPECT(av_resume(player) == 0);
    EXPECT(av_close(player) == 0);

    reset_fake();
    fake.has_audio = false;
    player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0);
    pause_during_start = player;
    EXPECT(av_start(player) == 0);
    EXPECT(CODE(pause_during_start_result) == E_FAILED);
    drain(player, &videos, &audios);
    EXPECT(videos == 30);
    EXPECT(av_close(player) == 0);

    reset_fake();
    fake.fail_begin = 1;
    player = open_ready(&init);
    EXPECT(av_enable(player, 0) == 0);
    EXPECT(CODE(av_start(player)) == E_FAILED);
    EXPECT(CODE(av_pause(player)) == E_FAILED);
    fake.count_error = 1;
    EXPECT(CODE(av_count(player)) == E_FAILED);
    EXPECT(av_close(player) == 0);
    EXPECT(atomic_load(&fake.wrong_sizes) == 0);
}

typedef struct {
    void *player;
    atomic_int stop;
    atomic_uint_fast64_t calls;
} Stress;

static void *stress_reader(void *argument) {
    Stress *s = argument;
    uint64_t calls = 0;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        VideoInfo video;
        AudioInfo audio;
        av_video(s->player, &video);
        av_audio(s->player, &audio);
        av_active(s->player);
        av_time(s->player);
        calls += 4;
    }
    atomic_fetch_add(&s->calls, calls);
    return NULL;
}

static void stress_rounds(unsigned rounds, int print) {
    reset_fake();
    fake.video_frames = 500;
    fake.audio_frames = 500;
    InitData init = make_init();
    Stress s;
    memset(&s, 0, sizeof(s));
    s.player = av_init(&init);
    EXPECT(av_add(s.player, "/app0/movie.mp4") == 0);
    EXPECT(wait_until_event(2, 1));
    EXPECT(av_enable(s.player, 0) == 0 && av_enable(s.player, 1) == 0);
    HostThread readers[3];
    for (int i = 0; i < 3; ++i) host_thread_start(&readers[i], 0, stress_reader, &s);
    unsigned starts = 0;
    for (unsigned i = 0; i < rounds; ++i) {
        if (av_start(s.player) == 0) ++starts;
        host_sleep_ns(1000000 * (1 + i % 7));
        if (i % 3 == 0 && !av_pause(s.player)) {
            host_sleep_ns(1000000);
            av_resume(s.player);
        }
        av_stop(s.player);
    }
    atomic_store(&s.stop, 1);
    for (int i = 0; i < 3; ++i) host_thread_join(readers[i]);
    EXPECT(starts > 0);
    EXPECT(av_close(s.player) == 0);
    EXPECT(atomic_load(&live_textures) == 0 && atomic_load(&live_memory) == 0);
    EXPECT(__atomic_load_n(&fake.packets_live, __ATOMIC_RELAXED) == 0);
    if (print) printf("stress: %u starts of %u rounds, %llu calls from 3 threads\n", starts, rounds, (unsigned long long)atomic_load(&s.calls));
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 4096);
    bind();
    sys_av_player_set_decoder(&fake_decoder);
    if (argc > 1 && !strcmp(argv[1], "--stress")) {
        stress_rounds(argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 500, 1);
        return failures ? 1 : 0;
    }
    test_arguments();
    test_playback();
    test_stop_restart_and_pause();
    test_looping();
    test_auto_start();
    test_failures();
    test_edges();
    test_brazilian_opening();
    stress_rounds(40, 0);
    if (failures) return 1;
    puts("sys AvPlayer: PASS");
    return 0;
}
