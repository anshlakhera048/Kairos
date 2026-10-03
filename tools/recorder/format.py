"""Kairos market data binary format v1: writer (and a reader for verification).

See docs/design/data-format.md for the spec.
"""

import os
import struct
import time
import zlib

MAGIC = 0x5249414B  # "KAIR"
VERSION = 1
HEADER_SIZE = 64

HEADER_STRUCT = struct.Struct("<IHH16s16sqIIQ")  # magic, ver, hsize, sym, venue, pscale, qscale, crc, wall (64B)
EVENT_STRUCT = struct.Struct("<BBHIQQQQQ")      # type, rsv, rsv, n_levels, seq, exch_ts, mono, wall, rsv (48B)
LEVEL_STRUCT = struct.Struct("<qqB7s")          # price_ticks, qty_lots, side, pad (24B)

# Event types
SNAPSHOT = 0
DIFF = 1
TRADE = 2

# Sides
BID = 0
ASK = 1


class Writer:
    """Append-only writer for one capture file."""

    def __init__(self, path, symbol, venue, price_scale=100, qty_scale=100_000_000):
        self.path = path
        self.symbol = symbol
        self.venue = venue
        self.price_scale = price_scale
        self.qty_scale = qty_scale
        self._f = open(path, "wb")
        self._crc = 0
        # Header with crc=0 placeholder; finalized on close().
        self._f.write(
            HEADER_STRUCT.pack(
                MAGIC,
                VERSION,
                HEADER_SIZE,
                symbol.encode("ascii")[:16].ljust(16, b"\0"),
                venue.encode("ascii")[:16].ljust(16, b"\0"),
                price_scale,
                qty_scale,
                0,
                time.time_ns(),
            )
        )
        self._f.flush()
        self.events = 0

    def _write(self, data: bytes):
        self._crc = zlib.crc32(data, self._crc)
        self._f.write(data)

    def _now(self):
        return time.monotonic_ns(), time.time_ns()

    def snapshot(self, seq, bids, asks, exchange_ts_ns=0):
        """bids/asks: iterable of (price_float, qty_float)."""
        mono, wall = self._now()
        levels = [(p, q, BID) for p, q in bids] + [(p, q, ASK) for p, q in asks]
        self._write(
            EVENT_STRUCT.pack(SNAPSHOT, 0, 0, len(levels), seq, exchange_ts_ns, mono, wall, 0)
        )
        for p, q, side in levels:
            self._write(
                LEVEL_STRUCT.pack(
                    round(p * self.price_scale),
                    round(q * self.qty_scale),
                    side,
                    b"\0" * 7,
                )
            )
        self.events += 1

    def diff(self, seq, changes, exchange_ts_ns=0):
        """changes: iterable of (side, price_float, qty_float); qty 0 = remove."""
        mono, wall = self._now()
        changes = list(changes)
        self._write(
            EVENT_STRUCT.pack(DIFF, 0, 0, len(changes), seq, exchange_ts_ns, mono, wall, 0)
        )
        for side, p, q in changes:
            self._write(
                LEVEL_STRUCT.pack(
                    round(p * self.price_scale),
                    round(q * self.qty_scale),
                    side,
                    b"\0" * 7,
                )
            )
        self.events += 1

    def trade(self, seq, side, price, qty, exchange_ts_ns=0):
        """side: taker side (BID = buyer-initiated)."""
        mono, wall = self._now()
        self._write(
            EVENT_STRUCT.pack(TRADE, 0, 0, 1, seq, exchange_ts_ns, mono, wall, 0)
        )
        self._write(
            LEVEL_STRUCT.pack(
                round(price * self.price_scale),
                round(qty * self.qty_scale),
                side,
                b"\0" * 7,
            )
        )
        self.events += 1

    def flush(self):
        self._f.flush()
        os.fsync(self._f.fileno())

    def close(self):
        # Finalize: write crc32 into the header.
        self.flush()
        self._f.seek(52)
        self._f.write(struct.pack("<I", self._crc & 0xFFFFFFFF))
        self._f.close()
        return self.events


class Reader:
    """Simple reader (for verification; the C++ reader is the real one)."""

    def __init__(self, path):
        self._f = open(path, "rb")
        header = self._f.read(HEADER_SIZE)
        (magic, version, hsize, sym, venue, pscale, qscale, crc, wall) = HEADER_STRUCT.unpack(header)
        assert magic == MAGIC, f"bad magic {magic:#x}"
        assert version == VERSION, f"bad version {version}"
        self.symbol = sym.rstrip(b"\0").decode("ascii")
        self.venue = venue.rstrip(b"\0").decode("ascii")
        self.price_scale = pscale
        self.qty_scale = qscale
        self.stored_crc = crc

    def events(self):
        while True:
            hdr = self._f.read(EVENT_STRUCT.size)
            if len(hdr) < EVENT_STRUCT.size:
                break
            (etype, _, _, n_levels, seq, exch_ts, mono, wall, _) = EVENT_STRUCT.unpack(hdr)
            levels = []
            for _ in range(n_levels):
                data = self._f.read(LEVEL_STRUCT.size)
                pt, ql, side, _ = LEVEL_STRUCT.unpack(data)
                levels.append((pt / self.price_scale, ql / self.qty_scale, side))
            yield {
                "type": etype,
                "n_levels": n_levels,
                "seq": seq,
                "exchange_ts_ns": exch_ts,
                "mono_ns": mono,
                "wall_ns": wall,
                "levels": levels,
            }

    def verify_crc(self):
        self._f.seek(HEADER_SIZE)
        crc = 0
        while True:
            chunk = self._f.read(1 << 20)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
        return (crc & 0xFFFFFFFF) == self.stored_crc

    def close(self):
        self._f.close()
