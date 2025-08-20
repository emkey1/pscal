# Pscal Built-in Functions

This document lists the built-in procedures and functions provided by Pscal.

## General
- `inttostr` – Convert integer to string.
- `length` – Length of string or array.
- `halt` – Stop program execution.
- `delay` – Pause for specified milliseconds.
- `new` – Allocate memory for a pointer.
- `dispose` – Free memory.
- `exit` – Exit program with code.
- `ord` – Character code.
- `inc` – Increment variable.
- `dec` – Decrement variable.
- `low` – Lowest index of array/string.
- `high` – Highest index of array/string.
- `screencols` – Number of columns in console.
- `screenrows` – Number of rows in console.
- `chr` – Convert code to character.
- `succ` – Successor of ordinal value.
- `upcase` – Convert character to uppercase.
- `pos` – Position of substring.
- `copy` – Copy substring.
- `realtostr` – Convert real to string.
- `paramcount` – Number of command line parameters.
- `paramstr` – Command line parameter by index.
- `wherex` – Current cursor X position.
- `wherey` – Current cursor Y position.
- `gotoxy` – Move cursor.
- `keypressed` – True if key waiting.
- `readkey` – Read key. Optionally accepts a `VAR` char to store the key pressed.
- `textcolor` – Set text color.
- `textbackground` – Set background color.
- `textcolore` – Extended text color.
- `textbackgrounde` – Extended background color.
- `quitrequested` – True if window close requested.
- `real` – Convert integer to real.

## File I/O
- `assign` – Bind a file variable to a name.
- `reset` – Open file for reading.
- `rewrite` – Open file for writing.
- `close` – Close file.
- `readln` – Read line from file or console.
- `eof` – Test end of file.
- `ioresult` – Return last I/O error code.

## Memory Streams
- `mstreamcreate` – Create memory stream.
- `mstreamloadfromfile` – Load file into memory stream.
- `mstreamsavetofile` – Save memory stream to file.
- `mstreamfree` – Release memory stream.

## Math
- `abs` – Absolute value.
- `round` – Round real to nearest integer.
- `sqr` – Square of number.
- `sqrt` – Square root.
- `exp` – Exponential.
- `ln` – Natural logarithm.
- `cos` – Cosine.
- `sin` – Sine.
- `tan` – Tangent.
- `trunc` – Truncate real to integer.

## Random
- `randomize` – Seed random generator.
- `random` – Random number.

## DOS/OS
- `dos_getenv` – Get environment variable.
- `dos_exec` – Execute shell command.
- `dos_mkdir` – Create directory.
- `dos_rmdir` – Remove directory.
- `dos_findfirst` – Begin directory search.
- `dos_findnext` – Continue directory search.
- `dos_getdate` – Get system date.
- `dos_gettime` – Get system time.
- `dos_getfattr` – Get file attributes.

## Networking
- `api_send` – Send network packet.
- `api_receive` – Receive network packet.

## SDL graphics and audio
The following built-ins are available when Pscal is compiled with SDL support.

- `initgraph` – Initialize graphics.
- `closegraph` – Close graphics.
- `graphloop` – Poll events and delay.
- `updatescreen` – Present renderer.
- `cleardevice` – Clear renderer.
- `setcolor` – Set drawing color.
- `setrgbcolor` – Set drawing color by RGB.
- `setalphablend` – Configure alpha blending.
- `putpixel` – Draw pixel.
- `drawline` – Draw line.
- `drawrect` – Draw rectangle.
- `fillrect` – Filled rectangle.
- `drawcircle` – Draw circle.
- `fillcircle` – Filled circle.
- `drawpolygon` – Draw polygon.
- `getpixelcolor` – Read pixel color.
- `getmaxx` – Width of window.
- `getmaxy` – Height of window.
- `gettextsize` – Measure text.
- `outtextxy` – Draw text at position.
- `waitkeyevent` – Wait for key event.
- `setrendertarget` – Select render target.
- `createtexture` – Create texture.
- `createtargettexture` – Create target texture.
- `destroytexture` – Free texture.
- `loadimagetotexture` – Load image file into texture.
- `rendercopy` – Copy texture to renderer.
- `rendercopyrect` – Copy part of texture.
- `rendercopyex` – Render with rotation or flip.
- `updatetexture` – Update texture contents.
- `rendertexttotexture` – Render text into texture.
- `initsoundsystem` – Initialize audio.
- `quitsoundsystem` – Shut down audio.
- `loadsound` – Load sound file.
- `playsound` – Play sound.
- `issoundplaying` – Query if sound playing.
- `inittextsystem` – Initialize text subsystem.
- `quittextsystem` – Shut down text subsystem.
- `getmousestate` – Query mouse position and buttons.
- `getticks` – Milliseconds since start.

