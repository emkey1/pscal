program RPNCalc;

const
    MAX_STACK = 1024;
    MAX_TOKENS = 256;
    PI_VALUE = 3.141592653589793;

type
    TNumber = double;
    TStack = array[0..MAX_STACK-1] of TNumber;
    TString = string;
    TTokens = array[0..MAX_TOKENS-1] of TString;

var
    stack: TStack;
    sp: integer;
    line: TString;
    tokens: TTokens;
    tokenCount: integer;
    i: integer;
    a, b: TNumber;

procedure InitStack;
begin
    sp := 0;
end;

function IsNumber(const s: TString): boolean;
var
    v: TNumber;
    code: integer;
begin
    Val(s, v, code);
    result := code = 0;
end;

procedure Push(x: TNumber);
begin
    if sp >= MAX_STACK then
    begin
        writeln('Error: stack overflow');
        halt(1);
    end;
    stack[sp] := x;
    inc(sp);
end;

function Pop: TNumber;
begin
    if sp = 0 then
    begin
        writeln('Error: stack underflow');
        halt(1);
    end;
    dec(sp);
    result := stack[sp];
end;

function Peek: TNumber;
begin
    if sp = 0 then
    begin
        writeln('Error: stack underflow');
        halt(1);
    end;
    result := stack[sp - 1];
end;

procedure DupTop;
begin
    Push(Peek);
end;

procedure SwapTop;
var
    x, y: TNumber;
begin
    x := Pop;
    y := Pop;
    Push(x);
    Push(y);
end;

procedure OverTop;
begin
    if sp < 2 then
    begin
        writeln('Error: stack underflow');
        halt(1);
    end;
    Push(stack[sp - 2]);
end;

procedure RotTop;
var
    x, y, z: TNumber;
begin
    if sp < 3 then
    begin
        writeln('Error: stack underflow');
        halt(1);
    end;
    z := Pop;
    y := Pop;
    x := Pop;
    Push(y);
    Push(z);
    Push(x);
end;

function NumMin(x, y: TNumber): TNumber;
begin
    if x < y then
        result := x
    else
        result := y;
end;

function NumMax(x, y: TNumber): TNumber;
begin
    if x > y then
        result := x
    else
        result := y;
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

        if cnt >= MAX_TOKENS then
        begin
            writeln('Error: too many tokens on line');
            halt(1);
        end;

        toks[cnt] := copy(src, start, p - start);
        inc(cnt);
    end;
end;

procedure ExecuteToken(const t: TString);
var
    v: TNumber;
    code: integer;
    n: integer;
    r: int64;
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
    else if t = 'pi' then
        Push(PI_VALUE)
    else if t = 'e' then
        Push(Exp(1.0))
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
        if b = 0.0 then
        begin
            writeln('Error: divide by zero');
            halt(1);
        end;
        Push(a / b);
    end
    else if t = '%' then
    begin
        b := Pop;
        a := Pop;
        if b = 0.0 then
        begin
            writeln('Error: mod by zero');
            halt(1);
        end;
        Push(a - Int(a / b) * b);
    end
    else if t = '^' then
    begin
        b := Pop;
        a := Pop;
        Push(Power(a, b));
    end
    else if t = 'abs' then
    begin
        a := Pop;
        Push(Abs(a));
    end
    else if t = 'neg' then
    begin
        a := Pop;
        Push(-a);
    end
    else if t = 'inc' then
    begin
        a := Pop;
        Push(a + 1.0);
    end
    else if t = 'dec' then
    begin
        a := Pop;
        Push(a - 1.0);
    end
    else if t = 'sq' then
    begin
        a := Pop;
        Push(a * a);
    end
    else if t = 'sqrt' then
    begin
        a := Pop;
        if a < 0.0 then
        begin
            writeln('Error: sqrt of negative number');
            halt(1);
        end;
        Push(Sqrt(a));
    end
    else if t = 'sin' then
    begin
        a := Pop;
        Push(Sin(a));
    end
    else if t = 'cos' then
    begin
        a := Pop;
        Push(Cos(a));
    end
    else if t = 'tan' then
    begin
        a := Pop;
        Push(Tan(a));
    end
    else if t = 'asin' then
    begin
        a := Pop;
        if (a < -1.0) or (a > 1.0) then
        begin
            writeln('Error: asin domain is [-1, 1]');
            halt(1);
        end;
        Push(ArcSin(a));
    end
    else if t = 'acos' then
    begin
        a := Pop;
        if (a < -1.0) or (a > 1.0) then
        begin
            writeln('Error: acos domain is [-1, 1]');
            halt(1);
        end;
        Push(ArcCos(a));
    end
    else if t = 'atan' then
    begin
        a := Pop;
        Push(ArcTan(a));
    end
    else if t = 'ln' then
    begin
        a := Pop;
        if a <= 0.0 then
        begin
            writeln('Error: ln domain is x > 0');
            halt(1);
        end;
        Push(Ln(a));
    end
    else if t = 'log10' then
    begin
        a := Pop;
        if a <= 0.0 then
        begin
            writeln('Error: log10 domain is x > 0');
            halt(1);
        end;
        Push(Log10(a));
    end
    else if t = 'exp' then
    begin
        a := Pop;
        Push(Exp(a));
    end
    else if t = 'floor' then
    begin
        a := Pop;
        Push(Floor(a));
    end
    else if t = 'ceil' then
    begin
        a := Pop;
        Push(Ceil(a));
    end
    else if t = 'round' then
    begin
        a := Pop;
        r := Round(a);
        Push(r);
    end
    else if t = 'trunc' then
    begin
        a := Pop;
        Push(Trunc(a));
    end
    else if t = 'min' then
    begin
        b := Pop;
        a := Pop;
        Push(NumMin(a, b));
    end
    else if t = 'max' then
    begin
        b := Pop;
        a := Pop;
        Push(NumMax(a, b));
    end
    else if t = 'deg' then
    begin
        a := Pop;
        Push(a * 180.0 / PI_VALUE);
    end
    else if t = 'rad' then
    begin
        a := Pop;
        Push(a * PI_VALUE / 180.0);
    end
    else if t = 'dup' then
        DupTop
    else if t = 'swap' then
        SwapTop
    else if t = 'over' then
        OverTop
    else if t = 'rot' then
        RotTop
    else if t = 'drop' then
        a := Pop
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
        write(stack[i]:0:10);
        if i < sp - 1 then
            write(' ');
    end;
    writeln;
end;

begin
    InitStack;
    writeln('RPN Calculator - enter space-separated tokens.');
    writeln('Blank line exits.');
    writeln('Trig uses radians. Use "rad" to convert degrees to radians.');
    writeln('Operators/constants:');
    writeln('+ - * / % ^ abs neg inc dec sq sqrt');
    writeln('sin cos tan asin acos atan ln log10 exp');
    writeln('floor ceil round trunc min max');
    writeln('pi e deg rad dup swap over rot drop');

    while true do
    begin
        readln(line);
        if length(trim(line)) = 0 then
            break;

        Tokenize(line, tokens, tokenCount);
        for i := 0 to tokenCount - 1 do
            ExecuteToken(tokens[i]);
        PrintStack;
    end;
end.
