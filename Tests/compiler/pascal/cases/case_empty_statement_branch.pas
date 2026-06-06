program CaseEmptyStatementBranch;

var
  ch: Char;

begin
  ch := 'D';
  case ch of
    'G': Writeln('wrong');
    'D': ;
    'R': Writeln('wrong');
  end;
  Writeln('PASS: case empty statement branch');
end.
