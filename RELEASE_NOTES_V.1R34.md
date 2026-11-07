# PSCAL V.1R34 Release Notes

Base: `V.1R33`

## Fixed in the iOS app
- `ping` gets replies. Every request went out with its checksum's two bytes swapped, so every host, a LAN host, the router and the app's own loopback alike, dropped it as corrupt. Each packet then ended in `Request timeout for icmp_seq N (Resource temporarily unavailable)`, with 100% loss.
