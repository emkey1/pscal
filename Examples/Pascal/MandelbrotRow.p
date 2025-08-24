program MandelbrotRowDemo;
const
  Width = 80;
  Height = 24;
  MaxIterations = 1000;
  MinRe = -2.0;
  MaxRe = 1.0;
  MinIm = -1.2;
  MaxIm = MinIm + (MaxRe - MinRe) * Height / Width;
var
  reFactor, imFactor, c_im: real;
  row: array[0..Width-1] of integer;
  x, y: integer;
begin
  reFactor := (MaxRe - MinRe) / (Width - 1);
  imFactor := (MaxIm - MinIm) / (Height - 1);
  for y := 0 to Height - 1 do
  begin
    c_im := MaxIm - y * imFactor;
    MandelbrotRow(MinRe, reFactor, c_im, MaxIterations, Width - 1, row);
    for x := 0 to Width - 1 do
    begin
      if row[x] < MaxIterations then
        write('*')
      else
        write(' ');
    end;
    writeln;
  end;
end.
