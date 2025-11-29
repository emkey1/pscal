#include <stdio.h>

int helper() {
    return scratch;
}
int main() {
    int scratch = 9;
    return helper();
}