unit Base64;

interface

const
  Base64Chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

var
  DecodeTable: array[0..127] of integer; { Simple ASCII range is enough }

function EncodeStringBase64(const s: string): string;
function DecodeStringBase64(const s: string): string;

implementation

procedure InitializeDecodeTable;
var
  i: integer;
  c: char;
begin
  { Initialize with -1 (invalid) }
  for i := 0 to 127 do
    DecodeTable[i] := -1;

  { Map Base64 characters to their 6-bit values }
  for i := 1 to length(Base64Chars) do
  begin
    c := Base64Chars[i];
    DecodeTable[Ord(c)] := i - 1;
  end;

  { Mark padding char (optional, useful for decoder logic) }
  DecodeTable[Ord('=')] := -2;
end;

{ --- Encoding --- }

function EncodeStringBase64(const s: string): string;
var
  i, len: integer;
  b1, b2, b3: integer; { Byte values (0-255) }
  c1, c2, c3, c4: integer; { 6-bit chunk values (0-63) }
  encoded: string;
  combined: integer; { Use integer for bit manipulation }
begin
  InitializeDecodeTable; { Ensure table is ready }
  encoded := '';
  len := length(s);
  i := 1;

  while i <= len do
  begin
    { Read up to 3 bytes }
    b1 := Ord(s[i]);
    inc(i);

    if i <= len then
       b2 := Ord(s[i])
    else
       b2 := -1; { Sentinel for padding }
    inc(i);

    if i <= len then
       b3 := Ord(s[i])
    else
       b3 := -1; { Sentinel for padding }
    inc(i);

    { Combine 3 bytes (24 bits) into one integer }
    { Handle padding cases carefully }

    c1 := b1 shr 2; { Top 6 bits of b1 }

    if b2 = -1 then begin { One input byte, two padding chars (==) }
       c2 := (b1 and 3) shl 4; { Bottom 2 bits of b1, shifted left }
       encoded := encoded + Base64Chars[c1+1] + Base64Chars[c2+1] + '==';
    end else begin { At least two input bytes }
       c2 := ((b1 and 3) shl 4) or (b2 shr 4); { Bottom 2 of b1 + top 4 of b2 }

       if b3 = -1 then begin { Two input bytes, one padding char (=) }
          c3 := (b2 and 15) shl 2; { Bottom 4 bits of b2, shifted left }
          encoded := encoded + Base64Chars[c1+1] + Base64Chars[c2+1] + Base64Chars[c3+1] + '=';
       end else begin { Three input bytes, no padding }
          c3 := ((b2 and 15) shl 2) or (b3 shr 6); { Bottom 4 of b2 + top 2 of b3 }
          c4 := b3 and 63; { Bottom 6 bits of b3 }
          encoded := encoded + Base64Chars[c1+1] + Base64Chars[c2+1] + Base64Chars[c3+1] + Base64Chars[c4+1];
       end;
    end;
  end;
  EncodeStringBase64 := encoded;
end;

{ --- Decoding --- }

function DecodeStringBase64(const s: string): string;
var
  i, len, padding: integer;
  c1, c2, c3, c4: integer; { 6-bit values }
  b1, b2, b3: integer; { Decoded byte values }
  decoded: string;
  ch: char;
begin
  InitializeDecodeTable;
  decoded := '';
  len := length(s);
  padding := 0;

  { Basic validation: length must be multiple of 4 }
  if (len mod 4) <> 0 then
  begin
     writeln('Warning: Base64 input length not multiple of 4.');
     DecodeStringBase64 := '';
     exit;
  end;

  { Count padding }
  if len >= 1 then if s[len] = '=' then inc(padding);
  if len >= 2 then if s[len-1] = '=' then inc(padding);

  i := 1;
  while i <= len do
  begin
     { Read 4 chars, decode using table }
     c1 := DecodeTable[Ord(s[i])]; inc(i);
     c2 := DecodeTable[Ord(s[i])]; inc(i);
     if i <= len then c3 := DecodeTable[Ord(s[i])] else c3 := -1; inc(i); // Handle end padding
     if i <= len then c4 := DecodeTable[Ord(s[i])] else c4 := -1; inc(i); // Handle end padding

      { Check for invalid characters (ignore padding chars here) }
      if (c1 = -1) or (c2 = -1) then
      begin
         writeln('Warning: Invalid character found in Base64 input.');
         DecodeStringBase64 := '';
         exit;
      end;

      if (c3 = -1) and ((i - 1) <= (len - padding)) then
      begin
         writeln('Warning: Invalid character found in Base64 input.');
         DecodeStringBase64 := '';
         exit;
      end;

      if (c4 = -1) and (i <= (len - padding)) then
      begin
         writeln('Warning: Invalid character found in Base64 input.');
         DecodeStringBase64 := '';
         exit;
      end;

     { Combine 4x6 bits into 3x8 bits }
     b1 := (c1 shl 2) or (c2 shr 4);
     decoded := decoded + Chr(b1);

     if c3 <> -2 then { Check not padding '=' }
     begin
        b2 := ((c2 and 15) shl 4) or (c3 shr 2);
        decoded := decoded + Chr(b2);
     end;

     if c4 <> -2 then { Check not padding '=' }
     begin
         b3 := ((c3 and 3) shl 6) or c4;
         decoded := decoded + Chr(b3);
     end;
  end;

  DecodeStringBase64 := decoded;
end;

begin
 { Unit initialization code (optional, runs when unit is used) }
 InitializeDecodeTable;
end.
