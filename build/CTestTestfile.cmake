# CMake generated Testfile for 
# Source directory: /root/zjchain/third_party/libBLS
# Build directory: /root/zjchain/third_party/libBLS/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(bls_tests "/root/zjchain/third_party/libBLS/build/bls_unit_test")
set_tests_properties(bls_tests PROPERTIES  _BACKTRACE_TRIPLES "/root/zjchain/third_party/libBLS/CMakeLists.txt;248;add_test;/root/zjchain/third_party/libBLS/CMakeLists.txt;0;")
add_test(dkg_tests "/root/zjchain/third_party/libBLS/build/dkg_unit_test")
set_tests_properties(dkg_tests PROPERTIES  _BACKTRACE_TRIPLES "/root/zjchain/third_party/libBLS/CMakeLists.txt;258;add_test;/root/zjchain/third_party/libBLS/CMakeLists.txt;0;")
add_test(bls_test "/root/zjchain/third_party/libBLS/build/bls_test")
set_tests_properties(bls_test PROPERTIES  _BACKTRACE_TRIPLES "/root/zjchain/third_party/libBLS/CMakeLists.txt;268;add_test;/root/zjchain/third_party/libBLS/CMakeLists.txt;0;")
add_test(dkg_attack "/root/zjchain/third_party/libBLS/build/dkg_attack")
set_tests_properties(dkg_attack PROPERTIES  _BACKTRACE_TRIPLES "/root/zjchain/third_party/libBLS/CMakeLists.txt;274;add_test;/root/zjchain/third_party/libBLS/CMakeLists.txt;0;")
add_test(utils_tests "/root/zjchain/third_party/libBLS/build/utils_unit_test")
set_tests_properties(utils_tests PROPERTIES  _BACKTRACE_TRIPLES "/root/zjchain/third_party/libBLS/CMakeLists.txt;284;add_test;/root/zjchain/third_party/libBLS/CMakeLists.txt;0;")
subdirs("threshold_encryption")
