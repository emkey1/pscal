unit SysUtils;

interface

type
  TReplaceFlag = (rfReplaceAll, rfIgnoreCase);
  TReplaceFlags = set of TReplaceFlag;

// --- String Manipulation ---
function UpperCase(S: string): string;
function LowerCase(S: string): string;
function Trim(S: string): string;
function TrimLeft(S: string): string;
function TrimRight(S: string): string;
function QuotedStr(S: string): string; // Simplified: Doesn't handle internal quotes
function StringReplace(const S, OldPattern, NewPattern: string;
  Flags: TReplaceFlags): string;

// --- File System ---
// FileExists is provided as a VM builtin.
function ExpandFileName(const FileName: string): string;
function ExtractFilePath(const FileName: string): string;
function IncludeTrailingPathDelimiter(const S: string): string;
function ExcludeTrailingPathDelimiter(const S: string): string;

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
  while First <= Len do
  begin
    if S[First] <> ' ' then
      break;
    Inc(First);
  end;
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
  while Last > 0 do
  begin
    if S[Last] <> ' ' then
      break;
    Dec(Last);
  end;
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
  while First <= Len do
  begin
    if S[First] <> ' ' then
      break;
    Inc(First);
  end;

  // Find last non-space character
  Last := Len;
  while Last >= First do
  begin
    if S[Last] <> ' ' then
      break;
    Dec(Last);
  end;

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

function StringReplace(const S, OldPattern, NewPattern: string;
  Flags: TReplaceFlags): string;
var
  SourceLen, PatternLen: Integer;
  I: Integer;
  Buffer, Slice, PatternToMatch: string;
  DoReplaceAll, DoIgnoreCase: Boolean;
begin
  PatternLen := Length(OldPattern);
  SourceLen := Length(S);

  if PatternLen = 0 then
  begin
    StringReplace := S;
    exit;
  end;

  DoReplaceAll := rfReplaceAll in Flags;
  DoIgnoreCase := rfIgnoreCase in Flags;

  if DoIgnoreCase then
    PatternToMatch := LowerCase(OldPattern)
  else
    PatternToMatch := OldPattern;

  Buffer := '';
  I := 1;

  while I <= SourceLen do
  begin
    Slice := Copy(S, I, PatternLen);

    if DoIgnoreCase then
    begin
      if LowerCase(Slice) = PatternToMatch then
      begin
        Buffer := Buffer + NewPattern;
        Inc(I, PatternLen);
        if not DoReplaceAll then
        begin
          Buffer := Buffer + Copy(S, I, SourceLen - I + 1);
          StringReplace := Buffer;
          exit;
        end;
        continue;
      end;
    end
    else if Slice = PatternToMatch then
    begin
      Buffer := Buffer + NewPattern;
      Inc(I, PatternLen);
      if not DoReplaceAll then
      begin
        Buffer := Buffer + Copy(S, I, SourceLen - I + 1);
        StringReplace := Buffer;
        exit;
      end;
      continue;
    end;

    Buffer := Buffer + S[I];
    Inc(I);
  end;

  StringReplace := Buffer;
end;


// --- Other sections would go here ---

function IncludeTrailingPathDelimiter(const S: string): string;
begin
  if Length(S) = 0 then
    IncludeTrailingPathDelimiter := '/'
  else if (S[Length(S)] = '/') or (S[Length(S)] = '\') then
    IncludeTrailingPathDelimiter := S
  else
    IncludeTrailingPathDelimiter := S + '/';
end;

function ExcludeTrailingPathDelimiter(const S: string): string;
var
  L: Integer;
begin
  L := Length(S);
  while (L > 1) and ((S[L] = '/') or (S[L] = '\')) do
    Dec(L);
  ExcludeTrailingPathDelimiter := Copy(S, 1, L);
end;

function ExtractFilePath(const FileName: string): string;
var
  L: Integer;
begin
  L := Length(FileName);
  while (L > 0) and (FileName[L] <> '/') and (FileName[L] <> '\') do
    Dec(L);
  if L = 0 then
    ExtractFilePath := ''
  else
    ExtractFilePath := Copy(FileName, 1, L);
end;

function ExpandFileName(const FileName: string): string;
var
  base: string;
begin
  if Length(FileName) = 0 then
  begin
    ExpandFileName := GetCurrentDir;
    exit;
  end;
  if (FileName[1] = '/') or (FileName[1] = '\') then
  begin
    ExpandFileName := FileName;
    exit;
  end;
  if (Length(FileName) >= 2) and (FileName[2] = ':') then
  begin
    ExpandFileName := FileName;
    exit;
  end;
  base := IncludeTrailingPathDelimiter(GetCurrentDir);
  ExpandFileName := base + FileName;
end;

end.
