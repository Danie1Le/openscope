# SCPI reference

OpenScope is controlled with SCPI text, one line at a time. This document lists the grammar the instrument accepts, the error codes it reports and the commands it implements. It follows IEEE 488.2 and SCPI-99; where those leave a choice, the choice OpenScope made is stated here.

Examples in the grammar sections may use commands that later releases add to show the rules; until a command exists it answers `-113,"Undefined header"`. The [Commands](#commands) table lists what the instrument implements now.

## Lines

- A line is one program message. The transport ends it with `\n`, optionally preceded by `\r`; neither is part of the line.
- A line holds at most 4096 bytes, not counting the terminator. A longer line is not parsed or run at all: it only queues `-363,"Input buffer overrun"`.
- An empty line, or one holding only white space, does nothing.

## Headers

- **Short and long forms.** Every mnemonic has a short form (its upper-case letters in this document) and a long form (the whole word): `MEAS:FREQ?` and `MEASure:FREQuency?` are the same command. Only those two forms match; `SYSTE:ERR?` is an undefined header.
- **Case.** Headers and keywords are case-insensitive: `syst:err?`, `SYST:ERR?` and `System:Error?` are the same.
- **Numeric suffixes.** A mnemonic written `CHANnel<1-4>` takes a number from 1 to 4 straight after it (`CHAN2`, `CHANNEL2`). Without a number it means 1, so `CHAN:SCAL` is `CHAN1:SCAL`. A number outside the range (`CHAN5:SCAL`, `CHAN0:SCAL`) reports `-114,"Header suffix out of range"`; a number on a mnemonic that takes none (`SYST2:ERR?`) is an undefined header (`-113`).
- **Optional nodes.** A node in square brackets may be left out: `SYSTem:ERRor[:NEXT]?` accepts both `SYST:ERR?` and `SYST:ERR:NEXT?`.
- **Queries.** A header ending in `?` is a query and replies. The same header without `?` is a separate command, and one that does not exist is an undefined header (`*RST?`, `SYST:ERR`).
- **Common commands** start with `*` (`*IDN?`) and have a single form and no suffix.

## Several commands on one line

Commands on one line are separated by `;` and run in order.

- **Paths.** A header that starts with `:` is resolved from the root. A header without the leading `:` continues from the path of the previous command on the line: that command's header minus its last node (SCPI-99 compound headers). So `SYST:ERR?;ERR?` reads two errors, while `SYST:ERR?;SYST:ERR?` means `SYST:ERR?` then `SYST:SYST:ERR?`, an undefined header; write `SYST:ERR?;:SYST:ERR?` instead. Each line starts at the root. Common commands neither use nor change the path: in `SYST:ERR?;*CLS;ERR?` the last command is `SYST:ERR?`.
- **Replies.** The replies of all queries on a line are joined with `;` into one reply, in order: `*IDN?;SYST:ERR?` replies `OpenScope,OS-4,...;0,"No error"`.
- **Errors.** The whole line is checked for syntax before any of it runs, so a syntax error anywhere on a line runs nothing on it. Any other error, including an execution error a command reports while running, stops the line at the command that caused it: the commands before it have run and their replies are returned; the command with the error and every command after it are discarded. A command rejected before it runs (a header, parameter or suffix error) never changes instrument state. The entry a measurement query queues to report its [measurement status](#measurements) is not such an error: the query has replied, so the line carries on.

## White space

White space is any byte from 0x00 to 0x20 except `\n` (IEEE 488.2 white space): space, tab, `\r`, NUL and the other control characters. `\n` ends a line, so it never appears inside one; a caller of the facade that passes it inside a line gets a syntax error.

- Allowed, and ignored, at the start and end of a line, around `;` and around `,`. So a stray `\r`, as in a line ending `\r\r\n`, is ignored.
- Required between a header and its first parameter: `FREQ 1` has a parameter; `FREQ,1` is a syntax error.
- Allowed between a number and its unit suffix: `1 kHz` is `1kHz`.
- Not allowed inside a header (`SYST: ERR?`), inside a number (`1 E3`) or inside a keyword.

## Parameters

Parameters follow the header and are separated from each other by `,`. Trailing parameters shown in square brackets are optional, as in `SOURce<n>:FAULt GLITch|DROPout|FSTep[,<magnitude>[,<duration>]]`; a left-out parameter takes the default the command documents. A command that receives fewer parameters than it requires reports `-109,"Missing parameter"`; more than it takes, `-108,"Parameter not allowed"`. The count is checked before the values. A number where a keyword belongs, a keyword where a number belongs, or any [string, block or non-decimal number](#strings-blocks-and-non-decimal-numbers) reports `-104,"Data type error"`.

### Numbers

Decimal numbers with an optional sign, fraction and exponent: `5`, `+5`, `-2.5`, `5.`, `.5`, `25E-1`, `0.25e+1`. A number outside the command's range reports `-222,"Data out of range"`, as does a number beyond a double's range, too large (`1E999`) or too small (`1E-999`).

A number may carry a unit suffix: the parameter's unit, optionally behind an SI multiplier. Suffixes are case-insensitive.

| Multiplier | Factor | Multiplier | Factor |
|---|---|---|---|
| `EX` | 10^18 | `M` | 10^-3 |
| `PE` | 10^15 | `U` | 10^-6 |
| `T` | 10^12 | `N` | 10^-9 |
| `G` | 10^9 | `P` | 10^-12 |
| `MA` | 10^6 | `F` | 10^-15 |
| `K` | 10^3 | `A` | 10^-18 |

So `10mV` is 0.01 V and `250US` is 250 µs. Because case does not matter, `M` means milli, except before `HZ`: following IEEE 488.2, `MHZ` in any case (`mHz` included) is megahertz. A suffix that is not the parameter's unit, or a suffix on a number that takes no unit, reports `-131,"Invalid suffix"`.

### Keywords

A keyword parameter accepts the short or long form of one of its choices, in any case (`NORM`, `normal`). A choice may take a numeric suffix like a header (`CHANnel<1-4>` accepts `CHAN2` and `CHANNEL`, which means `CHAN1`). Any other keyword, a suffix out of range included, reports `-224,"Illegal parameter value"`.

### Booleans

`ON`, `OFF`, or a number rounded to an integer, where any non-zero value means `ON` (SCPI-99): `0.4` is `OFF`; `0.5` and `-1` are `ON`. Other keywords report `-224`; a number with a suffix reports `-131`.

### Strings, blocks and non-decimal numbers

No OpenScope command takes these IEEE 488.2 data types, but they are parsed as a bench instrument parses them, so a well-formed one reports `-104,"Data type error"` (and the commands before it on the line still run), not a syntax error:

- Strings in double or single quotes, with the quote doubled inside: `"text"`, `'it''s'`.
- Non-decimal numbers: `#H1F`, `#Q17`, `#B101` (letter in either case).
- Definite-length blocks, `#` then a digit n from 1 to 9, then n digits giving the length, then exactly that many bytes of any value: `#15hello`.
- Indefinite-length blocks, `#0` then any bytes up to the end of the line. A `;` inside one is data, so nothing after `#0` on the line runs as a command.

A malformed one is a syntax error: an unclosed string, `#H` without digits, a block shorter than its length, or anything but white space, `,` or `;` straight after it.

### Numeric keywords

The numeric keywords `MIN`, `MAX` and `DEF` are not accepted. Where a number is expected they report `-104`; where a keyword or boolean is expected they are an unknown keyword and report `-224`.

### Syntax errors

Besides the cases above, these are syntax errors (`-102`), and nothing on the line runs: an empty command (a leading `;`, `;;`, or a trailing `;` before the end of the line), a doubled or trailing `:` in a header (`::SYST:ERR?`, `SYST:ERR?:`), a `,` with no parameter after it (`FREQ 1,`), and `\n` inside a line.

## Error queue

Each Session has its own error queue (ADR 0002). `SYST:ERR?` removes and returns the oldest entry as `<code>,"<message>"`, or `0,"No error"` when the queue is empty. `*CLS` empties the calling Session's queue only; `*RST` leaves it alone.

The queue holds 20 entries. When it is full, the newest entry is replaced by `-350,"Queue overflow"` and further errors are dropped until entries are read, so the oldest errors, which name the first cause of a failure, are kept.

## Error codes

| Code | Message | Reported when |
|---|---|---|
| `0` | No error | `SYST:ERR?` on an empty queue |
| `-102` | Syntax error | The line breaks the grammar; nothing on it runs |
| `-104` | Data type error | A number where a keyword belongs, a keyword where a number belongs, or a string, block or non-decimal number |
| `-108` | Parameter not allowed | More parameters than the command takes |
| `-109` | Missing parameter | Fewer parameters than the command requires |
| `-113` | Undefined header | No command matches the header, including a suffix on a mnemonic that takes none |
| `-114` | Header suffix out of range | The header matches a command except that a mnemonic's suffix is outside its range (`CHAN5`) |
| `-131` | Invalid suffix | A number's suffix is not the parameter's unit with an optional multiplier |
| `-200` | Execution error;No signal | A measurement that needs edges or cycles found top minus base under 0.2 vertical divisions; the query replied `9.91E37` (see [Measurements](#measurements)) |
| `-200` | Execution error;Too few cycles | A measurement found under one whole cycle or no complete edge; the query replied `9.91E37` |
| `-221` | Settings conflict | A query reads a channel that is not in the latest capture (it was not enabled when the capture was taken), or `SOUR<n>:FAUL` finds its Generator already holding 64 faults |
| `-222` | Data out of range | A number outside the command's range or beyond a double's range |
| `-224` | Illegal parameter value | A keyword that is not one of the parameter's choices |
| `-230` | Data corrupt or stale | A query reads capture data before the first capture has completed (or since `*RST`), or `MEAS:STAT?` finds no capture completed since the statistics were last reset |
| `-231` | Data questionable;Clipped | A warning: a measurement replied its value, but a sample of the channel hit an input-range limit; or `MEAS:STAT?` replied statistics that count such a value |
| `-350` | Queue overflow | The error queue was full |
| `-363` | Input buffer overrun | The line is longer than 4096 bytes; nothing on it runs |

## Commands

| Command | Reply | Notes |
|---|---|---|
| `*IDN?` | `OpenScope,OS-4,<serial>,<version>` | |
| `*RST` | none | Restores every instrument-wide setting to its `*RST` value below and the Running run state, restarts every Generator from phase zero at sample index 0 with no faults (see Generators), and drops the latest capture, the capture in progress and the measurement statistics; leaves the calling Session's error queue, `WAV` and `FFT` settings alone (ADR 0002) |
| `*CLS` | none | Empties the calling Session's error queue |
| `*OPC?` | `1` | Replies once no operation is pending (see [Run control](#run-control)) |
| `*WAI` | none | Holds the rest of the Session's commands until no operation is pending |
| `SYSTem:ERRor[:NEXT]?` | `<code>,"<message>"` | Removes and returns the oldest error |
| `RUN` | none | Enters the Running run state |
| `STOP` | none | Enters the Stopped run state |
| `SINGle` | none | Arms one capture (the Armed-single run state), then stops |

### Run control

The run state says what the instrument does with captures. It is separate from the trigger mode, which only decides what happens when no trigger event arrives.

| Run state | Entered by | Captures |
|---|---|---|
| Running | `RUN`, `*RST` | Taken continuously; each completed capture becomes the latest capture |
| Stopped | `STOP`, or the capture of a `SINGle` | None; the latest capture stays as it is, so repeated queries read the same data |
| Armed-single | `SINGle` | Exactly one, then the instrument is Stopped |

`STOP` and `SINGle` drop the capture in progress, so the capture a `SINGle` takes starts at the next sample index after it; sending `SINGle` while armed starts over. `RUN` keeps a capture already in progress (from Armed-single) and otherwise starts a new one at the next sample index. None of them changes the latest capture: until the Single's capture completes, queries read the capture from before. Run control is instrument-wide, like the settings.

An armed Single is the pending operation that `*OPC?` and `*WAI` wait for. Nothing else is ever pending, so in the Running and Stopped run states `*OPC?` replies `1` and `*WAI` does nothing at once.

- `SINGle;*OPC?` replies `1` only once the Single's capture is complete. The wait also ends when the instrument leaves Armed-single any other way: `STOP`, `RUN` or `*RST`, from any Session. Every Session waiting then carries on, in the order they started waiting, even when the first to carry on sends `SINGle` again: the others waited for the Single that ended, not for the new one.
- There is no timeout on the instrument: in `NORMal` trigger mode with no trigger event, `*OPC?` waits as long as it takes. The client's own timeout is the guard. Disconnecting ends the wait, with no reply.
- `*WAI` holds the rest of the line, and the Session's later lines, until the operation ends; `SINGle;*WAI;WAV:DATA?` reads the Single's capture. `*OPC?` holds them the same way, so commands after it on the line also run once the operation has ended: `SINGle;*OPC?;WAV:PRE?` replies `1;` and the preamble of the Single's capture.
- A Session's commands run in order: while one of its lines is held, its next line waits until the reply has been sent. So the `STOP` that ends a wait comes from another Session (another connection or the display), or the client disconnects.
- A Session waiting on `*OPC?` or `*WAI` is not idle, whatever the transport's idle timeout.

### Consistency model

Every query of capture data reads the latest capture at the moment it runs. In the Running run state, two queries, even on one line, may read two different captures. To query one capture several times, take it with `SINGle;*OPC?` and then send the queries: the instrument is Stopped, so the latest capture no longer changes until the next `RUN` or `SINGle` (or `*RST`, which drops it).

`MEAS:STAT?` is the exception: it reads the [measurement statistics](#statistics), which count every capture since the last reset, not the latest capture alone. While Running they change with every capture, so two `MEAS:STAT?` queries may count different captures. After `SINGle;*OPC?` they hold still, but they still cover every capture since the reset, the Single's last; reset them first (`MEAS:STAT:RES;:SINGle;*OPC?`) for statistics of the Single's capture alone.

### Settings

Each setting has a setter and a query: `SOUR2:FREQ 5kHz` sets it, `SOUR2:FREQ?` replies with its value. A rejected setter (`-222` for a value out of range, `-224` for an unknown keyword) leaves the setting as it was. Generators, channels, acquisition, the timebase and the trigger are instrument-wide: a change from one Session is seen by all.

Query replies write numbers in the shortest form that reads back as the same value, with an upper-case exponent: `1000`, `0.001`, `1E+06`. Booleans reply `1` or `0`, keywords their short form (`SIN`), and the record length a whole number.

| Setting | Values | `*RST` | Notes |
|---|---|---|---|
| `SOURce<n>:FUNCtion` | `SINusoid`, `SQUare`, `TRIangle`, `RAMP`, `DC`, `NOISe` | `SIN` | The shape Generator n feeds Channel n, below |
| `SOURce<n>:FREQuency` | 0.001 Hz to 1 GHz (unit `HZ`) | 1 kHz | Ignored by `DC` and `NOISe`. Changing it keeps the phase continuous |
| `SOURce<n>:AMPLitude` | 0 V to 100 V peak to peak (unit `V`) | 2 V | Peak-to-peak amplitude, as on a bench generator: the signal swings from offset - amplitude/2 to offset + amplitude/2. Ignored by `DC` |
| `SOURce<n>:OFFSet` | -50 V to 50 V (unit `V`) | 0 V | Added to the whole signal |
| `SOURce<n>:NOISe` | 0 V to 50 V (unit `V`) | 0 V | RMS of Gaussian noise added to the signal |
| `CHANnel<n>:DISPlay` | boolean | CH1 `ON`, CH2 to CH4 `OFF` | Only enabled channels are kept in a capture |
| `CHANnel<n>:SCALe` | 1 mV to 10 V per division (unit `V`) | 1 V | Vertical scale |
| `CHANnel<n>:OFFSet` | -50 V to 50 V (unit `V`) | 0 V | The voltage at the centre of the input range |
| `ACQuire:SRATe` | 1 to 1E9 samples per second (no unit) | 1E+06 | The sample rate of the sample clock, shared by all channels |
| `ACQuire:POINts` | 1 to 1048576 (no unit) | 10000 | The record length: sample indexes per capture. The range applies to the value as sent, so `0.6` and `1048576.4` get `-222`. A fraction inside it rounds to the nearest whole number, half away from zero (`2.5` is 3) |
| `TIMebase:SCALe` | 1 ns to 100 s per division (unit `S`) | 1 ms | The display viewport only; it never changes what is captured (ADR 0004) |
| `TRIGger:SOURce` | `CHANnel<n>` | `CHAN1` | The trigger channel. It may be a disabled channel. The query replies `CHAN<n>` |
| `TRIGger:LEVel` | -100 V to 100 V (unit `V`) | 0 V | The trigger level |
| `TRIGger:SLOPe` | `POSitive`, `NEGative` | `POS` | The edge that is a trigger event: rising or falling through the level |
| `TRIGger:MODE` | `AUTO`, `NORMal` | `AUTO` | The trigger mode. `SINGle` is not a trigger mode (`SINGle` is a run-control command) and reports `-224` |
| `TRIGger:HOLDoff` | 0 s to 10 s (unit `S`) | 0 s | The least time from the trigger event of one capture to the trigger event of the next, counted in samples: rounded to the nearest whole number of samples at the sample rate |
| `TRIGger:POSition` | 0 to 100 percent (no unit) | 50 | The pre-trigger position: where in the capture time zero sits, as a percentage of the way from the first sample to the last |

`<n>` is 1 to 4 and defaults to 1.

### Generators

Generator n always feeds Channel n. Time is the sample index divided by the sample rate (ADR 0003). The same seed and the same commands at the same sample indexes give bit-identical samples on every run.

A setting change takes effect at the next sample index. Frequency and sample-rate changes keep the phase continuous: the waveform carries on from where it was at the new rate, with no jump. So a Generator's output depends on the instrument's seed, the sample index, the settings and [faults](#faults) sent since the last `*RST`, and the sample indexes they were sent at. `*RST` restarts every Generator as if it had run at its defaults since sample index 0, so the samples after `*RST` are the ones a freshly started instrument with the same seed produces at the same sample indexes.

Over one cycle, with A half the peak-to-peak amplitude and the offset added on top:

- `SINusoid`: A sin(2π phase), starting at zero and rising.
- `SQUare`: +A for the first half of the cycle, -A for the second.
- `TRIangle`: rises from 0 to +A at a quarter cycle, falls to -A at three quarters, and rises back to 0.
- `RAMP`: rises linearly from -A to +A across the cycle, then drops back.
- `DC`: the offset alone.
- `NOISe`: uniform random values from -A to +A, independent from one sample index to the next.

### Faults

`SOURce<n>:FAULt GLITch|DROPout|FSTep[,<magnitude>[,<duration>]]` injects one fault into Generator n: a one-off disturbance of its signal, for testing trigger and measurement behaviour against glitches, dropouts and frequency steps. It is an action, so it has no query and no `*RST` value. The fault starts at the next sample index and the next capture that covers that index shows it in `WAV:DATA?`. It does not drop the capture in progress.

`<magnitude>` and `<duration>` are plain numbers: neither takes a unit suffix (`-131`), because the unit of the magnitude depends on the kind of fault. `<duration>` is a count of sample indexes from 1 to 1048576; as for `ACQ:POIN`, the range applies to the value as sent and a fraction inside it rounds to the nearest whole number, half away from zero. A left-out parameter takes the default of the kind of fault. A magnitude beyond the limit of its kind, or a duration outside its range, reports `-222`; a keyword that is not a kind reports `-224`; nothing is injected and the rest of the line is discarded.

| Kind | Magnitude | Default magnitude | Default duration | What it does |
|---|---|---|---|---|
| `GLITch` | -100 to 100 V | 2 V | 1 sample | A short spike: the magnitude is added to the signal over the duration |
| `DROPout` | -100 to 100 V | 0 V | 100 samples | A flat segment: the signal, noise included, is replaced by the level in the magnitude over the duration |
| `FSTep` | -1E9 to 1E9 Hz | 1000 Hz | 1000 samples | A phase-continuous change of frequency: the magnitude is added to the frequency over the duration, then the frequency returns to what it was. The phase gained stays, so the waveform carries on shifted, with no jump at either end. Does nothing to `DC` and `NOISe` |

Each fault is one-shot: it covers its duration once and never repeats. A glitch or dropout leaves the signal as it was afterwards; a frequency step leaves the phase shift. A glitch or dropout is applied to the sample after the function, offset and noise, in the order they were injected, and before the channel clips it to its input range, so a spike beyond the range is held at the limit; a frequency step acts on the phase the function is computed from. A frequency step adds a fixed number of cycles per sample, worked out from the magnitude at the sample rate in force when it was injected; changing the frequency or the sample rate during the step does not change it.

A fault is placed by the sample index, so with the same seed and the same commands at the same sample indexes it lands at the same index with the same shape on every run (ADR 0003). A fault on a Generator whose channel is not enabled still passes with the sample clock and shows in a capture that covers its indexes once the channel is enabled. `*RST` forgets every fault, those still running included, along with the rest of the Generator state. A Generator keeps at most 64 faults at once: a fault that has ended frees its slot (its phase shift is kept), and injecting into a Generator that already holds 64 reports `-221,"Settings conflict"` and injects nothing.

`SOUR1:FAUL GLIT,1.5,2` adds 1.5 V to two samples; `SOUR2:FAUL FST,500,200` runs Generator 2 500 Hz faster for 200 samples.

### Channels and captures

A channel's input range is its offset plus or minus four vertical divisions (four times its scale). A sample outside it is clipped: held at the nearer limit. The sample rate and record length alone define a capture: one record length of sample indexes, positioned by the trigger (below). Only the latest capture is kept.

A capture takes its settings from its first sample and holds one signal, so changing a Generator setting (`SOUR<n>:FUNC`, `FREQ`, `AMPL`, `OFFS`, `NOIS`), a channel setting (`CHAN<n>:DISP`, `SCAL`, `OFFS`), an acquisition setting (`ACQ:SRAT`, `POIN`) or a trigger setting (`TRIG:...`) drops the capture in progress, restarts the trigger and resets the [measurement statistics](#statistics), and the next capture starts collecting samples at the next sample index. Any such change does this, from any Session and even one that sets the value already in place; an armed Single stays armed and takes its capture from the next sample index. The latest capture already completed stays as it was and stays readable. Timebase changes do none of this (the timebase is a display viewport, ADR 0004), nor do a Session's own `WAV:...` and `FFT:...` settings or a fault (`SOUR<n>:FAUL`), which is meant to be captured.

### Trigger

A trigger event is the sample index where the trigger channel reaches the trigger level in the slope's direction (at or above the level for `POS`, at or below it for `NEG`) after first moving at least the hysteresis band past the level on the other side. The band is fixed at 0.1 vertical divisions of the trigger channel (0.1 V at 1 V/div). So after one trigger event the next needs the signal to go back through the band first, and noise smaller than the band around the level gives no false trigger events. After a change that restarts the trigger, the signal must pass the band before the first trigger event. The trigger sees the trigger channel's samples after clipping, whether the channel is enabled or not.

Time zero of a capture sits at its trigger position: sample round(position / 100 × (record length − 1)) of the capture, rounding half away from zero. 50% of 10000 points is sample 5000; 0% is the first sample and 100% the last. That many samples before time zero are the pre-trigger samples.

Each capture collects samples from the sample index after the previous capture ends (or after a change that restarts it). A trigger event places time zero only when both hold:

- the capture already holds its pre-trigger samples, all taken after the previous capture ended, so captures never overlap and every capture is complete from its first sample;
- the holdoff has passed: the trigger event is at least `TRIG:HOLD` × sample rate samples after the trigger event that placed the previous capture.

Other trigger events are ignored. Once time zero is placed, the capture completes with the samples after it.

In `NORMal` mode a capture is taken only on a trigger event; without one, the latest capture stays as it is for as long as it takes. In `AUTO` mode, once the pre-trigger samples have filled, the instrument waits one record length of samples for a trigger event (the Auto timeout). When none places the capture by then, the last record length of samples becomes an untriggered capture: time zero sits at the trigger position, and `WAV:PRE?` reports `1` in its untriggered field. The next capture starts collecting at the next sample index, so a signal that never triggers gives an untriggered capture every record length plus pre-trigger samples: at the `*RST` settings, samples 5000 to 14999, then 20000 to 29999, and so on.

The holdoff and the Auto timeout are counted on the sample clock, not the wall clock (ADR 0003).

### Waveform transfer

| Command | Values | New Session | Notes |
|---|---|---|---|
| `WAVeform:SOURce` | `CHANnel<n>` | `CHAN1` | The channel `WAV:DATA?` and `WAV:PRE?` read. The query replies `CHAN<n>` |
| `WAVeform:FORMat` | `ASCii`, `REAL` | `ASC` | How `WAV:DATA?` writes samples |
| `WAVeform:DATA?` | | | The source channel's samples in the latest capture, in volts |
| `WAVeform:PREamble?` | | | `<sample rate>,<points>,<scale>,<offset>,<trigger position>,<untriggered>` |

`WAV:SOUR` and `WAV:FORM` belong to each Session (ADR 0002): another client's settings never change what this Session reads. A new Session starts at `CHAN1` and `ASC`, and `*RST` leaves both alone, as it leaves the error queue.

`WAV:DATA?` and `WAV:PRE?` read the latest capture at the moment they run. Before any capture has completed, or after `*RST` until the next one does, they reply nothing and report `-230,"Data corrupt or stale"`. When the source channel is not in the latest capture (it was not enabled when the capture was taken), they reply nothing and report `-221,"Settings conflict"`. Enabling or disabling a channel does not change the latest capture: a channel disabled since can still be read, a channel enabled since cannot until the next capture.

`WAV:DATA?` writes one value per sample index of the capture, oldest first:

- `ASC`: decimal values separated by `,`, each the shortest form that reads back as the same float32 (`0.75,0.75,-0.25,-0.25`).
- `REAL`: an IEEE 488.2 definite-length block of IEEE 754 float32 values: `#`, one digit n, n digits giving the byte count, then 4 bytes per sample, least significant byte first. 100 points are `#3400` followed by 400 bytes. This is the layout PyVISA's `query_binary_values` reads with its defaults (`datatype='f'`, little-endian). The reply line still ends with `\n` after the block.

`WAV:PRE?` describes the latest capture with the settings it was taken with, which may differ from the current ones:

| Field | Meaning |
|---|---|
| sample rate | Samples per second, as `ACQ:SRAT?` writes it |
| points | The record length: values `WAV:DATA?` returns |
| scale | The source channel's vertical scale in volts per division |
| offset | The source channel's offset in volts |
| trigger position | The sample (counting from 0) where time zero sits: the trigger event, or for an untriggered capture the pre-trigger position. Sample k is at (k - trigger position) / sample rate seconds |
| untriggered | `1` for an untriggered capture (Auto mode, no trigger event), `0` for one placed by a trigger event |

### Measurements

| Command | Reply | Measures |
|---|---|---|
| `MEASure:VPP? CHANnel<n>` | volts | Peak to peak: maximum minus minimum |
| `MEASure:VMIN? CHANnel<n>` | volts | The lowest sample |
| `MEASure:VMAX? CHANnel<n>` | volts | The highest sample |
| `MEASure:VAVerage? CHANnel<n>` | volts | The mean of the samples |
| `MEASure:VRMS? CHANnel<n>` | volts | The root mean square of the samples, DC included |
| `MEASure:FREQuency? CHANnel<n>` | hertz | 1 / `PERiod` |
| `MEASure:PERiod? CHANnel<n>` | seconds | The mean length of the whole cycles, between 50% crossings |
| `MEASure:DUTYcycle? CHANnel<n>` | percent | Positive duty cycle: the share of the whole cycles spent above the 50% level |
| `MEASure:RISetime? CHANnel<n>` | seconds | The mean time of the complete rising edges from the 10% level to the 90% level |
| `MEASure:FALLtime? CHANnel<n>` | seconds | The mean time of the complete falling edges from the 90% level to the 10% level |

The channel parameter is required; `CHAN` alone means `CHAN1`. Each measurement reads every sample of the channel in the latest capture at the moment the query runs, so the timebase never changes it (ADR 0004). Like the waveform queries, a measurement before any capture has completed, or after `*RST` until the next one does, replies nothing and reports `-230`, and one on a channel not in the latest capture replies nothing and reports `-221`. Results are computed once per capture: asking again before the next capture completes returns the same result.

Values are written like settings (shortest form, upper-case exponent) at float32 precision, the precision of the samples they come from: `1.6`, `-0.5`, `1.118034`, `4E-04`.

#### Measurement status

Every result carries a measurement status, decided in this order:

1. **No signal**: only for a measurement that needs edges or cycles (the timing measurements: `FREQ`, `PER`, `DUTY`, `RIS` and `FALL`). Top minus base is under 0.2 vertical divisions of the channel's scale (the scale the capture was taken at): a flat signal, noise alone, or a signal held flat at an input-range limit. The amplitude measurements above never report it: a flat signal's `VPP` is 0 and its `VMIN`, `VMAX`, `VAV` and `VRMS` are its level.
2. **Too few cycles**: the measurement needs more of the signal than the capture holds: under one whole cycle for `FREQ`, `PER` and `DUTY`, no complete rising edge for `RIS` and no complete falling edge for `FALL` (see [Edges and cycles](#edges-and-cycles)). `FREQ`, `PER` and `DUTY` also report it when the number of whole cycles is not known (a doubtful cycle, below). The amplitude measurements above never report it.
3. **Clipped**: a sample of the channel in the capture hit an input-range limit (offset plus or minus four divisions), so the value may be wrong.
4. **OK**.

A query reports its status every time it runs, to the calling Session only:

| Status | Reply | Error queue entry |
|---|---|---|
| OK | the value | none |
| Clipped | the value | `-231,"Data questionable;Clipped"` |
| No signal | `9.91E37` (SCPI's not-a-number) | `-200,"Execution error;No signal"` |
| Too few cycles | `9.91E37` | `-200,"Execution error;Too few cycles"` |

The entry does not stop the line: `MEAS:VPP? CHAN1;VRMS? CHAN1` on a clipped signal replies both values and queues two `-231` entries.

#### Statistics

| Command | Reply | Notes |
|---|---|---|
| `MEASure:STATistics? <type>,CHANnel<n>` | `<current>,<mean>,<minimum>,<maximum>,<standard deviation>,<count>` | `<type>` is a measurement's mnemonic from the table above (`VPP`, `VMAX`, `VAVerage`, ...); both parameters are required |
| `MEASure:STATistics:RESet` | none | Resets the statistics of every measurement on every channel |

Measurement statistics follow every measurement of every enabled channel across the captures since the last reset, so a script can check how stable a value is. Every capture that completes counts, untriggered captures included, even when several complete between two queries and only the last becomes the latest capture. They are instrument-wide: every Session reads the same statistics, and a reset from one Session resets them for all.

The statistics reset on `MEAS:STAT:RES`, on `*RST` and on every setting command that shapes captures (`SOUR<n>:...`, `CHAN<n>:...`, `ACQ:...`, `TRIG:...`), from any Session and even one that sets the value already in place, so they never mix data from two settings. A capture interrupted by the change is dropped (see [Channels and captures](#channels-and-captures)), so the first capture counted after a reset starts after it. `MEAS:STAT:RES` changes no setting and keeps the capture in progress: it is counted when it completes, although some of its samples came before the reset. Other commands keep them: `TIM:SCAL` (the timebase only zooms the display and changes no captured data, ADR 0004), a Session's own `WAV:...` and `FFT:...` settings, faults (`SOUR<n>:FAUL`, which are meant to show in the statistics), run control (`RUN`, `STOP`, `SINGle`), queries, and a setter that is rejected.

The fields:

- **current**: the result of the latest capture.
- **mean**, **minimum**, **maximum**: over the values counted.
- **standard deviation**: the population standard deviation of the values counted (the root of their mean squared deviation from the mean), so one value has a standard deviation of 0.
- **count**: how many values were counted.

The values are written like measurement values: shortest form at float32 precision; the count is a whole number.

Measurement status works as for a single measurement:

- An OK or Clipped result has a value, and the value is counted. A No signal or Too few cycles result has no value (`MEAS:<type>?` replies `9.91E37`), and it is left out of every statistic, the count included: the count says how many captures gave a value.
- The query reports the status of the current result as `MEAS:<type>?` does: `-200` with the status for No signal or Too few cycles, `-231` for Clipped. A field with no value replies `9.91E37`: the current value when the current result has none, and the mean, minimum, maximum and standard deviation while the count is 0.
- When a Clipped value has been counted, the statistics may be wrong: the query reports one `-231,"Data questionable;Clipped"` even after the signal has come back into range, until the next reset.
- None of these entries stops the line.

Before a capture has completed since the last reset, `MEAS:STAT?` replies nothing and reports `-230,"Data corrupt or stale"`, even while the latest capture can still be read: there are no statistics yet. On a channel that is not in the latest capture, it replies nothing and reports `-221`. A capture is counted as it becomes the latest capture, so the current result is always the latest capture's.

`MEAS:STAT:RES;:SINGle;*OPC?;:MEAS:STAT? VPP,CHAN1` replies `1;` and the statistics of one capture, the Single's: its value four times (current, mean, minimum and maximum), a standard deviation of `0` and a count of `1`.

#### Top and base

Top and base are a signal's settled high and low levels, taken from histograms of its samples so that a spike or a short glitch never becomes either:

1. A histogram of 256 equal bins spans the minimum to the maximum. A settled level holds at least 1/128 of the capture's samples, and never fewer than two, so even in a short capture a single spike is not a level.
2. Outliers are set aside: at each end of the histogram, samples cut off from the rest by empty bins that add up to fewer than a settled level. When the samples left could not hold a settled level (a capture of two or three samples), every sample is kept.
3. A second histogram of 256 bins spans the samples that are left. In each half of it (128 bins), the fullest bin and the 4 bins either side of it are a level when, together, they hold a settled level's share of the samples, at least 24 samples, and twice what as many bins of that half hold on average. Counting the neighbouring bins finds a level that noise spreads over several bins; the last two tests keep chance clusters in a short capture from counting. The level is the mean of the samples in the fullest bin. A half with no level (a triangle or ramp sweeps evenly through its range) takes the highest or lowest sample that is left instead, never a value from the middle of the span.

A real level held by fewer than 1/128 of the samples and cut off from the rest, such as the top of a pulse train with a duty cycle under 1/128, is set aside the same way: top and base are then both the other level, so the timing measurements report No signal.

#### Edges and cycles

The timing measurements work from levels between base and top: the 10%, 50% and 90% reference levels, and the 30%, 40%, 60% and 70% levels for counting whole cycles.

- **Edges.** An edge is a move of the signal across a hysteresis band, from beyond one of its two levels to beyond the other: a rising edge goes from under the lower level to over the upper one, a falling edge the reverse. Noise, ringing or a glitch that crosses the 50% level without reaching the far level is no edge, so rising and falling edges alternate. A capture that starts between the two levels has no edge until the signal first passes one of them: which side it came from is unknown. A move that the capture ends in the middle of is no edge either: an edge is complete once it reaches its far level. Times are linear interpolations between samples.
- **Rise and fall time** use edges across the 10% and 90% levels. An edge's time runs from where it last left its starting level to where it first reached the far level. Rise and fall time are the mean of those times over every complete edge of that direction.
- **Cycles** use edges across the 30% and 70% levels: centred on 50%, so that a cycle whose sampled peak falls short of 90% (a noisy triangle or ramp at about 10 samples per cycle) still counts, and wide enough that noise around 50% does not. An edge happens where it last crossed the 50% level. One whole cycle runs from one edge's 50% crossing to the next in the same direction. `PER` is the time from the first edge's crossing to the crossing that ends the last whole cycle, divided by the number of whole cycles; `FREQ` is its inverse. `DUTY` is the time above the 50% level (from a rising crossing to the next falling one) within those cycles, as a percentage of that span.
- **Doubtful cycles.** Between two such edges the signal may cross the 40% and 60% levels and turn back without reaching 30% or 70%. When that move lasts at least an eighth of the mean cycle, it may be a cycle whose peak the samples missed, so the number of whole cycles is not known: `FREQ`, `PER` and `DUTY` report Too few cycles rather than a value that may be a whole cycle off. A shorter move is taken as noise: noise on a slow edge crosses 40% and 60% back and forth only while the edge is near 50%, at most a tenth of a cycle. A glitch that crosses 50% without reaching 40% (or 60%) changes nothing; one that passes 30% and 70% is a cycle of its own, however short. A missed cycle still goes unseen when its peak stays short of 60% (40%), or when it is narrower than an eighth of a cycle, such as one pulse that falls short of 70% in a train under 12.5% duty.

On a clean sine with at least 10 cycles and 10 samples per cycle, `FREQ` is within 0.1% of the Generator's frequency; with noise at 1% of the amplitude it is within 0.5%. Triangles and ramps with that noise keep every cycle down to 10 samples per cycle. A step edge's 50% crossing is known only to within the sample interval it falls in, so a square wave or ramp over few cycles can read up to one sample period off over the span of its whole cycles. With noise at 1% of the amplitude a square wave's rise and fall times stay within 2 sample periods of its ideal edge (its Generator steps between two samples, which interpolation reads as 0.8 sample periods). A slow edge has no such bound: noise moves where it last leaves its starting level later and where it first reaches the far level earlier, by as much as the noise divided by the edge's slope, so a slow noisy edge reads shorter than it is.

### FFT

| Command | Values | New Session | Notes |
|---|---|---|---|
| `FFT:SOURce` | `CHANnel<n>` | `CHAN1` | The channel `FFT:DATA?` and `FFT:SPAC?` read. The query replies `CHAN<n>` |
| `FFT:WINDow` | `RECTangular`, `HANNing`, `BHARris` | `HANN` | The window applied to the samples before the transform |
| `FFT:DATA?` | | | The source channel's magnitude spectrum in the latest capture, in dBV RMS |
| `FFT:SPACing?` | | | The bin spacing in hertz: the frequency step from one `FFT:DATA?` value to the next |

`FFT:SOUR` and `FFT:WIND` belong to each Session (ADR 0002), like `WAV:SOUR`, and are separate from it: a Session can read the waveform of one channel and the spectrum of another. A new Session starts at `CHAN1` and `HANN`, and `*RST` leaves both alone. `FFT:DATA?` and `FFT:SPAC?` read the latest capture at the moment they run and report `-230` and `-221` in the same cases as `WAV:DATA?`.

The FFT is a radix-2 transform, so it takes a power-of-two number of samples: the first N samples of the capture, where N is the largest power of two that is not more than the record length. At the `*RST` record length of 10000 it uses the first 8192 samples; at a power-of-two record length it uses every sample. Up to half the capture can go unused (a record length of 16383 uses 8192), so pick a power of two to use it all. Dropping the last samples, rather than padding with zeros, keeps a tone that completes a whole number of cycles in N samples exactly on one bin.

`FFT:DATA?` replies N/2 + 1 levels as an IEEE 488.2 definite-length block of float32, laid out as `WAV:FORM REAL` writes samples. Value k is the level at k × spacing hertz, from DC (k = 0) to half the sample rate (k = N/2), and `FFT:SPAC?` replies the spacing, sample rate / N, from the settings the latest capture was taken with. At 1 MS/s and the `*RST` record length of 10000 the spacing is `122.0703125` Hz and the block holds 4097 values.

Levels are dBV RMS: 20 log10 of the RMS voltage at that frequency. The window's coherent gain is divided out, so a sine of peak amplitude A volts whose frequency falls exactly on a bin reads 20 log10(A / √2) in every window (a 2 Vpp sine reads -3.0103 dBV), and DC reads 20 log10 of its voltage. A bin with no signal reads `-300`, the floor, instead of minus infinity. The windows trade frequency resolution against leakage, the spread of a tone that falls between bins into the bins around it:

| Window | Formula, n from 0 to N - 1 | Bins a bin-centred tone spreads over | A tone half-way between bins reads | Highest sidelobe |
|---|---|---|---|---|
| `RECT` | 1 | 1 | 3.92 dB low | -13 dB |
| `HANN` | 0.5 - 0.5 cos(2πn/N) | 3 | 1.42 dB low | -31 dB |
| `BHAR` | 0.35875 - 0.48829 cos(2πn/N) + 0.14128 cos(4πn/N) - 0.01168 cos(6πn/N) | 7 | 0.83 dB low | -92 dB |

The windows are periodic (their period is N), as is usual for spectral analysis. A spectrum of fewer than 4 samples (a record length of 1 to 3) uses no window whatever `FFT:WIND` says: a window that short no longer has the shape, or the mean, its formula gives.
