program TestWeather;

const
  API_KEY = '633f6c4c95d54b8a6a96d3e365ffcfb8';
  ZIPCODE = '98672';  { change to your desired zip code }

var
  response: string;
  ms: memorystream;
  url: string;

begin
  { Construct the request URL with zip code and API key. }
  url := 'http://api.openweathermap.org/data/2.5/weather?zip=' + ZIPCODE + ',us&appid=' + API_KEY;
  
  { Since api_send() expects two arguments, we pass an empty string as the body.
    OpenWeatherMap accepts GET requests, so an empty payload is acceptable. }
  ms := api_send(url, '');
  
  { Convert the memory stream response to a string. }
  response := api_receive(ms);
  
  writeln('Response from OpenWeatherMap:');
  writeln(response);
  
  { Optionally, verify that the response contains expected weather data.
    Here we check if the response includes the "weather" field. }
  if pos('"weather"', response) > 0 then
    writeln('Test passed: Weather data found.')
  else
    writeln('Test failed: Weather data not found.');
end.

