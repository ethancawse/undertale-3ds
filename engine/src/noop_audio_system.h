#ifndef _BS_NOOP_AUDIO_SYSTEM_H_
#define _BS_NOOP_AUDIO_SYSTEM_H_

#include "common.h"
#include "audio_system.h"

// A no-op audio system that silently ignores all audio calls.
// Useful for headless mode or platforms without audio support.

typedef struct {
    int32_t key; // sound asset, stream or instance id
    float value;
} NoopGainEntry;

typedef struct {
    AudioSystem base;
    // Gains set through audio_sound_gain, so audio_sound_get_gain reads them back.
    // Game code fades music by reading the gain, lowering it and waiting for it
    // to reach zero (Undertale's Asriel fight); a constant 1.0 would loop forever.
    NoopGainEntry* gains; // stb_ds hashmap
} NoopAudioSystem;

NoopAudioSystem* NoopAudioSystem_create(void);

#endif /* _BS_NOOP_AUDIO_SYSTEM_H_ */
