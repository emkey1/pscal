program VariantRecordBasic;

type
  ExprType = (VarExpr, AbsExpr, AppExpr);

  ExprPtr = ^Expression;
  Expression = record
    case et: ExprType of
      VarExpr: (name: Char);
      AbsExpr: (param: Char; body: ExprPtr);
      AppExpr: (func, arg: ExprPtr);
  end;

var
  e: ExprPtr;

begin
  New(e);
  e^.et := VarExpr;
  e^.name := 'x';

  if (e^.et = VarExpr) and (e^.name = 'x') then
    Writeln('PASS: variant record')
  else
    Writeln('FAIL: variant record');

  Dispose(e);
end.
