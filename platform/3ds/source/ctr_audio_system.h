#ifndef CTRPORT_CTR_AUDIO_SYSTEM_H
#define CTRPORT_CTR_AUDIO_SYSTEM_H

// AudioSystem on libctru's NDSP.
//
// Sound effects (embedded WAV) are decoded once into linear memory, kept in an
// LRU cache and played straight from there by the DSP. Music (external .ogg,
// compressed OGG in AUDO, create_stream files) is decoded on a separate thread
// into a few wave buffers per stream. If NDSP cannot start (usually because
// sdmc:/3ds/dspfirm.cdc is missing) the system runs silently: every call still
// works and sounds "play" for their real length, so scripts that wait for a
// sound to end keep working.
//
// Embedded sounds are read from data.win on a loader thread the first time
// they play, so a cache miss doesn't stall the game; the voice starts once its
// data arrives.
//
// The caller casts the result to AudioSystem* and hands it to Runner_create.

#include <stdint.h>

typedef struct CtrAudioSystem CtrAudioSystem;

CtrAudioSystem* CtrAudioSystem_create(void);

// Full path of the data.win being run, for the loader thread's own file
// handle. Call before Runner_create; without it every load is synchronous.
void CtrAudioSystem_setDataWinPath(CtrAudioSystem* ca, const char* path);

// Music stream underruns (a stream ran out of decoded audio) since the system started.
uint32_t CtrAudioSystem_underruns(CtrAudioSystem* ca);

// Frees cached sound effects to give linear memory to something that needs
// it more (texture tiles, which cost more to load again), until `bytes` are
// freed or none can go: the cache keeps 2 MB, and the sounds playing or
// played in the last 10 seconds. A freed sound plays again the way it did
// the first time. Returns the bytes freed. Game thread.
uint32_t CtrAudioSystem_shedSfx(uint32_t bytes);

#endif
