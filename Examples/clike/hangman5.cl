// Hangman game rewritten from Pascal hangman5 to CLike language.
// Uses MStreams to manage the word list and CRT formatting routines.

int HEADER_ROW = 1;
int SUBTITLE_ROW = 2;
int WORD_ROW = 4;
int HANGMAN_ROW = 6;
int GUESSBAR_ROW = 15;
int GUESSED_ROW = 17;
int PROMPT_ROW = 19;
int MSG_ROW = 21;
int GAME_HEIGHT = 22;
int MAX_ELEMENT_WIDTH = 40;
int HANGMAN_WIDTH = 12;
int BORDER_PADDING = 2;

int vHeaderRow, vSubtitleRow, vWordRow, vHangmanRow, vGuessBarRow, vGuessedRow, vPromptRow, vMsgRow;
int borderTop, borderBottom, borderLeft, borderRight;
int centerCol, hangmanStartCol, effectiveWidth;

int wins = 0;
int losses = 0;

int center_padding(str s) {
    int pad;
    pad = (effectiveWidth - strlen(s)) / 2;
    if (pad < 0) pad = 0;
    return pad;
}

void draw_border(int top, int bottom, int left, int right) {
    int i;
    textcolor(1);
    gotoxy(left, top); printf("╔");
    gotoxy(right, top); printf("╗");
    gotoxy(left, bottom); printf("╚");
    gotoxy(right, bottom); printf("╝");
    i = left + 1;
    while (i < right) { gotoxy(i, top); printf("═"); gotoxy(i, bottom); printf("═"); i++; }
    i = top + 1;
    while (i < bottom) { gotoxy(left, i); printf("║"); gotoxy(right, i); printf("║"); i++; }
    textcolor(15);
}

void draw_hangman(int wrong) {
    int r, c;
    r = vHangmanRow;
    c = hangmanStartCol + 2;
    gotoxy(c, r);     printf(" +---+  ");
    gotoxy(c, r+1);   printf(" |   |  ");
    gotoxy(c, r+2);   printf(" %s   |  ", wrong > 0 ? "O" : " ");
    switch (wrong) {
        case 0:
        case 1:
        case 2:
            gotoxy(c, r+3); printf("     |  ");
            break;
        case 3:
            gotoxy(c, r+3); printf("/|   |  ");
            break;
        default:
            gotoxy(c, r+3); printf("/|\\  |  ");
            break;
    }
    gotoxy(c, r+4);   printf(" %s   |  ", wrong > 4 ? "|" : " ");
    switch (wrong) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
            gotoxy(c, r+5); printf("     |  ");
            break;
        case 6:
            gotoxy(c, r+5); printf("/    |  ");
            break;
        default:
            gotoxy(c, r+5); printf("/ \\  |  ");
            break;
    }
    gotoxy(c, r+6);   printf("     |  ");
    gotoxy(c, r+7);   printf("=========");
}

void show_guesses_bar(int wrong, int max_wrong) {
    int i, remaining, pad;
    remaining = max_wrong - wrong;
    pad = (effectiveWidth - (14 + max_wrong + 6)) / 2;
    if (pad < 0) pad = 0;
    gotoxy(borderLeft + 1 + pad, vGuessBarRow);
    printf("Guesses Left: ");
    textcolor(10);
    i = 0; while (i < remaining) { printf("#"); i++; }
    textcolor(12);
    i = 0; while (i < wrong) { printf("#"); i++; }
    textcolor(15);
    printf(" [%d/%d]", remaining, max_wrong);
}

void sort_string(str s) {
    int i, j, len;
    char tmp;
    len = strlen(s);
    i = 1;
    while (i <= len) {
        j = i + 1;
        while (j <= len) {
            if (s[i] > s[j]) {
                tmp = s[i];
                s[i] = s[j];
                s[j] = tmp;
            }
            j++;
        }
        i++;
    }
}

struct WordNode {
    str word;
    struct WordNode* next;
};

struct WordNode* load_words(str path, int* word_count, int min_length, int max_length, int word_limit) {
    struct WordNode* words;
    mstream ms;
    str buffer;
    int len, start, i, j, word_len, valid;

    words = NULL;
    *word_count = 0;
    ms = mstreamcreate();
    if (!mstreamloadfromfile(&ms, path)) {
        printf("Failed to load word list: %s\n", path);
        mstreamfree(&ms);
        return NULL;
    }
    buffer = mstreambuffer(ms) + "\n";
    len = strlen(buffer);
    start = 1;
    for (i = 1; i <= len && *word_count < word_limit; i++) {
        if (buffer[i] == '\n') {
            word_len = i - start;
            if (word_len >= min_length && word_len <= max_length) {
                valid = 1;
                j = start;
                while (j < start + word_len) {
                    if ((buffer[j] >= 'a' && buffer[j] <= 'z') ||
                        (buffer[j] >= 'A' && buffer[j] <= 'Z')) {
                        buffer[j] = upcase(buffer[j]);
                    } else {
                        valid = 0;
                    }
                    j++;
                }
                if (valid) {
                    struct WordNode* node;
                    new(&node);
                    if (node == NULL) {
                        printf("Out of memory loading words.\n");
                        break;
                    }
                    node->word = copy(buffer, start, word_len);
                    node->next = words;
                    words = node;
                    *word_count = *word_count + 1;
                }
            }
            start = i + 1;
        }
    }
    mstreamfree(&ms);
    return words;
}

void cleanup_words(struct WordNode* words) {
    struct WordNode* tmp;
    tmp = words;
    while (tmp != NULL) {
        struct WordNode* next;
        next = tmp->next;
        tmp->next = NULL;  // prevent dispose from traversing the rest of the list
        dispose(&tmp);     // also releases tmp->word
        tmp = next;
    }
}

void show_hint(str secret, str so_far, int *hint_used) {
    int len, i, attempts, index, pad, msgLen;
    str idxStr;
    gotoxy(borderLeft + 1, vMsgRow);
    clreol();
    if (*hint_used) {
        pad = center_padding("Hint Used Already.");
        gotoxy(borderLeft + 1 + pad, vMsgRow);
        textcolor(3);
        printf("Hint Used Already.");
        textcolor(15);
        return;
    }
    len = strlen(secret);
    index = -1;
    attempts = 0;
    while (attempts < len * 2 && index == -1) {
        i = random(len) + 1;
        if (so_far[i] == '-') index = i;
        attempts++;
    }
    if (index != -1) {
        idxStr = inttostr(index);
        msgLen = strlen("Hint: Letter at position ") + strlen(idxStr) + strlen(" is ''") + 1;
        pad = (effectiveWidth - msgLen) / 2;
        if (pad < 0) pad = 0;
        gotoxy(borderLeft + 1 + pad, vMsgRow);
        textcolor(3);
        printf("Hint: Letter at position %d is '%c'", index, secret[index]);
        textcolor(15);
        *hint_used = 1;
    } else {
        pad = center_padding("No more hints available.");
        gotoxy(borderLeft + 1 + pad, vMsgRow);
        textcolor(3);
        printf("No more hints available.");
        textcolor(15);
    }
}

int play_round(struct WordNode* words, int word_count, int max_wrong) {
    str secret, so_far, guessed, guess, msg;
    char ch;
    int wrong, done, len, i, j, found, index;
    struct WordNode* current;
    int hint_used;
    int cols, rows, topMargin, padding;

    index = random(word_count);
    current = words;
    while (index > 0) { current = current->next; index--; }
    secret = current->word;
    so_far = secret;
    len = strlen(so_far);
    i = 1; while (i <= len) { so_far[i] = '-'; i++; }
    guessed = ""; wrong = 0; done = 0; hint_used = 0;

    cols = screencols();
    rows = screenrows();
    topMargin = (rows - GAME_HEIGHT) / 2;
    vHeaderRow = topMargin + HEADER_ROW;
    vSubtitleRow = topMargin + SUBTITLE_ROW;
    vWordRow = topMargin + WORD_ROW;
    vHangmanRow = topMargin + HANGMAN_ROW;
    vGuessBarRow = topMargin + GUESSBAR_ROW;
    vGuessedRow = topMargin + GUESSED_ROW;
    vPromptRow = topMargin + PROMPT_ROW;
    vMsgRow = topMargin + MSG_ROW;
    borderTop = topMargin;
    borderBottom = topMargin + GAME_HEIGHT;
    centerCol = cols / 2;
    hangmanStartCol = centerCol - HANGMAN_WIDTH / 2;
    borderLeft = centerCol - (MAX_ELEMENT_WIDTH / 2) - BORDER_PADDING;
    borderRight = centerCol + (MAX_ELEMENT_WIDTH / 2) + BORDER_PADDING;
    effectiveWidth = borderRight - borderLeft - 1;

    while (!done) {
        ClrScr();
        draw_border(borderTop, borderBottom, borderLeft, borderRight);
        padding = center_padding("Hangman Game With Pointers");
        gotoxy(borderLeft + 1 + padding, vHeaderRow); textcolor(10); printf("Hangman Game With Pointers"); textcolor(15);
        padding = center_padding("Guess the word");
        gotoxy(borderLeft + 1 + padding, vSubtitleRow); printf("Guess the word");
        padding = center_padding(so_far);
        gotoxy(borderLeft + 1 + padding, vWordRow); printf("%s", so_far);
        draw_hangman(wrong);
        show_guesses_bar(wrong, max_wrong);
        sort_string(guessed);
        if (strlen(guessed) > 0) {
            msg = "Letters chosen so far: " + guessed;
            padding = center_padding(msg);
            gotoxy(borderLeft + 1 + padding, vGuessedRow);
            printf("%s", msg);
        }
        gotoxy(borderLeft + 1, vMsgRow); clreol();
        msg = "Enter a letter (A-Z, or ? for hint): ";
        padding = center_padding(msg);
        gotoxy(borderLeft + 1 + padding, vPromptRow);
        printf("%s", msg);
        scanf(guess);
        if (strlen(guess) == 0) continue;
        if (guess[1] == '?') { show_hint(secret, so_far, &hint_used); continue; }
        ch = upcase(guess[1]);
        found = 0; len = strlen(guessed); i = 1;
        while (i <= len) { if (guessed[i] == ch) found = 1; i++; }
        if (found) {
            gotoxy(borderLeft + 1, vMsgRow); clreol();
            textcolor(14);
            printf("You already guessed '%c'. Try again.", ch);
            textcolor(15);
            readkey();
            continue;
        }
        guessed = guessed + ch;
        found = 0; len = strlen(secret); i = 1;
        while (i <= len) { if (secret[i] == ch) { so_far[i] = ch; found = 1; } i++; }
        if (!found) { wrong++; }
        else {
            len = strlen(secret); j = 1; done = 1;
            while (j <= len) { if (so_far[j] != secret[j]) done = 0; j++; }
        }
        if (wrong >= max_wrong) done = 1;
    }

    ClrScr();
    draw_border(borderTop, borderBottom, borderLeft, borderRight);
    padding = center_padding("Hangman Game Over");
    gotoxy(borderLeft + 1 + padding, vHeaderRow); textcolor(10); printf("Hangman Game Over"); textcolor(15);
    padding = center_padding(so_far);
    gotoxy(borderLeft + 1 + padding, vWordRow); printf("%s", so_far);
    draw_hangman(wrong);
    show_guesses_bar(wrong, max_wrong);
    sort_string(guessed);
    if (strlen(guessed) > 0) {
        msg = "Letters chosen: " + guessed;
        padding = center_padding(msg);
        gotoxy(borderLeft + 1 + padding, vGuessedRow); printf("%s", msg);
    }
    if (wrong >= max_wrong) {
        msg = "Sorry, you lost. The word was: " + secret;
        padding = center_padding(msg);
        gotoxy(borderLeft + 1 + padding, vMsgRow); textcolor(12); printf("%s", msg); textcolor(15);
        return 0;
    } else {
        msg = "Congratulations, you guessed the word: " + secret;
        padding = center_padding(msg);
        gotoxy(borderLeft + 1 + padding, vMsgRow); textcolor(2); printf("%s", msg); textcolor(15);
        return 1;
    }
}

int main() {
    struct WordNode* words;
    int word_count;
    int playing, result, padding;
    str input, word_file, scoreMsg, promptMsg;

    if (paramcount() >= 1) word_file = paramstr(1); else word_file = "etc/words";
    words = load_words(word_file, &word_count, 6, 9, 2048);
    if (word_count == 0) { printf("No words loaded.\n"); return 0; }

    randomize();
    playing = 1;
    while (playing) {
        result = play_round(words, word_count, 8);
        if (result) wins++; else losses++;
        scoreMsg = "Score: " + inttostr(wins) + " wins / " + inttostr(losses) + " losses";
        padding = center_padding(scoreMsg);
        gotoxy(borderLeft + 1 + padding, vMsgRow + 1);
        printf("%s", scoreMsg);
        promptMsg = "Play again? (Y/Enter=Yes, N=No): ";
        padding = center_padding(promptMsg);
        gotoxy(borderLeft + 1 + padding, vMsgRow + 3);
        printf("%s", promptMsg);
        scanf(input);
        if (strlen(input) == 0) continue;
        if (upcase(input[1]) != 'Y') playing = 0;
    }

    cleanup_words(words);
    return 0;
}

