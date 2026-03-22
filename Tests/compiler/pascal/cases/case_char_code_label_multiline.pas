program CaseCharCodeLabelMultiline;

var
  ch: Char;

begin
  ch := #13;
  case ch of
    'q':
      Writeln('q');
    #13:
      begin
        Writeln('PASS: case char code label multiline');
      end;
  end;
end.
