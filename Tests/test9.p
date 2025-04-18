program TestProcRec;
var
    student: record
                id: integer;
                name: string;
                grade: real
             end;

procedure PrintStudent(s: record
                            id: integer;
                            name: string;
                            grade: real
                        end);
begin
    writeln('Student ID: ', s.id);
    writeln('Name: ', s.name);
    writeln('Grade: ', s.grade:0:1);  { One digit after decimal }
end;

begin
    student.id := 123;
    student.grade := 88.5;
    write('Enter student name:');
    readln(student.name);
    PrintStudent(student);
end.
