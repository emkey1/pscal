#!/usr/bin/env bash
# The landscape shader builtins call OpenGL directly. A 3D window drawn with
# the SDL renderer (the default on Apple platforms) has no GL context, and
# LandscapeDrawCloudLayer there used to crash inside glCreateShader. Each must
# draw when LandscapeShadersAvailable() is true and give a runtime error when
# it is false, never a signal. Exits 77 (skipped) where no 3D window opens.
set -u
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REA_BIN="${REA_BIN:-$ROOT_DIR/build/bin/rea}"
if [ ! -x "$REA_BIN" ]; then
    echo "[SKIP] landscape_gl_context: no rea at $REA_BIN"
    exit 77
fi
if [ "${RUN_SDL:-0}" != "1" ]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
    export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
fi
work="$(mktemp -d "${TMPDIR:-/tmp}/landscape_gl.XXXXXX")" || exit 1
trap 'rm -rf "$work"' EXIT
cat > "$work/context.rea" <<'REA'
InitGraph3D(64, 48, "landscape context", 24, 8);
if (LandscapeShadersAvailable()) {
  writeln("context: shaders");
} else {
  writeln("context: renderer");
}
LandscapeDrawCloudLayer(0.0, 0.0, 0.0, 1.0, 0.5, 0.5, 1.0, 0.3, 0.8, 0.3);
LandscapeDrawSkyDome(100.0);
writeln("drawn");
CloseGraph3D();
REA
"$REA_BIN" --no-cache "$work/context.rea" > "$work/out" 2> "$work/err"
rc=$?
fail() {
    echo "[FAIL] landscape_gl_context: $1 (status $rc)"
    sed 's/^/    out: /' "$work/out"
    sed 's/^/    err: /' "$work/err"
    exit 1
}
[ "$rc" -ge 128 ] && fail "rea ended on a signal"
if grep -q '^context: shaders$' "$work/out"; then
    [ "$rc" -eq 0 ] && grep -q '^drawn$' "$work/out" || fail "shaders available but the draw failed"
elif grep -q '^context: renderer$' "$work/out"; then
    [ "$rc" -ne 0 ] || fail "renderer window drew the shader layers"
    grep -q 'LandscapeDrawCloudLayer needs an OpenGL context' "$work/err" || fail "no clear error"
    grep -q '^drawn$' "$work/out" && fail "ran past the error"
else
    echo "[SKIP] landscape_gl_context: no 3D window here"
    sed 's/^/    err: /' "$work/err" | head -3
    exit 77
fi
echo "[PASS] landscape_gl_context"
