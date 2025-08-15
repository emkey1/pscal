program WeatherOfflineTest;

var
  response: string;

begin
  response := api_receive(api_send('file:///workspace/pscal/Tests/weather_sample.json', ''));
  if pos('"name":"Mountain View"', response) = 0 then halt(1);
  if pos('"description":"clear sky"', response) = 0 then halt(1);
  if pos('"temp":282.55', response) = 0 then halt(1);
  if pos('"humidity":100', response) = 0 then halt(1);
  if pos('"speed":1.5', response) = 0 then halt(1);
end.
