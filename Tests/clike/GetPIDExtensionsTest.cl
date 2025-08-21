int main() {
    if (getpid() > 0) {
        printf("ok\n");
    } else {
        printf("bad\n");
    }
    return 0;
}
