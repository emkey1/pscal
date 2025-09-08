unit Dos;

interface

{ All DOS routines are now provided as VM builtins. }

const
  myBlack        = 0; // Working around some parser oddness

type
  SearchRec = record
    Name: string;
    Attr: integer;
  end;

implementation
begin
end.
