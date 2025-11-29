#include <stdio.h>

int clash(int value, int value) {
    return value;
}
int main() {
    return clash(1, 2);
}