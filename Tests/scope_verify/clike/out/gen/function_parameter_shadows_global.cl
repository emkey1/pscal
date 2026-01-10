#include <stdio.h>

        int total = 7;
        int bump(int total) {
            return total + 1;
        }
        int main() {
            printf("bump=%d
", bump(3));
            printf("global=%d
", total);
            return 0;
        }