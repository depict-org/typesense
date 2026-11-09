#!/usr/bin/env python3
"""Synthetic loopback-only import regression; no third-party Python dependencies.

Run against a disposable server: python3 test/repro_import_utf8.py --port 19817
The collection is created and deleted by this script. Exit 1 means lost bytes,
failed imports, missing documents, or a transport failure. Timing is not measured.
"""
import argparse
import hashlib
import gzip
import http.client
import json
import socket
import time
from pathlib import Path


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def fixture(size, count, alphabet):
    docs = [{"id": str(i), "text": ""} for i in range(count)]
    overhead = len(b"\n".join(map(encode, docs)))
    remaining = size - overhead
    pattern = alphabet.encode("utf-8")
    for i, doc in enumerate(docs):
        length = remaining // (count - i)
        # Complete code points only; ASCII padding fixes the exact body size.
        text = pattern * (length // len(pattern))
        doc["text"] = text.decode("utf-8") + "x" * (length - len(text))
        remaining -= length
    body = b"\n".join(map(encode, docs))
    assert len(body) == size
    return docs, body


def rpc(args, method, path, body=None, wire="length", step=65536):
    conn = http.client.HTTPConnection("127.0.0.1", args.port, timeout=60)
    try:
        conn.connect()
        conn.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.putrequest(method, path)
        conn.putheader("X-TYPESENSE-API-KEY", args.api_key)
        conn.putheader("Content-Type", "application/json")
        conn.putheader("User-Agent", "depict-fleet-probe/w28-tsutf8")
        conn.putheader("x-depict-fleet-probe", "1")
        if body is not None and args.gzip and "/documents/import?" in path:
            conn.putheader("Content-Encoding", "gzip")
        if body is not None:
            conn.putheader("Transfer-Encoding" if wire == "chunked" else "Content-Length",
                           "chunked" if wire == "chunked" else str(len(body)))
        conn.endheaders()
        if body is not None:
            for start in range(0, len(body), step):
                chunk = body[start:start + step]
                if wire == "chunked":
                    conn.send(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                else:
                    conn.send(chunk)
                if args.delay:
                    time.sleep(args.delay)
            if wire == "chunked":
                conn.send(b"0\r\n\r\n")
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


def mismatch(expected, actual):
    before, after = expected.encode("utf-8"), actual.encode("utf-8")
    i = next((i for i, pair in enumerate(zip(before, after)) if pair[0] != pair[1]),
             min(len(before), len(after)))
    return {"sent_bytes": len(before), "stored_bytes": len(after),
            "lost_bytes": len(before) - len(after), "first_mismatch": i,
            "sent_hex": before[max(0, i - 8):i + 16].hex(),
            "stored_hex": after[max(0, i - 8):i + 16].hex(),
            "sent_sha256": hashlib.sha256(before).hexdigest(),
            "stored_sha256": hashlib.sha256(after).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=19817)
    parser.add_argument("--api-key", default="synthetic-tsutf8")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--delay", type=float, default=0.001)
    parser.add_argument("--gzip", action="store_true",
                        help="Use gzip level 0 to exercise decompression across large wire chunks")
    parser.add_argument("--sizes", type=int, nargs="+", default=[127*1024, 192*1024, 256*1024, 508*1024])
    parser.add_argument("--counts", type=int, nargs="+", default=[1, 100])
    parser.add_argument("--alphabets", nargs="+", default=["two", "three", "four"])
    args = parser.parse_args()
    alphabets = {"two": "åäöéü", "three": "€漢字", "four": "🙂🚀"}
    rows = []
    collection = "synthetic_utf8_regression"
    schema = encode({"name": collection, "fields": [
        {"name": "text", "type": "string", "index": False}]})
    created = False
    try:
        for size in args.sizes:
            for count in args.counts:
                for alphabet in args.alphabets:
                    docs, body = fixture(size, count, alphabets[alphabet])
                    wire_body = gzip.compress(body, compresslevel=0, mtime=0) if args.gzip else body
                    for wire, step in [("length", 65536), ("length", 4093),
                                       ("chunked", 16381), ("chunked", 65535)]:
                        row = dict(size=size, docs=count, alphabet=alphabet, wire=wire, step=step,
                                   gzip=args.gzip, wire_bytes=len(wire_body),
                                   body_sha256=hashlib.sha256(body).hexdigest())
                        try:
                            if created:
                                status, _ = rpc(args, "DELETE", f"/collections/{collection}")
                                assert status == 200, f"Collection deletion failed: {status}"
                                created = False
                            status, _ = rpc(args, "POST", "/collections", schema)
                            assert status == 201, f"Collection creation failed: {status}"
                            created = True
                            status, response = rpc(args, "POST", f"/collections/{collection}/documents/import?action=upsert",
                                                   wire_body, wire, step)
                            results = [json.loads(line) for line in response.splitlines() if line]
                            row.update(status=status, import_rows=len(results),
                                       import_errors=[r for r in results if not r.get("success")][:2])
                            status, exported = rpc(args, "GET", f"/collections/{collection}/documents/export")
                            assert status == 200, f"Export failed: {status}"
                            stored = {d["id"]: d for d in map(json.loads, exported.splitlines())}
                            row["stored_docs"] = len(stored)
                            row["mismatches"] = [{"id": d["id"], **mismatch(d["text"], stored[d["id"]]["text"])}
                                                 for d in docs if d["id"] in stored and d != stored[d["id"]]]
                            row["missing_ids"] = [d["id"] for d in docs if d["id"] not in stored]
                            row["pass"] = (row["status"] == 200 and len(results) == len(stored) == count and
                                           not row["import_errors"] and not row["mismatches"] and not row["missing_ids"])
                        except Exception as error:
                            row.update(error=repr(error), **{"pass": False})
                        rows.append(row)
                        args.output.write_text(json.dumps(rows, indent=2) + "\n")
                        print(json.dumps({k: v for k, v in row.items() if k not in ["mismatches", "body_sha256"]}
                                         | {"corrupted_docs": len(row.get("mismatches", []))}), flush=True)
    finally:
        if created:
            rpc(args, "DELETE", f"/collections/{collection}")
    print(f"{sum(row['pass'] for row in rows)}/{len(rows)} byte-exact rows", flush=True)
    return int(any(not row["pass"] for row in rows))


if __name__ == "__main__":
    raise SystemExit(main())
