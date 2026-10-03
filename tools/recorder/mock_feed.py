#!/usr/bin/env python3
"""Mock Coinbase websocket server for testing the recorder's sync logic.

Serves a scripted feed: snapshot, sequenced l2updates (with an intentional
gap to trigger resync), and matches. Runs on localhost (no proxy involved).
"""

import asyncio
import json
import sys

HOST, PORT = "127.0.0.1", 8765

SCRIPT = [
    # snapshot
    {"type": "snapshot", "product_id": "BTC-USD", "sequence": 100,
     "bids": [["50000.00", "1.0"], ["49999.00", "2.0"]],
     "asks": [["50001.00", "1.5"], ["50002.00", "0.5"]],
     "time": "2026-10-03T00:00:00.000000Z"},
    # clean updates
    {"type": "l2update", "product_id": "BTC-USD", "sequence": 101,
     "changes": [["buy", "50000.00", "0.5"]], "time": "2026-10-03T00:00:01.000000Z"},
    {"type": "l2update", "product_id": "BTC-USD", "sequence": 102,
     "changes": [["sell", "50003.00", "1.0"]], "time": "2026-10-03T00:00:02.000000Z"},
    # GAP: skip 103 -> recorder must resync (drop state, wait for snapshot)
    {"type": "l2update", "product_id": "BTC-USD", "sequence": 104,
     "changes": [["buy", "49998.00", "3.0"]], "time": "2026-10-03T00:00:03.000000Z"},
    # resnapshot after gap
    {"type": "snapshot", "product_id": "BTC-USD", "sequence": 104,
     "bids": [["50000.00", "0.5"], ["49998.00", "3.0"]],
     "asks": [["50001.00", "1.5"], ["50003.00", "1.0"]],
     "time": "2026-10-03T00:00:04.000000Z"},
    {"type": "l2update", "product_id": "BTC-USD", "sequence": 105,
     "changes": [["sell", "50001.00", "0.0"]], "time": "2026-10-03T00:00:05.000000Z"},
    {"type": "match", "product_id": "BTC-USD", "sequence": 106,
     "price": "50001.00", "size": "0.25", "side": "s",
     "time": "2026-10-03T00:00:06.000000Z"},
]


async def handle(reader, writer):
    # Minimal websocket handshake.
    data = await reader.readuntil(b"\r\n\r\n")
    headers = data.decode()
    key = [l for l in headers.split("\r\n") if l.lower().startswith("sec-websocket-key")][0].split(":")[1].strip()
    import hashlib, base64
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    writer.write(
        f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n".encode()
    )
    await writer.drain()

    def ws_frame(payload: bytes):
        # Server -> client: unmasked text frame.
        out = bytearray([0x81])
        n = len(payload)
        if n < 126:
            out.append(n)
        elif n < 65536:
            out += bytes([126]) + n.to_bytes(2, "big")
        else:
            out += bytes([127]) + n.to_bytes(8, "big")
        out += payload
        return bytes(out)

    # Read (and ignore) the subscribe frame; then play the script in a loop.
    try:
        while True:
            for msg in SCRIPT:
                writer.write(ws_frame(json.dumps(msg).encode()))
                await writer.drain()
                await asyncio.sleep(0.05)
    except Exception:
        pass
    finally:
        writer.close()


async def main():
    server = await asyncio.start_server(handle, HOST, PORT)
    print(f"mock feed on {HOST}:{PORT}", flush=True)
    await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
