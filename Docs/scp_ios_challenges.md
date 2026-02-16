# scp on iOS/iPadOS: Challenges and Mitigations

## Key Challenges
- **SSH stack availability**: `scp` needs libssh/OpenSSH client code; iOS can’t use system SSH.
- **Sandboxed filesystem**: iOS confines access; `/var/mobile/...` outside the app container will fail.
- **Network permissions/backgrounding**: Requires entitlements; sockets may be suspended when app backgrounds.
- **Host key verification**: Need persistent `known_hosts` and strict/ask modes.
- **Auth methods/key formats**: Password and key-based auth (ed25519/rsa); handle passphrases; agent forwarding optional.
- **Path canonicalization**: Prevent escapes; normalize to sandbox-safe roots.
- **Timeouts/partials**: Mobile links drop; must avoid corrupt outputs.
- **Progress/cancel UX**: Long transfers need feedback and abort.
- **Concurrency**: Avoid blocking UI; run transfers on worker thread.
- **Binary size/crypto**: Full OpenSSH inflates size; trim cipher/KEX set.
- **macOS vs iOS builds**: Different OpenSSL/ssh availability; keep build switches consistent.

## Mitigations
- Bundle and statically link existing vendored OpenSSH; ensure symbols are in `pscal_core_static`.
- Use sandbox-safe roots (default `${PSCALI_WORKSPACE_ROOT}`); allow `PSCALI_SCP_ROOT` override; prepend for local/remote paths.
- Require foreground transfers initially; confirm outbound network entitlement.
- Store `known_hosts` at `${PSCALI_WORKSPACE_ROOT}/.ssh/known_hosts`; default ask/accept-once; opt-in strict.
- Support password + ed25519/rsa keys first; passphrase prompt callback; skip agent forwarding v1.
- Normalize paths with `fsNormalizePath`; reject `..` that escapes sandbox.
- Implement temp-file + rename (`.part`) and configurable timeouts.
- Expose progress callbacks and cancel flag; reuse thread pool/heartbeat.
- Run `scp` on a worker thread with completion callbacks.
- Trim crypto to common modern set (chacha20/aes-ctr, curve25519, ed25519) for size.
- Keep CMake host/iOS OpenSSL selection; gate scp behind a feature flag built for both targets.

## Suggested Next Steps
1. Define CLI/ENV knobs: `PSCALI_SCP_ROOT`, `PSCALI_KNOWN_HOSTS`, `PSCALI_SCP_STRICT=0/1`.
2. Wire OpenSSH scp client into the build (static) and expose a VM builtin wrapper.
3. Add sandbox path rebasing + `.part` rename logic in the wrapper.
4. Add progress/cancel hooks to the front end; basic UI log output.
5. Test on-device (foreground) against a LAN SSH server; verify known_hosts storage and path rebasing.
