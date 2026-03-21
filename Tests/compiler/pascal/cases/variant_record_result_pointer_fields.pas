program VariantRecordResultPointerFields;

type
  ExprType = (VarExpr, AbsExpr, AppExpr);

  ExprPtr = ^Expression;
  Expression = record
    case et: ExprType of
      VarExpr: (name: Char);
      AbsExpr: (param: Char; body: ExprPtr);
      AppExpr: (func, arg: ExprPtr);
  end;

function NewVar(name: Char): ExprPtr;
begin
  New(Result);
  Result^.et := VarExpr;
  Result^.name := name;
end;

var
  e: ExprPtr;

begin
  e := NewVar('x');
  if (e^.et = VarExpr) and (e^.name = 'x') then
    Writeln('PASS: variant result pointer fields')
  else
    Writeln('FAIL: variant result pointer fields');
  Dispose(e);
end.
