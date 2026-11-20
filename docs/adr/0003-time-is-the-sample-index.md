# Time is the sample index, not the wall clock

A generator's output is a pure function of its seed, its settings and the sample index (t = index / sample rate), and trigger rules such as the Auto timeout and pre-trigger fill are counted in samples. A separate pacer releases sample blocks at wall-clock speed only for the live display. We chose this so tests can run unpaced, as fast as the CPU allows, and still get bit-identical captures every run; the wall clock is used only to measure latency. A physical scope's Auto timeout is real time, so this is a deliberate deviation.
