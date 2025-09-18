int main() {
  mstream ms = mstreamcreate();
  if (ms == NULL) {
    printf("create failed\n");
    return 1;
  }
  printf("allocated\n");
  if (ms != NULL) {
    printf("not nil\n");
  }
  mstreamfree(&ms);
  if (ms == NULL) {
    printf("freed\n");
  }
  return 0;
}
