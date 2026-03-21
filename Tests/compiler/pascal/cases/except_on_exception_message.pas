program ExceptOnExceptionMessage;

begin
  try
    raise 'boom';
  except
    on E: Exception do
      Writeln('PASS: except on message = ', E.Message);
  end;
end.
