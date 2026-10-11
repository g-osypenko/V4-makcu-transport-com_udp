# Request: Raw movement at 1 kHz over the UDP (Ethernet) API

**Firmware:** V4.094 (test). **Mouse:** Logitech Lightspeed receiver 046D:C547 at 1000 Hz on USB3,
through a USB hub that also carries an RTL8152 USB-Ethernet adapter. **Transport:** UDP port 8080,
RAW wrapper (`0x55` + 8-byte transaction + MAK_API frame), no encryption.

## Goal

We send Raw movement (`0x69 RAW_MOVE` + `0x6A RAW_STATUS consume=1`) at 1000 commands/s.

- **Over COM (CH343, 4 Mbaud) this already works:** 997–1001 HID reports/s, 99.7–100 % of
  intervals are 1 ms, and the path received by the PC matches the path sent, count for count.
- **Over UDP the same client reaches only ~283 reports/s.** The measurements below show that the
  limit is the per-frame cost of the UDP command path, not the network.

## Measurements (V4.094, repeatable; scripts available on request)

1. **Every datagram reaches the Ethernet interface.** We sent 1000 queries (`0x6A`, id 0) at 1 ms
   intervals. The netif RX counter (`0x3D [22,0]`) rose by 1018, but only 581 queries were answered.
2. **Each valid frame occupies the UDP command server for ~1.72 ms** (range 1.60–1.85 ms, not
   aligned to 1 ms). Behaviour within one cycle:
   - a datagram arriving within ~0.65 ms after an accepted one is discarded;
   - one datagram arriving later waits and is served exactly 1.72 ms after the previous one;
   - a third datagram during that time is discarded.

   The cost is the same for `0x02`, `0x04`, `0x6A`, `0x3F`, SET `0x16` (no reply), an unknown
   opcode (reply `FF`) and KM text `0x6B`. `0x3D` costs more (RTT 1.96 ms). The maximum is about
   **581 frames/s**. Lost requests are never executed: a lost `0x69` is unknown to a later `0x6A`
   query.
3. **One frame per datagram, strictly.** A datagram is accepted only if its length equals
   9 + 5 + LEN. All of the following are dropped:
   - two or more frames after one header;
   - two or more complete `0x55` records;
   - padding;
   - fragmented datagrams.
4. **Raw needs two frames per report.** A `0x69` move uses one record, and each record must be
   freed by its own `0x6A consume=1` (16 records). With at most ~581 frames/s, the ceiling is
   ~290 reports/s. Our best client keeps exactly one datagram in flight and sends the next one
   when the reply arrives. It gets 282–283 reports/s with an exact path and zero rejects.
5. **`0x18 MOVE` over UDP loses most of the path.** At 500 commands/s with interpolation 0, the
   HID rate is 554 Hz but only 28 % of the path arrives. At 1000 commands/s: 415 Hz, 14 %. Over
   COM, `0x18` at 500 commands/s keeps the path exact.
6. **The UDP transport already accepts many records per datagram — it is not the bottleneck.**
   The performance test (`0x3F`) takes a batch of records in one datagram (`0x3F 06 <count> <id×count>`,
   count up to 62). Sending 62-record batches at the datagram limit, the board accepted
   **34,643 records/s** (589 datagrams/s × 62, peak 35,109/s) — about 60× the ~581 single-frame
   ceiling. So the ~1.72 ms per-frame cost is per *reply-bearing single* frame, not a transport
   limit. Movement (`0x69`) is the only thing still capped at one record per datagram: it rejects
   any payload other than exactly 10 bytes (`state 255 / reason 1`).

## Requests (any of A–C would unblock 1 kHz over UDP)

- **A. Let `0x69` (and `0x6A`) batch, the way `0x3F` already does.** Your UDP transport already
  accepts up to 62 records in one datagram for the performance test (`0x3F 06 <count> ...`), at
  34,643 records/s measured. Accepting the same batch framing for Raw movement — several
  `(id, x, y, timeout)` records in one `0x69`, with batched replies — reaches 1000 reports/s at
  today's ~580 datagrams/s (2 moves + 2 acks per datagram every 2 ms is already enough).
- **B. A cumulative Raw receipt.** For example, `0x6A` with `consume = 2` would release every
  terminal record with `id <= request_id` and return the count and the first failure. One receipt
  per ~10 moves would lift UDP Raw to ~530 reports/s. Together with A or C it reaches 1 kHz.
- **C. A shorter UDP command cycle.** Today it is ~1.72 ms per frame; COM handles more than 2000
  frames/s. Two possible changes:
  - pipeline the forwarding instead of stop-and-wait;
  - remove the fixed ~1 ms after the reply.
- **D. (Optional) A multi-record Raw move.** One frame with N `(id, x, y)` records, each going
  out in its own HID report, the same as N × `0x69`.

## Questions

1. Is ~1.72 ms per UDP frame expected on MAKCU? Is there a setting or a test build that changes it?
2. Does the encrypted UDP path (`0x03` record) have the same per-frame limit? Can its plaintext
   carry more than one command?
3. Is UDP batching (like BLE `MBAT` on MAKXD) planned for MAKCU?

We can run test builds and report the same measurements: HID rate, interval histogram, path sent
vs received, and Raw counters.
