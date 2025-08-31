void setval(int* p, double r) { *p = r; }
int main() {
    int g = 65;
    setval(&g, g + 1.5);
    printf("%c\n", toupper(g));
    return 0;
}
