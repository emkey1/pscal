unit MathLib;

interface

{ Thin wrappers over VM builtins. Kept for compatibility with existing code
  that `uses MathLib;` so projects need not be updated. }

function ArcTan(x: real): real;
function ArcSin(x: real): real;
function ArcCos(x: real): real;
function Cotan(x: real): real;
function Power(base, exponent: real): real;
function Log10(x: real): real;
function Sinh(x: real): real;
function Cosh(x: real): real;
function Tanh(x: real): real;
function Max(a, b: real): real;
function Min(a, b: real): real;
function Floor(x: real): integer;
function Ceil(x: real): integer;

implementation

function ArcTan(x: real): real; begin ArcTan := arctan(x); end;
function ArcSin(x: real): real; begin ArcSin := arcsin(x); end;
function ArcCos(x: real): real; begin ArcCos := arccos(x); end;
function Cotan(x: real): real; begin Cotan := cotan(x); end;
function Power(base, exponent: real): real; begin Power := power(base, exponent); end;
function Log10(x: real): real; begin Log10 := log10(x); end;
function Sinh(x: real): real; begin Sinh := sinh(x); end;
function Cosh(x: real): real; begin Cosh := cosh(x); end;
function Tanh(x: real): real; begin Tanh := tanh(x); end;
function Max(a, b: real): real; begin Max := max(a, b); end;
function Min(a, b: real): real; begin Min := min(a, b); end;
function Floor(x: real): integer; begin Floor := floor(x); end;
function Ceil(x: real): integer; begin Ceil := ceil(x); end;

end.
