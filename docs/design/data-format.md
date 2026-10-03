# Kairos Market Data Binary Format (v1)

Compact, append-only, mmap-able records for L2 order book data and trades.
Written by `tools/recorder`, read by the C++ replay engine (`kairos::data`).

## Design goals

- **Fixed-size everything**: the reader walks the file with pure pointer
  arithmetic. No length-prefixed strings, no varints.
- **Little-endian** throughout.
- **Self-describing header**: version, symbol, venue, price/qty scales.
- **Dual timestamps** on every event (see below).

## File layout

```
[File header: 64 bytes][Event 1][Event 2]...
Event = [Event header: 48 bytes][n_levels × Level entry: 24 bytes]
```

### File header (64 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic = `0x5249414B` (ASCII "KAIR") |
| 4 | 2 | version = 1 |
| 6 | 2 | header_size = 64 |
| 8 | 16 | symbol, null-padded ASCII (e.g. `"BTC-USD"`) |
| 24 | 16 | venue, null-padded ASCII (e.g. `"coinbase"`) |
| 40 | 8 | price_scale, int64 (price_ticks = round(price × scale)) |
| 48 | 4 | qty_scale, uint32 (qty_lots = round(qty × scale)) |
| 52 | 4 | crc32 of all bytes after the header (0 until the file is finalized) |
| 56 | 8 | created_wall_ns, uint64 |

Recommended scales for Coinbase BTC-USD: price_scale = 100 (cents),
qty_scale = 100_000_000 (satoshis). Both fit comfortably in int64.

### Event header (48 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type: 0 = snapshot, 1 = diff, 2 = trade |
| 1 | 1 | reserved |
| 2 | 2 | reserved |
| 4 | 4 | n_levels, uint32 (level entries following this header) |
| 8 | 8 | seq, uint64 (exchange sequence number; 0 if the venue provides none) |
| 16 | 8 | exchange_ts_ns, uint64 (0 if unknown) |
| 24 | 8 | local_mono_ns, uint64 (monotonic clock at receive) |
| 32 | 8 | local_wall_ns, uint64 (wall clock at receive) |
| 40 | 8 | reserved |

### Level entry (24 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | price_ticks, int64 |
| 8 | 8 | qty_lots, int64 (0 = level removed, for diffs) |
| 16 | 1 | side: 0 = bid, 1 = ask |
| 17 | 7 | reserved |

### Event encoding

- **Snapshot** (`type=0`): the full L2 book at `seq`. All bids then all
  asks as level entries (order within a side is unspecified; the reader
  must not rely on it).
- **Diff** (`type=1`): level updates. `qty_lots == 0` removes the level.
  Entries may mix sides.
- **Trade** (`type=2`): exactly one level entry; `side` is the *taker* side
  (0 = buyer-initiated, i.e. the taker was a bid; 1 = seller-initiated).

## Timestamps: why both

- `exchange_ts_ns` orders events in replay. It is the venue's clock, so it
  is consistent across all events in the file and gives deterministic
  replay. (Caveat: exchange timestamps have finite resolution — Coinbase
  ISO8601 has microsecond precision — so two events can share a timestamp.
  Replay breaks ties by file order, which is receive order.)
- `local_mono_ns` / `local_wall_ns` measure **feed latency**
  (exchange → me): `local - exchange`. The latency model (Phase 2C) is
  calibrated from this distribution. Monotonic is used for intervals;
  wall anchors the capture to real time.

## File rotation

The recorder rotates files by time (default: hourly) or size (default:
1 GB), whichever comes first. Each file has its own header. File names:
`{venue}_{symbol}_{start_wall_ns}.kai`. A capture session is the set of
files; the replay engine accepts a list.

## Checksums and validation

- The reader validates `magic` and `version` on open.
- The recorder writes a per-file CRC32 (of everything after the header)
  into the header's reserved space at close/rotation. The replay engine
  verifies it.
- Sequence gaps are detected at record time (see recorder docs); a gap
  forces a resnapshot, recorded as a fresh `snapshot` event. **The file
  never contains an undetected gap**: every gap produces either a
  resnapshot or the recorder aborts the file.

## C++ reader

`include/kairos/data_format.hpp`: `mmap`s the file, validates the header,
and provides a forward iterator over events (`EventView { header, levels }`).
No allocation, no exceptions.
