# Network Troubleshooting

This guide collects common issues when working with the network APIs.

## TLS and Certificate Problems

* **Unknown certificate authority** – The server's certificate may not be signed by a trusted CA.  Set a path to a bundle using `httpsetoption(s, "ca_path", "/path/to/ca-bundle.crt")` or disable verification with `httpsetoption(s, "verify_peer", 0)` for testing.
* **Hostname mismatch** – When the certificate's CN does not match the host, the request fails.  Verify the URL or disable host checks with `httpsetoption(s, "verify_host", 0)`.
* **Expired or self-signed certificate** – Refresh the certificate or add the issuing CA to the bundle.

## Rate Limiting

Servers may throttle clients that download too quickly.  Limit transfer speed with
`httpsetoption(s, "max_recv_speed", 10240)` to cap reads to ~10 KB/s, or insert
pauses between requests.  On receiving HTTP status 429, respect the `Retry-After`
header before issuing further requests.
