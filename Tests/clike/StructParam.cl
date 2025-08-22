struct WordNode { int val; };

void play_round(struct WordNode* words, int word_count, int max_wrong) {
    printf("%d\n", words->val);
    printf("%d\n", word_count);
    printf("%d\n", max_wrong);
}

int main() {
    struct WordNode* node;
    new(&node);
    node->val = 5;
    play_round(node, 1, 2);
    dispose(&node);
    return 0;
}
