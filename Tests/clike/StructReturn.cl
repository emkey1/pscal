struct WordNode { int val; };

struct WordNode* make() {
    struct WordNode* n;
    new(&n);
    n->val = 7;
    return n;
}

int main() {
    struct WordNode* w = make();
    printf("%d\n", w->val);
    dispose(&w);
    return 0;
}
