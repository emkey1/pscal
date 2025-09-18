unit Base64;

interface

function EncodeStringBase64(const s: string): string;
function DecodeStringBase64(const s: string): string;

implementation

function EncodeBase64Char(value: integer): char;
begin
  if (value >= 0) and (value <= 25) then
    EncodeBase64Char := chr(ord('A') + value)
  else if (value >= 26) and (value <= 51) then
    EncodeBase64Char := chr(ord('a') + (value - 26))
  else if (value >= 52) and (value <= 61) then
    EncodeBase64Char := chr(ord('0') + (value - 52))
  else if value = 62 then
    EncodeBase64Char := '+'
  else if value = 63 then
    EncodeBase64Char := '/'
  else
    EncodeBase64Char := '?';
end;

function Base64Index(ch: char): integer;
begin
  if ch = '=' then
  begin
    Base64Index := -2; { Padding }
    exit;
  end;

  if (ch >= 'A') and (ch <= 'Z') then
    Base64Index := Ord(ch) - Ord('A')
  else if (ch >= 'a') and (ch <= 'z') then
    Base64Index := Ord(ch) - Ord('a') + 26
  else if (ch >= '0') and (ch <= '9') then
    Base64Index := Ord(ch) - Ord('0') + 52
  else if ch = '+' then
    Base64Index := 62
  else if ch = '/' then
    Base64Index := 63
  else
    Base64Index := -1; { Invalid character }
end;

{ --- Encoding --- }

function EncodeStringBase64(const s: string): string;
var
  i, len: integer;
  b1, b2, b3: integer; { Byte values (0-255) }
  c1, c2, c3, c4: integer; { 6-bit chunk values (0-63) }
  encoded: string;
begin
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
       encoded := encoded + EncodeBase64Char(c1) + EncodeBase64Char(c2) + '==';
    end else begin { At least two input bytes }
       c2 := ((b1 and 3) shl 4) or (b2 shr 4); { Bottom 2 of b1 + top 4 of b2 }

       if b3 = -1 then begin { Two input bytes, one padding char (=) }
          c3 := (b2 and 15) shl 2; { Bottom 4 bits of b2, shifted left }
          encoded := encoded + EncodeBase64Char(c1) + EncodeBase64Char(c2) + EncodeBase64Char(c3) + '=';
       end else begin { Three input bytes, no padding }
          c3 := ((b2 and 15) shl 2) or (b3 shr 6); { Bottom 4 of b2 + top 2 of b3 }
          c4 := b3 and 63; { Bottom 6 bits of b3 }
          encoded := encoded + EncodeBase64Char(c1) + EncodeBase64Char(c2) + EncodeBase64Char(c3) + EncodeBase64Char(c4);
       end;
    end;
  end;
  EncodeStringBase64 := encoded;
end;

{ --- Decoding --- }

function DecodeStringBase64(const s: string): string;
var
  i, len: integer;
  c1, c2, c3, c4: integer; { 6-bit values }
  b1, b2, b3: integer; { Decoded byte values }
  decoded: string;
begin
  decoded := '';
  len := length(s);
  { Basic validation: length must be multiple of 4 }
  if (len mod 4) <> 0 then
  begin
     writeln('Warning: Base64 input length not multiple of 4.');
     DecodeStringBase64 := '';
     exit;
  end;

  i := 1;
  while i <= len do
  begin
     { Read 4 chars, decode using lookup helper }
     c1 := Base64Index(s[i]); inc(i);
     c2 := Base64Index(s[i]); inc(i);
     c3 := Base64Index(s[i]); inc(i);
     c4 := Base64Index(s[i]); inc(i);

     { Check for invalid characters (ignore padding chars here) }
     if (c1 < 0) or (c1 = -2) or (c2 < 0) or (c2 = -2) then
     begin
        writeln('Warning: Invalid character found in Base64 input.');
        DecodeStringBase64 := '';
        exit;
     end;

     if c3 = -1 then
     begin
        writeln('Warning: Invalid character found in Base64 input.');
        DecodeStringBase64 := '';
        exit;
     end;

     if c4 = -1 then
     begin
        writeln('Warning: Invalid character found in Base64 input.');
        DecodeStringBase64 := '';
        exit;
     end;

     { Combine 4x6 bits into 3x8 bits }
     b1 := (c1 shl 2) or (c2 shr 4);
     decoded := decoded + Chr(b1);

     if c3 >= 0 then { Check not padding '=' }
     begin
        b2 := ((c2 and 15) shl 4) or (c3 shr 2);
        decoded := decoded + Chr(b2);

        if c4 >= 0 then { Full block }
        begin
          b3 := ((c3 and 3) shl 6) or c4;
          decoded := decoded + Chr(b3);
        end
        else if c4 <> -2 then
        begin
          writeln('Warning: Invalid character found in Base64 input.');
          DecodeStringBase64 := '';
          exit;
        end;
     end
     else
     begin
        { Padding should only appear in the last two positions }
        if c3 <> -2 then
        begin
          writeln('Warning: Invalid character found in Base64 input.');
          DecodeStringBase64 := '';
          exit;
        end;

        if c4 <> -2 then
        begin
          writeln('Warning: Invalid character found in Base64 input.');
          DecodeStringBase64 := '';
          exit;
        end;
     end;
  end;

  DecodeStringBase64 := decoded;
end;

begin
end.
