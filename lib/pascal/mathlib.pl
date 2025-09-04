unit MathLib;

interface

const
  { Common mathematical constants }
  Pi      = 3.1415926535897932384626433832795;
  E       = 2.7182818284590452353602874713527;
  Ln2     = 0.69314718055994530941723212145818;
  Ln10    = 2.3025850929940456840179914546844;
  TwoPi   = 2.0 * Pi;
  PiOver2 = Pi / 2.0;

{ Thin wrappers over VM builtins. Kept for compatibility with existing code
  that `uses MathLib;` so projects need not be updated. Prefer calling the
  builtins directly in new code. }

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
function DegToRad(deg: real): real;
function RadToDeg(rad: real): real;

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
function DegToRad(deg: real): real; begin DegToRad := deg * Pi / 180.0; end;
function RadToDeg(rad: real): real; begin RadToDeg := rad * 180.0 / Pi; end;

end.
