int main() {
    mstream ms;
    ms = mstreamcreate();
    mstreamloadfromfile(&ms, "etc/words");
    str buf;
    buf = mstreambuffer(ms);
    printf("%c%c%c%c\n", buf[1], buf[2], buf[3], buf[4]);
    int count;
    count = 0;
    int len;
    len = strlen(buf);
    int i;
    for (i = 1; i <= len; i = i + 1) {
        if (buf[i] == '\n') {
            count = count + 1;
        }
    }
    printf("%d\n", count);
    mstreamfree(&ms);
    return 0;
}
