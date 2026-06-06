program UnicodeCharCodeLiterals;

begin
  Writeln('hex=', #$2192);
  Writeln('hexord=', Ord(#$2192));
  Writeln('dec=', #8594);
  Writeln('decord=', Ord(#8594));
  Writeln('byte=', #219);
  Writeln('bytelen=', Length(#219));
end.
