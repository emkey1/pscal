program TryFinallyBasic;

begin
  WriteLn('before');
  try
    WriteLn('body');
  finally
    WriteLn('cleanup');
  end;
  WriteLn('after');
end.
