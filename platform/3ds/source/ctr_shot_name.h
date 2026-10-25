#pragma once

// Bench screenshots named after their scene: top_00941_ch3_rhythm.bmp, shot_00941_ch3_rhythm.bmp. Scenes that shoot
// at the same frame number used to overwrite each other's pictures in one run. The scene id goes after the frame
// number and any suffix (top_00941p_ch3_rhythm.bmp for a paused shot), so tools that read the frame number from the
// front of the name (picture_check.py) keep working, and the id becomes part of the suffix that pairs a top capture
// with its application capture. Outside a bench scene names stay as they were.
//
// Late code (platform/3ds/late_reserve.ld, ctr_shot_name.c): the renderer's naming code keeps its size and address
// and only calls CtrShotName_format instead of snprintf, with the same arguments.

#include <stddef.h>

// snprintf for a screenshot path that ends in ".bmp", with the running bench scene's id inserted before the extension.
int CtrShotName_format(char* out, size_t size, const char* format, ...) __attribute__((format(printf, 3, 4)));
// The running bench scene's id while a scene sets up or is measured, else nullptr (ctr_benchmark.c).
const char* CtrBench_lateSceneId(void);
