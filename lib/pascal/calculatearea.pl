unit CalculateArea;
interface

function RectangleArea(len, width: real): real;
function CircleArea(radius: real) : real;
function TriangleArea( side1, side2, side3: real): real;

implementation

const
   PI = 3.14159;

function RectangleArea(len, width: real): real;
begin
   RectangleArea := len * width;
end;

function CircleArea(radius: real) : real;
begin
   CircleArea := PI * radius * radius;
end;

function TriangleArea( side1, side2, side3: real): real;
var
   s, area: real;

begin
   s := (side1 + side2 + side3)/2.0;
   area := sqrt(s * (s - side1)*(s-side2)*(s-side3));
   TriangleArea := area;
end;

end.
