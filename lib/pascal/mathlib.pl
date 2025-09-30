unit MathLib;

interface

function PiValue: real;
function EValue: real;
function Ln2Value: real;
function Ln10Value: real;
function TwoPiValue: real;
function PiOver2Value: real;

implementation

const
  MathLibPiConst      = 3.1415926535897932384626433832795;
  MathLibEConst       = 2.7182818284590452353602874713527;
  MathLibLn2Const     = 0.69314718055994530941723212145818;
  MathLibLn10Const    = 2.3025850929940456840179914546844;
  MathLibTwoPiConst   = 2.0 * MathLibPiConst;
  MathLibPiOver2Const = MathLibPiConst / 2.0;

function PiValue: real;
begin
  PiValue := MathLibPiConst;
end;

function EValue: real;
begin
  EValue := MathLibEConst;
end;

function Ln2Value: real;
begin
  Ln2Value := MathLibLn2Const;
end;

function Ln10Value: real;
begin
  Ln10Value := MathLibLn10Const;
end;

function TwoPiValue: real;
begin
  TwoPiValue := MathLibTwoPiConst;
end;

function PiOver2Value: real;
begin
  PiOver2Value := MathLibPiOver2Const;
end;

end.
