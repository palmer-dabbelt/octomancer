# Setting timecode over BLE: seven encodings, one live parameter

**Camera:** Blackmagic Pocket Cinema Camera 6K Pro (`A:1EAE18A7`)
**Tested:** 2026-08-25, 11:53–11:58 local, camera idle and not recording
**Tool:** `octomancer-sync --poke HEX`
**Reproduce:** `octomancer-sync --camera <id> --poke "<hex>" --poke-watch 4`

## Summary

Two questions were open after `doc/ble-write-failure-report.md`: whether the
single 9.4 write we had tried failed because of its *encoding*, and whether any
other parameter could move the timecode. Both now have answers.

| | |
| --- | --- |
| 9.4 (timecode value), seven distinct encodings | **all silently ignored** |
| 4.7 (Timecode Source), int8 | **writable, and it moves the generator** |

The encoding hypothesis is dead. The parameter that actually does something was
sitting in the connection state dump the whole time.

## Finding 1 — 9.4 ignores every encoding, not just ours

The earlier report tested exactly one shape and could not distinguish "wrong
encoding" from "not implemented". We now have seven, all GATT-accepted and all
without effect:

| encoding | bytes | result |
| --- | --- | --- |
| BCD HHMMSSFF LE (the original attempt) | `ff 08 00 00 09 04 03 00 12 33 22 11` | ignored |
| BCD LE, reserved byte `0xff` | `ff 08 00 ff 09 04 03 00 12 33 22 11` | ignored |
| BCD, big-endian | `ff 08 00 00 09 04 03 00 11 22 33 12` | ignored |
| u32 frame counter since midnight | `ff 08 00 00 09 04 03 00 64 ff 0e 00` | ignored |
| four binary int8s (h, m, s, f) | `ff 08 00 00 09 04 01 00 0b 16 21 0c` | ignored |
| int32[2], mirroring the RTC's {time, date} | `ff 0c 00 00 09 04 03 00 12 33 22 11 25 08 26 20` | ignored |
| op=1 (offset) rather than op=0 (assign) | `ff 08 00 00 09 04 03 01 01 00 00 00` | ignored |

Two of these deserve comment.

**The reserved byte was the best remaining structural guess and it is not the
gate.** Every 9.4 message the camera *emits* carries `0xff` in header byte 3;
every message we had ever sent carried `0x00`, because `build_packet` hardcodes
it. Matching the camera's own dialect exactly changes nothing.

**Five of the seven were run with the generator frozen**, which is what makes
them clean. Free-running timecode masks a failed write: the value keeps
advancing either way, so "no effect" and "small effect" look alike. Setting 4.7
to 1 (below) parks the timecode at `00:00:00:00` and stops it, and against a
stopped clock any change at all would have been unmistakable. Nothing moved.

The control ran in the same session and in the same code path: white balance
(1.2, int16[2]) written as `ff 08 00 00 01 02 02 00 e0 15 00 00` took effect and
was echoed back as `1.2 [5600, 0]`. The tunnel was demonstrably live throughout.

**Conclusion: 9.4 is read-only telemetry.** The gap reported to Blackmagic is
real and is not an encoding mistake at our end.

## Finding 2 — 4.7 Timecode Source is writable

`4.7` appears in the connection state dump as `[int8] op=2 [0]`. It is
undocumented in the parameter table, it was missed when the earlier report
enumerated the state dump (that list omits group 4 entirely), and
`bmd::param_name()` does not know it — so `--watch` renders the most
interesting parameter on the camera as an anonymous `4.7  0`.

Writing it works, and it is echoed back:

```
--> ff 05 00 00 04 07 01 00 01 00 00 00      (4.7 = 1)
    timecode before: 11:56:24:20
    ECHO 4.7 changed to [1]
    timecode after:  00:00:00:00   (moved -42984.83s, 4.0s elapsed)

--> ff 05 00 00 04 07 01 00 00 00 00 00      (4.7 = 0)
    timecode before: 00:00:00:00
    ECHO 4.7 changed to [0]
    timecode after:  11:56:53:07   (moved +43013.29s, 4.0s elapsed)
```

With 4.7 = 1 the timecode reads `00:00:00:00` and **does not advance** — five
consecutive four-second observations all measured exactly +0.00 s. Returning
4.7 to 0 restores free-running time of day immediately.

The value is a boolean, not a wider enum: writing 2 or 3 both clamp to 1.

This is the first write of any kind that has moved this camera's timecode
generator. It is not yet a jam-sync route — mode 1 parks at zero and stays
there rather than accepting a value — but it establishes that the generator is
reachable over BLE at all, which nothing before this did.

## Consequence for octomancer

`octomancer-sync` reads 9.4 and compares it against the bench. If a user sets
Timecode Source to 1 on the camera, 9.4 reports `00:00:00:00` and stops, and
the daemon sees a stationary clock roughly twelve hours wrong. That is
indistinguishable, to the current code, from a camera whose clock is
catastrophically off — and the response to a large error is to write the RTC,
which will not fix it, because in that mode the timecode is not following the
RTC at all.

Reading 4.7 on connect and refusing to sync when it is 1 is the obvious guard.
It is not implemented yet.

## What was not established

- **What mode 1 *is*.** It parks at zero and holds while the camera is idle.
  Record Run and a dormant external input both look like that when nothing is
  recording and nothing is plugged in. Distinguishing them needs a take rolled
  in that mode, which was not done.
- **Whether 9.4 becomes writable during recording.** Not tested; deliberately,
  since a timecode discontinuity mid-take is the one failure worse than a
  wrong clock.
