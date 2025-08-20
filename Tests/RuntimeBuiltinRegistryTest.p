program RuntimeBuiltinRegistryTest;
begin
  if TestDynamicBuiltin() <> 123 then
    halt(1);
end.
