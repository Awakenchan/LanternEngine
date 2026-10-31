// SPDX-License-Identifier: GPL-3.0-or-later
#include "av_player.h"
#include "sys.h"
#include "runtime.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define E_PARAMS ((int32_t)0x806A0001)
#define E_FAILED ((int32_t)0x806A0002)
#define E_NOT_SUPPORTED ((int32_t)0x806A0004)
#define WARN_LOOPING ((int32_t)0x806A00A1)

enum { EVENT_STOP = 1, EVENT_READY = 2, EVENT_PLAY = 3, EVENT_PAUSE = 4, EVENT_BUFFERING = 5, EVENT_WARNING = 0x20 };
enum { MAX_FRAMES = 32, AUDIO_BUFFER_BYTES = 8 * 4 * 1024, CONTROLLER_TICK_NS = 5000000, MAX_WARNINGS = 8 };
enum { VIDEO_QUEUE_FULL = 30, AUDIO_QUEUE_FULL = 8 };

typedef enum {
    STATE_INITIAL = 1,
    STATE_ADDING,
    STATE_READY,
    STATE_PLAY,
    STATE_STOP,
    STATE_END,
    STATE_PAUSE,
    STATE_BUFFERING,
    STATE_STARTING,
    STATE_ERROR,
} State;

typedef struct {
    void *object;
    void *(SYS_ABI *allocate)(void *object, uint32_t alignment, uint32_t size);
    void (SYS_ABI *deallocate)(void *object, void *memory);
    void *(SYS_ABI *allocate_texture)(void *object, uint32_t alignment, uint32_t size);
    void (SYS_ABI *deallocate_texture)(void *object, void *memory);
} Allocator;

typedef struct {
    void *object;
    void (SYS_ABI *call)(void *object, int32_t event, int32_t source, void *data);
} EventCall;

typedef struct {
    Allocator memory;
    SysAvFileCalls file;
    EventCall event;
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
    union {
        uint8_t bytes[16];
        struct {
            uint16_t channel_count;
            uint8_t reserved[2];
            uint32_t sample_rate, size;
            char language[4];
        } audio;
    } details;
} FrameInfo;

typedef struct {
    void *data;
    uint8_t reserved[4];
    uint64_t timestamp;
    union {
        uint8_t bytes[80];
        struct {
            uint32_t width, height;
            float aspect_ratio;
            uint8_t language[4];
            uint32_t framerate, crop_left, crop_right, crop_top, crop_bottom, pitch;
            uint8_t luma_bit_depth, chroma_bit_depth;
            bool full_range;
        } video;
    } details;
} FrameInfoEx;
_Static_assert(sizeof(InitData) == 120, "SceAvPlayerInitData");
_Static_assert(sizeof(FrameInfo) == 40, "SceAvPlayerFrameInfo");
_Static_assert(sizeof(FrameInfoEx) == 104, "SceAvPlayerFrameInfoEx");
_Static_assert(sizeof(SysAvStreamInfo) == 40, "SceAvPlayerStreamInfo");

typedef struct PacketNode {
    void *packet;
    struct PacketNode *next;
} PacketNode;

typedef struct {
    PacketNode *head, *tail;
    unsigned count;
} PacketQueue;

typedef struct {
    void *buffer;
    SysAvFrame info;
} ReadyFrame;

typedef struct {
    void *free_buffers[MAX_FRAMES];
    unsigned free_count, buffer_total;
    ReadyFrame frames[MAX_FRAMES];
    unsigned frame_head, frame_count;
    ReadyFrame current;
    PacketQueue packets;
    bool enabled, decoding, thread_exists;
} Track;

typedef struct Player {
    InitData init;
    char language[4];
    HostMutex lock;
    HostCond changed;
    HostMutex file_lock;
    SysAvMedia *media;
    State state, previous;
    bool add_pending, looping, stopping, closing, ended_input, paused, stop_sent, playing, threads_exist;
    bool sync_to_clock;
    bool brazilian_opening;
    int preferred_audio;
    int32_t warnings[MAX_WARNINGS];
    unsigned warning_count;
    Track tracks[2];
    uint32_t video_bytes;
    uint64_t start_ns, pause_at_ns, pause_total_ns, audio_clock_ns, audio_timestamp, duration;
    bool started_clock, audio_seen;
    HostThread controller, demuxer, decoders[2];
} Player;

static const SysAvDecoder *decoder;
static const SysAvFileCalls no_file;

void sys_av_player_set_decoder(const SysAvDecoder *value) {
    decoder = value;
}

static int32_t SYS_ABI file_open(void *object, const char *name) {
    Player *player = object;
    host_lock(&player->file_lock);
    int32_t result = player->init.file.open(player->init.file.object, name);
    host_unlock(&player->file_lock);
    return result;
}

static int32_t SYS_ABI file_close(void *object) {
    Player *player = object;
    host_lock(&player->file_lock);
    int32_t result = player->init.file.close(player->init.file.object);
    host_unlock(&player->file_lock);
    return result;
}

static int32_t SYS_ABI file_read(void *object, uint8_t *buffer, uint64_t position, uint32_t length) {
    Player *player = object;
    host_lock(&player->file_lock);
    int32_t result = player->init.file.read_offset(player->init.file.object, buffer, position, length);
    host_unlock(&player->file_lock);
    return result;
}

static uint64_t SYS_ABI file_size(void *object) {
    Player *player = object;
    host_lock(&player->file_lock);
    uint64_t result = player->init.file.size(player->init.file.object);
    host_unlock(&player->file_lock);
    return result;
}

static void wake_all(Player *player) {
    host_cond_broadcast(&player->changed);
}

static bool transition_allowed(const Player *player, State to) {
    State from = player->state;
    if (from == STATE_STOP || from == STATE_END || from == STATE_ERROR) return to != STATE_PLAY && to != STATE_PAUSE && to != STATE_BUFFERING;
    if (from == STATE_STARTING) return to != STATE_PAUSE && to != STATE_BUFFERING;
    if (from == STATE_PAUSE) return to != STATE_BUFFERING;
    return true;
}

static bool set_state(Player *player, State to) {
    if (!transition_allowed(player, to)) return false;
    player->previous = player->state;
    player->state = to;
    wake_all(player);
    return true;
}

static void emit(Player *player, int32_t event, void *data) {
    if (player->init.event.call) player->init.event.call(player->init.event.object, event, 0, data);
}

static void push_packet(PacketQueue *queue, void *packet) {
    PacketNode *node = malloc(sizeof(*node));
    node->packet = packet;
    node->next = NULL;
    if (queue->tail) queue->tail->next = node;
    else queue->head = node;
    queue->tail = node;
    ++queue->count;
}

static void *pop_packet(PacketQueue *queue) {
    PacketNode *node = queue->head;
    if (!node) return NULL;
    void *packet = node->packet;
    queue->head = node->next;
    if (!queue->head) queue->tail = NULL;
    --queue->count;
    free(node);
    return packet;
}

static void clear_packets(PacketQueue *queue) {
    for (void *packet; (packet = pop_packet(queue));) decoder->free_packet(packet);
}

static bool input_done(const Player *player) {
    return player->ended_input && !player->tracks[0].packets.count && !player->tracks[1].packets.count &&
           !player->tracks[0].frame_count && !player->tracks[1].frame_count && !player->tracks[0].decoding &&
           !player->tracks[1].decoding;
}

static bool source_active(const Player *player) {
    return !input_done(player);
}

static void *decoder_thread(void *argument);
static void *demuxer_thread(void *argument);

static void push_frame(Track *track, void *buffer, const SysAvFrame *info) {
    track->frames[(track->frame_head + track->frame_count++) % MAX_FRAMES] = (ReadyFrame){buffer, *info};
}

typedef struct {
    Player *player;
    int kind;
} DecoderStart;

static void *decoder_thread(void *argument) {
    DecoderStart start = *(DecoderStart *)argument;
    free(argument);
    Player *player = start.player;
    Track *track = &player->tracks[start.kind];
    runtime_thread_attach_host("bb:hle");
    bool flushed = false;
    host_lock(&player->lock);
    while (!player->stopping) {
        void *packet = pop_packet(&track->packets);
        if (!packet) {
            if (!player->ended_input) {
                host_cond_wait(&player->changed, &player->lock);
                continue;
            }
            if (flushed) break;
            flushed = true;
        }
        host_unlock(&player->lock);
        int sent = decoder->send_packet(player->media, start.kind, packet);
        if (packet) decoder->free_packet(packet);
        host_lock(&player->lock);
        if (sent < 0) {
            set_state(player, STATE_ERROR);
            break;
        }
        int result = SYS_AV_FRAME;
        while (result == SYS_AV_FRAME && !player->stopping) {
            while (!track->free_count && !player->stopping) host_cond_wait(&player->changed, &player->lock);
            if (player->stopping) break;
            void *buffer = track->free_buffers[--track->free_count];
            uint32_t bytes = start.kind == SYS_AV_VIDEO ? player->video_bytes : AUDIO_BUFFER_BYTES;
            SysAvFrame frame;
            memset(&frame, 0, sizeof(frame));
            host_unlock(&player->lock);
            result = decoder->receive_frame(player->media, start.kind, buffer, bytes, &frame);
            host_lock(&player->lock);
            if (result == SYS_AV_FRAME) {
                push_frame(track, buffer, &frame);
                wake_all(player);
            } else {
                track->free_buffers[track->free_count++] = buffer;
            }
        }
        if (result == SYS_AV_ERROR) {
            set_state(player, STATE_ERROR);
            break;
        }
        if (result == SYS_AV_END) break;
    }
    track->decoding = false;
    wake_all(player);
    host_unlock(&player->lock);
    return NULL;
}

static bool queues_full(const Player *player) {
    const Track *video = &player->tracks[SYS_AV_VIDEO], *audio = &player->tracks[SYS_AV_AUDIO];
    return (!video->enabled || video->packets.count > VIDEO_QUEUE_FULL) && (!audio->enabled || audio->packets.count > AUDIO_QUEUE_FULL);
}

static void *demuxer_thread(void *argument) {
    Player *player = argument;
    runtime_thread_attach_host("bb:hle");
    host_lock(&player->lock);
    for (;;) {
        while (!player->stopping && queues_full(player)) host_cond_timedwait(&player->changed, &player->lock, CONTROLLER_TICK_NS);
        if (player->stopping) break;
        host_unlock(&player->lock);
        void *packet = NULL;
        int kind = -1;
        int read = decoder->read_packet(player->media, &packet, &kind);
        host_lock(&player->lock);
        if (read < 0) {
            set_state(player, STATE_ERROR);
            host_unlock(&player->lock);
            return NULL;
        }
        if (read == 0) {
            if (!player->looping) break;
            if (player->warning_count < MAX_WARNINGS) player->warnings[player->warning_count++] = WARN_LOOPING;
            host_unlock(&player->lock);
            decoder->rewind(player->media);
            host_lock(&player->lock);
            continue;
        }
        if (player->stopping) {
            decoder->free_packet(packet);
            break;
        }
        if (kind == SYS_AV_VIDEO || kind == SYS_AV_AUDIO) {
            push_packet(&player->tracks[kind].packets, packet);
            wake_all(player);
        } else {
            decoder->free_packet(packet);
        }
    }
    player->ended_input = true;
    wake_all(player);
    while (player->tracks[0].decoding || player->tracks[1].decoding) host_cond_wait(&player->changed, &player->lock);
    if (!player->stopping) {
        player->playing = false;
        set_state(player, STATE_END);
    }
    host_unlock(&player->lock);
    return NULL;
}

static void join_workers(Player *player) {
    if (!player->threads_exist) return;
    player->threads_exist = false;
    host_unlock(&player->lock);
    host_thread_join(player->demuxer);
    for (int kind = 0; kind < 2; ++kind) {
        if (player->tracks[kind].thread_exists) host_thread_join(player->decoders[kind]);
        player->tracks[kind].thread_exists = false;
    }
    host_lock(&player->lock);
}

static void free_track_memory(Player *player, Track *track, int kind) {
    void *buffers[MAX_FRAMES * 2];
    unsigned count = 0;
    for (unsigned i = 0; i < track->free_count; ++i) buffers[count++] = track->free_buffers[i];
    for (unsigned i = 0; i < track->frame_count; ++i) buffers[count++] = track->frames[(track->frame_head + i) % MAX_FRAMES].buffer;
    if (track->current.buffer) buffers[count++] = track->current.buffer;
    track->free_count = track->frame_count = track->frame_head = track->buffer_total = 0;
    track->current.buffer = NULL;
    clear_packets(&track->packets);
    host_unlock(&player->lock);
    for (unsigned i = 0; i < count; ++i) {
        if (kind == SYS_AV_VIDEO) player->init.memory.deallocate_texture(player->init.memory.object, buffers[i]);
        else player->init.memory.deallocate(player->init.memory.object, buffers[i]);
    }
    host_lock(&player->lock);
}

static void source_teardown(Player *player) {
    player->stopping = true;
    wake_all(player);
    join_workers(player);
    decoder->end(player->media);
    free_track_memory(player, &player->tracks[SYS_AV_VIDEO], SYS_AV_VIDEO);
    free_track_memory(player, &player->tracks[SYS_AV_AUDIO], SYS_AV_AUDIO);
    player->stopping = false;
    player->playing = false;
    player->paused = false;
    player->ended_input = false;
    player->started_clock = false;
    player->audio_seen = false;
    player->pause_total_ns = 0;
}

static bool source_stop(Player *player) {
    if (!player->playing) return false;
    source_teardown(player);
    return true;
}

static bool start_thread(HostThread *thread, void *(*entry)(void *), void *argument) {
    return !host_thread_start(thread, 0, entry, argument);
}

static bool source_start(Player *player) {
    Track *video = &player->tracks[SYS_AV_VIDEO], *audio = &player->tracks[SYS_AV_AUDIO];
    if (!video->enabled && !audio->enabled) return false;
    join_workers(player);
    decoder->end(player->media);
    if (!decoder->begin(player->media, &player->video_bytes)) return false;
    unsigned framebuffers = (unsigned)player->init.video_framebuffers;
    if (player->init.video_framebuffers < 2) framebuffers = 2;
    if (framebuffers > 16) framebuffers = 16;
    host_unlock(&player->lock);
    void *video_memory[MAX_FRAMES], *audio_memory[MAX_FRAMES];
    if (video->enabled)
        for (unsigned i = 0; i < framebuffers; ++i)
            video_memory[i] = player->init.memory.allocate_texture(player->init.memory.object, 0x100, player->video_bytes);
    if (audio->enabled)
        for (unsigned i = 0; i < framebuffers * 2; ++i)
            audio_memory[i] = player->init.memory.allocate(player->init.memory.object, 0x10, AUDIO_BUFFER_BYTES);
    host_lock(&player->lock);
    if (video->enabled)
        for (unsigned i = 0; i < framebuffers; ++i) video->free_buffers[video->free_count++] = video_memory[i];
    if (audio->enabled)
        for (unsigned i = 0; i < framebuffers * 2; ++i) audio->free_buffers[audio->free_count++] = audio_memory[i];
    video->buffer_total = video->free_count;
    audio->buffer_total = audio->free_count;
    player->ended_input = false;
    player->playing = true;
    player->threads_exist = true;
    player->stopping = false;
    player->start_ns = host_monotonic_ns();
    player->started_clock = true;
    for (int kind = 0; kind < 2; ++kind) {
        if (!player->tracks[kind].enabled) continue;
        player->tracks[kind].decoding = true;
        player->tracks[kind].thread_exists = true;
        DecoderStart *start = malloc(sizeof(*start));
        start->player = player;
        start->kind = kind;
        start_thread(&player->decoders[kind], decoder_thread, start);
    }
    start_thread(&player->demuxer, demuxer_thread, player);
    return true;
}

static uint64_t current_time_held(const Player *player) {
    if (!player->started_clock) return 0;
    if (!source_active(player)) return player->duration;
    uint64_t now = player->paused ? player->pause_at_ns : host_monotonic_ns();
    if (player->tracks[SYS_AV_AUDIO].enabled) {
        uint64_t from = player->audio_seen ? player->audio_clock_ns : now;
        int64_t elapsed = (int64_t)(now - from - player->pause_total_ns) / 1000000;
        return (uint64_t)((int64_t)(player->audio_seen ? player->audio_timestamp : 0) + elapsed);
    }
    int64_t elapsed = (int64_t)(now - player->start_ns - player->pause_total_ns) / 1000000;
    if (elapsed <= 0) return 0;
    return player->duration && (uint64_t)elapsed > player->duration ? player->duration : (uint64_t)elapsed;
}

static void pick_streams_and_start(Player *player);

static void player_notify(Player *player, int32_t event) {
    if (player->init.auto_start || !player->init.event.call) {
        if (event == EVENT_READY) {
            pick_streams_and_start(player);
            return;
        }
    }
    emit(player, event, NULL);
}

static bool player_start_held(Player *player) {
    bool can = player->state == STATE_READY || player->state == STATE_STOP;
    if (!can && player->media) {
        if (source_stop(player) && set_state(player, STATE_STOP)) {
            player->stop_sent = true;
            host_unlock(&player->lock);
            player_notify(player, EVENT_STOP);
            host_lock(&player->lock);
            can = true;
        }
    }
    if (!can) return false;
    player->stop_sent = false;
    set_state(player, STATE_STARTING);
    if (!source_start(player)) {
        set_state(player, STATE_ERROR);
        return false;
    }
    set_state(player, STATE_PLAY);
    return true;
}

static int brazilian_opening_audio(Player *player) {
    if (!player->brazilian_opening) return -1;
    int count = decoder->stream_count(player->media);
    int original_layout = count == 9;
    for (int i = 0; i < count; ++i) {
        SysAvStreamInfo info = {0};
        if (!decoder->stream_info(player->media, (uint32_t)i, &info)) return -1;
        if (info.type == SYS_AV_AUDIO && (!memcmp(info.details.audio.language, "por", 3) ||
                                          !memcmp(info.details.audio.language, "ptb", 3))) return i;
        if (i == 0 ? info.type != SYS_AV_VIDEO : info.type != SYS_AV_AUDIO) original_layout = 0;
        if (i && info.details.audio.language[0] && memcmp(info.details.audio.language, "und", 3)) original_layout = 0;
    }
    /* The original CUSA03173 opening has one video followed by eight untagged audio
     * tracks. Brazilian Portuguese is the eighth audio track. No movie is repacked. */
    return original_layout ? 8 : -1;
}

static int stream_info(Player *player, uint32_t index, SysAvStreamInfo *info) {
    if (!decoder->stream_info(player->media, index, info)) return 0;
    if (info->type == SYS_AV_AUDIO && player->preferred_audio >= 0)
        return decoder->stream_info(player->media, (uint32_t)player->preferred_audio, info);
    return 1;
}

static int enable_stream(Player *player, uint32_t index) {
    SysAvStreamInfo info = {0};
    if (player->preferred_audio >= 0 && decoder->stream_info(player->media, index, &info) && info.type == SYS_AV_AUDIO)
        index = (uint32_t)player->preferred_audio;
    return decoder->enable_stream(player->media, index);
}

static void pick_streams_and_start(Player *player) {
    host_lock(&player->lock);
    int chosen[3] = {-1, -1, -1};
    int count = player->media ? decoder->stream_count(player->media) : 0;
    bool stop = count <= 0;
    for (int i = 0; i < count; ++i) {
        SysAvStreamInfo info;
        memset(&info, 0, sizeof(info));
        if (!stream_info(player, (uint32_t)i, &info)) {
            stop = true;
            break;
        }
        const char *language = info.type == SYS_AV_VIDEO ? info.details.video.language
                               : info.type == SYS_AV_AUDIO ? info.details.audio.language
                               : info.details.text.language;
        if (info.type > SYS_AV_TEXT) continue;
        if (chosen[info.type] == -1) chosen[info.type] = i;
        if (player->language[0] && !strncmp(player->language, language, 4)) chosen[info.type] = i;
    }
    bool started = false;
    if (!stop) {
        for (int type = 0; type < 3; ++type) {
            if (chosen[type] < 0) continue;
            int kind = enable_stream(player, (uint32_t)chosen[type]);
            if (kind == SYS_AV_VIDEO || kind == SYS_AV_AUDIO) player->tracks[kind].enabled = true;
        }
        started = player_start_held(player);
    } else if (source_stop(player)) {
        set_state(player, STATE_STOP);
        player->stop_sent = true;
    }
    host_unlock(&player->lock);
    if (started) emit(player, EVENT_PLAY, NULL);
    else if (stop) emit(player, EVENT_STOP, NULL);
}

static void *controller_thread(void *argument) {
    Player *player = argument;
    runtime_thread_attach_host("bb:hle");
    host_lock(&player->lock);
    while (!player->closing) {
        if (player->add_pending) {
            player->add_pending = false;
            SysAvMedia *media = player->media;
            host_unlock(&player->lock);
            bool found = media && decoder->find_streams(media);
            host_lock(&player->lock);
            if (found) {
                player->preferred_audio = brazilian_opening_audio(player);
                player->duration = decoder->duration(player->media);
                set_state(player, STATE_READY);
                host_unlock(&player->lock);
                player_notify(player, EVENT_READY);
                host_lock(&player->lock);
            } else {
                if (player->warning_count < MAX_WARNINGS) player->warnings[player->warning_count++] = E_NOT_SUPPORTED;
                set_state(player, STATE_ERROR);
            }
            continue;
        }
        if (player->warning_count) {
            int32_t warning = player->warnings[0];
            memmove(player->warnings, player->warnings + 1, --player->warning_count * sizeof(*player->warnings));
            host_unlock(&player->lock);
            emit(player, EVENT_WARNING, &warning);
            host_lock(&player->lock);
            continue;
        }
        int32_t event = 0;
        if (player->state == STATE_END) {
            if (!source_active(player) && !player->stop_sent) {
                player->stop_sent = true;
                event = EVENT_STOP;
            }
        } else if (player->state == STATE_BUFFERING) {
            if (player->tracks[SYS_AV_VIDEO].packets.count > 10 || player->ended_input) {
                set_state(player, STATE_PLAY);
                event = EVENT_PLAY;
            }
        } else if (player->state == STATE_PLAY) {
            if (!player->tracks[SYS_AV_VIDEO].packets.count && !player->ended_input) {
                set_state(player, STATE_BUFFERING);
                event = EVENT_BUFFERING;
            }
        }
        if (event) {
            host_unlock(&player->lock);
            emit(player, event, NULL);
            host_lock(&player->lock);
            continue;
        }
        if (player->state == STATE_END || player->state == STATE_BUFFERING || player->state == STATE_PLAY)
            host_cond_timedwait(&player->changed, &player->lock, CONTROLLER_TICK_NS);
        else
            host_cond_wait(&player->changed, &player->lock);
    }
    host_unlock(&player->lock);
    return NULL;
}

static SYS_ABI void *av_init(InitData *data) {
    if (!data || !data->memory.allocate || !data->memory.allocate_texture || !data->memory.deallocate || !data->memory.deallocate_texture) return NULL;
    Player *player = calloc(1, sizeof(*player));
    if (!player) return NULL;
    player->init = *data;
    player->preferred_audio = -1;
    if (data->default_language) memcpy(player->language, data->default_language, sizeof(player->language));
    host_mutex_init(&player->lock);
    host_mutex_init(&player->file_lock);
    host_cond_init(&player->changed);
    player->state = STATE_INITIAL;
    player->previous = STATE_INITIAL;
    player->sync_to_clock = true;
    host_thread_start(&player->controller, 0, controller_thread, player);
    return player;
}

static SYS_ABI int32_t av_post_init(Player *player, const void *data) {
    return !player || !data ? E_PARAMS : 0;
}

static int source_is_stream(const char *path) {
    const char *end = strpbrk(path, "?#");
    size_t length = strstr(path, "://") && end ? (size_t)(end - path) : strlen(path);
    for (size_t i = length; i > 0; --i) {
        if (path[i - 1] != '.') continue;
        char extension[8] = {0};
        for (size_t j = 0; j + 1 < sizeof(extension) && i + j < length && path[i + j] != '/'; ++j) {
            char c = path[i + j];
            extension[j] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
        }
        return !strcmp(extension, "m3u8");
    }
    return 0;
}

static SYS_ABI int32_t av_add_source(Player *player, const char *path) {
    if (!player) return E_PARAMS;
    if (!path || !*path) return E_FAILED;
    if (source_is_stream(path)) return E_NOT_SUPPORTED;
    host_lock(&player->lock);
    if (player->media || !decoder) {
        host_unlock(&player->lock);
        return E_FAILED;
    }
    const SysAvFileCalls *file = &no_file;
    SysAvFileCalls guarded;
    if (player->init.file.open && player->init.file.close && player->init.file.read_offset && player->init.file.size) {
        guarded = (SysAvFileCalls){player, file_open, file_close, file_read, file_size};
        file = &guarded;
    }
    host_unlock(&player->lock);
    SysAvMedia *media = decoder->open(path, file);
    host_lock(&player->lock);
    if (!media) {
        set_state(player, STATE_ERROR);
        host_unlock(&player->lock);
        return E_FAILED;
    }
    player->media = media;
    const char *language = getenv("BB_LANGUAGE");
    const char *name = strrchr(path, '/');
    player->brazilian_opening = language && !strcmp(language, "17") &&
                               !strcmp(name ? name + 1 : path, "sprj_opening.mp4");
    set_state(player, STATE_ADDING);
    player->add_pending = true;
    wake_all(player);
    host_unlock(&player->lock);
    return 0;
}

static SYS_ABI int32_t av_close(Player *player) {
    if (!player) return E_PARAMS;
    host_lock(&player->lock);
    if (player->media) source_teardown(player);
    player->closing = true;
    wake_all(player);
    join_workers(player);
    host_unlock(&player->lock);
    host_thread_join(player->controller);
    if (player->media) decoder->close(player->media);
    free(player);
    return 0;
}

static SYS_ABI int32_t av_stream_count(Player *player) {
    if (!player) return E_PARAMS;
    host_lock(&player->lock);
    int32_t count = player->media ? decoder->stream_count(player->media) : E_FAILED;
    host_unlock(&player->lock);
    return count < 0 ? E_FAILED : count;
}

static SYS_ABI int32_t av_stream_info(Player *player, uint32_t index, SysAvStreamInfo *info) {
    if (!player || !info) return E_PARAMS;
    host_lock(&player->lock);
    int ok = player->media && stream_info(player, index, info);
    host_unlock(&player->lock);
    return ok ? 0 : E_FAILED;
}

static SYS_ABI int32_t av_enable_stream(Player *player, uint32_t index) {
    if (!player) return E_PARAMS;
    host_lock(&player->lock);
    int kind = player->media ? enable_stream(player, index) : -1;
    if (kind == SYS_AV_VIDEO || kind == SYS_AV_AUDIO) player->tracks[kind].enabled = true;
    host_unlock(&player->lock);
    return kind < 0 ? E_FAILED : 0;
}

static SYS_ABI int32_t av_start(Player *player) {
    if (!player) return E_PARAMS;
    host_lock(&player->lock);
    bool started = player->media && player_start_held(player);
    host_unlock(&player->lock);
    if (started) emit(player, EVENT_PLAY, NULL);
    return started ? 0 : E_FAILED;
}

static SYS_ABI int32_t av_stop(Player *player) {
    if (!player) return E_PARAMS;
    host_lock(&player->lock);
    bool stopped = player->media && player->state != STATE_STOP && source_stop(player) && set_state(player, STATE_STOP);
    if (stopped) player->stop_sent = true;
    host_unlock(&player->lock);
    if (stopped) emit(player, EVENT_STOP, NULL);
    return stopped ? 0 : E_FAILED;
}

static SYS_ABI int32_t av_pause(Player *player) {
    if (!player) return E_PARAMS;
    host_lock(&player->lock);
    if (player->state == STATE_END) {
        host_unlock(&player->lock);
        return 0;
    }
    if (!player->media || player->state == STATE_PAUSE || player->state == STATE_READY || player->state == STATE_INITIAL || player->state == STATE_ADDING) {
        host_unlock(&player->lock);
        return E_FAILED;
    }
    bool changed = set_state(player, STATE_PAUSE);
    if (changed) {
        player->pause_at_ns = host_monotonic_ns();
        player->paused = true;
    }
    host_unlock(&player->lock);
    if (!changed) return E_FAILED;
    emit(player, EVENT_PAUSE, NULL);
    return 0;
}

static SYS_ABI int32_t av_resume(Player *player) {
    if (!player) return E_PARAMS;
    host_lock(&player->lock);
    if (!player->media || player->state != STATE_PAUSE) {
        host_unlock(&player->lock);
        return E_FAILED;
    }
    player->pause_total_ns += host_monotonic_ns() - player->pause_at_ns;
    player->paused = false;
    State back = player->previous;
    set_state(player, back);
    host_unlock(&player->lock);
    int32_t event = back == STATE_PLAY ? EVENT_PLAY : back == STATE_STOP ? EVENT_STOP : back == STATE_BUFFERING ? EVENT_BUFFERING : back == STATE_READY ? EVENT_READY : 0;
    if (event) emit(player, event, NULL);
    return 0;
}

static SYS_ABI int32_t av_set_looping(Player *player, bool looping) {
    if (!player) return E_PARAMS;
    host_lock(&player->lock);
    bool ok = player->media != NULL;
    if (ok) player->looping = looping;
    host_unlock(&player->lock);
    return ok ? 0 : E_FAILED;
}

static SYS_ABI uint64_t av_current_time(Player *player) {
    if (!player) return (uint64_t)(int64_t)E_PARAMS;
    host_lock(&player->lock);
    uint64_t time = player->media ? current_time_held(player) : 0;
    host_unlock(&player->lock);
    return time;
}

static SYS_ABI bool av_is_active(Player *player) {
    if (!player) return false;
    host_lock(&player->lock);
    bool active = player->media && player->state != STATE_STOP && player->state != STATE_ERROR && source_active(player);
    host_unlock(&player->lock);
    return active;
}

static bool take_frame(Player *player, Track *track, ReadyFrame *out) {
    if (track->current.buffer) {
        track->free_buffers[track->free_count++] = track->current.buffer;
        track->current.buffer = NULL;
    }
    *out = track->frames[track->frame_head];
    track->frame_head = (track->frame_head + 1) % MAX_FRAMES;
    --track->frame_count;
    track->current = *out;
    wake_all(player);
    return true;
}

static SYS_ABI bool av_get_video_ex(Player *player, FrameInfoEx *info) {
    if (!player || !info) return false;
    host_lock(&player->lock);
    Track *track = &player->tracks[SYS_AV_VIDEO];
    bool ready = player->media && !player->paused && source_active(player) && track->frame_count != 0;
    if (ready && track->frames[track->frame_head].info.timestamp > current_time_held(player)) ready = false;
    if (ready) {
        ReadyFrame frame;
        take_frame(player, track, &frame);
        memset(info, 0, sizeof(*info));
        info->data = frame.buffer;
        info->timestamp = frame.info.timestamp;
        info->details.video.width = frame.info.width;
        info->details.video.height = frame.info.height;
        info->details.video.aspect_ratio = frame.info.aspect_ratio;
        info->details.video.crop_left = frame.info.crop_left;
        info->details.video.crop_right = frame.info.crop_right;
        info->details.video.crop_top = frame.info.crop_top;
        info->details.video.crop_bottom = frame.info.crop_bottom;
        info->details.video.pitch = frame.info.pitch;
        info->details.video.luma_bit_depth = 8;
        info->details.video.chroma_bit_depth = 8;
    }
    host_unlock(&player->lock);
    return ready;
}

static SYS_ABI bool av_get_audio(Player *player, FrameInfo *info) {
    if (!player || !info) return false;
    host_lock(&player->lock);
    Track *track = &player->tracks[SYS_AV_AUDIO];
    bool ready = player->media && !player->paused && source_active(player) && track->frame_count != 0;
    if (ready) {
        ReadyFrame frame;
        take_frame(player, track, &frame);
        player->audio_timestamp = frame.info.timestamp;
        player->audio_clock_ns = host_monotonic_ns();
        player->audio_seen = true;
        player->pause_total_ns = 0;
        memset(info, 0, sizeof(*info));
        info->data = frame.buffer;
        info->timestamp = frame.info.timestamp;
        info->details.audio.sample_rate = frame.info.sample_rate;
        info->details.audio.size = frame.info.bytes;
        info->details.audio.channel_count = frame.info.channel_count;
    }
    host_unlock(&player->lock);
    return ready;
}

static const SysExport av_exports[] = {
    {"aS66RI0gGgo", "sceAvPlayerInit", av_init},
    {"HD1YKVU26-M", "sceAvPlayerPostInit", av_post_init},
    {"KMcEa+rHsIo", "sceAvPlayerAddSource", av_add_source},
    {"OVths0xGfho", "sceAvPlayerSetLooping", av_set_looping},
    {"NkJwDzKmIlw", "sceAvPlayerClose", av_close},
    {"ET4Gr-Uu07s", "sceAvPlayerStart", av_start},
    {"ZC17w3vB5Lo", "sceAvPlayerStop", av_stop},
    {"UbQoYawOsfY", "sceAvPlayerIsActive", av_is_active},
    {"9y5v+fGN4Wk", "sceAvPlayerPause", av_pause},
    {"w5moABNwnRY", "sceAvPlayerResume", av_resume},
    {"JdksQu8pNdQ", "sceAvPlayerGetVideoDataEx", av_get_video_ex},
    {"wwM99gjFf1Y", "sceAvPlayerCurrentTime", av_current_time},
    {"hdTyRzCXQeQ", "sceAvPlayerStreamCount", av_stream_count},
    {"d8FcbzfAdQw", "sceAvPlayerGetStreamInfo", av_stream_info},
    {"ODJK2sn9w4A", "sceAvPlayerEnableStream", av_enable_stream},
    {"Wnp1OVcrZgk", "sceAvPlayerGetAudioData", av_get_audio},
};
SYS_LIBRARY(sys_av_player, "libSceAvPlayer", av_exports);
