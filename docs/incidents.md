# Incident notes

Short notes on real bugs that a sanitizer, the fuzzer, the soak test or another verification test found, as the spec's Testing Decisions ask: what was seen, why, the fix and the test that now guards it. Newest first. Longer fuzz runs are recorded at the end.

## #65: the hour-long soak never finished on a hosted runner

- **Found by:** the Soak workflow on `main` with `seconds=3600` (run 45060107416).
- **Symptom:** attempt 1 lost its runner after about 41 minutes; attempt 2 ran past the step's own timeout and had to be force-cancelled.
- **Cause:** the test, not the engine. `Engine.events` in the TCP suite kept every engine log line, and the churn worker logs two per connection, so pytest grew by 0.9 to 3.4 MiB/s until the runner ran out of memory and stalled. The unpaced churn also held about 14,000 sockets in TIME_WAIT.
- **Fix:** the suite keeps only the newest 1000 events, `wait_for` fails loudly when events it had not looked at were dropped, and the churn pauses 10 ms a round (#66).
- **Regression tests:** `test_engine_fixture.py` (bounded history, the dropped-events failure, the memory reading), and the soak itself, which now also fails if the test's own resident memory grows by 32 MiB.

## #58: every query through the client took 44 ms

- **Found by:** the TCP verification suite's latency tests (#15).
- **Symptom:** a reply to a pipelined line, such as the `SYST:ERR?` the client sends after every line, arrived about 40 ms late, so a query through `openscope-client` took 44 ms against NFR-4's 20 ms.
- **Cause:** the server never set `TCP_NODELAY`, so Nagle's algorithm held the second reply until the client's delayed acknowledgement of the first.
- **Fix:** the acceptor sets `TCP_NODELAY` before it listens, and accepted sockets inherit it (#63). `*IDN?` plus `SYST:ERR?` went from 44.06 ms to 0.10 ms.
- **Regression tests:** `ScpiServerTest.SendsEachReplyWithoutWaitingForAnAckWithTcpNoDelay`, and `test_pipelined_replies_are_not_delayed` (median of 50 queries through the client under 20 ms).

## #52: false No signal on clean triangle, ramp and sine captures

- **Found by:** a sweep of signal shapes and sampling rates run while reviewing #10.
- **Symptom:** a clean triangle at 30 samples per cycle, or a sine with 1% noise at 10, read `No signal`, so a valid signal got no frequency.
- **Cause:** a signal sampled at the same few phases each cycle makes a comb of narrow histogram clusters, and `find_levels` took a cluster the signal only passes through as a settled level. Top and base could then both land mid-span.
- **Fix:** a level's bins must also hold samples that dwell there, so a cluster the signal only passes through is no level (#53).
- **Regression tests:** `MeasurementTest.ClustersTheSignalPassesThroughAreNoLevels`, `FewSamplingPhasesTest` and `NoSignalSweepTest` (shape × samples per cycle × cycles × noise).

## Longer fuzz runs

The CI fuzz smoke runs 60 s on every pull request. Longer runs are done on demand (README, Fuzzing) and recorded here.

| Date | Target | Commit | Length | Executions | Corpus | Result |
|---|---|---|---|---|---|---|
FUZZ_RECORD
