#include <stdio.h>

        const int LIMIT = 4;
        int compute() {
            const int LIMIT = 2;
            return LIMIT;
        }
        int main() {
            printf("inner=%d
", compute());
            printf("outer=%d
", LIMIT);
            return 0;
        }