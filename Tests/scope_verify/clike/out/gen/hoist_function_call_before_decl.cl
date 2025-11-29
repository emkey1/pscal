#include <stdio.h>

        int addOne(int value);
        int main() {
            printf("result=%d
", addOne(2));
            return 0;
        }
        int addOne(int value) {
            return value + 1;
        }