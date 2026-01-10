#include <stdio.h>

        int odd(int n);
        int even(int n) {
            if (n == 0) {
                return 1;
            }
            return odd(n - 1);
        }
        int odd(int n) {
            if (n == 0) {
                return 0;
            }
            return even(n - 1);
        }
        int main() {
            printf("even?%d
", even(4));
            printf("odd?%d
", odd(5));
            return 0;
        }