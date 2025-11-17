# CMake generated Testfile for 
# Source directory: /Users/mke/PBuild
# Build directory: /Users/mke/PBuild/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[pascal_tests]=] "bash" "/Users/mke/PBuild/Tests/run_pascal_tests.sh")
set_tests_properties([=[pascal_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/mke/PBuild/CMakeLists.txt;1367;add_test;/Users/mke/PBuild/CMakeLists.txt;0;")
add_test([=[clike_tests]=] "bash" "/Users/mke/PBuild/Tests/run_clike_tests.sh")
set_tests_properties([=[clike_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/mke/PBuild/CMakeLists.txt;1368;add_test;/Users/mke/PBuild/CMakeLists.txt;0;")
add_test([=[rea_tests]=] "bash" "/Users/mke/PBuild/Tests/run_rea_tests.sh")
set_tests_properties([=[rea_tests]=] PROPERTIES  ENVIRONMENT "REA_SKIP_TESTS=constructor_init field_access_assign field_access_read method_this_assign new_alloc new_assign_ptr" _BACKTRACE_TRIPLES "/Users/mke/PBuild/CMakeLists.txt;1369;add_test;/Users/mke/PBuild/CMakeLists.txt;0;")
add_test([=[json2bc_tests]=] "bash" "/Users/mke/PBuild/Tests/run_json2bc_tests.sh")
set_tests_properties([=[json2bc_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/mke/PBuild/CMakeLists.txt;1371;add_test;/Users/mke/PBuild/CMakeLists.txt;0;")
subdirs("src/ext_builtins")
subdirs("Examples")
