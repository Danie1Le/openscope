# OpenScope

A software oscilloscope: simulated signals are sampled, triggered into captures, measured, and exposed to scripts over SCPI and to people through a live display.

## Signals

**Channel**:
One of four numbered inputs (CH1 to CH4). Trigger sources and waveform sources are always channels.
_Avoid_: Trace (for the data), input (alone)

**Enabled channel**:
A channel that is switched on. Only enabled channels are kept in a capture and can be measured; the trigger sees every channel whether enabled or not.
_Avoid_: Displayed channel, visible channel

**Input range**:
The span of voltages a channel can capture: its offset plus or minus four vertical divisions. Set by the channel's vertical scale and offset.
_Avoid_: Screen range, display range

**Clipped**:
A sample that fell outside its channel's input range and was held at the range limit.
_Avoid_: Saturated, overrange

**Generator**:
The simulated signal producer feeding one channel; Channel n is always fed by Generator n. Its output depends only on its seed, the sample index and the settings and faults applied since the last `*RST`, with the sample indexes they took effect at (changes are phase-continuous).
_Avoid_: SignalSource, source (alone), signal generator

**Fault**:
A one-off disturbance a generator adds to its signal starting at a given sample index: a glitch (short spike), a dropout (flat segment) or a frequency step.
_Avoid_: Error, anomaly, injection (as a noun)

**Hardware input**:
A real analog signal from a microcontroller that takes the place of a channel's generator.
_Avoid_: Hardware source

## Control

**Session**:
One connected client (a script over TCP or a browser over WebSocket). It owns its own error queue, waveform-transfer settings and FFT settings (source and window); everything else is shared instrument state.
_Avoid_: Connection, client (as the noun for the state)

## Acquisition

**Sample clock**:
The single clock that samples every channel at the same instants. Time on the instrument is the sample index divided by the sample rate.
_Avoid_: Wall clock, system time

**Sample index**:
The position of a sample on the sample clock, counting from zero since acquisition started. It is the instrument's notion of time.
_Avoid_: Timestamp

**Sample block**:
A run of consecutive sample indexes carrying one sample per enabled channel at each index, plus the trigger channel's samples when it is disabled.
_Avoid_: Chunk, packet, buffer

**Dropped block**:
A sample block lost because processing fell behind. Any capture that would span it is discarded, never published with a gap.
_Avoid_: Lost samples, overrun (alone)

**Live mode**:
The instrument running in real time: an acquisition thread takes sample blocks as the pacer releases them and a processing thread assembles them into captures. In test mode, by contrast, a test steps the sample clock by hand.
_Avoid_: Real-time mode, free-run

**Pacer**:
What releases sample indexes at wall-clock speed in live mode. It decides only when samples are taken; time on the instrument stays the sample index.
_Avoid_: Timer, scheduler

**Skipped samples**:
Samples the pacer released that live mode never acquired, because acquisition fell more than one round behind the wall clock. No capture has a gap: the sample clock just runs behind the wall clock. Counted apart from dropped blocks.
_Avoid_: Lost samples, dropped samples

**Sample rate**:
Samples per second on the sample clock, shared by all channels.

**Record length**:
The number of sample indexes in one capture.
_Avoid_: Points (except as the SCPI keyword), buffer size

**Capture**:
One triggered record: the samples of every enabled channel over the same record-length span of sample indexes, positioned around a trigger point.
_Avoid_: Acquisition, frame, record (alone), sweep

**Latest capture**:
The most recently completed capture. It is the only capture the instrument keeps; every measurement and waveform query reads it.
_Avoid_: Current capture, history

**Run state**:
What the instrument is doing with captures: Running, Stopped, or Armed-single.
_Avoid_: Acquisition mode

**Single**:
A run-control action that arms the instrument for exactly one capture, then puts it in the Stopped run state.
_Avoid_: Single trigger mode

**Pending operation**:
An armed Single, until its capture completes or the instrument leaves the Armed-single run state. `*OPC?` and `*WAI` hold a Session's commands while one is pending.
_Avoid_: Busy, operation in progress

## Trigger

**Trigger mode**:
What the instrument does when no trigger event arrives: Auto (capture anyway after a timeout) or Normal (wait). Single is not a trigger mode.

**Trigger event**:
The sample index where the trigger channel crosses the trigger level in the chosen slope direction, after first moving past the level by a fixed hysteresis band on the other side.
_Avoid_: Trigger (alone, when the event is meant), crossing

**Trigger position**:
Where in a capture time zero sits, as a percentage of the way from its first sample to its last. The samples before time zero are the pre-trigger samples.
**Holdoff**:
The least time, counted in samples on the sample clock, from the trigger event that placed one capture to the trigger event that may place the next.

**Auto timeout**:
In Auto trigger mode, one record length of samples, counted from when the pre-trigger samples have filled. If no trigger event places the capture by then, an untriggered capture is taken.

**Untriggered capture**:
A capture taken in Auto trigger mode because no trigger event arrived before the Auto timeout. Time zero sits at the trigger position instead of at a trigger event.
_Avoid_: Auto capture, free-run capture

## Measurement

**Top** and **Base**:
The two settled voltage levels of a signal, taken as the most common high and low values in a capture, falling back to the maximum and minimum once outliers are set aside. A spike or short glitch is never top or base.
_Avoid_: High/low, max/min (they are not the same thing)

**Reference levels**:
The 10%, 50% and 90% points between base and top. Rise and fall times run from 10% to 90%; frequency and duty cycle use 50% crossings, of edges counted across 30% and 70%.
_Avoid_: Thresholds

**Edge**:
A move of the signal from beyond one level of a hysteresis band to beyond the other: across the 10% and 90% reference levels for rise and fall time, across 30% and 70% for counting whole cycles. A move that crosses 50% and turns back before the far level is no edge. An edge is complete once it reaches the far level.
_Avoid_: Transition, crossing (a crossing is one level, an edge is the whole move)

**Doubtful cycle**:
A move between two edges across 30% and 70% that crosses 40% and 60% and turns back, lasting at least an eighth of a cycle: it may be a cycle whose peak the samples missed, so frequency, period and duty cycle report Too few cycles.

**Measurement status**:
The verdict that travels with every measured value: OK, No signal, Clipped or Too few cycles. A value is never reported without one. Too few cycles covers under one whole cycle, no complete edge, and whole cycles whose number is not known.
_Avoid_: Error code, quality

**Measurement statistics**:
Current, mean, minimum, maximum and standard deviation of one measurement across captures since the last statistics reset. Any setting change resets them.

**Spectrum**:
The magnitude of one channel of the latest capture at each frequency bin, in dBV RMS, computed by the FFT through a window.
_Avoid_: FFT (for the result), frequency domain trace

**Window**:
The weighting (rectangular, Hann or Blackman-Harris) applied to the samples before the FFT, trading frequency resolution against leakage. Chosen per Session.
_Avoid_: Taper, apodization

**Bin spacing**:
The frequency step between neighbouring bins of a spectrum: the sample rate divided by the number of samples the FFT takes.
_Avoid_: Resolution bandwidth, frequency resolution

## Display

**Timebase**:
The horizontal viewport (seconds per division and position) over a capture. It changes what is shown, never what is captured.
_Avoid_: Horizontal acquisition settings
