#include <stdio.h>

        int oddSum(int n);
        int evenSum(int n) {
            if (n <= 0) {
                return 0;
            }
            return n + oddSum(n - 1);
        }
        int oddSum(int n) {
            if (n <= 0) {
                return 0;
            }
            return n + evenSum(n - 1);
        }
        int main() {
            printf("sum=%d
", evenSum(4));
            return 0;
        }