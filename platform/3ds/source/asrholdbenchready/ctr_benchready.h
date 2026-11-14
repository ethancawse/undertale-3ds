#ifndef CTR_BENCHREADY_H
#define CTR_BENCHREADY_H

// Benchmark adapters only: set existing bench.lastStatus to now. Call after
// readiness/clear and during settling to prevent a status repaint. No other
// state or screen policy changes; normal play must never call this function.
void CtrBench_readySuppressStatus(void);

#endif
