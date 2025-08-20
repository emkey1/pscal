program WordWrapDemo;

var
  originalText: string;
  formattedText: string;
  lineWidth: integer;

{ 
  This procedure formats a paragraph by word-wrapping it to a max width.
  It contains a nested helper function to handle the logic of finding words.
}
procedure FormatParagraph(text: string; width: integer; var outText: string);
var
  currentIndex: integer;
  currentLine: string;
  nextWord: string;

  {
    This function is nested inside FormatParagraph. It can access
    its parent's variables (like 'text'). It finds the next word
    starting from the position given by 'startIndex'.
  }
  function GetNextWord(var startIndex: integer): string;
  var
    wordStart, wordEnd: integer;
  begin
    // Skip any leading spaces to find the start of the next word
    wordStart := startIndex;
    while (wordStart <= length(text)) and (text[wordStart] = ' ') do
      inc(wordStart);

    // Find the end of the word (the next space or end of string)
    wordEnd := wordStart;
    while (wordEnd <= length(text)) and (text[wordEnd] <> ' ') do
      inc(wordEnd);

    // Extract the word and update the main index for the next call
    GetNextWord := copy(text, wordStart, wordEnd - wordStart);
    startIndex := wordEnd;
  end;

begin // Start of FormatParagraph body
  outText := '';
  currentLine := '';
  currentIndex := 1;

  while currentIndex <= length(text) do
  begin
    nextWord := GetNextWord(currentIndex); // Call the nested function
    
    if length(nextWord) > 0 then
    begin
      // Check if the new word fits on the current line
      if length(currentLine) + length(nextWord) + 1 > width then
      begin
        // It doesn't fit. Finalize the current line and start a new one.
        outText := outText + currentLine + #10; // #10 is the line feed character
        currentLine := nextWord;
      end
      else
      begin
        // It fits. Add it to the current line.
        if length(currentLine) > 0 then
          currentLine := currentLine + ' '; // Add a space before the next word
        currentLine := currentLine + nextWord;
      end;
    end;
  end;

  // Add the last remaining line to the output
  if length(currentLine) > 0 then
    outText := outText + currentLine;
end;


begin // Main program block
  lineWidth := 40;
  originalText := 'This is a sample paragraph that is quite long and contains ' +
                  'several sentences. The purpose of this program is to ' +
                  'demonstrate how a nested function can be used as a helper ' +
                  'to a more complex procedure, like this word-wrapping algorithm.';

  writeln('--- Original Text ---');
  writeln(originalText);
  writeln;

  FormatParagraph(originalText, lineWidth, formattedText);

  writeln('--- Formatted to ', lineWidth, ' columns ---');
  writeln(formattedText);
end.
