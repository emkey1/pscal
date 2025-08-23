// Hangman game rewritten from Pascal hangman5 to CLike language.
// Uses MStreams to manage the word list.

void draw_hangman(int wrong) {
    printf(" +---+\n");
    printf(" |   |\n");
    printf(" %s   |\n", wrong > 0 ? "O" : " ");
    switch (wrong) {
        case 0:
        case 1:
        case 2:
            printf("     |\n");
            break;
        case 3:
            printf("/|   |\n");
            break;
        default:
            printf("/|\\  |\n");
            break;
    }
    printf(" %s   |\n", wrong > 4 ? "|" : " ");
    switch (wrong) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
            printf("     |\n");
            break;
        case 6:
            printf("/    |\n");
            break;
        default:
            printf("/ \\  |\n");
            break;
    }
    printf("=========\n");
}

void show_guesses_bar(int wrong, int max_wrong) {
    int i, remaining;
    remaining = max_wrong - wrong;
    printf("Guesses Left: ");
    i = 0;
    while (i < remaining) { printf("#"); i++; }
    i = 0;
    while (i < wrong) { printf("#"); i++; }
    printf(" [%d/%d]\n", remaining, max_wrong);
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

struct WordNode* load_words(int* word_count, int min_length, int max_length, int word_limit) {
    struct WordNode* words;
    mstream ms;
    str buffer;
    int len, start, i, j, word_len, valid;

    words = NULL;
    *word_count = 0;
    ms = mstreamcreate();
    mstreamloadfromfile(&ms, "etc/words");
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
        dispose(&tmp->word);
        dispose(&tmp);
        tmp = next;
    }
}

void show_hint(str secret, str so_far, int *hint_used) {
    int len, i, attempts, index;
    if (*hint_used) {
        printf("Hint Used Already.\n");
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
        printf("Hint: Letter at position %d is '%c'\n", index, secret[index]);
        *hint_used = 1;
    } else {
        printf("No more hints available.\n");
    }
}

int play_round(struct WordNode* words, int word_count, int max_wrong) {
    str secret, so_far, guessed, guess;
    char ch;
    int wrong, done, len, i, j, found, index;
    struct WordNode* current;
    int hint_used;

    index = random(word_count);
    current = words;
    while (index > 0) {
        current = current->next;
        index--;
    }
    secret = current->word;
    so_far = secret;
    len = strlen(so_far);
    i = 1;
    while (i <= len) {
        so_far[i] = '-';
        i++;
    }
    guessed = "";
    wrong = 0;
    done = 0;
    hint_used = 0;

    while (!done) {
        ClrScr();
        GotoXY(1, 1);
        draw_hangman(wrong);
        GotoXY(1, 7);
        printf("Word: %s", so_far);
        GotoXY(1, 8);
        show_guesses_bar(wrong, max_wrong);
        GotoXY(1, 10);
        sort_string(guessed);
        if (strlen(guessed) > 0) printf("Letters chosen so far: %s", guessed);
        GotoXY(1, 11);
        printf("Enter a letter (A-Z, or ? for hint): ");
        scanf(guess);
        if (strlen(guess) == 0) continue;
        if (guess[1] == '?') {
            show_hint(secret, so_far, &hint_used);
            continue;
        }
        ch = upcase(guess[1]);
        found = 0;
        len = strlen(guessed);
        i = 1;
        while (i <= len) {
            if (guessed[i] == ch) found = 1;
            i++;
        }
        if (found) {
            printf("You already guessed '%c'. Try again.\n", ch);
            continue;
        }
        guessed = guessed + ch;
        found = 0;
        len = strlen(secret);
        i = 1;
        while (i <= len) {
            if (secret[i] == ch) {
                so_far[i] = ch;
                found = 1;
            }
            i++;
        }
        if (!found) {
            wrong++;
        } else {
            len = strlen(secret);
            j = 1;
            done = 1;
            while (j <= len) {
                if (so_far[j] != secret[j]) done = 0;
                j++;
            }
        }
        if (wrong >= max_wrong) done = 1;
    }

    ClrScr();
    GotoXY(1, 1);
    draw_hangman(wrong);
    GotoXY(1, 9);
    show_guesses_bar(wrong, max_wrong);
    GotoXY(1, 11);
    sort_string(guessed);
    if (strlen(guessed) > 0) printf("Letters chosen: %s", guessed);
    GotoXY(1, 13);
    if (wrong >= max_wrong) {
        printf("Sorry, you lost. The word was: %s", secret);
        return 0;
    } else {
        printf("Congratulations, you guessed the word: %s", secret);
        return 1;
    }
}

int main() {
    struct WordNode* words;
    int word_count;
    int wins, losses, playing, result;
    str input;

    words = load_words(&word_count, 6, 9, 2048);
    if (word_count == 0) {
        printf("No words loaded.\n");
        return 0;
    }

    randomize();
    wins = 0;
    losses = 0;
    playing = 1;

    while (playing) {
        result = play_round(words, word_count, 8);
        if (result) wins++; else losses++;
        GotoXY(1,12);
        printf("Score: %d wins / %d losses\n", wins, losses);
        printf("Play again? (Y/Enter=Yes, N=No): ");
        scanf(input);
        if (strlen(input) == 0) continue;
        if (upcase(input[1]) != 'Y') playing = 0;
    }

    cleanup_words(words);
    return 0;
}

