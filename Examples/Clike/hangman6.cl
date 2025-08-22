// Function to draw the hangman based on the number of wrong guesses
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

// Structure to hold a word in a linked list
struct WordNode {
    str word;
    struct WordNode* next;
};

// Loads words from a file into a linked list
struct WordNode* load_words(int* word_count, int min_length, int max_length, int word_limit) {
    struct WordNode* words = NULL;
    text f;
    str line;
    int i, len, valid;

    assign(f, "etc/words");
    reset(f);
    while (!eof(f) && *word_count < word_limit) {
        readln(f, line);
        len = strlen(line);
        if (len >= min_length && len <= max_length) {
            valid = 1;
            i = 1;
            while (i <= len) {
                if ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z')) {
                    line[i] = upcase(line[i]);
                } else {
                    valid = 0;
                }
                i++;
            }
            if (valid) {
                struct WordNode* node;
                new(&node);
                node->word = copy(line, 1, len);
                node->next = words;
                words = node;
                *word_count = *word_count + 1;
            }
        }
    }
    close(f);
    return words;
}

// Plays a single round of Hangman
void play_round(struct WordNode* words, int word_count, int max_wrong) {
    str secret, so_far, used, guess;
    int wrong, done, ch, len, i, j, found, index;
    struct WordNode* current;

    // Select a random word
    index = random(word_count);
    current = words;
    while (index > 0) {
        current = current->next;
        index--;
    }
    secret = current->word;

    // Initialize game state
    so_far = secret;
    len = strlen(so_far);
    i = 1;
    while (i <= len) {
        so_far[i] = '-';
        i++;
    }
    used = "";
    wrong = 0;
    done = 0;

    // Main game loop
    while (!done) {
        draw_hangman(wrong);
        printf("Word: %s\n", so_far);
        printf("Used: %s\n", used);
        printf("Guess: ");
        if (scanf(guess) != 1 || strlen(guess) == 0) continue;
        ch = upcase(guess[1]);

        found = 0;
        len = strlen(used);
        i = 1;
        while (i <= len) {
            if (used[i] == ch) found = 1;
            i++;
        }
        if (found) {
            printf("Already guessed.\n");
            continue;
        }

        used = used + chr(ch);

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

    // Game over message
    if (wrong >= max_wrong) {
        draw_hangman(wrong);
        printf("You lose! The word was %s\n", secret);
    } else {
        printf("You win! The word was %s\n", secret);
    }
}

// Frees the memory used by the linked list of words
void cleanup_words(struct WordNode* words) {
    struct WordNode* tmp = words;
    while (tmp != NULL) {
        struct WordNode* next = tmp->next;
        dispose(&tmp->word);
        dispose(&tmp);
        tmp = next;
    }
}

// Main function to orchestrate the game
int main() {
    struct WordNode* words;
    int word_count = 0;
    str guess;

    words = load_words(&word_count, 6, 9, 2048);
    if (word_count == 0) {
        printf("No words loaded.\n");
        return 0;
    }

    randomize();

    int playing = 1;
    while (playing) {
        play_round(words, word_count, 8);
        printf("Play again (Y/N)? ");
        if (scanf(guess) != 1 || strlen(guess) == 0 || upcase(guess[1]) != 'Y') {
            playing = 0;
        }
    }

    cleanup_words(words);
    return 0;
}
