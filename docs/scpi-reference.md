# SCPI reference

OpenScope is controlled with SCPI text, one line at a time. This document lists the grammar the instrument accepts, the error codes it reports and the commands it implements. It follows IEEE 488.2 and SCPI-99; where those leave a choice, the choice OpenScope made is stated here.

## Lines

- A line is one program message. The transport ends it with `\n`, optionally preceded by `\r`; neither is part of the line.
- A line holds at most 4096 bytes, not counting the terminator. A longer line is not parsed or run at all: it only queues `-363,"Input buffer overrun"`.
- An empty line, or one holding only spaces and tabs, does nothing.

## Headers

- **Short and long forms.** Every mnemonic has a short form (its upper-case letters in this document) and a long form (the whole word): `MEAS:FREQ?` and `MEASure:FREQuency?` are the same command. Only those two forms match; `SYSTE:ERR?` is an undefined header.
- **Case.** Headers and keywords are case-insensitive: `syst:err?`, `SYST:ERR?` and `System:Error?` are the same.
- **Numeric suffixes.** A mnemonic written `CHANnel<1-4>` takes a number from 1 to 4 straight after it (`CHAN2`, `CHANNEL2`). Without a number it means 1, so `CHAN:SCAL` is `CHAN1:SCAL`. A number outside the range, or a number on a mnemonic that takes none, is an undefined header (`-113`).
- **Optional nodes.** A node in square brackets may be left out: `SYSTem:ERRor[:NEXT]?` accepts both `SYST:ERR?` and `SYST:ERR:NEXT?`.
- **Queries.** A header ending in `?` is a query and replies. The same header without `?` is a separate command, and one that does not exist is an undefined header (`*RST?`, `SYST:ERR`).
- **Common commands** start with `*` (`*IDN?`) and have a single form and no suffix.

## Several commands on one line

Commands on one line are separated by `;` and run in order.

- **Paths.** A header that starts with `:` is resolved from the root. A header without the leading `:` continues from the path of the previous command on the line: that command's header minus its last node (SCPI-99 compound headers). So `SYST:ERR?;ERR?` reads two errors, while `SYST:ERR?;SYST:ERR?` means `SYST:ERR?` then `SYST:SYST:ERR?`, an undefined header; write `SYST:ERR?;:SYST:ERR?` instead. Each line starts at the root. Common commands neither use nor change the path: in `SYST:ERR?;*CLS;ERR?` the last command is `SYST:ERR?`.
- **Replies.** The replies of all queries on a line are joined with `;` into one reply, in order: `*IDN?;SYST:ERR?` replies `OpenScope,OS-4,...;0,"No error"`.
- **Errors.** The whole line is checked for syntax before any of it runs, so a syntax error anywhere on a line runs nothing on it. Any other error stops the line at the command that caused it: the commands before it have run and their replies are returned; the command with the error and every command after it are discarded. A command with an error never changes instrument state.

## Whitespace

Whitespace is a space or a tab.

- Allowed, and ignored, at the start and end of a line, around `;` and around `,`.
- Required between a header and its first parameter: `*CLS 1` has a parameter; `SYST:ERR?,1` is a syntax error.
- Allowed between a number and its unit suffix: `1 kHz` is `1kHz`.
- Not allowed inside a header (`SYST: ERR?`), inside a number (`1 E3`) or inside a keyword.

## Parameters

Parameters follow the header and are separated from each other by `,`. A command that receives fewer parameters than it takes reports `-109,"Missing parameter"`; more, `-108,"Parameter not allowed"`. The count is checked before the values. A number where a keyword belongs, or a keyword where a number belongs, reports `-104,"Data type error"`.

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

### Not supported

String data (`"text"`), block data (`#...`) and non-decimal numbers (`#H1F`) are not part of the grammar and are syntax errors, as is a trailing `;` before the end of a line. The numeric keywords `MIN`, `MAX` and `DEF` are not accepted; they report `-104`.

## Error queue

Each Session has its own error queue (ADR 0002). `SYST:ERR?` removes and returns the oldest entry as `<code>,"<message>"`, or `0,"No error"` when the queue is empty. `*CLS` empties the calling Session's queue only; `*RST` leaves it alone.

The queue holds 20 entries. When it is full, the newest entry is replaced by `-350,"Queue overflow"` and further errors are dropped until entries are read, so the oldest errors, which name the first cause of a failure, are kept.

## Error codes

| Code | Message | Reported when |
|---|---|---|
| `0` | No error | `SYST:ERR?` on an empty queue |
| `-102` | Syntax error | The line breaks the grammar; nothing on it runs |
| `-104` | Data type error | A number where a keyword belongs, or a keyword where a number belongs |
| `-108` | Parameter not allowed | More parameters than the command takes |
| `-109` | Missing parameter | Fewer parameters than the command takes |
| `-113` | Undefined header | No command matches the header, including a header suffix out of range (OpenScope does not use SCPI's `-114`) |
| `-131` | Invalid suffix | A number's suffix is not the parameter's unit with an optional multiplier |
| `-222` | Data out of range | A number outside the command's range or beyond a double's range |
| `-224` | Illegal parameter value | A keyword that is not one of the parameter's choices |
| `-350` | Queue overflow | The error queue was full |
| `-363` | Input buffer overrun | The line is longer than 4096 bytes; nothing on it runs |

## Commands

| Command | Reply | Notes |
|---|---|---|
| `*IDN?` | `OpenScope,OS-4,<serial>,<version>` | |
| `*RST` | none | Restores instrument-wide settings; leaves the error queue alone |
| `*CLS` | none | Empties the calling Session's error queue |
| `SYSTem:ERRor[:NEXT]?` | `<code>,"<message>"` | Removes and returns the oldest error |
