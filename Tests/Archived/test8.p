program TestRecords;
var
    person: record
               name: string;
               age: integer
           end;
begin
    write('Enter your name: ');
    readln(person.name);
    person.age := 21;
    writeln('Name: ', person.name, ', Age: ', person.age);
end.
