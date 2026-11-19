program MTStaticArrayCoWRace;
{ Threads fill their own part of a global static array while the main thread
  keeps passing the whole array by value, as a program showing a picture in
  progress does (Examples/clike/sdl/mandelbrot_interactive: four workers write
  pixelData, the main loop hands it to UpdateTexture).

  Passing it took a shared reference, so the next element a worker wrote found
  the array shared and replaced the global's storage with a copy. A worker that
  had already checked was still writing into the old storage: those elements
  never reached the global, and the old storage was freed when the call
  returned, with the worker still writing. On an iPad that was a segmentation
  fault in the interpreter; here it shows as elements left at an earlier pass.

  While a thread job runs, a by-value read of a shared variable now copies, so
  the global keeps its storage. Every element must end at the last pass. }
const
  SIZE = 1000000;
  PARTS = 4;
  PASSES = 6;
type
  TBuf = array[0..SIZE - 1] of Byte;
var
  buf: TBuf;
  done: array[1..PARTS] of Boolean;

procedure Fill(part: Integer);
var p, i, lo, hi: Integer;
begin
  lo := (part - 1) * (SIZE div PARTS);
  hi := part * (SIZE div PARTS) - 1;
  for p := 1 to PASSES do
    for i := lo to hi do
      buf[i] := p;
  done[part] := true;
end;

procedure Fill1; begin Fill(1); end;
procedure Fill2; begin Fill(2); end;
procedure Fill3; begin Fill(3); end;
procedure Fill4; begin Fill(4); end;

function Peek(a: TBuf): Integer;
begin
  Peek := a[SIZE - 1];
end;

var
  h: array[1..PARTS] of Integer;
  i, wrong, peeks, seen: Integer;
begin
  for i := 0 to SIZE - 1 do buf[i] := 0;
  for i := 1 to PARTS do done[i] := false;
  h[1] := CreateThread(@Fill1);
  h[2] := CreateThread(@Fill2);
  h[3] := CreateThread(@Fill3);
  h[4] := CreateThread(@Fill4);
  peeks := 0;
  seen := 0;
  while not (done[1] and done[2] and done[3] and done[4]) do begin
    seen := seen + Peek(buf);
    peeks := peeks + 1;
  end;
  for i := 1 to PARTS do WaitForThread(h[i]);
  wrong := 0;
  for i := 0 to SIZE - 1 do
    if buf[i] <> PASSES then wrong := wrong + 1;
  if peeks = 0 then
    writeln('FAIL: the array was never passed while the threads ran')
  else if wrong = 0 then
    writeln('PASS: every element reached the last pass')
  else
    writeln('FAIL: ', wrong, ' of ', SIZE, ' elements were left at an earlier pass');
end.
