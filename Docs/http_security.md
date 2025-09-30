# HTTP Security, TLS, Pinning, and Proxies

This guide collects practical notes for configuring HTTPS, certificate pinning, DNS overrides, and proxies for the Pscal VM HTTP API (usable from Pascal and CLike front ends).

## Certificate Pinning (Pinned Public Key)

Pscal exposes libcurl’s `CURLOPT_PINNEDPUBLICKEY` via the session option `pin_sha256`. It expects either:

- A literal hash string of the form `sha256//BASE64==`, or
- A file path to a single key/cert; libcurl extracts the public key and evaluates the pin.

Pinning verifies that the server’s leaf (end-entity) certificate presents the expected public key. This reduces risk from misissued/forged certs but requires careful rotation planning.

Warning: Treat pinning as a sharp tool. Keep a backup pin for key rotation, and be ready to update the app when certificates renew.

### Compute a sha256 pin from a live host

Replace `example.com` with your hostname. You can also use the helper script `tools/pin-from-host.sh`:

```
tools/pin-from-host.sh example.com
# or with explicit port and SNI
tools/pin-from-host.sh example.com:8443 --sni service.example.com
# from a PEM file
tools/pin-from-host.sh --pem cert.pem
```

Manually, using OpenSSL:

```
# 1) Fetch the leaf cert’s public key
openssl s_client -connect example.com:443 -servername example.com </dev/null 2>/dev/null \
  | openssl x509 -pubkey -noout \
  | openssl pkey -pubin -outform der \
  | openssl dgst -sha256 -binary \
  | base64

# Output looks like: 8w1i0... (base64)
# Prefix with sha256// for libcurl format
export PIN_SHA256="sha256//$(openssl s_client -connect example.com:443 -servername example.com </dev/null 2>/dev/null \
  | openssl x509 -pubkey -noout \
  | openssl pkey -pubin -outform der \
  | openssl dgst -sha256 -binary \
  | base64)"
```

Then run a pinned request (Pascal):

```
RUN_NET_TESTS=1 PIN_SHA256="$PIN_SHA256" pascal Examples/Pascal/HttpPinningDemo
```

Successful runs print the HTTP status and any libcurl error context:

```
Status: 200
ErrCode: 0
ErrMsg:
```

`Status: 200` confirms the request completed and the pinned certificate matched. A
non-zero `ErrCode` or a populated `ErrMsg` indicates the TLS handshake or pin
validation failed and you should re-check the hash you exported.

CLike equivalent:

```
RUN_NET_TESTS=1 PIN_SHA256="$PIN_SHA256" build/bin/clike Examples/clike/HttpPinningDemo
```

### Compute from an existing certificate file

If you have a PEM file:

```
openssl x509 -in cert.pem -pubkey -noout \
  | openssl pkey -pubin -outform der \
  | openssl dgst -sha256 -binary \
  | base64
```

Prefix result with `sha256//` as above.

## TLS knobs

Set with `HttpSetOption/httpsetoption` on a session:

- `tls_min` / `tls_max`: integers 10/11/12/13 → TLS v1.0/1.1/1.2/1.3 (min and cap; max applied when supported by libcurl build).
- `alpn`: 0/1 toggle for ALPN negotiation when available.
- `ciphers`: OpenSSL-style cipher list, applied to `CURLOPT_SSL_CIPHER_LIST`.
- `ca_path`: path to a CA bundle to override system defaults.
- `verify_peer` / `verify_host`: 0/1 for TLS verification checks (defaults on).

## Proxies

- `proxy`: proxy URL (e.g., `http://proxy.example:8080`).
- `proxy_userpwd`: `user:pass` credentials for the proxy.
- `proxy_type`: one of `http`, `https` (if supported by libcurl build), `socks5`, `socks4`.

Examples (Pascal):

```
s := HttpSession();
HttpSetOption(s, 'proxy', 'http://proxy.example:8080');
HttpSetOption(s, 'proxy_userpwd', 'alice:secret');
HttpSetOption(s, 'proxy_type', 'http');
```

SOCKS5 example:

```
HttpSetOption(s, 'proxy', 'socks5h://127.0.0.1:1080');
HttpSetOption(s, 'proxy_type', 'socks5');
```

Notes:
- `socks5h` ensures hostname resolution happens via the proxy (Tor-style). You can still set `proxy_type` to `socks5` for clarity; libcurl will parse the scheme.
- HTTPS proxies (`proxy_type=https`) are supported only on libcurl builds with HTTPS-proxy support enabled.

## DNS overrides (static resolve)

- `resolve_add`: `host:port:address` item appended to the per-session resolve list.
- `resolve_clear`: clears the resolve overrides for the session.

This maps a name to a specific address at the libcurl level and is useful for A/B testing, pinning to a specific IP, or testing against staging endpoints.

Example:

```
HttpSetOption(s, 'resolve_add', 'example.com:443:93.184.216.34');
```

## Apply to both sync and async

All options above are honored by both sync and async requests. Async requests snapshot the session options at submission time.

## Troubleshooting

- Pinning failures → SSL error message and `HttpErrorCode/httperrorcode` 4 (SSL). Verify your base64 pin and that no TLS offload is altering the cert.
- Proxy failures → see `HttpLastError/httplasterror`. Some proxies require specific auth methods or block CONNECT.
- Cipher/TLS min/max → ensure your libcurl/OpenSSL build supports requested TLS versions and cipher names.
