#ifndef _BS_AUDIO_SYSTEM_H_
#define _BS_AUDIO_SYSTEM_H_

#include "common.h"
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "data_win.h"
#include "file_system.h"
#include "utils.h"

typedef struct {
    float current;
    float start;
    float target;
    float timeRemaining;
    float totalTime;
} AudioGroupGain;

// ===[ AudioSystem Vtable ]===

typedef struct AudioSystem AudioSystem;

typedef struct {
    void (*init)(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem);
    void (*destroy)(AudioSystem* audio);
    void (*update)(AudioSystem* audio, float deltaTime);
    int32_t (*playSound)(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop);
    void (*setSoundSpatial)(AudioSystem* audio, int32_t instanceId, float x, float y, float z, float ref, float max, float factor);
    void (*setListenerPosition)(AudioSystem* audio, float x, float y, float z);
    void (*stopSound)(AudioSystem* audio, int32_t soundOrInstance);
    void (*stopAll)(AudioSystem* audio);
    bool (*isPlaying)(AudioSystem* audio, int32_t soundOrInstance);
    void (*pauseSound)(AudioSystem* audio, int32_t soundOrInstance);
    void (*resumeSound)(AudioSystem* audio, int32_t soundOrInstance);
    void (*pauseAll)(AudioSystem* audio);
    void (*resumeAll)(AudioSystem* audio);
    void (*suspend)(AudioSystem* audio);
    void (*resume)(AudioSystem* audio);
    void (*setSoundGain)(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs);
    float (*getSoundGain)(AudioSystem* audio, int32_t soundOrInstance);
    void (*setSoundPitch)(AudioSystem* audio, int32_t soundOrInstance, float pitch);
    float (*getSoundPitch)(AudioSystem* audio, int32_t soundOrInstance);
    float (*getTrackPosition)(AudioSystem* audio, int32_t soundOrInstance);
    void (*setTrackPosition)(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds);
    // Total length of a sound in seconds. Accepts either a SOND index or an active sound instance id.
    // Returns 0.0 if unknown (e.g. stream not yet loaded or invalid index).
    float (*getSoundLength)(AudioSystem* audio, int32_t soundOrInstance);
    void (*setMasterGain)(AudioSystem* audio, float gain);
    void (*setMasterGainForListener)(AudioSystem* audio, float gain, int32_t listenerId);
    void (*setChannelCount)(AudioSystem* audio, int32_t count);
    void (*setGroupGain)(AudioSystem* audio, int32_t groupIndex, float gain, uint32_t timeMs);
    void (*groupLoad)(AudioSystem* audio, int32_t groupIndex);
    bool (*groupIsLoaded)(AudioSystem* audio, int32_t groupIndex);
    int32_t (*createStream)(AudioSystem* audio, const char* filename);
    bool (*destroyStream)(AudioSystem* audio, int32_t streamIndex);
    // Optional (nullptr: ignored). The gain of the emitter a sound instance plays on (audio_emitter_gain), on top of
    // the instance's own gain; audio_sound_get_gain does not include it.
    void (*setSoundEmitterGain)(AudioSystem* audio, int32_t instanceId, float gain);
} AudioSystemVtable;

// ===[ AudioSystem Base Struct ]===

struct AudioSystem {
    AudioSystemVtable* vtable;
    DataWin* dw;
    DataWin** audioGroups;
    float listenerX, listenerY, listenerZ;
    // audio_listener_orientation. All zero (a new system) means GameMaker's default: look at (0, 0, 1), up (0, 1, 0).
    float listenerAtX, listenerAtY, listenerAtZ, listenerUpX, listenerUpY, listenerUpZ;
    int32_t falloffModel; // audio_falloff_set_model; AUDIO_FALLOFF_NONE (0) in a new system, GameMaker's default
    AudioGroupGain* groupGains;
    uint32_t groupGainCount;
};

// audio_falloff_* values. GameMaker's numbering follows OpenAL's AL_*_DISTANCE models (0xD001-0xD006, in this order).
enum {
    AUDIO_FALLOFF_NONE = 0,
    AUDIO_FALLOFF_INVERSE_DISTANCE = 1,
    AUDIO_FALLOFF_INVERSE_DISTANCE_CLAMPED = 2,
    AUDIO_FALLOFF_LINEAR_DISTANCE = 3,
    AUDIO_FALLOFF_LINEAR_DISTANCE_CLAMPED = 4,
    AUDIO_FALLOFF_EXPONENT_DISTANCE = 5,
    AUDIO_FALLOFF_EXPONENT_DISTANCE_CLAMPED = 6,
};

// Distance gain of a positioned sound, by the formulas in GameMaker's manual (audio_falloff_set_model). The result is
// kept in 0..1, as OpenAL's default source gain limits do; unknown models (the newer "scaled" ones) attenuate like the
// clamped inverse model.
static inline float AudioSystem_falloffGain(int32_t model, float distance, float ref, float max, float factor) {
    if (ref < 0.0001f) ref = 0.0001f;
    float gain = 1.0f;
    switch (model) {
        case AUDIO_FALLOFF_NONE:
            return 1.0f;
        case AUDIO_FALLOFF_INVERSE_DISTANCE:
            gain = ref / (ref + factor * (distance - ref));
            break;
        case AUDIO_FALLOFF_LINEAR_DISTANCE:
        case AUDIO_FALLOFF_LINEAR_DISTANCE_CLAMPED:
            if (model == AUDIO_FALLOFF_LINEAR_DISTANCE_CLAMPED && distance < ref) distance = ref;
            if (distance > max) distance = max;
            gain = max > ref ? 1.0f - factor * (distance - ref) / (max - ref) : (distance <= ref ? 1.0f : 0.0f);
            break;
        case AUDIO_FALLOFF_EXPONENT_DISTANCE:
        case AUDIO_FALLOFF_EXPONENT_DISTANCE_CLAMPED:
            if (model == AUDIO_FALLOFF_EXPONENT_DISTANCE_CLAMPED) {
                if (distance < ref) distance = ref;
                if (distance > max) distance = max;
            }
            if (distance < 0.0001f) return 1.0f;
            gain = powf(distance / ref, -factor);
            break;
        default:
            if (distance < ref) distance = ref;
            if (distance > max) distance = max;
            gain = ref / (ref + factor * (distance - ref));
            break;
    }
    if (!(gain > 0.0f)) return 0.0f; // also NaN
    return gain < 1.0f ? gain : 1.0f;
}

// Where a sound at (x, y, z) sits between the listener's left (-1) and right (+1): the cosine of the angle between
// the listener-to-sound direction and the listener's right. Right is look-at x up, OpenAL's (right-handed) rule, so
// with the default orientation +x is on the LEFT. GameMaker's manual says the opposite, but both DELTARUNE scenes
// that pan compensate for exactly this (one mirrors the listener, one flips "up"), so they were tuned on a runtime
// that follows OpenAL. 0 when the sound is at the listener or straight ahead, behind, above or below it.
static inline float AudioSystem_panPosition(const AudioSystem* audio, float x, float y, float z) {
    float ax = audio->listenerAtX, ay = audio->listenerAtY, az = audio->listenerAtZ;
    float ux = audio->listenerUpX, uy = audio->listenerUpY, uz = audio->listenerUpZ;
    if (ax == 0.0f && ay == 0.0f && az == 0.0f && ux == 0.0f && uy == 0.0f && uz == 0.0f) {
        az = 1.0f;
        uy = 1.0f;
    }
    float rx = ay * uz - az * uy, ry = az * ux - ax * uz, rz = ax * uy - ay * ux;
    float rLength = sqrtf(rx * rx + ry * ry + rz * rz);
    float dx = x - audio->listenerX, dy = y - audio->listenerY, dz = z - audio->listenerZ;
    float dLength = sqrtf(dx * dx + dy * dy + dz * dz);
    if (rLength < 1e-6f || dLength < 1e-6f) return 0.0f;
    float pan = (dx * rx + dy * ry + dz * rz) / (rLength * dLength);
    return pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
}

static inline int32_t AudioSystem_soundGroup(const AudioSystem* audio, int32_t soundIndex) {
    if (audio == nullptr || audio->dw == nullptr || soundIndex < 0 ||
        (uint32_t)soundIndex >= audio->dw->sond.count) return 0;
    int32_t group = audio->dw->sond.sounds[soundIndex].audioGroup;
    return group >= 0 ? group : 0;
}

static inline float AudioSystem_getGroupGain(const AudioSystem* audio, int32_t groupIndex) {
    if (audio == nullptr || groupIndex < 0 || (uint32_t)groupIndex >= audio->groupGainCount) return 1.0f;
    return audio->groupGains[groupIndex].current;
}

static inline float AudioSystem_soundGroupGain(const AudioSystem* audio, int32_t soundIndex) {
    return AudioSystem_getGroupGain(audio, AudioSystem_soundGroup(audio, soundIndex));
}

static inline void AudioSystem_setGroupGain(AudioSystem* audio, int32_t groupIndex, float gain, uint32_t timeMs) {
    if (audio == nullptr || groupIndex < 0) return;
    uint32_t count = audio->dw != nullptr ? audio->dw->agrp.count : 0;
    if (count == 0) count = 1; // Legacy games have only the default audio group.
    if ((uint32_t)groupIndex >= count) return;

    if (audio->groupGains == nullptr) {
        audio->groupGains = (AudioGroupGain*)safeCalloc(count, sizeof(AudioGroupGain));
        audio->groupGainCount = count;
        for (uint32_t i = 0; i < count; i++) {
            audio->groupGains[i].current = 1.0f;
            audio->groupGains[i].start = 1.0f;
            audio->groupGains[i].target = 1.0f;
        }
    }

    AudioGroupGain* group = &audio->groupGains[groupIndex];
    group->start = group->current;
    group->target = gain;
    group->totalTime = (float)timeMs / 1000.0f;
    group->timeRemaining = group->totalTime;
    if (timeMs == 0) group->current = gain;
}

static inline bool AudioSystem_updateGroupGains(AudioSystem* audio, float deltaTime) {
    if (audio == nullptr || deltaTime <= 0) return false;
    bool changed = false;
    for (uint32_t i = 0; i < audio->groupGainCount; i++) {
        AudioGroupGain* group = &audio->groupGains[i];
        if (group->timeRemaining <= 0) continue;
        group->timeRemaining -= deltaTime;
        if (group->timeRemaining <= 0) {
            group->timeRemaining = 0;
            group->current = group->target;
        } else {
            group->current = group->start + (group->target - group->start) *
                (1.0f - group->timeRemaining / group->totalTime);
        }
        changed = true;
    }
    return changed;
}

#endif /* _BS_AUDIO_SYSTEM_H_ */
