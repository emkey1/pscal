unit OpenAI;

interface

function OpenAIChat(const Model, UserPrompt: string): string;
function OpenAIChatWithSystem(const Model, SystemPrompt, UserPrompt: string): string;
function OpenAIChatWithOptions(const Model, SystemPrompt, UserPrompt,
  OptionsJson, ApiKey, BaseUrl: string): string;
function OpenAIChatRaw(const Model, MessagesJson: string): string;
function OpenAIChatCustom(const Model, MessagesJson, OptionsJson, ApiKey,
  BaseUrl: string): string;

implementation

const
  HexDigits = '0123456789ABCDEF';

type
  TCharSet = set of char;

function OpenAIHexDigitValue(ch: char): integer;
begin
  if (ch >= '0') and (ch <= '9') then
    OpenAIHexDigitValue := Ord(ch) - Ord('0')
  else if (ch >= 'A') and (ch <= 'F') then
    OpenAIHexDigitValue := Ord(ch) - Ord('A') + 10
  else if (ch >= 'a') and (ch <= 'f') then
    OpenAIHexDigitValue := Ord(ch) - Ord('a') + 10
  else
    OpenAIHexDigitValue := -1;
end;

function OpenAIHexValue(const Hex: string): integer;
var
  I, Digit: integer;
begin
  OpenAIHexValue := -1;
  if Length(Hex) <> 4 then
    exit;
  OpenAIHexValue := 0;
  for I := 1 to 4 do
  begin
    Digit := OpenAIHexDigitValue(Hex[I]);
    if Digit < 0 then
    begin
      OpenAIHexValue := -1;
      exit;
    end;
    OpenAIHexValue := (OpenAIHexValue shl 4) or Digit;
  end;
end;

function OpenAIHexByte(Value: integer): string;
begin
  Value := Value and $FF;
  OpenAIHexByte := '';
  OpenAIHexByte := OpenAIHexByte + HexDigits[(Value shr 4) and $0F + 1];
  OpenAIHexByte := OpenAIHexByte + HexDigits[(Value and $0F) + 1];
end;

function OpenAIJsonEscape(const S: string): string;
var
  I, Code: integer;
  Ch: char;
begin
  OpenAIJsonEscape := '';
  for I := 1 to Length(S) do
  begin
    Ch := S[I];
    case Ch of
      '"': OpenAIJsonEscape := OpenAIJsonEscape + '\"';
      '\': OpenAIJsonEscape := OpenAIJsonEscape + '\\';
      #8: OpenAIJsonEscape := OpenAIJsonEscape + '\b';
      #9: OpenAIJsonEscape := OpenAIJsonEscape + '\t';
      #10: OpenAIJsonEscape := OpenAIJsonEscape + '\n';
      #12: OpenAIJsonEscape := OpenAIJsonEscape + '\f';
      #13: OpenAIJsonEscape := OpenAIJsonEscape + '\r';
    else
      Code := Ord(Ch);
      if Code < 32 then
        OpenAIJsonEscape := OpenAIJsonEscape + '\u00' + OpenAIHexByte(Code)
      else
        OpenAIJsonEscape := OpenAIJsonEscape + Ch;
    end;
  end;
end;

function OpenAIParseJsonString(const Source: string; var Index: integer;
  var Success: boolean): string;
var
  Len, Code: integer;
  Ch: char;
  Hex: string;
begin
  Len := Length(Source);
  Success := False;
  OpenAIParseJsonString := '';
  while Index <= Len do
  begin
    Ch := Source[Index];
    if Ch = '"' then
    begin
      Inc(Index);
      Success := True;
      exit;
    end
    else if Ch = '\' then
    begin
      Inc(Index);
      if Index > Len then
        exit;
      Ch := Source[Index];
      case Ch of
        '"', '\', '/': OpenAIParseJsonString := OpenAIParseJsonString + Ch;
        'b': OpenAIParseJsonString := OpenAIParseJsonString + #8;
        'f': OpenAIParseJsonString := OpenAIParseJsonString + #12;
        'n': OpenAIParseJsonString := OpenAIParseJsonString + #10;
        'r': OpenAIParseJsonString := OpenAIParseJsonString + #13;
        't': OpenAIParseJsonString := OpenAIParseJsonString + #9;
        'u':
          begin
            if Index + 4 > Len then
              exit;
            Hex := Copy(Source, Index + 1, 4);
            Code := OpenAIHexValue(Hex);
            if Code < 0 then
              exit;
            if Code > 255 then
              Code := Code mod 256;
            OpenAIParseJsonString := OpenAIParseJsonString + Chr(Code);
            Index := Index + 4;
          end;
      else
        OpenAIParseJsonString := OpenAIParseJsonString + Ch;
      end;
    end
    else
      OpenAIParseJsonString := OpenAIParseJsonString + Ch;
    Inc(Index);
  end;
end;

function OpenAIExtractFirstContent(const Response: string): string;
const
  Whitespace: TCharSet = [' ', #9, #10, #13];
var
  SearchStart, Idx, Len, Cursor: integer;
  Parsed: string;
  Success: boolean;
begin
  Len := Length(Response);
  SearchStart := 1;
  while SearchStart <= Len do
  begin
    Idx := Pos('"content"', Copy(Response, SearchStart, Len - SearchStart + 1));
    if Idx = 0 then
      break;
    Idx := Idx + SearchStart - 1;
    Cursor := Idx + Length('"content"');
    while (Cursor <= Len) and (Response[Cursor] in Whitespace) do
      Inc(Cursor);
    if (Cursor <= Len) and (Response[Cursor] = ':') then
    begin
      Inc(Cursor);
      while (Cursor <= Len) and (Response[Cursor] in Whitespace) do
        Inc(Cursor);
      if (Cursor <= Len) and (Response[Cursor] = '"') then
      begin
        Inc(Cursor);
        Parsed := OpenAIParseJsonString(Response, Cursor, Success);
        if Success then
        begin
          OpenAIExtractFirstContent := Parsed;
          exit;
        end;
      end;
    end;
    SearchStart := Idx + 8;
  end;
  OpenAIExtractFirstContent := Response;
end;

function OpenAIComposeMessages(const SystemPrompt, UserPrompt: string): string;
var
  ResultStr: string;
begin
  ResultStr := '[';
  if SystemPrompt <> '' then
  begin
    ResultStr := ResultStr + '{"role":"system","content":"' +
      OpenAIJsonEscape(SystemPrompt) + '"}';
    if UserPrompt <> '' then
      ResultStr := ResultStr + ',';
  end;
  ResultStr := ResultStr + '{"role":"user","content":"' +
    OpenAIJsonEscape(UserPrompt) + '"}]';
  OpenAIComposeMessages := ResultStr;
end;

function OpenAIChatRaw(const Model, MessagesJson: string): string;
begin
  OpenAIChatRaw := OpenAIChatCompletions(Model, MessagesJson);
end;

function OpenAIChatCustom(const Model, MessagesJson, OptionsJson, ApiKey,
  BaseUrl: string): string;
begin
  OpenAIChatCustom := OpenAIChatCompletions(Model, MessagesJson, OptionsJson,
    ApiKey, BaseUrl);
end;

function OpenAIChatWithOptions(const Model, SystemPrompt, UserPrompt,
  OptionsJson, ApiKey, BaseUrl: string): string;
var
  Messages, Response, Content: string;
begin
  Messages := OpenAIComposeMessages(SystemPrompt, UserPrompt);
  Response := OpenAIChatCompletions(Model, Messages, OptionsJson, ApiKey, BaseUrl);
  Content := OpenAIExtractFirstContent(Response);
  OpenAIChatWithOptions := Content;
end;

function OpenAIChatWithSystem(const Model, SystemPrompt,
  UserPrompt: string): string;
begin
  OpenAIChatWithSystem := OpenAIChatWithOptions(Model, SystemPrompt, UserPrompt,
    '', '', '');
end;

function OpenAIChat(const Model, UserPrompt: string): string;
begin
  OpenAIChat := OpenAIChatWithSystem(Model, '', UserPrompt);
end;

end.
