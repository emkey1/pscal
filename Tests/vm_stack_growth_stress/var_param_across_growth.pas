program VarParamAcrossGrowth;
{ VM 2.0 Phase 3: the core address-escape guarantee this phase must
  preserve is that GET_LOCAL_ADDRESS (backing VAR parameters) pushes a
  real Value* into `frame->slots`, and that pointer must stay valid even
  after the operand stack has grown many times (new pages mprotect'd
  into the reservation) since the address was taken. This recurses far
  past the old 8192-Value stack ceiling -- forcing many growth events --
  while holding a VAR-parameter reference taken near the top of the call
  chain, mutating it only at the deepest point, then reading it back at
  the top. A dangling/stale pointer would either crash (under ASan) or
  silently read/write the wrong memory. }

function recurseThenSet(var target: integer; n: integer): integer;
begin
  if n <= 0 then
  begin
    target := 12345;
    recurseThenSet := 0;
  end
  else
    recurseThenSet := recurseThenSet(target, n - 1);
end;

var
  v: integer;
  dummy: integer;
begin
  v := 111;
  dummy := recurseThenSet(v, 60000);
  writeln(v);
end.
