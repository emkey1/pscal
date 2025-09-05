# CLike Simple Web Server

## Overview
- Purpose: Minimal HTTP 1.1 server implemented in CLike for demos and local testing.
- Default root: Creates `/tmp/clike_htdocs.<PID>` with a default `index.html` if missing.
- Design: Single listener socket, multi-threaded worker pool, bounded in-memory queue, simple static file serving.

## Features
- Index handling: `GET /` serves on-disk `index.html` if present; otherwise returns a directory listing.
- Static files: Serves files under the configured root; MIME type chosen by file extension.
- Path safety: Percent-decodes, strips query string, canonicalizes `.`/`..`, collapses duplicate `/`.
- Worker pool: Fixed-size pool consumes from a bounded queue; prevents unbounded thread growth.
- Metrics: Counters for accepted, enqueued, served, dropped; queue depth. Periodic heartbeat logs once/minute.
- Logging: Per-request line with method, path, status, and detail; periodic heartbeat with throughput.

## Usage
Binary: `build/bin/clike`

Source: `Examples/clike/simple_web_server`

Examples:
- Default temp root (port 5555):
  - `build/bin/clike Examples/clike/simple_web_server`
- Serve existing directory on default port:
  - `build/bin/clike Examples/clike/simple_web_server /path/to/htdocs`
- Custom port and root:
  - `build/bin/clike Examples/clike/simple_web_server 8080 /path/to/htdocs`
  - or `build/bin/clike Examples/clike/simple_web_server /path/to/htdocs 8080`
- Tuning threads and queue capacity (3rd/4th args):
  - `build/bin/clike Examples/clike/simple_web_server /path 8080 16 128`
  - Threads clamped to 1..64; queue capacity to 8..256.

## HTTP Behavior
- Request line: Parses the first line only; headers are ignored.
- Method: Logs the method; only GET is intended.
- Path parsing:
  - Strips query string (`?` and after).
  - Percent-decodes `%XX` hex sequences with validation.
  - Normalizes path: removes `.` segments, resolves `..`, collapses `//`.
- Routing:
  - `/`: If `index.html` exists, serves it; else lists the directory.
  - `/index.html`: If file exists, serves from disk; otherwise falls back to a default in-memory page.
  - Other paths: Serves file if present; 404 if missing.
- Headers: Sends `Content-Type` and `Content-Length`.

## Error Handling
- 404 Not Found: Returned when the requested file does not exist under the root.
- 500 Internal Server Error: Returned when a readable file unexpectedly fails to load.
- VM I/O safety: Uses `fileexists(path)` before attempting `mstreamloadfromfile` to avoid VM runtime errors.

## Security
- Traversal protection: After decoding, canonicalization prevents `..` from escaping the root.
- Static-only: No script execution, no directory write operations, no network exposure recommended.

## MIME Types
- Mapped: `.html|.htm`, `.css`, `.js`, `.json`, `.txt`, `.png`, `.jpg|.jpeg`.
- Default: `application/octet-stream` for unknown extensions.

## Concurrency Model
- Accept loop: Accepts sockets; enqueues with brief retries; drops (closes) if queue is full.
- Workers: `MaxThreads` workers dequeue sockets, call `serveConn`, and update metrics.
- Queue: Bounded ring buffer (`QCap`) guarded by a mutex.

## Metrics & Heartbeat
- Counters: `accepted`, `enqueued`, `served`, `dropped`; live queue depth.
- Per‑request log: `[GET] /path -> 200 detail | q=depth served=S enq=E drop=D`.
- Heartbeat (every 60s): `[HB] q=depth/cap thr=T accepted=A enq=E served=S drop=D dps=ΔS`.

## Tests
- Network test: `Tests/clike/SimpleWebServerClient.cl` (marked `.net`).
  - Verifies index handling, percent-decoding (spaces), and traversal blocking.
- Run with: `RUN_NET_TESTS=1 ctest --test-dir build --output-on-failure -R clike_tests`.

## Implementation Notes
- File: `Examples/clike/simple_web_server`.
- Important functions:
  - `serveConn(int)`: Request parsing, routing, response assembly.
  - `decodePercent(str)`: Percent-decoding with hex validation.
  - `normalizePath(str)`: Canonical path processing (`.`/`..`, slashes).
  - `worker()`, `enqueue/dequeue`: Concurrency and queue control.
  - `heartbeat()`: Periodic metrics logging.
- Builtins used: sockets (`socket*`), memory streams (`mstream*`), filesystem helpers (`fileexists`, `findfirst/findnext`).

## Limitations
- Minimal HTTP (no keep-alive, no header processing beyond basics).
- No caching, compression, TLS, or non-GET methods.
- For demo and local testing only; do not expose publicly.

