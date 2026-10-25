#pragma once

// Logs each step of the HOME Menu and sleep transitions (see ctr_apt_trace.c).
// Call once the renderer and audio system exist.
void CtrAptTrace_init(void);

// Stops the trace's heartbeat logging; call before closing the log at exit.
void CtrAptTrace_stop(void);
