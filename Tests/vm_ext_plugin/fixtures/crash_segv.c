// Adversarial fixture for Tests/vm_ext_plugin/run.sh: pscal_ext_register()
// itself segfaults. The loader's fork-based probe (plugin_loader.c) must
// contain this crash to the forked child and report a clean load failure
// -- the host process must exit normally (status 1), never crash itself.
int pscal_ext_register(const void *host, unsigned int abi) {
    (void)host;
    (void)abi;
    volatile int *bad = (int *)0;
    *bad = 1;
    return 0;
}
