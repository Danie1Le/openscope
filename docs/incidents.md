# Incident notes

Short notes on real bugs that a sanitizer, the fuzzer, the soak test or another verification test found, as the spec's Testing Decisions ask: what was seen, why, the fix and the test that now guards it. Newest first. Longer fuzz runs are recorded at the end.

## [#71](https://github.com/Danie1Le/openscope/issues/71): Running at 4 channels x 1M points dropped blocks with no client

- **Found by:** measuring the #67 threading fix ([#68](https://github.com/Danie1Le/openscope/pull/68)) with the engine's `live` and `stopping` counters; under TSan, the same drops made `test_the_holdoff_spaces_the_trigger_events_of_captures` flaky.
- **Symptom:** with no client connected, Running with all four channels at 1 MS/s and `ACQ:POIN 1048576` dropped about 16 blocks and discarded one capture a second (330 blocks and 17 captures in 20 s here, 294 and 17 on a hosted runner), against NFR-1's zero.
- **Cause:** the processing thread measured every Running capture for the statistics before publishing it. `measure_all` of four 1M-sample channels takes 111 to 122 ms, and the ring buffer holds 64 blocks, about 64 ms. Before #68, a client's waveform or FFT query held the instrument's mutex and paused acquisition, which hid it. Under TSan a Single also made processing measure a second, stale capture assembled after it, so the first blocks after `RUN` overflowed.
- **Fix:** the processing thread publishes each capture, a Single's included, as it completes, and a measuring thread measures and counts it, through a bounded queue that processing waits on rather than leave a capture uncounted; `MEAS:STAT?` waits until every capture published before it is counted, and a capture completed before a statistics reset is never counted after it ([#75](https://github.com/Danie1Le/openscope/pull/75)). Five Singles at 4 x 1M, which had dropped 43 to 46 blocks, drop none. 0 dropped blocks in 20 s here and 0 on a hosted runner afterwards.
- **Regression tests:** `LiveLongestRecordTest` (Running and Singles at 4 x 1M, release builds only, with and without a client), `MeasuringQueueTest`, the `BlockProcessingTest` hand-off, reset, STOP and SINGle tests, the CI benchmark job's `live_run` at 4 x 1M points (reported), and the TSan holdoff test (0 of 48 runs failed, 31 of 48 before).

## [#65](https://github.com/Danie1Le/openscope/issues/65): the hour-long soak never finished on a hosted runner

- **Found by:** the Soak workflow on `main` with `seconds=3600` ([run 45060107416](https://github.com/Danie1Le/openscope/actions/runs/45060107416)).
- **Symptom:** attempt 1 lost its runner after about 41 minutes; attempt 2 ran past the step's own timeout and had to be force-cancelled.
- **Cause:** the test, not the engine. `Engine.events` in the TCP suite kept every engine log line, and the churn worker logs two per connection, so pytest grew by 0.9 to 3.4 MiB/s until the runner ran out of memory and stalled. The unpaced churn also held about 14,000 sockets in TIME_WAIT.
- **Fix:** the suite keeps only the newest 1000 events, `wait_for` fails loudly when events it had not looked at were dropped, and the churn pauses 10 ms a round ([#66](https://github.com/Danie1Le/openscope/pull/66)).
- **Regression tests:** `test_engine_fixture.py` (bounded history, the dropped-events failure, the memory reading), and the soak itself, which now also fails if the test's own resident memory grows by 32 MiB.

## [#58](https://github.com/Danie1Le/openscope/issues/58): every query through the client took 44 ms

- **Found by:** the TCP verification suite's latency tests ([#15](https://github.com/Danie1Le/openscope/issues/15)).
- **Symptom:** a reply to a pipelined line, such as the `SYST:ERR?` the client sends after every line, arrived about 40 ms late, so a query through `openscope-client` took 44 ms against NFR-4's 20 ms.
- **Cause:** the server never set `TCP_NODELAY`, so Nagle's algorithm held the second reply until the client's delayed acknowledgement of the first.
- **Fix:** the acceptor sets `TCP_NODELAY` before it listens, and accepted sockets inherit it ([#63](https://github.com/Danie1Le/openscope/pull/63)). `*IDN?` plus `SYST:ERR?` went from 44.06 ms to 0.10 ms.
- **Regression tests:** `ScpiServerTest.SendsEachReplyWithoutWaitingForAnAckWithTcpNoDelay`, and `test_pipelined_replies_are_not_delayed` (median of 50 queries through the client under 20 ms).

## [#52](https://github.com/Danie1Le/openscope/issues/52): false No signal on clean or lightly noisy triangle, ramp and sine captures

- **Found by:** a sweep of signal shapes and sampling rates run while reviewing [#10](https://github.com/Danie1Le/openscope/issues/10).
- **Symptom:** a clean triangle at 30 samples per cycle, or a sine with 1% noise at 10, read `No signal`, so a valid signal got no frequency.
- **Cause:** a signal sampled at the same few phases each cycle makes a comb of narrow histogram clusters, and `find_levels` took a cluster the signal only passes through as a settled level. Top and base could then both land mid-span.
- **Fix:** a level's bins must also hold samples that dwell there, so a cluster the signal only passes through is no level ([#53](https://github.com/Danie1Le/openscope/pull/53)).
- **Regression tests:** `SignalLevelsTest.ClustersTheSignalPassesThroughAreNoLevels`, `FewSamplingPhasesTest` and `NoSignalSweepTest` (shape × samples per cycle × cycles × noise).

## Longer fuzz runs

The CI fuzz smoke runs 60 s on every pull request. Longer runs are done on demand (README, Fuzzing) and recorded here.

| Date | Target | Commit | Length | Executions | Corpus | Result |
|---|---|---|---|---|---|---|
| 2026-11-23 | `execute_line_fuzzer` (the only target) | c365d09 (#69, with the #68 reply limit) | 1 201 s (`-max_total_time=1200`) | 28 701 (23 a second) | the 7 seeds in `fuzz/corpus` grew to 1 066 inputs (1.4 MiB); 8 934 edges covered | no crash, leak, timeout or sanitizer report; peak RSS 476 MiB |

The run used the `fuzz` preset (Clang 18, ASan + UBSan) in `openscope-dev:24.04` with `-max_len=8192 -timeout=10 -dict=fuzz/scpi.dict` and an empty output corpus, as in README, Fuzzing. Each input builds a fresh Instrument, so a run executes about 25 inputs a second. The fuzzer advances 64 samples a line, so its captures stay short: it cannot reach the 64 MiB reply limit (`-223`), whose boundary the command-tree and waveform tests pin instead.
