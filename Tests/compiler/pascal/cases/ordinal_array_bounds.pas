program OrdinalArrayBounds;

type
  TDir = (North, East, South, West);

var
  letters: array['A'..'C'] of Integer;
  flags: array[false..true] of Integer;
  dirs: array[North..West] of Integer;
  nums: array[-1..1] of Integer;
  c: Char;
  b: Boolean;
  d: TDir;

begin
  c := 'B';
  letters['A'] := 10;
  letters[c] := 20;
  letters['C'] := 30;

  b := true;
  flags[false] := 7;
  flags[b] := 11;

  d := South;
  dirs[North] := 1;
  dirs[East] := 2;
  dirs[d] := 3;
  dirs[West] := 4;

  nums[-1] := 5;
  nums[0] := 6;
  nums[1] := 7;

  Writeln('PASS: ordinal array bounds ',
          letters['A'] + letters['B'] + letters['C'],
          ' ',
          flags[false] + flags[true],
          ' ',
          dirs[North] + dirs[East] + dirs[South] + dirs[West],
          ' ',
          nums[-1] + nums[0] + nums[1]);
end.
