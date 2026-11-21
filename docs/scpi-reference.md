# SCPI reference

OpenScope is controlled with SCPI text, one line at a time. This document lists the grammar the instrument accepts, the error codes it reports and the commands it implements. It follows IEEE 488.2 and SCPI-99; where those leave a choice, the choice OpenScope made is stated here.

Examples in the grammar sections use commands that later releases add (such as `MEAS:FREQ?`) to show the rules; until a command exists it answers `-113,"Undefined header"`. The [Commands](#commands) table lists what the instrument implements now.

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
- **Errors.** The whole line is checked for syntax before any of it runs, so a syntax error anywhere on a line runs nothing on it. Any other error, including an execution error a command reports while running, stops the line at the command that caused it: the commands before it have run and their replies are returned; the command with the error and every command after it are discarded. A command rejected before it runs (a header, parameter or suffix error) never changes instrument state.

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
| `-221` | Settings conflict | A query reads a channel that is not in the latest capture (it was not enabled when the capture was taken) |
| `-222` | Data out of range | A number outside the command's range or beyond a double's range |
| `-224` | Illegal parameter value | A keyword that is not one of the parameter's choices |
| `-230` | Data corrupt or stale | A query reads capture data before the first capture has completed (or since `*RST`) |
| `-350` | Queue overflow | The error queue was full |
| `-363` | Input buffer overrun | The line is longer than 4096 bytes; nothing on it runs |

## Commands

| Command | Reply | Notes |
|---|---|---|
| `*IDN?` | `OpenScope,OS-4,<serial>,<version>` | |
| `*RST` | none | Restores every instrument-wide setting to its `*RST` value below, restarts every Generator from phase zero at sample index 0 (see Generators), and drops the latest capture and the capture in progress; leaves the error queue alone |
| `*CLS` | none | Empties the calling Session's error queue |
| `SYSTem:ERRor[:NEXT]?` | `<code>,"<message>"` | Removes and returns the oldest error |

### Settings

Each setting has a setter and a query: `SOUR2:FREQ 5kHz` sets it, `SOUR2:FREQ?` replies with its value. A rejected setter (`-222` for a value out of range, `-224` for an unknown keyword) leaves the setting as it was. Generators, channels, acquisition and the timebase are instrument-wide: a change from one Session is seen by all.

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

`<n>` is 1 to 4 and defaults to 1.

### Generators

Generator n always feeds Channel n. Time is the sample index divided by the sample rate (ADR 0003). The same seed and the same commands at the same sample indexes give bit-identical samples on every run.

A setting change takes effect at the next sample index. Frequency and sample-rate changes keep the phase continuous: the waveform carries on from where it was at the new rate, with no jump. So a Generator's output depends on the instrument's seed, the sample index and the settings sent since the last `*RST`, including the sample indexes they were sent at. `*RST` restarts every Generator as if it had run at its defaults since sample index 0, so the samples after `*RST` are the ones a freshly started instrument with the same seed produces at the same sample indexes.

Over one cycle, with A half the peak-to-peak amplitude and the offset added on top:

- `SINusoid`: A sin(2π phase), starting at zero and rising.
- `SQUare`: +A for the first half of the cycle, -A for the second.
- `TRIangle`: rises from 0 to +A at a quarter cycle, falls to -A at three quarters, and rises back to 0.
- `RAMP`: rises linearly from -A to +A across the cycle, then drops back.
- `DC`: the offset alone.
- `NOISe`: uniform random values from -A to +A, independent from one sample index to the next.

### Channels and captures

A channel's input range is its offset plus or minus four vertical divisions (four times its scale). A sample outside it is clipped: held at the nearer limit. The sample rate and record length alone define a capture. Captures are taken back to back, each spanning one record length of sample indexes, and only the latest capture is kept.

A capture takes its settings from its first sample, so changing a channel setting (`CHAN<n>:DISP`, `SCAL`, `OFFS`) or an acquisition setting (`ACQ:SRAT`, `POIN`) drops the capture in progress, and the next capture starts at the next sample index. The latest capture already completed stays as it was. Generator and timebase changes do not drop the capture in progress.

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
| trigger position | The sample (counting from 0) where time zero sits; sample k is at (k - trigger position) / sample rate seconds |
| untriggered | `1` for an untriggered capture, `0` for one placed by a trigger event |

Every capture is untriggered until the trigger is added, so the trigger position is 0 and the flag is 1.

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
