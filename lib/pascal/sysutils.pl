unit SysUtils;

interface

// --- String Manipulation ---
function UpperCase(S: string): string;
function LowerCase(S: string): string;
function Trim(S: string): string;
function TrimLeft(S: string): string;
function TrimRight(S: string): string;
function QuotedStr(S: string): string; // Simplified: Doesn't handle internal quotes

// --- File System ---
function FileExists(FileName: string): Boolean;

// --- Conversions ---
// function StrToIntDef(S: string; Default: Longint): Longint; // Needs TryStrToInt built-in
// function FloatToStr(Value: Extended): string; // Needs complex formatting support

// --- Date/Time ---
// function Now: TDateTime; // Needs built-in support
// function Date: TDateTime; // Needs built-in support
// function Time: TDateTime; // Needs built-in support

implementation

// --- String Manipulation ---

function UpperCase(S: string): string;
var
  I: Integer;
  Res: string;
begin
  SetLength(Res, Length(S)); // Assumes SetLength exists or string assignment pre-allocates
  if Length(S) > 0 then
  begin
    for I := 1 to Length(S) do
    begin
      // Use built-in UpCase which works on single chars
      // Note: pscal's UpCase returns CHAR, need conversion if Res[I] assignment expects CHAR
      // Assuming direct string indexing works and assigns CHAR implicitly, OR
      // that UpCase returns a single-char string (less likely standard).
      // Let's assume we need Ord/Chr if UpCase returns CHAR.
      // Simpler approach if assignment handles it: Res[I] := UpCase(S[I]);
      // Safer approach:
      if (S[I] >= 'a') and (S[I] <= 'z') then
         Res[I] := Chr(Ord(S[I]) - 32) // Basic ASCII conversion
      else
         Res[I] := S[I];
    end;
  end
  else
    Res := '';

  UpperCase := Res;
end;

function LowerCase(S: string): string;
var
  I: Integer;
  Res: string;
begin
  SetLength(Res, Length(S)); // Assumption
   if Length(S) > 0 then
   begin
     for I := 1 to Length(S) do
     begin
       if (S[I] >= 'A') and (S[I] <= 'Z') then
          Res[I] := Chr(Ord(S[I]) + 32) // Basic ASCII conversion
       else
          Res[I] := S[I];
     end;
   end
   else
     Res := '';

  LowerCase := Res;
end;

function TrimLeft(S: string): string;
var
  First, Len: Integer;
begin
  Len := Length(S);
  First := 1;
  while (First <= Len) and (S[First] = ' ') do
    Inc(First);
  if First > Len then
    TrimLeft := ''
  else
    TrimLeft := Copy(S, First, Len - First + 1);
end;

function TrimRight(S: string): string;
var
  Last: Integer;
begin
  Last := Length(S);
  while (Last > 0) and (S[Last] = ' ') do
    Dec(Last);
  if Last < 1 then
    TrimRight := ''
  else
    TrimRight := Copy(S, 1, Last);
end;

function Trim(S: string): string;
var
  First, Last, Len: Integer;
begin
  Len := Length(S);
  First := 1;
  // Find first non-space character
  while (First <= Len) and (S[First] = ' ') do
    Inc(First);

  // Find last non-space character
  Last := Len;
  while (Last >= First) and (S[Last] = ' ') do
    Dec(Last);

  if First > Last then // String was all spaces or empty
    Trim := ''
  else
    Trim := Copy(S, First, Last - First + 1); // Use built-in Copy
end;

// Simplified QuotedStr - doesn't handle doubling internal quotes
function QuotedStr(S: string): string;
begin
  QuotedStr := '''' + S + ''''; // Basic concatenation
end;


// --- File System ---

function FileExists(FileName: string): Boolean;
var
  F: Text; // Use Text for Reset/IOResult checking
  IOStatus: Integer;
begin
  // Standard Pascal trick: Try to open for reading, check IOResult
  {$I-} // Disable IO checking temporarily
  Assign(F, FileName);
  Reset(F); // Try to open for reading
  {$I+} // Re-enable IO checking

  IOStatus := IOResult; // Get result of Reset

  if IOStatus = 0 then // If Reset succeeded (file exists and readable)
  begin
    Close(F); // Close the file we opened
    FileExists := True;
  end
  else // Reset failed
  begin
    FileExists := False;
  end;
end;

// --- Other sections would go here ---

end.
