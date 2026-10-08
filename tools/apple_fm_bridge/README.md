# Aether FM Bridge

An iPadOS/iOS 27 app that serves Apple's Foundation Models to
`tools/aether_doc_bench.py` as an OpenAI-shaped endpoint, so the benchmark can run
the on-device model like any other destination.

- `GET /health` → `{"ok": true}` (no auth)
- `GET /v1/models` → the served models with availability, context size and, for
  the on-device model, the variant the system chose (`AFM 3 Core` on an M4 iPad
  Pro; the iOS 27 SDK also defines `coreAdvanced3`, which the app cannot select)
- `POST /v1/chat/completions` → one completion. System messages become the
  session instructions; earlier turns are folded into the prompt. `temperature`,
  `max_tokens` and `seed` map to `GenerationOptions` (nucleus 0.95 with the seed;
  greedy at temperature 0).

Every route except `/health` needs `Authorization: Bearer <token>`. The app makes
a random token on first launch, shows it on screen and keeps it in its
preferences.

## Models

| id | model | context |
|---|---|---|
| `apple-on-device` | `SystemLanguageModel.default` | 4,096 tokens, shared by instructions, prompt and answer |
| `apple-pcc`, `-light`, `-moderate`, `-deep` | `PrivateCloudComputeLanguageModel`, optional reasoning level | 32,768 |

Private Cloud Compute needs Apple's managed entitlement
(developer.apple.com/private-cloud-compute). Without it the first PCC
generation trips an assertion inside FoundationModels and kills the app, so this
build refuses PCC requests with `503 pcc_not_entitled` unless it is compiled with
the `AETHER_FM_PCC` Swift flag (and the entitlement). PCC also has a daily
per-person request limit.

## Errors

- context window exceeded → 400 `context_length_exceeded`
- guardrail refusal → 200 with empty content and `finish_reason: content_filter`
  (scored as the model's answer, not an outage)
- PCC quota/network/service, unavailable model → 503 (the harness counts these as
  infrastructure failures and re-runs the case)

Each request's options are appended to `Documents/requests.log` before generation,
so a crash inside the framework leaves the request that caused it.

## Build and run

The project carries no signing team; pass yours at build time:

```bash
xcodebuild -project tools/apple_fm_bridge/AetherFMBridge.xcodeproj -scheme AetherFMBridge -configuration Release -destination 'id=<device udid>' -allowProvisioningUpdates DEVELOPMENT_TEAM=<team id> build
```

Install and launch with `xcrun devicectl device install app` and
`xcrun devicectl device process launch --device <udid> org.aether.fmbridge`.
The device must trust the Mac, have Developer Mode on and Apple Intelligence
enabled. Keep the app in the foreground on power while it serves: iPadOS
suspends a backgrounded app (the app keeps the screen awake itself). Over USB the
Mac reaches it at the device's CoreDevice tunnel address
(`xcrun devicectl device info details`); another host needs the device's LAN or
tailnet address.

To give the harness the token without transcribing it, copy the app's
preferences over the cable and keep the token in a private file (never a tracked
one):

```bash
xcrun devicectl device copy from --device <udid> --domain-type appDataContainer --domain-identifier org.aether.fmbridge --source Library/Preferences/org.aether.fmbridge.plist --destination /tmp/fmbridge.plist
```

Then read `bridgeToken` with `/usr/libexec/PlistBuddy`, and export it as the
destination's `api_key_env` (see
`Tests/aether_doc_bench/results/afm3_20261008/destinations.apple_fm.example.json`).
