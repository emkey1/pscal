program TestConcat;
var
  s1, s2, s3: String;
begin
  s1 := 'Hello, ';
  s2 := 'World!';
  s3 := s1 + s2;
  WriteLn(s3);
  if s3 = 'Hello, World!' then
    WriteLn('Concatenation passed.')
  else
    WriteLn('Concatenation failed.');
end.
