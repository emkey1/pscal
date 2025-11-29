#include <stdio.h>

        int main() {
            int flag = 1;
            int total = 0;
            if (flag > 0) {
                int flag = 5;
                total = total + flag;
            }
            printf("total=%d
", total);
            printf("flag=%d
", flag);
            return 0;
        }