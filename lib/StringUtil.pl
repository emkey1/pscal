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
  reversed := ''; // Initialize empty string
  // Loop from the last character down to the first
  for i := length(s) downto 1 do
  begin
    // Append character to the reversed string
    // Assumes string indexing s[i] and concatenation '+' work
    reversed := reversed + s[i];
  end;
  // Assign the final reversed string to the function result
  ReverseString := reversed;
end;

begin
  // This block executes when the unit is loaded by a program
  // We can add a simple message for testing purposes, though it might
  // clutter the output of the main program.
  // writeln('[StringUtil unit initialized]');

end.
