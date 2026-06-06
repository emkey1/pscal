program TryFinallyOuterExcept;

begin
  try
    try
      WriteLn('body');
      raise 'boom';
    finally
      WriteLn('cleanup');
    end;
  except
    on E: Exception do
      WriteLn('caught ', E.Message);
  end;
end.
