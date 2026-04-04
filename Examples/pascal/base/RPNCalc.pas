program RPNCalc;

const
    MAX_STACK = 1024;
    MAX_TOKENS = 256;

type
    TStack = array[0..MAX_STACK-1] of integer;
    TString = string;
    TTokens = array[0..MAX_TOKENS-1] of TString;

var
    stack: TStack;
    sp: integer;
    line: TString;
    tokens: TTokens;
    tokenCount: integer;
    i: integer;
    tok: TString;
    a, b: integer;

procedure InitStack;
begin
    sp := 0;
end;

function IsNumber(const s: TString): boolean;
var
    v: integer;
begin
    result := false;
    if length(s) = 0 then
        exit;
    if s[1] = '-' then
    begin
        if length(s) = 1 then
            exit;
        v := 2
    end
    else
        v := 1;

    while v <= length(s) do
    begin
        if not (s[v] in ['0'..'9']) then
            exit;
        inc(v);
    end;
    result := true;
end;

procedure Push(x: integer);
begin
    if sp >= MAX_STACK then
    begin
        writeln('Error: stack overflow');
        halt(1);
    end;
    stack[sp] := x;
    inc(sp);
end;

function Pop: integer;
begin
    if sp = 0 then
    begin
        writeln('Error: stack underflow');
        halt(1);
    end;
    dec(sp);
    result := stack[sp];
end;

procedure DupTop;
var
    v: integer;
begin
    v := Pop;
    Push(v);
    Push(v);
end;

procedure SwapTop;
var
    a, b: integer;
begin
    a := Pop;
    b := Pop;
    Push(a);
    Push(b);
end;

procedure Tokenize(const src: TString; var toks: TTokens; out cnt: integer);
var
    p, start, len: integer;
begin
    cnt := 0;
    p := 1;
    len := length(src);

    while p <= len do
    begin
        while (p <= len) and (src[p] = ' ') do
            inc(p);
        if p > len then
            break;

        start := p;
        while (p <= len) and (src[p] <> ' ') do
            inc(p);
        toks[cnt] := copy(src, start, p - start);
        inc(cnt);
        if cnt >= MAX_TOKENS then
        begin
            writeln('Error: too many tokens on line');
            halt(1);
        end;
    end;
end;

procedure ExecuteToken(const t: TString);
var
    v, i, pow, base: integer;
    code: integer;
begin
    if IsNumber(t) then
    begin
        Val(t, v, code);
        if code <> 0 then
        begin
            writeln('Error: invalid number "', t, '"');
            halt(1);
        end;
        Push(v);
    end
    else if t = '+' then
    begin
        b := Pop;
        a := Pop;
        Push(a + b);
    end
    else if t = '-' then
    begin
        b := Pop;
        a := Pop;
        Push(a - b);
    end
    else if t = '*' then
    begin
        b := Pop;
        a := Pop;
        Push(a * b);
    end
    else if t = '/' then
    begin
        b := Pop;
        a := Pop;
        if b = 0 then
        begin
            writeln('Error: divide by zero');
            halt(1);
        end;
        Push(a div b);
    end
    else if t = '%' then
    begin
        b := Pop;
        a := Pop;
        if b = 0 then
        begin
            writeln('Error: mod by zero');
            halt(1);
        end;
        Push(a mod b);
    end
    else if t = '^' then
    begin
        b := Pop;
        a := Pop;
        if b < 0 then
        begin
            writeln('Error: negative exponent');
            halt(1);
        end;
        pow := 1;
        base := a;
        for i := 1 to b do
            pow := pow * base;
        Push(pow);
    end
    else if t = 'dup' then
        DupTop
    else if t = 'swap' then
        SwapTop
    else if t = 'drop' then
        Pop
    else
    begin
        writeln('Error: unknown token "', t, '"');
        halt(1);
    end;
end;

procedure PrintStack;
var
    i: integer;
begin
    if sp = 0 then
    begin
        writeln('(empty)');
        exit;
    end;
    write('Stack [bottom -> top]: ');
    for i := 0 to sp - 1 do
    begin
        write(stack[i]);
        if i < sp - 1 then
            write(' ');
    end;
    writeln;
end;

begin
    InitStack;
    writeln('RPN Calculator - enter space-separated tokens.');
    writeln('Press Enter to evaluate the current line; blank line exits.');
    while true do
    begin
        readln(line);
        if length(trim(line)) = 0 then
        begin
            break;
        end;
        Tokenize(line, tokens, tokenCount);
        for i := 0 to tokenCount - 1 do
            ExecuteToken(tokens[i]);
        PrintStack;
    end;
end.
