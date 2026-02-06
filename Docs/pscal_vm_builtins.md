# Pscal Built-in Functions

This document lists the built-in procedures and functions provided by the Pscal
VM. For instructions on adding your own routines, see
[`extended_builtins.md`](extended_builtins.md).

## General

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| inttostr | (i: Integer) | String | Convert integer to string. |
| formatfloat | (x: Numeric [, precision: Integer]) | String | Convert numeric value to string with fixed decimal precision. |
| realtostr | (r: Real) | String | Convert real to string. |
| real | (x: Ordinal or Real) | Real | Convert value to real. |
| length | (s: String or Array) | Integer | Length of string or array. |
| setlength | (var s: String, len: Integer) | void | Resize a string. |
| val | (s: String, var dest: Integer/Real, var code: Integer) | void | Parse string to numeric. |
| halt | ([code: Integer]) | void | Stop program execution. |
| delay | (ms: Integer) | void | Pause for specified milliseconds. |
| new | (var p: Pointer) | void | Allocate memory for a pointer. |
| dispose | (var p: Pointer) | void | Free memory. |
| exit | () | void | Exit current routine. |
| ord | (x: Ordinal) | Integer | Ordinal value. |
| chr | (code: Integer) | Char | Convert code to character. |
| inc | (var x: Ordinal [, delta: Ordinal]) | void | Increment variable. |
| dec | (var x: Ordinal [, delta: Ordinal]) | void | Decrement variable. |
| low | (a: Array or String) | Integer | Lowest index. |
| high | (a: Array or String) | Integer | Highest index. |
| succ | (x: Ordinal) | Ordinal | Successor of value. |
| upcase | (ch: Char or String) | Char | Convert character or first character of string to uppercase. Alias: `toupper`. |
| pos | (sub: String or Char, s: String) | Integer | Position of substring. |
| copy | (s: String or Char, index: Integer, count: Integer) | String | Copy substring. |
| paramcount | () | Integer | Number of command line parameters. |
| paramstr | (index: Integer) | String | Command line parameter by index. |
| quitrequested | () | Boolean | True if window close requested. |

## Console and Text

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| screencols | () | Integer | Number of columns in console. |
| screenrows | () | Integer | Number of rows in console. |
| wherex | () | Integer | Current cursor X position. |
| wherey | () | Integer | Current cursor Y position. |
| gotoxy | (x: Integer, y: Integer) | void | Move cursor. |
| clrscr | () | void | Clear screen. |
| clreol | () | void | Clear to end of line. |
| insline | () | void | Insert line. |
| deline | () | void | Delete line. |
| cursoron / showcursor | () | void | Show cursor. |
| cursoroff / hidecursor | () | void | Hide cursor. |
| savecursor | () | void | Save cursor position. |
| restorecursor | () | void | Restore cursor position. |
| window | (left: Integer, top: Integer, right: Integer, bottom: Integer) | void | Set output window. |
| keypressed | () | Boolean | True if key waiting. |
| readkey | ([var c: Char]) | Char | Read key. Optionally stores into VAR char. |
| write | ([file: File,] ...) | void | Write values to file or console (all integer sizes, boolean, and float types incl. 80-bit). |
| writeln | ([file: File,] ...) | void | Write values and newline (all integer sizes, boolean, and float types incl. 80-bit). |
| read | ([file: File,] var ...) | void | Read values from file or console. |
| readln | ([file: File,] var ...) | void | Read line and parse into vars (all integer sizes, boolean, and float types incl. 80-bit). |
| textcolor | (color: Integer) | void | Set text color. |
| textbackground | (color: Integer) | void | Set background color. |
| textcolore | (color: Integer) | void | Set text color using 256-color palette. |
| textbackgrounde | (color: Integer) | void | Set background color using 256-color palette. |
| boldtext / highvideo | () | void | Enable bold text. |
| lowvideo | () | void | Enable dim text. |
| normalcolors / normvideo | () | void | Reset text attributes. |
| blinktext | () | void | Enable blinking text. |
| underlinetext | () | void | Enable underlined text. |
| invertcolors | () | void | Swap foreground and background colors. |
| beep | () | void | Emit a bell. |
| popscreen | () | void | Leave alternate screen. |
| pushscreen | () | void | Enter alternate screen. |

## File I/O

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| assign | (var f: File, name: String) | void | Bind a file variable to a name. |
| reset | (var f: File) | void | Open file for reading. |
| rewrite | (var f: File) | void | Open file for writing. |
| append | (var f: File) | void | Open file for appending. |
| close | (var f: File) | void | Close file. |
| rename | (var f: File, newName: String) | void | Rename file. |
| erase | (var f: File) | void | Delete file. (CLike front end calls this `remove`.) |
| eof | ([f: File]) | Boolean | Test end of file. |
| read | ([f: File,] var ...) | void | Read from file or console. |
| readln | ([f: File,] var ...) | void | Read line and parse into vars (all integer sizes, boolean, and float types incl. 80-bit). |
| ioresult | () | Integer | Return last I/O error code. |

## Memory Streams

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| MStreamCreate | () | MStream | Create a memory stream. |
| MStreamFromString | (text: String) | MStream | Create and populate a memory stream from the given string. |
| MStreamLoadFromFile | (ms: MStream, file: String) | Boolean | Load file contents into a memory stream. |
| MStreamSaveToFile | (ms: MStream, file: String) | Boolean | Persist a memory stream to disk. |
| MStreamFree | (ms: MStream) | void | Release a memory stream instance. |
| MStreamBuffer | (ms: MStream) | String | Return stream contents as a string. |
| MStreamAppendByte | (ms: MStream, byte: Integer) | void | Append a single byte to the stream (capacity grows as needed). Available in Pascal, CLike, and Rea frontends. |

## Threading and Synchronization

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| spawn | (address: Integer) | Integer | Start a new thread at the given bytecode address and return its id. |
| join | (tid: Integer) | void | Wait for the specified thread to finish. |
| CreateThread | (procAddr: Pointer, arg: Pointer = nil) | Thread | Start a new thread invoking the given routine with `arg`. Backward-compatible with 1-arg form. |
| WaitForThread | (t: Thread) | Integer | Wait for the given thread handle to complete, returning `0` on success and `1` when the worker reported a failure. Consumes the stored status flag so idle workers without cached results immediately return to the pool. |
| ThreadSpawnBuiltin | (target: String/Integer, args: Value...) | Thread | Spawn an allow-listed VM builtin on a worker thread and return its handle. |
| ThreadPoolSubmit | (target: String/Integer, args: Value...) | Thread | Queue an allow-listed builtin on the worker pool without blocking the caller; the thread handle can be joined later. |
| ThreadGetResult | (t: Thread, consumeStatus: Boolean = false) | Any | Retrieve the stored result for a builtin worker. When `consumeStatus` is true the cached status flag is also cleared. |
| ThreadGetStatus | (t: Thread, dropResult: Boolean = false) | Boolean | Read the stored success flag for a worker thread. Passing `dropResult = true` clears any cached return value. |
| ThreadSetName | (t: Thread, name: String) | Boolean | Assign a human-readable label to a worker slot; returns `true` when the rename succeeded. |
| ThreadLookup | (nameOrId: String/Integer) | Thread | Resolve a worker by name or id; returns `-1` if no matching slot is active. |
| ThreadPause | (t: Thread) | Boolean | Request that a worker pause cooperatively. |
| ThreadResume | (t: Thread) | Boolean | Clear a pending pause request so the worker can continue. |
| ThreadCancel | (t: Thread) | Boolean | Request that a worker cancel itself at the next safe poll point. |
| ThreadStats | () | Array<Record> | Produce an array of per-worker records describing pool utilisation, lifecycle flags, timing, and metrics. |
| mutex | () | Integer | Create a standard mutex and return its identifier. |
| rcmutex | () | Integer | Create a recursive mutex and return its identifier. |
| lock | (mid: Integer) | void | Acquire the mutex with the given identifier. |
| unlock | (mid: Integer) | void | Release the specified mutex. |
| destroy | (mid: Integer) | void | Destroy the specified mutex. |

> **Worker reuse tip:** `WaitForThread` clears the stored status flag as part of the join so threads without cached results re-enter the pool immediately. Builtins that publish result values still reserve the worker until you consume them—call `ThreadGetResult(handle, true)` to clear both the value and status in one go, or pair `ThreadGetResult` with `ThreadGetStatus(handle, true)` when you prefer to fetch the value before discarding it.

For pool sizing guidance, naming rules, and the schema returned by
`ThreadStats`, see the dedicated [threading guide](threading.md).

Allow-listed targets are capped at re-entrant helpers that avoid shared global
state: `delay`, `httprequest`, `httprequesttofile`, `httprequestasync`,
`httprequestasynctofile`, `httptryawait`, `httpawait`, `httpisdone`,
`httpcancel`, `httpgetasyncprogress`, `httpgetasynctotal`, `httpgetlastheaders`,
`httpgetheader`, `httpclearheaders`, `httpsetheader`, `httpsetoption`,
`httperrorcode`, `httplasterror`, `apireceive`, `apisend`, and `dnslookup`.

Sample exsh transcript demonstrating argument prefixes and result collection:

```sh
$ build/bin/exsh -c 'tid=$(builtin ThreadSpawnBuiltin str:dnslookup str:localhost); \
    WaitForThread "$tid"; \
    printf "status:%s\n" "$EXSH_LAST_STATUS"; \
    printf "result:%s\n" "$(builtin ThreadGetResult "$tid")"'
status:0
result:127.0.0.1
```

The resolved address will reflect the local resolver configuration (`127.0.0.1`,
`::1`, etc.), but the exit status tracks whether the worker reported success.

## exsh orchestration

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| __shell_exec | (meta: String, argv: String...) | void | Launch a process described by the metadata/argument vector. Supports `bg=1` metadata for background execution and simple `<`, `>`, `>>` redirections encoded as argument tokens. |
| __shell_pipeline | (meta: String) | void | Initialise pipeline state before emitting a sequence of `__shell_exec` calls. The metadata string carries the stage count and negation flag. |
| __shell_and | (meta: String) | void | Update the shell status for `&&` lists. Primarily used by the exsh compiler. |
| __shell_or | (meta: String) | void | Update the shell status for `||` lists. Primarily used by the exsh compiler. |
| __shell_subshell | (meta: String) | void | Reset pipeline/bookkeeping before running a subshell block. |
| __shell_loop | (meta: String) | void | Reset pipeline/bookkeeping before running a loop body. |
| __shell_loop_end | () | void | Signal the end of the current loop body, allowing loop-control requests to unwind. |
| __shell_if | (meta: String) | void | Reset pipeline/bookkeeping before running a conditional branch. |
| cd | (path: String) | void | Change the current working directory and update the `PWD` environment variable. |
| pwd | () | void | Print the current working directory. |
| exit | ([code: Integer]) | void | Exit the current shell script with the provided status code. |
| eval | (chunks: String...) | void | Concatenate arguments with spaces and execute the resulting source in the current shell context. |
| export | (assignments: String...) | void | Set one or more environment variables using `NAME=value` pairs. |
| unset | (names: String...) | void | Remove environment variables from the current process. |
| alias | ([assignments: String...]) | void | Define shell aliases or list existing ones when called without arguments. |

## HTTP (Synchronous)

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| HttpSession | () | Integer (session) | Create a reusable HTTP session (libcurl easy) with keep‑alive. |
| HttpClose | (session: Integer) | void | Destroy a session and free resources. |
| HttpSetHeader | (session: Integer, name: String, value: String) | void | Add a request header to the session. |
| HttpClearHeaders | (session: Integer) | void | Clear all accumulated headers. |
| HttpSetOption | (session: Integer, key: String, value: Int or String) | void | Set options such as `timeout_ms` (Int), `follow_redirects` (Int 0/1), `user_agent` (String), `accept_encoding` (String), cookie persistence via `cookie_file`/`cookie_jar` (String), retry/backoff via `retry_max`/`retry_delay_ms`, rate limiting with `max_recv_speed`/`max_send_speed`, and streaming uploads via `upload_file` (String). |
| HttpRequest | (session: Integer, method: String, url: String, body: String|MStream|nil, out: MStream) | Integer (status) | Perform a request; writes response body into `out`. Returns HTTP status or -1 on transport error. |

Notes
- `body` may be nil for GET or other methods without payload; strings and mstreams are supported.
- `out` must be an initialized `MStream`; it is cleared before writing.
- Errors and transport failures return -1 and report details on stderr.

## Math

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| abs | (x: Integer or Real) | same as x | Absolute value. |
| arccos | (x: Real) | Real | Arc cosine. |
| arcsin | (x: Real) | Real | Arc sine. |
| arctan | (x: Real) | Real | Arc tangent. |
| cos | (x: Real) | Real | Cosine. |
| cosh | (x: Real) | Real | Hyperbolic cosine. |
| cotan | (x: Real) | Real | Cotangent. |
| exp | (x: Real) | Real | Exponential. |
| ln | (x: Real) | Real | Natural logarithm. |
| log10 | (x: Real) | Real | Base-10 logarithm. |
| power | (base: Numeric, exponent: Numeric) | Integer/Real | Raise to power. |
| max | (a: Numeric, b: Numeric) | Integer/Real | Maximum of two values. |
| min | (a: Numeric, b: Numeric) | Integer/Real | Minimum of two values. |
| round | (x: Real) | Integer | Round to nearest integer. |
| floor | (x: Real) | Integer | Floor. |
| ceil | (x: Real) | Integer | Ceiling. |
| sin | (x: Real) | Real | Sine. |
| sinh | (x: Real) | Real | Hyperbolic sine. |
| sqr | (x: Integer or Real) | same as x | Square of number. |
| sqrt | (x: Real) | Real | Square root. |
| tan | (x: Real) | Real | Tangent. |
| tanh | (x: Real) | Real | Hyperbolic tangent. |
| trunc | (x: Real) | Integer | Truncate real to integer. |

Numeric builtins preserve integer types when all inputs are integral. In particular, `power`, `max`, and `min` return integers when given only integer arguments and fall back to real otherwise.

## Random

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| randomize | () | void | Seed random generator. |
| random | ([limit: Integer]) | Real/Integer | Random number. |

## DOS/OS

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| dosGetenv / getenv | (name: String) | String | Get environment variable. |
| getenvint | (name: String) | Integer | Get environment variable as int. |
| dosExec / exec | (command: String) | Integer | Execute shell command. |
| dosMkdir / mkdir | (path: String) | Integer | Create directory. |
| dosRmdir / rmdir | (path: String) | Integer | Remove directory. |
| dosFindfirst / findfirst | (pattern: String, attr: Integer) | Integer | Begin directory search. |
| dosFindnext / findnext | () | Integer | Continue directory search. |
| dosGetdate / getdate | (var Year, Month, Day, Dow: Word) | void | Retrieve system date components. |
| dosGettime / gettime | (var Hour, Minute, Second, Sec100: Word) | void | Retrieve system time components. |
| dosGetfattr / getfattr | (path: String) | Integer | Get file attributes. |

## Networking

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| apiSend | (data: String) | Integer | Send network packet. |
| apiReceive | () | String | Receive network packet. |
| DnsLookup | (host: String) | String | Resolve hostname to IPv4 address or empty string on error. |
| SocketCreate | (kind: Integer) | Integer | Create TCP (0) or UDP (1) socket. Returns handle or -1. |
| SocketConnect | (s: Integer, host: String, port: Integer) | Integer | Connect socket to remote host/port. Returns 0 or -1. |
| SocketBind | (s: Integer, port: Integer) | Integer | Bind socket to local port. Returns 0 or -1. |
| SocketListen | (s: Integer, backlog: Integer) | Integer | Begin listening for connections. Returns 0 or -1. |
| SocketAccept | (s: Integer) | Integer | Accept connection; returns new socket or -1. |
| SocketSend | (s: Integer, data: String\|MStream) | Integer | Send data; returns bytes sent or -1. |
| SocketReceive | (s: Integer, maxLen: Integer) | MStream | Receive up to maxLen bytes. Returns memory stream or nil. |
| SocketClose | (s: Integer) | Integer | Close socket. Returns 0 or -1. |
| SocketSetBlocking | (s: Integer, blocking: Boolean) | Integer | Toggle blocking mode (0 on success). |
| SocketPoll | (s: Integer, timeoutMs: Integer, flags: Integer) | Integer | Poll for read (1) or write (2); returns bitmask or -1. |
| SocketLastError | () | Integer | Last socket/DNS error code. |

## SDL graphics and audio

These built-ins are available when Pscal is built with SDL support and can be
imported from each front end (Pascal, CLike, and Rea).

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| initgraph | (width: Integer, height: Integer) | void | Initialize graphics. |
| initgraph3d | (width: Integer, height: Integer, title: String, depthBits: Integer, stencilBits: Integer) | void | Initialize an OpenGL-backed SDL window with the requested depth and stencil buffer sizes. |
| closegraph | () | void | Close graphics. |
| closegraph3d | () | void | Close the OpenGL window and delete its context. |
| graphloop | () | void | Poll events and delay. |
| glbegin | (mode: String\|Integer) | void | Begin an immediate-mode primitive using the named GLenum (for example `"triangles"` or `"quads"`). |
| glclear | (mask: Integer = GL_COLOR_BUFFER_BIT \| GL_DEPTH_BUFFER_BIT) | void | Clear buffers with `glClear`; defaults to color and depth. |
| glclearcolor | (r: Real, g: Real, b: Real, a: Real) | void | Set the RGBA clear color. |
| glcleardepth | (depth: Real) | void | Set the depth buffer clear value (clamped to 0..1). |
| glcolor3f | (r: Real, g: Real, b: Real) | void | Set the current vertex color (components are clamped to 0..1). |
| gldepthtest | (enable: Boolean) | void | Enable or disable depth testing. |
| gldepthmask | (enable: Boolean) | void | Toggle depth buffer writes using `glDepthMask`. |
| gldepthfunc | (func: String\|Integer) | void | Set the depth comparison (e.g. `"less"`, `"lequal"`, `"greater"`, `"always"`). |
| gllinewidth | (width: Real) | void | Set the current OpenGL line width (must be positive). |
| glend | () | void | End the current immediate-mode primitive. |
| glloadidentity | () | void | Replace the current matrix with the identity matrix. |
| glmatrixmode | (mode: String\|Integer) | void | Select the active matrix stack (`"projection"`, `"modelview"`, `"texture"`, or a raw GLenum). |
| glpopmatrix | () | void | Pop the top matrix from the current stack. |
| glpushmatrix | () | void | Push a copy of the current matrix onto the stack. |
| glrotatef | (angle: Real, x: Real, y: Real, z: Real) | void | Apply a rotation (degrees) about the supplied axis. |
| glscalef | (x: Real, y: Real, z: Real) | void | Apply a non-uniform scale to the current matrix. |
| glfrustum | (left: Real, right: Real, bottom: Real, top: Real, near: Real, far: Real) | void | Configure a perspective frustum using `glFrustum`. |
| glperspective | (fovY: Real, aspect: Real, near: Real, far: Real) | void | Convenience helper that computes a symmetric frustum from a field of view and aspect ratio. |
| glsetswapinterval | (interval: Integer) | void | Set the OpenGL swap interval (0 disables vsync, 1 enables it). |
| glishardwareaccelerated | () | Boolean | Returns `true` when the current OpenGL context reports hardware acceleration via `SDL_GL_ACCELERATED_VISUAL`. |
| glswapwindow | () | void | Swap the OpenGL window buffers to present the rendered frame. |
| gltranslatef | (x: Real, y: Real, z: Real) | void | Apply a translation to the current matrix. |
| glvertex3f | (x: Real, y: Real, z: Real) | void | Emit a vertex for the active primitive. |
| glviewport | (x: Integer, y: Integer, width: Integer, height: Integer) | void | Configure the OpenGL viewport rectangle. |
| updatescreen | () | void | Present renderer. |
| cleardevice | () | void | Clear renderer. |
| setcolor | (color: Integer) | void | Set drawing color. |
| setrgbcolor | (r: Integer, g: Integer, b: Integer) | void | Set drawing color by RGB. |
| setalphablend | (enable: Boolean) | void | Configure alpha blending. |
| putpixel | (x: Integer, y: Integer) | void | Draw pixel. |
| drawline | (x1: Integer, y1: Integer, x2: Integer, y2: Integer) | void | Draw line. |
| drawrect | (x1: Integer, y1: Integer, x2: Integer, y2: Integer) | void | Draw rectangle. |
| fillrect | (x1: Integer, y1: Integer, x2: Integer, y2: Integer) | void | Filled rectangle. |
| drawcircle | (x: Integer, y: Integer, r: Integer) | void | Draw circle. |
| fillcircle | (x: Integer, y: Integer, r: Integer) | void | Filled circle. |
| drawpolygon | (points: Array) | void | Draw polygon. |
| getpixelcolor | (x: Integer, y: Integer) | Integer | Read pixel color. |
| getmaxx | () | Integer | Width of window. |
| getmaxy | () | Integer | Height of window. |
| getscreensize | (var width: Integer, var height: Integer) | void | Query the current SDL window dimensions. |
| gettextsize | (text: String) | (w: Integer, h: Integer) | Measure text. |
| outtextxy | (x: Integer, y: Integer, text: String) | void | Draw text at position. |
| waitkeyevent | () | Integer | Wait for key event. |
| setrendertarget | (texture: Texture) | void | Select render target. |
| createtexture | (w: Integer, h: Integer) | Texture | Create texture. |
| createtargettexture | (w: Integer, h: Integer) | Texture | Create target texture. |
| destroytexture | (texture: Texture) | void | Free texture. |
| loadimagetotexture | (file: String, texture: Texture) | Boolean | Load image into texture. |
| rendercopy | (texture: Texture, x: Integer, y: Integer) | void | Copy texture to renderer. |
| rendercopyrect | (texture: Texture, sx: Integer, sy: Integer, sw: Integer, sh: Integer, dx: Integer, dy: Integer) | void | Copy part of texture. |
| rendercopyex | (texture: Texture, sx: Integer, sy: Integer, sw: Integer, sh: Integer, dx: Integer, dy: Integer, angle: Real, flip: Integer) | void | Render with rotation or flip. |
| updatetexture | (texture: Texture, data: String) | void | Update texture contents. |
| rendertexttotexture | (text: String, texture: Texture) | void | Render text into texture. |
| initsoundsystem | () | void | Initialize audio. |
| quitsoundsystem | () | void | Shut down audio. |
| loadsound | (file: String) | Sound | Load sound file. |
| freesound | (sound: Sound) | void | Free a loaded sound. |
| playsound | (sound: Sound) | void | Play sound. |
| stopallsounds | () | void | Halt all playing sounds immediately. |
| issoundplaying | (sound: Sound) | Boolean | Query if sound playing. |
| inittextsystem | (fontPath: String, fontSize: Integer) | void | Initialize text subsystem with a TTF font. |
| quittextsystem | () | void | Shut down text subsystem. |
| getmousestate | (var x: Integer, var y: Integer, var buttons: Integer [, var insideWindow: Integer]) | void | Query mouse position and button state. When the optional `insideWindow` VAR parameter is supplied it receives `1` if the cursor is inside the focused window, otherwise `0`. |
| getticks | () | Integer | Milliseconds since start. |
| pollkey | () | Integer | Poll for key press. |
| pollkeyany | () | Integer | Returns the next pending key from either the SDL window queue (when graphics are active) or the console input buffer, falling back to 0 when no input is available. |
| iskeydown | (key: String\|Integer) | Boolean | Return `true` while the requested key is held down (uses SDL scancodes/key names). |

For OpenGL programs, PSCAL now exposes additional fixed-function helpers alongside the existing pipeline calls: `GLColor4f`, `GLNormal3f`, `GLLineWidth`, `GLDepthMask`, `GLDepthFunc`, `GLEnable`, `GLDisable`, `GLCullFace`, `GLShadeModel`, `GLLightfv`, `GLMaterialfv`, `GLMaterialf`, `GLColorMaterial`, and `GLBlendFunc` simplify configuring lighting, materials, blending, and depth state directly from Pascal, Rea, and the other front ends. Use `GLCullFace('back')`, `GLCullFace('front')`, or `GLCullFace('front_and_back')` (or pass the corresponding numeric `GLenum`) to toggle which primitives are rejected during rasterization.

### Basic OpenGL render loop

```pascal
program SwapDemo;
var
  frame: Integer;
begin
  InitGraph3D(640, 480, 'Swap Demo', 24, 8);
  GLViewport(0, 0, 640, 480);
  GLClearDepth(1.0);
  GLDepthTest(true);
  GLDepthMask(true);
  GLSetSwapInterval(1); { enable vsync }

  for frame := 0 to 599 do
  begin
    GLClearColor(0.1, 0.1, 0.15, 1.0);
    GLClear();

    GLMatrixMode('modelview');
    GLLoadIdentity();
    GLRotatef(frame * 0.5, 0.0, 1.0, 0.0);

    GLBegin('triangles');
      GLColor3f(1.0, 0.0, 0.0);
      GLVertex3f(0.0, 0.5, 0.0);
      GLColor3f(0.0, 1.0, 0.0);
      GLVertex3f(-0.5, -0.5, 0.0);
      GLColor3f(0.0, 0.0, 1.0);
      GLVertex3f(0.5, -0.5, 0.0);
    GLEnd();

    if frame = 300 then
      GLSetSwapInterval(0); { drop vsync after five seconds }

    GLSwapWindow();
    GraphLoop(1);          { keep SDL responsive without busy waiting }
  end;

  CloseGraph3D;
end.
```

## Examples

### Pascal

```pascal
program DemoBuiltins;
begin
  writeln('Random: ', Random(100));
  writeln('Uppercase: ', UpCase('a'));
end.
```

### CLike

```c
int main() {
  printf("Random: %d\n", random(100));
  printf("Uppercase: %c %c\n", upcase('a'), toupper('b'));
  return 0;
}
```
