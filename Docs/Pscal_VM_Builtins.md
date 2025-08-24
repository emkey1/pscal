# Pscal Built-in Functions

This document lists the built-in procedures and functions provided by the Pscal
VM.

## General

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| inttostr | (i: Integer) | String | Convert integer to string. |
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
| upcase | (ch: Char) | Char | Convert character to uppercase. |
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
| read | ([file: File,] var ...) | void | Read values from file or console. |
| readln | ([file: File,] var ...) | void | Read line from file or console. |
| textcolor | (color: Integer) | void | Set text color. |
| textbackground | (color: Integer) | void | Set background color. |
| textcolore | (color: Integer) | void | Extended text color. |
| textbackgrounde | (color: Integer) | void | Extended background color. |
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
| close | (var f: File) | void | Close file. |
| eof | ([f: File]) | Boolean | Test end of file. |
| read | ([f: File,] var ...) | void | Read from file or console. |
| readln | ([f: File,] var ...) | void | Read line from file or console. |
| ioresult | () | Integer | Return last I/O error code. |

## Memory Streams

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| mstreamcreate | () | MStream | Create memory stream. |
| mstreamloadfromfile | (ms: MStream, file: String) | Boolean | Load file into memory stream. |
| mstreamsavetofile | (ms: MStream, file: String) | Boolean | Save memory stream to file. |
| mstreamfree | (ms: MStream) | void | Release memory stream. |
| mstreambuffer | (ms: MStream) | String | Return contents as string. |

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
| power | (base: Real, exponent: Real) | Real | Raise to power. |
| max | (a: Real, b: Real) | Real | Maximum of two values. |
| min | (a: Real, b: Real) | Real | Minimum of two values. |
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

## Random

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| randomize | () | void | Seed random generator. |
| random | ([limit: Integer]) | Real/Integer | Random number. |

## DOS/OS

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| dos_getenv / getenv | (name: String) | String | Get environment variable. |
| getenvint | (name: String) | Integer | Get environment variable as int. |
| dos_exec / exec | (command: String) | Integer | Execute shell command. |
| dos_mkdir / mkdir | (path: String) | Integer | Create directory. |
| dos_rmdir / rmdir | (path: String) | Integer | Remove directory. |
| dos_findfirst / findfirst | (pattern: String, attr: Integer) | Integer | Begin directory search. |
| dos_findnext / findnext | () | Integer | Continue directory search. |
| dos_getdate / getdate | () | Integer | Get system date packed as long. |
| dos_gettime / gettime | () | Integer | Get system time packed as long. |
| dos_getfattr / getfattr | (path: String) | Integer | Get file attributes. |

## Networking

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| api_send | (data: String) | Integer | Send network packet. |
| api_receive | () | String | Receive network packet. |

## SDL graphics and audio

These built-ins are available when Pscal is built with SDL support.

| Name | Parameters | Returns | Description |
| ---- | ---------- | ------- | ----------- |
| initgraph | (width: Integer, height: Integer) | void | Initialize graphics. |
| closegraph | () | void | Close graphics. |
| graphloop | () | void | Poll events and delay. |
| updatescreen | () | void | Present renderer. |
| cleardevice | () | void | Clear renderer. |
| setcolor | (color: Integer) | void | Set drawing color. |
| setrgbcolor | (r: Integer, g: Integer, b: Integer) | void | Set drawing color by RGB. |
| setalphablend | (enable: Boolean) | void | Configure alpha blending. |
| putpixel | (x: Integer, y: Integer) | void | Draw pixel. |
| drawline | (x1: Integer, y1: Integer, x2: Integer, y2: Integer) | void | Draw line. |
| drawrect | (x: Integer, y: Integer, w: Integer, h: Integer) | void | Draw rectangle. |
| fillrect | (x: Integer, y: Integer, w: Integer, h: Integer) | void | Filled rectangle. |
| drawcircle | (x: Integer, y: Integer, r: Integer) | void | Draw circle. |
| fillcircle | (x: Integer, y: Integer, r: Integer) | void | Filled circle. |
| drawpolygon | (points: Array) | void | Draw polygon. |
| getpixelcolor | (x: Integer, y: Integer) | Integer | Read pixel color. |
| getmaxx | () | Integer | Width of window. |
| getmaxy | () | Integer | Height of window. |
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
| issoundplaying | (sound: Sound) | Boolean | Query if sound playing. |
| inittextsystem | () | void | Initialize text subsystem. |
| quittextsystem | () | void | Shut down text subsystem. |
| getmousestate | () | (x: Integer, y: Integer, buttons: Integer) | Query mouse position and buttons. |
| getticks | () | Integer | Milliseconds since start. |
| pollkey | () | Integer | Poll for key press. |

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
  printf("Uppercase: %c\n", upcase('a'));
  return 0;
}
```

