#include <stdio.h>

        struct Point {
            int x;
            int y;
        };
        int main() {
            struct Point p;
            p.x = 2;
            p.y = 3;
            printf("point=%d,%d
", p.x, p.y);
            return 0;
        }