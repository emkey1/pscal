int main() {
    text f;
    int i;

    assign(f, "/tmp/clike_testfile.txt");
    rewrite(f);
    close(f);

    assign(f, "/tmp/clike_testfile.txt");
    append(f);
    i = ioresult();
    printf("%d\n", i);
    close(f);

    assign(f, "/tmp/clike_testfile.txt");
    rename(f, "/tmp/clike_testfile_renamed.txt");
    printf("%d\n", ioresult());

    assign(f, "/tmp/clike_testfile_renamed.txt");
    remove(f);
    printf("%d\n", ioresult());

    assign(f, "/tmp/clike_nonexistent.txt");
    reset(f);
    printf("%d\n", ord(ioresult() != 0));

    return 0;
}

