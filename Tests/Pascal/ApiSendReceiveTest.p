program ApiSendReceiveTest;

var
  ms: mstream;
  response: string;
  url: string;
begin
  url := 'http://example.com/';
  ms := api_send(url, '');
  response := api_receive(ms);
  if pos('Example Domain', response) = 0 then
  begin
    writeln('Unexpected API response');
    halt(1);
  end;
  writeln('API send/receive test passed');
end.
