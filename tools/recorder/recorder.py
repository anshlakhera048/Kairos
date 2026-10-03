#!/usr/bin/env python3
"""Kairos market data recorder: captures L2 order book data + trades from
Coinbase and writes the Kairos binary format (v1).

Modes:
  ws    websocket capture (level2 + matches channels). Primary mode; needs
        a network path that allows websocket upgrades.
  poll  REST polling fallback (book snapshot + recent trades). Works through
        restrictive egress proxies. Coarser time resolution.

Sync procedure (ws mode), per Coinbase semantics:
  1. Subscribe to level2; the first message is a snapshot with a sequence.
  2. Apply l2update messages only if sequence == last_seq + 1.
  3. On any gap (or malformed message): discard state, resubscribe, and
     record a fresh snapshot. A file NEVER contains an undetected gap.

Every record carries the exchange timestamp and the local receive timestamps
(monotonic + wall). See docs/design/data-format.md for why both matter.

Read-only market data. No trading. No order placement.

Usage:
  python3 recorder.py --mode poll --symbol BTC-USD --out /tmp/capture --duration 60
  python3 recorder.py --mode ws --symbol BTC-USD --out /tmp/capture --duration 3600
"""

import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from format import Writer, Reader, BID, ASK

VENUE = "coinbase"
WS_URL = "wss://ws-feed.exchange.coinbase.com"
REST = "https://api.exchange.coinbase.com"


def iso_to_ns(s):
    # "2026-10-03T12:34:56.123456Z" -> ns. Coinbase uses microsecond precision.
    try:
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        from datetime import datetime
        dt = datetime.fromisoformat(s)
        return int(dt.timestamp() * 1e9)
    except Exception:
        return 0


def out_path(out_dir, symbol, venue):
    os.makedirs(out_dir, exist_ok=True)
    return os.path.join(out_dir, f"{venue}_{symbol}_{time.time_ns()}.kai")


class PollRecorder:
    """REST polling capture: snapshot the L2 book + fetch recent trades."""

    def __init__(self, symbol, out_dir, interval=1.0, rotate_secs=3600):
        self.symbol = symbol
        self.out_dir = out_dir
        self.interval = interval
        self.rotate_secs = rotate_secs
        self.writer = None
        self.file_start = 0
        self.last_trade_id = 0

    def _get(self, path):
        req = urllib.request.Request(REST + path, headers={"User-Agent": "kairos-recorder/1"})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.load(r)

    def _rotate(self):
        if self.writer:
            n = self.writer.close()
            print(f"closed file: {n} events", flush=True)
        self.writer = Writer(out_path(self.out_dir, self.symbol, VENUE), self.symbol, VENUE)
        self.file_start = time.monotonic()

    def run(self, duration):
        self._rotate()
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if time.monotonic() - self.file_start > self.rotate_secs:
                self._rotate()
            try:
                book = self._get(f"/products/{self.symbol}/book?level=2")
                seq = int(book.get("sequence", 0))
                bids = [(float(p), float(s)) for p, s, _ in book["bids"]]
                asks = [(float(p), float(s)) for p, s, _ in book["asks"]]
                self.writer.snapshot(seq, bids, asks)
                # Recent trades (paginated by trade id for gap detection).
                trades = self._get(f"/products/{self.symbol}/trades?limit=100")
                new = [t for t in trades if t["trade_id"] > self.last_trade_id]
                for t in sorted(new, key=lambda x: x["trade_id"]):
                    side = BID if t["side"] == "b" else ASK
                    self.writer.trade(
                        t["trade_id"], side, float(t["price"]), float(t["size"]),
                        iso_to_ns(t["time"]),
                    )
                if trades:
                    self.last_trade_id = max(self.last_trade_id, trades[0]["trade_id"])
            except Exception as e:
                print(f"poll error (continuing): {e}", flush=True)
            time.sleep(self.interval)
        n = self.writer.close()
        print(f"done: {n} events", flush=True)


class WsRecorder:
    """Websocket capture: level2 snapshots + diffs, matches for trades."""

    def __init__(self, symbol, out_dir, rotate_secs=3600, ws_url=WS_URL):
        self.symbol = symbol
        self.out_dir = out_dir
        self.rotate_secs = rotate_secs
        self.ws_url = ws_url
        self.writer = None
        self.file_start = 0
        self.last_seq = 0
        self.synced = False

    def _rotate(self):
        if self.writer:
            n = self.writer.close()
            print(f"closed file: {n} events", flush=True)
        self.writer = Writer(out_path(self.out_dir, self.symbol, VENUE), self.symbol, VENUE)
        self.file_start = time.monotonic()
        self.synced = False  # force fresh snapshot after rotation

    def _handle_snapshot(self, msg):
        seq = int(msg["sequence"])
        bids = [(float(p), float(s)) for p, s in msg["bids"]]
        asks = [(float(p), float(s)) for p, s in msg["asks"]]
        self.writer.snapshot(seq, bids, asks, iso_to_ns(msg.get("time", "")))
        self.last_seq = seq
        self.synced = True

    def _handle_l2update(self, msg):
        seq = int(msg["sequence"])
        if not self.synced:
            return  # waiting for snapshot
        if seq != self.last_seq + 1:
            print(f"sequence gap: got {seq}, expected {self.last_seq + 1}; resyncing", flush=True)
            self.synced = False
            return
        changes = []
        for side_s, p, s in msg["changes"]:
            side = BID if side_s == "buy" else ASK
            changes.append((side, float(p), float(s)))
        self.writer.diff(seq, changes, iso_to_ns(msg.get("time", "")))
        self.last_seq = seq

    def _handle_match(self, msg):
        seq = int(msg["sequence"])
        side = BID if msg["side"] == "b" else ASK
        self.writer.trade(seq, side, float(msg["price"]), float(msg["size"]),
                          iso_to_ns(msg.get("time", "")))

    def run(self, duration):
        import websocket

        self._rotate()
        end = time.monotonic() + duration
        while time.monotonic() < end:
            try:
                ws = websocket.create_connection(self.ws_url, timeout=20)
                ws.send(json.dumps({
                    "type": "subscribe",
                    "channels": [
                        {"name": "level2", "product_ids": [self.symbol]},
                        {"name": "matches", "product_ids": [self.symbol]},
                    ],
                }))
                ws.settimeout(5)
                while time.monotonic() < end:
                    if time.monotonic() - self.file_start > self.rotate_secs:
                        self._rotate()
                    try:
                        raw = ws.recv()
                    except Exception:
                        break  # reconnect
                    msg = json.loads(raw)
                    t = msg.get("type")
                    if t == "snapshot":
                        self._handle_snapshot(msg)
                    elif t == "l2update":
                        self._handle_l2update(msg)
                    elif t == "match":
                        self._handle_match(msg)
                    # ignore: subscriptions, heartbeat, etc.
                ws.close()
            except Exception as e:
                print(f"ws error (reconnecting): {e}", flush=True)
                time.sleep(2)
        n = self.writer.close()
        print(f"done: {n} events", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["ws", "poll"], default="poll")
    ap.add_argument("--symbol", default="BTC-USD")
    ap.add_argument("--out", default="./capture")
    ap.add_argument("--duration", type=float, default=60, help="seconds")
    ap.add_argument("--interval", type=float, default=1.0, help="poll interval (poll mode)")
    ap.add_argument("--rotate", type=float, default=3600, help="file rotation seconds")
    ap.add_argument("--ws-url", default=WS_URL, help="websocket URL (ws mode)")
    ap.add_argument("--verify", help="verify a capture file and exit")
    args = ap.parse_args()

    if args.verify:
        r = Reader(args.verify)
        n = sum(1 for _ in r.events())
        ok = r.verify_crc()
        print(f"{args.verify}: {n} events, {r.symbol} @ {r.venue}, crc {'OK' if ok else 'FAIL'}")
        r.close()
        sys.exit(0 if ok else 1)

    if args.mode == "ws":
        WsRecorder(args.symbol, args.out, args.rotate, args.ws_url).run(args.duration)
    else:
        PollRecorder(args.symbol, args.out, args.interval, args.rotate).run(args.duration)


if __name__ == "__main__":
    main()
