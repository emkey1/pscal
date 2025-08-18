void draw_hangman(int wrong) {
    if (wrong == 0) {
        printf(" +---+");
        printf(" |   |");
        printf("     |");
        printf("     |");
        printf("     |");
        printf("     |");
        printf("=========");
    } else if (wrong == 1) {
        printf(" +---+");
        printf(" |   |");
        printf(" O   |");
        printf("     |");
        printf("     |");
        printf("     |");
        printf("=========");
    } else if (wrong == 2) {
        printf(" +---+");
        printf(" |   |");
        printf(" O   |");
        printf(" |   |");
        printf("     |");
        printf("     |");
        printf("=========");
    } else if (wrong == 3) {
        printf(" +---+");
        printf(" |   |");
        printf(" O   |");
        printf("/|   |");
        printf("     |");
        printf("     |");
        printf("=========");
    } else if (wrong == 4) {
        printf(" +---+");
        printf(" |   |");
        printf(" O   |");
        printf("/|\\  |");
        printf("     |");
        printf("     |");
        printf("=========");
    } else if (wrong == 5) {
        printf(" +---+");
        printf(" |   |");
        printf(" O   |");
        printf("/|\\  |");
        printf(" |   |");
        printf("     |");
        printf("=========");
    } else if (wrong == 6) {
        printf(" +---+");
        printf(" |   |");
        printf(" O   |");
        printf("/|\\  |");
        printf("/    |");
        printf("     |");
        printf("=========");
    } else {
        printf(" +---+");
        printf(" |   |");
        printf(" O   |");
        printf("/|\\  |");
        printf("/ \\  |");
        printf("     |");
        printf("=========");
    }
}

int main() {
    int max_wrong;
    int min_length;
    int max_length;
    int word_limit;
    str words[2048];
    int word_count;
    int f;
    str line;
    int i;
    int valid;

    int playing;
    playing = 1;

    max_wrong = 8;
    min_length = 6;
    max_length = 9;
    word_limit = 2048;
    word_count = 0;

    assign(f, "etc/words");
    reset(f);
    while (!eof(f) && word_count < word_limit) {
        readln(f, line);
        if (strlen(line) >= min_length && strlen(line) <= max_length) {
            valid = 1;
            i = 0;
            while (i < strlen(line)) {
                if ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z')) {
                    line[i] = upcase(line[i]);
                } else {
                    valid = 0;
                }
                i = i + 1;
            }
            if (valid) {
                words[word_count] = line;
                word_count = word_count + 1;
            }
        }
    }
    close(f);

    if (word_count == 0) {
        printf("No words loaded.");
        return 0;
    }

    randomize();

    while (playing) {
        str secret;
        str so_far;
        str used;
        str guess;
        int wrong;
        int done;
        int ch;
        int j;
        int found;

        secret = words[random(word_count)];
        so_far = secret;
        i = 0;
        while (i < strlen(so_far)) {
            so_far[i] = '-';
            i = i + 1;
        }
        used = "";
        wrong = 0;
        done = 0;
        while (!done) {
            draw_hangman(wrong);
            printf("Word: ", so_far);
            printf("Used: ", used);
            printf("Guess: ");
            scanf(guess);
            if (strlen(guess) == 0) continue;
            ch = upcase(guess[0]);

            found = 0;
            i = 0;
            while (i < strlen(used)) {
                if (used[i] == ch) {
                    found = 1;
                }
                i = i + 1;
            }
            if (found) {
                printf("Already guessed.");
                continue;
            }

            i = strlen(used);
            setlength(used, i + 1);
            used[i] = ch;

            found = 0;
            i = 0;
            while (i < strlen(secret)) {
                if (secret[i] == ch) {
                    so_far[i] = ch;
                    found = 1;
                }
                i = i + 1;
            }
            if (!found) {
                wrong = wrong + 1;
            } else {
                j = 0;
                done = 1;
                while (j < strlen(secret)) {
                    if (so_far[j] != secret[j]) done = 0;
                    j = j + 1;
                }
            }
            if (wrong >= max_wrong) {
                done = 1;
            }
        }
        if (wrong >= max_wrong) {
            draw_hangman(wrong);
            printf("You lose! The word was ", secret);
        } else {
            printf("You win! The word was ", secret);
        }
        printf("Play again (Y/N)? ");
        scanf(guess);
        if (strlen(guess) == 0 || upcase(guess[0]) != 'Y') {
            playing = 0;
        }
    }
    return 0;
}

