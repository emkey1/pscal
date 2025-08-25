unit StringUtil;

interface

const
  DEFAULT_SEPARATOR = '-';

function ReverseString(s: string): string;
  { Reverses the input string s }

implementation

function ReverseString(s: string): string;
var
  reversed: string;
  i: integer;
begin
  reversed := '';
  for i := length(s) downto 1 do
  begin
    reversed := reversed + s[i];
  end;
  ReverseString := reversed;
end;

begin
end.
