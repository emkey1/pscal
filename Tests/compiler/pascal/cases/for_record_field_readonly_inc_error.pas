program ForRecordFieldReadonlyIncError;

type
  TCursor = record
    R: Integer;
  end;

var
  cur: TCursor;

begin
  for cur.R := 1 to 3 do
    Inc(cur.R);
end.
