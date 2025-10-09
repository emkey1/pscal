#include <stdio.h>

        const int LIMIT = 5;
        int main() {
            int globalCopy = LIMIT;
            int LIMIT = 2;
            printf("local=%d
", LIMIT);
            printf("global=%d
", globalCopy);
            return 0;
        }