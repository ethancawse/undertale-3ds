// Scripted exercise of the audio system through its vtable, for checking the
// backend in an emulator without playing the game. Build with
// -DCTR_AUDIO_SELFTEST (e.g. CFLAGS=-DCTR_AUDIO_SELFTEST) and watch log.txt.
// It runs alongside whatever the game itself plays.
#ifdef CTR_AUDIO_SELFTEST

#include <string.h>

#include "audio_system.h"
#include "log.h"

void CtrAudioSelfTest_step(AudioSystem* audio, float dt);

static int32_t findSound(AudioSystem* audio, const char* name) {
    for (uint32_t i = 0; i < audio->dw->sond.count; i++) {
        if (strcmp(audio->dw->sond.sounds[i].name, name) == 0) return (int32_t) i;
    }
    logWarn("selftest: no sound %s\n", name);
    return -1;
}

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) logInfo("selftest PASS: " __VA_ARGS__);   \
        else logWarn("selftest FAIL: " __VA_ARGS__);        \
    } while (0)

void CtrAudioSelfTest_step(AudioSystem* audio, float dt) {
    static float t = 0.0f;
    static int stage = 0;
    static int32_t a = -1, b = -1, sfx = -1, tail = -1;
    static float sfxEnd = 0.0f;
    AudioSystemVtable* vt = audio->vtable;
    t += dt;

    int32_t ruins = findSound(audio, "mus_ruins");
    int32_t toriel = findSound(audio, "mus_toriel");
    int32_t birdsong = findSound(audio, "mus_birdsong");
    int32_t hurt = findSound(audio, "snd_hurt1");
    int32_t select = findSound(audio, "snd_select");
    int32_t buzzing = findSound(audio, "snd_buzzing"); // compressed OGG in AUDO

    switch (stage) {
        case 0:
            if (t < 2.0f) return;
            vt->stopAll(audio);
            a = vt->playSound(audio, ruins, 10, true);
            vt->setSoundGain(audio, a, 0.0f, 0);
            vt->setSoundGain(audio, a, 1.0f, 1000);
            CHECK(a >= 100000 && vt->isPlaying(audio, a), "music A started (id %d)\n", a);
            CHECK(vt->getSoundLength(audio, ruins) > 10.0f, "mus_ruins length %.2f s\n", vt->getSoundLength(audio, ruins));
            stage++;
            break;
        case 1:
            if (t < 5.0f) return;
            CHECK(vt->getSoundGain(audio, a) > 0.99f, "fade in reached %.2f\n", vt->getSoundGain(audio, a));
            CHECK(vt->getTrackPosition(audio, a) > 2.0f, "A position %.2f s after 3 s\n", vt->getTrackPosition(audio, a));
            b = vt->playSound(audio, toriel, 10, true);
            vt->setSoundGain(audio, b, 0.0f, 0);
            vt->setSoundGain(audio, b, 1.0f, 2000);
            vt->setSoundGain(audio, a, 0.0f, 2000);
            stage++;
            break;
        case 2:
            if (t < 7.5f) return;
            CHECK(vt->getSoundGain(audio, a) < 0.01f && vt->getSoundGain(audio, b) > 0.99f, "crossfade A %.2f B %.2f\n",
                  vt->getSoundGain(audio, a), vt->getSoundGain(audio, b));
            vt->stopSound(audio, a);
            CHECK(!vt->isPlaying(audio, a), "A stopped\n");
            vt->setTrackPosition(audio, b, 30.0f);
            stage++;
            break;
        case 3:
            if (t < 8.5f) return;
            CHECK(vt->getTrackPosition(audio, b) > 30.3f && vt->getTrackPosition(audio, b) < 32.0f, "B seeked, position %.2f s\n",
                  vt->getTrackPosition(audio, b));
            vt->setSoundPitch(audio, b, 1.5f);
            CHECK(vt->getSoundPitch(audio, b) == 1.5f, "B pitch 1.5\n");
            stage++;
            break;
        case 4:
            if (t < 10.0f) return;
            vt->setSoundPitch(audio, b, 1.0f);
            vt->pauseSound(audio, b);
            CHECK(!vt->isPlaying(audio, b), "B paused reads as not playing\n");
            stage++;
            break;
        case 5: {
            if (t < 11.0f) return;
            vt->resumeSound(audio, b);
            CHECK(vt->isPlaying(audio, b), "B resumed\n");
            // Voice stealing: 8 voices, B (priority 10) must survive 30 low-priority SFX.
            vt->setChannelCount(audio, 8);
            int played = 0;
            for (int i = 0; i < 30; i++) {
                if (vt->playSound(audio, i % 2 ? hurt : select, 1, false) >= 0) played++;
            }
            CHECK(played == 30 && vt->isPlaying(audio, b), "30 SFX over 8 voices played %d, B still playing\n", played);
            int32_t refused = -1;
            for (int i = 0; i < 8; i++) refused = vt->playSound(audio, hurt, 100, false);
            int32_t low = vt->playSound(audio, hurt, 0, false);
            CHECK(low < 0, "priority 0 sound refused when 8 priority-100 voices play (got %d, last high %d)\n", low, refused);
            vt->setChannelCount(audio, 128);
            stage++;
            break;
        }
        case 6: {
            if (t < 12.5f) return;
            vt->stopAll(audio);
            // More streams than contexts (8): the 9th steals the oldest.
            int32_t ids[10];
            int ok = 0;
            for (int i = 0; i < 10; i++) {
                ids[i] = vt->playSound(audio, i % 3 == 0 ? ruins : i % 3 == 1 ? toriel : birdsong, 5, true);
                if (ids[i] >= 0) ok++;
            }
            CHECK(ok == 10 && vt->isPlaying(audio, ids[9]) && !vt->isPlaying(audio, ids[0]), "10 streams started, %d ok, first stolen\n", ok);
            stage++;
            break;
        }
        case 7:
            if (t < 14.0f) return;
            vt->stopAll(audio);
            sfx = vt->playSound(audio, hurt, 5, false);
            sfxEnd = t + vt->getSoundLength(audio, sfx);
            tail = vt->playSound(audio, birdsong, 5, false);
            CHECK(tail >= 0, "stream starts right after stopAll released every stream context\n");
            vt->setTrackPosition(audio, tail, vt->getSoundLength(audio, birdsong) - 1.0f);
            CHECK(vt->playSound(audio, buzzing, 5, false) >= 0, "compressed AUDO sound streams\n");
            stage++;
            break;
        case 8:
            if (t < sfxEnd + 0.2f || t < 16.5f) return;
            CHECK(!vt->isPlaying(audio, sfx), "SFX finished after its length\n");
            CHECK(!vt->isPlaying(audio, tail), "non-looping stream finished after seeking near its end\n");
            vt->stopAll(audio);
            logInfo("selftest: done\n");
            stage++;
            break;
        default:
            break;
    }
}

#endif
