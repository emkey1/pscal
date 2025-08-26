void sort_string(str* sp) {
    int i, j, len;
    char tmp;
    str s = *sp;
    len = strlen(s); // strings index from one
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
    *sp = s;
}

void main() {
    // String literals are immutable; make a writable copy first.
    str sort = copy("This is a string", 1, strlen("This is a string"));
    sort_string(&sort);
    printf("Sorted String is %s\n", sort);
}
