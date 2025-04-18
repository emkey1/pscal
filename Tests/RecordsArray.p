program ArrayOfRecordsTest;
type
    TStudent = record
        id: integer;
        name: string;
    end;
var
    students: array[1..2] of TStudent;
    i: integer;
begin
    { Assign values to the array of records }
    students[1].id := 101;
    students[1].name := 'Alice';
    students[2].id := 102;
    students[2].name := 'Bob';

    { Print out the student records }
    for i := 1 to 2 do
    begin
        writeln('Student ', i, ': id = ', students[i].id, ', name = ', students[i].name);
    end;
end.

