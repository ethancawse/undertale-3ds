#ifndef _BS_STATE_HASH_H_
#define _BS_STATE_HASH_H_

// Per-frame runner state hashing for regression testing (--state-hash <file>). See state_hash.c.

struct Runner;
struct Renderer;

// Opt-in literal hash-operation transcript and getter return registry. Call
// before global scripts/first-room creation, so cached handles are observed.
// This adds diagnostics only; it never changes a value fed to the state hash.
void StateHash_traceBegin(const char* path, struct Renderer* renderer);

// Opens the output file and wraps the renderer's draw calls so each frame's draw calls are hashed too.
void StateHash_begin(const char* path, struct Renderer* renderer);
// Writes one line for the frame that just finished: frame, room, state hash, draw hash, draw call count.
void StateHash_endFrame(struct Runner* runner);
void StateHash_end(void);

#endif /* _BS_STATE_HASH_H_ */
