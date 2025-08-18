int main() {
    int i;
    i = 0;
    while (1) {
        i = i + 1;
        if (i == 2) {
            continue;
        }
        if (i == 4) {
            break;
        }
        printf(i);
    }
    return 0;
}

