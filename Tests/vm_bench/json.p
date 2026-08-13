program BenchJson;
{ JSON parse + walk via the yyjson builtins: build a document text once,
  then repeatedly parse it and walk every element, summing fields.
  Exercises ext-builtin dispatch and the yyjson handle layer. }

const
  RECORDS = 60;
  ROUNDS = 1500;

var
  t0, t1: double;
  jsonText: string;
  checkResult: integer;

procedure BuildText;
var
  i: integer;
begin
  jsonText := '{"items":[';
  for i := 1 to RECORDS do
  begin
    jsonText := jsonText + '{"id":' + IntToStr(i)
      + ',"name":"item' + IntToStr(i) + '"'
      + ',"score":' + IntToStr(i * 37 mod 101)
      + ',"flag":';
    if (i mod 2) = 0 then
      jsonText := jsonText + 'true'
    else
      jsonText := jsonText + 'false';
    jsonText := jsonText + '}';
    if i < RECORDS then
      jsonText := jsonText + ',';
  end;
  jsonText := jsonText + '],"count":' + IntToStr(RECORDS) + '}';
end;

function WalkOnce: integer;
var
  doc, root, items, obj, h: integer;
  i, n, sum: integer;
begin
  sum := 0;
  doc := YyjsonRead(jsonText);
  if doc < 0 then
  begin
    writeln('parse_failed');
    halt(1);
  end;
  root := YyjsonGetRoot(doc);
  items := YyjsonGetKey(root, 'items');
  n := YyjsonGetLength(items);
  for i := 0 to n - 1 do
  begin
    obj := YyjsonGetIndex(items, i);
    h := YyjsonGetKey(obj, 'id');
    sum := sum + YyjsonGetInt(h);
    YyjsonFreeValue(h);
    h := YyjsonGetKey(obj, 'score');
    sum := sum + YyjsonGetInt(h);
    YyjsonFreeValue(h);
    h := YyjsonGetKey(obj, 'name');
    sum := sum + length(YyjsonGetString(h));
    YyjsonFreeValue(h);
    h := YyjsonGetKey(obj, 'flag');
    sum := sum + YyjsonGetBool(h);
    YyjsonFreeValue(h);
    YyjsonFreeValue(obj);
  end;
  h := YyjsonGetKey(root, 'count');
  sum := sum + YyjsonGetInt(h);
  YyjsonFreeValue(h);
  YyjsonFreeValue(items);
  YyjsonFreeValue(root);
  YyjsonDocFree(doc);
  WalkOnce := sum;
end;

var
  r, total: integer;
begin
  BuildText;
  t0 := RealTimeClock();
  total := 0;
  for r := 1 to ROUNDS do
    total := (total + WalkOnce) mod 1000000007;
  checkResult := total;
  t1 := RealTimeClock();
  writeln('check=', checkResult);
  writeln('elapsed_s=', (t1 - t0):0:6);
end.
