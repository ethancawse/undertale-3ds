#ifndef _BS_VIDEO_H_
#define _BS_VIDEO_H_

#include "common.h"
#include "runner.h"

typedef struct VideoDecoder VideoDecoder;

typedef enum {
    VIDEO_FRAME_NONE = 0,
    VIDEO_FRAME_READY = 1,
    VIDEO_FRAME_LOOPED = 2,
    VIDEO_FRAME_HELD = 3,
} VideoFrameResult;

typedef struct {
    bool (*init)(void);
    void (*quit)(VideoDecoder* decoder);
    bool (*open)(VideoDecoder* decoder, const char* url);
    void (*close)(VideoDecoder* decoder);
    bool (*extractAudio)(VideoDecoder* decoder, Runner* runner, const char* url);
    bool (*isRunning)(VideoDecoder* decoder);
    bool (*isPaused)(VideoDecoder* decoder);
    void (*pause)(VideoDecoder* decoder);
    void (*resume)(VideoDecoder* decoder);
    void (*setLoop)(VideoDecoder* decoder, bool loop);
    void (*setMasterClock)(VideoDecoder* decoder, double positionSeconds);
    int32_t (*update)(VideoDecoder* decoder);
    void (*draw)(VideoDecoder* decoder, Runner* runner, int32_t surfaceId);
    double (*duration)(VideoDecoder* decoder);
    double (*position)(VideoDecoder* decoder);
    int32_t (*width)(VideoDecoder* decoder);
    int32_t (*height)(VideoDecoder* decoder);
    // Optional. Makes the surface frames are drawn onto (a renderer surface
    // id, or -1); without it the engine makes one of the video's size.
    int32_t (*createSurface)(VideoDecoder* decoder, Runner* runner);
    // Optional. Whether the open video has an audio track the platform's
    // audio system streams from the video file itself (createStream with
    // the game's file name); extractAudio is then not used.
    bool (*audioInFile)(VideoDecoder* decoder);
} VideoDecoderVtable;

struct VideoDecoder {
    VideoDecoderVtable* vtable;
    void* impl;
};

VideoDecoder* VideoDecoder_createBackend(void);

void Video_executePendingAsyncEvents(Runner* runner);

#endif /* _BS_VIDEO_H_ */