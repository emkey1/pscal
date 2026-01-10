# Nextvi(1) — General Commands Manual

## NAME
**Nextvi** — A small vi/ex terminal text editor

## SYNOPSIS
`vi [-emsv] [file ...]`

## DESCRIPTION
**Nextvi** is a modern clone of the command-line text editor `vi(1)`, initially developed by Bill Joy in 1976 for Unix-based systems. Nextvi builds upon many standard features from `vi(1)` including unique modal interface that allows users to switch between normal, insert, and command modes, for efficient text manipulation.

Additional enhancements include an unrestricted macro system, syntax highlighting, keymaps, bidirectional UTF-8 support, and numerous other features. Nextvi remains highly efficient, portable, and hackable, ensuring its continued relevance and high quality for years to come.

## OPTIONS
* **`-e`** : Enter Ex mode on startup
* **`-m`** : Disable initial file read message
* **`-s`** : Enter raw Ex mode on startup
* **`-v`** : Enter visual mode on startup (Default)

## MANPAGE NOTATION

| Notation | Description |
| :--- | :--- |
| `<x>` | A closure where x represents character literal |
| `[x]` | A closure where x represents optional argument |
| `{x}` | A closure where x represents required argument |
| `"x"` | A closure where x represents a string |
| `<^X>` | Represents a `ctrl` key X |
| `#` | Represents a positive number in a closure |
| `*` | Represents any character(s) in a closure |
| `< >` | Separates alternatives in a closure |
| `x-y` | Range from x to y |

## VI NORMAL

### Movement
* **`[#]j`** — Move # lines down
* **`[#]k`** — Move # lines up
* **`[#]+`** / **`[#]<^M>`** / **`[#]<Enter>`** — Move # lines down, cursor after indent
* **`[#]-`** — Move # lines up, cursor after indent
* **`[#]h`** — Move # columns left
* **`[#]l`** — Move # columns right
* **`[#]f{arg}`** — Move to arg character found forward # times
* **`[#]F{arg}`** — Move to arg character found backward # times
* **`[#]t{arg}`** — Move until arg character found forward # times
* **`[#]T{arg}`** — Move until arg character found backward # times
* **`[#],`** — Repeat last `<f F t T>` move backward # times
* **`[#];`** — Repeat last `<f F t T>` move forward # times

### Word Movement
* **`[#]E`** — Move to end of word # times, skip punctuation
* **`[#]e`** — Move to end of word # times
* **`[#]B`** — Move to start of word backward # times, skip punctuation
* **`[#]b`** — Move to start of word backward # times
* **`[#]W`** — Move to start of word forward # times, skip punctuation
* **`[#]w`** — Move to start of word forward # times
* **`vw`** — Toggle line only mode for `<E e B b W w>`

### Sections & Lines
* **`[#](`** — Move to next sentence boundary down # times
* **`[#])`** — Move to next sentence boundary up # times
* **`[#]{`** — Move to next `{` section down # times
* **`[#]}`** — Move to next `{` section up # times
* **`[#][`** — Move to next `<Newline>` section down # times
* **`[#]]`** — Move to next `<Newline>` section up # times
* **`^`** — Move to start of line after indent
* **`0`** — Move to start of line
* **`$`** — Move to end of line
* **`[#]|`** — Goto # col

### Character/Match Movement
* **`[#]<Space>`** — Move # characters forward, multiline
* **`[#]<^H>`** / **`[#]<Backspace>`** — Move # characters backward, multiline
* **`%`** — Move to closest `] ) }` `[ ( {` pair
* **`{#}%`** — Move to # percent line number

### Marks & Jumps
* **`'{a-z ` ' [] *}`** — Move to a line mark
* **`` `{a-z ` ' [] *} ``** — Move to a line mark with cursor position
* **`gg`** — Goto first line in buffer
* **`[#]G`** — Move to last line in buffer or # line

### Screen Positioning
* **`H`** — Move to highest line on a screen
* **`L`** — Move to lowest line on a screen
* **`M`** — Move to middle line on a screen
* **`z.`** — Center screen at cursor
* **`z<^M>`** / **`z<Enter>`** — Center screen at top row
* **`z-`** — Center screen at bottom row

### Scrolling
* **`[#]<^E>`** — Scroll down 1 or # lines, retain # and cursor position
* **`[#]<^Y>`** — Scroll up 1 or # lines, retain # and cursor position
* **`[#]<^D>`** — Scroll down half a screen size. If `[#]`, set scroll to # lines
* **`[#]<^U>`** — Scroll up half a screen size. If `[#]`, set scroll to # lines
* **`<^B>`** — Scroll up full screen size
* **`<^F>`** — Scroll down full screen size

### Options Toggles
* **`#`** — Show global and relative line numbers
* **`2#`** — Toggle show global line numbers permanently
* **`4#`** — Toggle show relative line numbers after indent permanently
* **`8#`** — Toggle show relative line numbers permanently
* **`V`** — Toggle show hidden characters: `<Space Tab Newline>`
* **`<^C>`** — Toggle show line motion numbers for `<l h e b E B w W>`
* **`{1-5}<^C>`** — Switch to line motion number mode #
* **`<^V>`** — Loop through line motion number modes

### Edits & History
* **`[#]<^R>`** — Redo # times
* **`[#]u`** — Undo # times
* **`<^I>`** / **`<Tab>`** — Open file path from cursor to end of line
* **`<^K>`** — Write current buffer to file. Force write on 2nd attempt
* **`[#]<^W>{arg}`** — Unindent arg region # times
* **`[#]<{arg}`** — Indent left arg region # times
* **`[#]>{arg}`** — Indent right arg region # times

### Registers & Macros
* **`"{arg}{arg1}`** — Operate on arg register according to arg1 motion
* **`R`** — Print registers and their contents
* **`[#]&{arg}`** — Execute arg register macro in non-blocking mode # times
* **`[#]@{arg}`** — Execute arg register macro in blocking mode # times
* **`[#]@@`** / **`[#]&&`** — Execute a last executed register macro # times
* **`[#].`** — Repeat last normal command # times
* **`[#]v.`** — Repeat last normal command moving down across # lines

### Ex Integration
* **`[#]Q`** — Enter ex mode. # retains current character offset
* **`:`** — Enter ex prompt
* **`[#]!{arg}`** — Enter pipe ex prompt based on # or arg region
* **`vv`** — Enter ex prompt with the last line from history buffer `b-1`
* **`[#]vr`** — Enter `%s/` ex prompt. Insert # words from cursor
* **`[#]vt[#arg]`** — Enter `.,.+0s/` ex prompt. Insert # of lines from cursor. Insert `#arg` words from cursor
* **`[#]v/`** — Enter `xkwd` ex prompt to set search keyword. Insert # words from cursor
* **`v;`** — Enter `!` ex prompt
* **`[#]vi`** — Enter `%s/` ex prompt. Contains regex for changing spaces to tabs. # modifies tab width
* **`[#]vI`** — Enter `%s/` ex prompt. Contains regex for changing tabs to spaces. # modifies tab width
* **`vo`** — Remove trailing white spaces and `<\r>` line endings

### Info & Formatting
* **`<^G>`** — Print buffer status infos
* **`1<^G>`** — Enable permanent status bar row
* **`2<^G>`** — Disable permanent status bar row
* **`ga`** — Print character info
* **`1ga`** — Enable permanent character info bar row
* **`2ga`** — Disable permanent character info bar row
* **`[#]gw`** — Hard word wrap a line to # col limit. Default: 80
* **`[#]gq`** — Hard word wrap a buffer to # col limit. Default: 80
* **`[#]g~{arg}`** — Switch character case for arg region # times
* **`[#]gu{arg}`** — Switch arg region to lowercase # times
* **`[#]gU{arg}`** — Switch arg region to uppercase # times
* **`[#]~`** — Switch character case # times forward

### Insert Modes
* **`i`** — Enter insert mode
* **`I`** — Enter insert mode at start of line after indent
* **`A`** — Enter insert mode at end of line
* **`a`** — Enter insert mode 1 character forward
* **`[#]s`** — Enter insert mode and delete # characters
* **`S`** — Enter insert mode and delete all characters
* **`o`** — Enter insert mode and create a new line down
* **`O`** — Enter insert mode and create a new line up
* **`[#]c{arg}`** — Enter insert mode and delete arg region # times
* **`C`** — Enter insert mode and delete from cursor to end of line

### Deletion & Yanking
* **`[#]d{arg}`** — Delete arg region # times
* **`D`** — Delete from cursor to end of line
* **`[#]x`** — Delete # characters from cursor forward
* **`[#]X`** — Delete # characters from cursor backward
* **`di{arg}`** — Delete inside arg pairs `<( ) ">`
* **`ci{arg}`** — Change inside arg pairs `<( ) ">`
* **`[#]r{arg}`** — Replace # characters with arg from cursor forward
* **`[#]K`** — Split a line # times
* **`[#]J`** — Join # lines
* **`vj`** — Toggle space padding when joining lines
* **`[#]y{arg}`** — Yank arg region # times
* **`[#]Y`** — Yank # lines
* **`[#]p`** — Paste default register below current line or after cursor position # times
* **`[#]P`** — Paste default register above current line or before cursor position # times

### Marks (Global) & Buffers
* **`m{a-z ` ' [] *}`** — Set buffer local line mark
* **`<^T>`** — Set global line mark 0. Global marks are always valid
* **`{0 2 4 6 8}<^T>`** — Set a global line mark #
* **`{1 3 5 7 9}<^T>`** — Switch to a global line mark #
* **`[#]<^7>{0-9}`** / **`[#]<^_>{0-9}`** / **`[#]<^/>{0-9}`** — Show buffer list and switch based on # or 0-9 index when prompted
* **`<^^>`** / **`<^6>`** — Swap to previous buffer
* **`[#]<^N>`** — Swap to next buffer, # changes direction `[forward backward]`
* **`\`** — Swap to `/fm/` buffer `b-2`
* **`{#}\`** — Swap from `/fm/` buffer `b-2` and backfill directory listing
* **`vb`** — Recurse into `b-1` history buffer. Insert current line into ex prompt on exit

### Keymaps & TD
* **`z1`** — Set alternative keymap to Farsi keymap
* **`z2`** — Set alternative keymap to Russian keymap
* **`ze`** — Switch to English keymap
* **`zf`** — Switch to alternative keymap
* **`zL`** — Set `td` ex option to 2
* **`zl`** — Set `td` ex option to 1
* **`zr`** — Set `td` ex option to -1
* **`zR`** — Set `td` ex option to -2

### Search
* **`[#]/`** — Regex search, move down 1 or # matches
* **`[#]?`** — Regex search, move up 1 or # matches
* **`[#]n`** / **`[#]N`** — Repeat regex search, move `[down up]` 1 or # matches
* **`<^A>`** — Regex search 1 word from cursor, no center, wraparound move `[up down]`
* **`*`** — Regex search, no center, wraparound move `[up down]`
* **`{#}*`** / **`{#}<^A>`** — Regex search, set keyword to # words from cursor
* **`<^]>`** — Filesystem regex search forward based on directory listing in `b-2`. Sets global line mark 0 for `<^P>` fallback
* **`{#}<^]>`** — Filesystem regex search forward, set keyword to # words from cursor
* **`[#]<^P>`** — Filesystem regex search backward

### System
* **`<^Z>`** — Suspend vi
* **`<^L>`** — Force redraw whole screen and update terminal dimensions
* **`Z{*}`** — Exit and clean terminal, force quit in an `&` macro
* **`Zz`** — Exit and submit history command, force quit in an `&` macro
* **`ZZ`** — Exit and write unsaved changes to a file

## VI REGIONS
Regions are vi normal commands that define `[h v]` range for vi motions. Commands described with the word "move" define a region.

`j + <^M> <Enter> - k h l f F t T , ; B E b e W w ( ) { } [ ] ^ 0 $ <Space> <^H> <Backspace> % ' ` G H L M / ? n N <^A>`

## VI MOTIONS
Motions are vi normal commands that run in a `[h v]` range. Commands described with the word "region" consume a region. Motions can be prefixed or suffixed by `[#]`.

`<^W> > < ! c d y " g~ gu gU`

**Special motions:**
* **`"`** — Special motions that consume a motion
* **`dd yy cc g~~ guu gUU >> << <^W><^W> !!`** — Special motions that can use `[#]` as number of lines

**Examples:**
* `3d/int` — Delete text until 3rd instance of "int" keyword
* `3dw` — Delete 3 words (prefix `[#]`)
* `d3w` — Delete 3 words (suffix `[#]`)
* `"ayl` — Yank a character into `a` register
* `"Ayw` — Append a word to `a` register

## VI/EX INSERT
* **`<^H>`** / **`<Backspace>`** — Delete a character, reset ex mode when empty
* **`<^U>`** — Delete util `<^X>` mark or everything
* **`<^W>`** — Delete a word
* **`<^T>`** — Increase indent
* **`<^D>`** — Decrease indent
* **`<^]>`** — Select paste register from 0-9 registers in a loop
* **`<^\>{arg}`** — Select paste register arg. `<^\>` selects default register
* **`<^P>`** — Paste a register
* **`<^X>`** — Mark autocomplete and `<^U>` starting position. `<^X>` resets the mark
* **`<^G>`** — Index current buffer for autocomplete
* **`<^Y>`** — Reset all indexed autocomplete data
* **`<^R>`** — Loop through autocomplete options backward
* **`<^N>`** — Loop through autocomplete options forward
* **`<^B>`** — Print autocomplete options when in vi insert
* **`<^B>`** (ex mode) — Recurse into `b-1` history buffer when in ex prompt. Insert current line into ex prompt on exit
* **`<^A>`** — Loop through lines in a history buffer `b-1`
* **`<^Z>`** — Suspend vi/ex
* **`<^L>`** — Redraw screen in vi mode, clean terminal in ex
* **`<^O>`** — Switch between vi and ex modes recursively
* **`<^E>`** — Switch to english keymap
* **`<^F>`** — Switch to alternative keymap
* **`<^V>{arg}`** — Read a literal character arg
* **`<^K>{arg}`** — Read a digraph sequence arg
* **`<^C>`** / **`<ESC>`** — Exit insert mode in vi, reset in ex
* **`<^M>`** / **`<Enter>`** — Insert `<Newline>` in vi, submit command in ex

## EX
Ex is a powerful line editor for Unix systems, initially developed by Bill Joy in 1976. This essential tool serves as the backbone of vi, enabling it to execute commands, macros and even transform into a purely command-line interface (CLI) when desired.

### EX PARSING
Parsing follows the structure:
`[<sep>][range][pad][cmd][<pad>][args]`

Ex commands are initiated and separated by `<:>` prefix. Fields can be padded by `<Space>` or `<Tab>`. There can only be one pad in between `[cmd]` and `[args]`. To avoid ambiguity in scripts, it is recommended to always use a pad between `[cmd]` and `[args]`.

**Examples:**
* `:evi.c` — Evaluates to `:e vi.c`
* `:eabc` — Evaluates to `:ea bc` not `:e abc`
* `:e  vi.c` — Edit ` vi.c`. `<pad>` is required

### EX ESCAPES
Special characters in `[args]` will become regular when escaped with `<\>`.

* `( ^ ] -` : Specials in regex `[]` expression
* `( ) { } + * ? ^ $ [ ] | \ . \< \>` : Specials in regex
* `% ! :` : Specials in ex

### EX EXPANSION
`<%>` in `[args]` substitutes current buffer pathname or any buffer pathname when followed by a corresponding buffer number. `"%#"` substitutes last swapped buffer pathname.

* *Example:* print pathname for buffer 69
    ` :!echo "%69"`

Every ex command is be able to receive stdout from an external program via a special expansion character `<!>`. If closing `<!>` was not specified, the end of the line becomes a terminator.

* *Example:* substitute "int" with the value of $RANDOM
    `:%s/int/!printf "%s" $RANDOM!`
* *Example:* insert output of ls shell command
    `:& i!ls`
* *Example:* insert output of ls more efficiently
    `:;c !ls!<^V><ESC>`

### EX RANGES
Some ex commands can be prefixed with ranges.
`[range]` implements vertical and horizontal ranges.
`[vrange]` implements vertical range and horizontal position.

**Syntax:**
* `[pad][%][, ;][pad][. $ ' > <][- + * / %][0-9]` — All ranges structure
* `{> <}[kwd][> <]` — Search range structure
* `'{<mark>}` — Mark range structure

**Symbols:**
`pad` (<Space>/<Tab>), `%` (First to last line), `,` (Vertical separator), `;` (Horizontal separator), `.` (Current position), `$` (Last line), `'` (Mark range), `>` (Search forward), `<` (Search backward), `- + * / %` (Arithmetic), `0-9` (Number/position).

**Examples:**
* `:1,5p` — Print lines 1,5
* `:.-5,.+5p` — Print 5 lines around current position
* `:>int>p` — Print first occurrence of "int"
* `:<int<p` — Print first occurrence of "int" in reverse
* `:.,>int>p` — Print until "int" is found
* `:<int<,.p` — Print until "int" is found in reverse
* `:>` — Search using previously set search keyword
* `:'d,'ap` — Print lines from mark `<d>` to mark `<a>`
* `:%p` — Print all lines in a buffer
* `:$p` — Print last line in a buffer
* `:$*50/100+1` — Goto 50% of the file
* `:;50` — Goto character offset 50
* `:10;50` — Goto line 10 character offset 50
* `:10;.+5` — Goto line 10 +5 character offset
* `:'a;'a` — Goto line mark `<a>` offset mark `<a>`
* `:;$` — Goto end of line
* `:5;>int>` — Search for "int" on line 5
* `:.;<int<` — Search for "int" in reverse on the current line
* `:;>int>+3;>>p` — Print text enclosed by "int" on the current line
* `:5,+10=` — +10 is relative to the initial current line, not 5
* `:;5;+10=` — +10 is relative to the initial current offset, not 5

### EX COMMANDS

* **`[vrange]f{> <}[kwd]`**
    Ranged search.
    * Example: `:f>int` (no range, current line only)
    * Example: `:f<int` (reverse)
    * Example: `:10,100f>int` (range given)

* **`[vrange]f+{> <}[kwd]`**
    Incrementing ranged search. Equivalent to the `:f` command, except subsequent commands within range move to the next match just like vi normal `[#]n` or `[#]N` commands.

* **`b[#]`**
    Print buffers or switch to a buffer.
    There are two temporary buffers: `b-1` (/hist/ ex history) and `b-2` (/fm/ directory listing).
    * Example: `:b5` (switch to 5th buffer)
    * Example: `:b-1` (switch to history)

* **`bp[path]`**
    Set current buffer path.

* **`bs[*]`**
    Set current buffer saved. Argument resets undo/redo history.

* **`[range]p`**
    Print line(s) from a buffer.
    * Example: `:1,10;5;5p` (utilize character offset ranges)
    * Example: `:1;5,10;5p` (interleaved character offset ranges)
    * Example: `:.;5;10p` (print current line from offset 5 to 10)

* **`[#]ea[kwd]`**
    Open file based on filename substring. Requires directory listing in `b-2` backfilled prior.
    * Example: `:fd` (backfill b-2)
    * Example: `:b-2:%!find .` (backfill b-2 using find)
    * Example: `:ea` (print entire listing)
    * Example: `:ea v` (open filename containing "v")
    * Example: `:15ea` (open path at index 15)

* **`[#]ea![kwd]`**
    Forced version of `ea`.

* **`[vrange]i[str]`**
    Enter ex insert mode before specified position. `str` specifies initial input.
    * Example: `:i hello<^M><ESC>`
    * Example: `:i hello<^M><^C>` (discard changes)
    * Example: `:i hello<^V><ESC>` (immediately insert "hello")

* **`[vrange]a[str]`**
    Enter ex insert mode after specified position.

* **`[range]c[str]`**
    Enter ex change mode. Optimal for modifying very long lines.
    * Example: `:c hello<^M><ESC>` (replace current line)
    * Example: `:1,5c hello<^M><ESC>` (replace lines 1-5)
    * Example: `:;c hello<^M><ESC>` (insert at current offset)

* **`[range]d`**
    Delete line(s).
    * Example: `:;0;.,1,.d` (delete from current position to start)
    * Example: `:;;$,.,$d` (delete from current position to end)

* **`e[path]`**
    Open a file at a path. No argument opens "unnamed" buffer.

* **`e![path]`**
    Force open a file at a path. No argument re-reads current buffer.

* **`[vrange]g{<*>}[kwd]{<*>}{cmd}`**
    Global command. Execute an ex command on a range of lines that matches an enclosed regex.
    * Example: `:g/^$/d` (remove all empty lines)
    * Example: `:g//p` (print lines matching previously set keyword)
    * Example: `:g/int/p\:ya ax` (print and append lines matching "int" to register `a`)
    * Example: `:g/int/g/;$/& A has a semicolon` (nested global command)

* **`[vrange]g!{<*>}[kwd]{<*>}{cmd}`**
    Inverted global command.

* **`[range]=[<0-3 *>][*]`**
    Print range numbers.
    * Example: `:;= 2` (print current character offset)
    * Example: `:'a=` (print value of mark `a`)
    * Example: `:1,75-100=1p` (calculate 75 - 100)

* **`[vrange]k{<mark>}`**
    Set a line mark. Valid marks: `<a-z> <`> <'> <[> <]> <*>`.

* **`&{macro}`**
    Global non-blocking macro. Execute raw vi/ex input sequence.
    * Example: `:&& ihello`
    * Example: `:& \:hello<^V><^M>` (execute :hello)
    * Example: `:& ci(int`

* **`@{macro}`**
    Global blocking macro. Execute raw vi/ex input sequence, waiting for input.
    * Example: `:@ ihello`
    * Example: `:@ ci(int<^V><^C>ci)INT`

* **`[#0 <$>][,{#1 <$>}][,{#2 <$>}]?{cmd}`**
    While loop conditional. Repeat cmd `#0` times or infinite with `<$>`.
    * Example: `:10000? & J` (join every line)
    * Example: `:$? u` (undo everything)
    * Example: `:? \![ -f ./vi.c ]:e ./vi.c:mpt` (edit vi.c only if exists)

* **`[range]pu[<reg>][*][\!{cmd}]`**
    Paste or pipe a register.
    * Example: `:1;5pu a`
    * Example: `:pu \!xclip -selection clipboard`

* **`[range]r[path]`** / **`[range]r[\!{cmd}]`**
    Read a file or a pipe.
    * Example: `:r vi.c`
    * Example: `:r \!ls`

* **`[range]w[path]`** / **`[range]w[\!{cmd}]`**
    Write a file or a pipe.
    * Example: `:w vi.c`
    * Example: `:w \!less`

* **`[range]w![path]`**
    Force write to a file.

* **`q`** / **`q!`**
    Exit / Force quit.

* **`wq`** / **`x`**
    Write and exit.

* **`wq!`** / **`x!`**
    Force write and quit.

* **`u`** / **`rd`**
    Undo / Redo.

* **`[vrange]s{<*>}[kwd]{<*>}{str}[<*>][<g>]`**
    Substitute. Find and replace text matching regex.
    * Example: `:%s/term1/term2/g` (global)
    * Example: `:%s/(int)|(void)/pre\0after` (backreference)

* **`[range]ya[<reg>][*]`**
    Yank into a register.
    * Example: `:ya 1x` (append to register 1)
    * Example: `:1,5;5;5ya a`

* **`ya![<reg>]`**
    Free a register.

* **`[range]![cmd]`**
    Run an external program.
    * Example: `:%!sort`
    * Example: `:%!sed -e 's/int/uint/g'`

* **`ft[filetype]`**
    Set a filetype.

* **`cm[keymap]`**
    Set a keymap.

* **`cm![keymap]`**
    Set an alternative keymap.

* **`fd[path]`**
    Set a secondary directory.

* **`fp[path]`**
    Set a directory path for `:fd` command.

* **`cd[path]`**
    Set a working directory.

* **`inc[regex]`**
    Include regex for `:fd` calculation.
    * Example: `:inc submodule.*\.c$`

* **`reg`**
    Print registers and their contents.

* **`bx[#]`**
    Set max number of buffers allowed.

* **`ac[regex]`**
    Set autocomplete filter regex.
    * Example: `:ac .+`

* **`uc`**
    Toggle multibyte utf-8 decoding.

* **`uz`**
    Toggle zero width placeholders.

* **`ub`**
    Toggle combining multicodepoint placeholders.

* **`ph[#clow] [#chigh] [#width] [#blen][*char]`**
    Redefine placeholders.
    * Example: `:ph 128 255 1 1~` (render 8 bit ascii as `~`)
    * Example: `:ph 3 3 2 1^C` (render control byte 03 as `^C`)

### EX OPTIONS
Ex options alter global variables. No argument inverts the current value unless stated otherwise.

* **`ai[val=1]`** — Indent new lines.
* **`ic[val=1]`** — Ignore case in regular expressions.
* **`ish[val=0]`** — Interactive shell for `<!>` commands.
* **`grp[val=0]`** — Regex search group definition.
* **`hl[val=1]`** — Highlight text.
* **`hlr[val=0]`** — Highlight text in reverse direction.
* **`hll[val=0]`** — Highlight current line.
* **`hlp[val=0]`** — Highlight `[]` `()` `{}` pairs.
* **`hlw[val=0]`** — Highlight current word.
* **`led[val=1]`** — Enable all terminal output.
* **`vis[val=0]`** — Control startup flags (e.g., `:vis 4` disable raw ex mode).
* **`mpt[val=0]`** — Control vi prompts (`[any key to continue]`).
* **`order[val=1]`** — Reorder characters.
* **`shape[val=1]`** — Perform Arabic script letter shaping.
* **`pac[val=0]`** — Print autocomplete suggestions on the fly.
* **`tbs[val=8]`** — Number of spaces used to represent a tab.
* **`td[val=1]`** — Text direction context (2: LTR, -2: RTL).
* **`pr[val=0]`** — Print register. Redirects `:p` output to a register.
* **`sep[val=:]`** — Ex separator character.
* **`lim[val=-1]`** — Line length render limit.
* **`seq[val=1]`** — Control Undo/Redo grouping.
* **`[hscroll]left[val=0]`** — Control horizontal scroll.

### EXINIT ENV VAR
`EXINIT` defines a sequence of vi/ex commands to be performed at startup.

**Examples:**
* Index `vi.c` for autocomplete:
    `export EXINIT="$(printf '%b' 'e ./vi.c:& i\x7\x3:bx 1:bx')"`
* Load `vi.c` into a history buffer:
    `export EXINIT='b-1:r ./vi.c:b-1'`
* Setup `@a` macro to create braces and enter insert mode:
    `export EXINIT="$(printf '%b' 'e:& io{\n}\x16\x3kA\x3:& 1G:& 2"aY')"`
* Set ex options for optimal long line performance:
    `export EXINIT='td 2:order 0:lim 5000'`

## REGEX
Pikevm is a fast non-backtracking NFA simulation regex engine. It evaluates in constant space and O(n + k) time complexity.

**Syntax:**
* `.` — Match any single char
* `[N-M]` — Match a set of alternate ranges N to M
* `{N,M}` — Match N to M times
* `()` — Grouping
* `(?:)` — Non capture grouping
* `*` — Repeated zero or more times
* `+` — Repeated one or more times
* `|` — Union, alternative branch
* `?` — One or zero matches greedy
* `??` — One or zero matches lazy
* `^` — Assert start of line
* `$` — Assert end of line
* `\<` — Assert start of word
* `\>` — Assert end of word
* `(?=)` — Assert positive lookahead
* `(?!)` — Assert negative lookahead
* `(?>)` — Assert positive lookbehind
* `(?<)` — Assert negative lookbehind
* `(?#)` — Lookbehind offset in bytes

**Lookaround Aspects:**
1.  Use non-capturing groups inside lookarounds to avoid issues.
2.  Lookarounds can be nested.
3.  Static lookarounds like `(?=^word)` are optimized.
4.  Best suited for asserting near the end of a complex pattern.
5.  Lookbehind offset begins scanning from current position minus specified value.

## SPECIAL MARKS
* **`*`** — Position of previous ex command
* **`[`** — First line of previous change
* **`]`** — Last line of previous change
* **`'`** — Position of previous line region
* **`` ` ``** — Position of previous line region

## SPECIAL REGISTERS
* **`/`** — Previous search keyword
* **`:`** — Previous ex command
* **`0`** — Previous value of default register (atomic)
* **`1-9`** — Previous value(s) of default register (nonatomic)

## CODE MAP
```text
+--------------+----------------------+
| 522 vi.h     |  definitions/aux     |
| 537 kmap.h   |  keymap translation  |
+--------------+----------------------+
| 307 conf.c   |  hl/ft/td config     |
| 345 term.c   |  low level IO        |
| 411 ren.c    |  positioning/syntax  |
| 578 lbuf.c   |  file/line buffer    |
| 613 uc.c     |  UTF-8 support       |
| 708 led.c    |  insert mode/output  |
| 756 regex.c  |  pikevm              |
| 1455 ex.c    |  ex options/commands |
| 1863 vi.c    |  normal mode/general |
| 7036 total   |  wc -l *.c|sort      |
+--------------+----------------------+
