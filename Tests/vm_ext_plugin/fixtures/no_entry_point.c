// Adversarial fixture for Tests/vm_ext_plugin/run.sh: a valid, loadable
// shared library that does NOT export pscal_ext_register. --ext must
// reject this with a clean "missing entry point" diagnostic, never crash.
int pscal_ext_plugin_fixture_dummy_symbol(void) {
    return 42;
}
