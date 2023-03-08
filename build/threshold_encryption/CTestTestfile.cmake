# CMake generated Testfile for 
# Source directory: /root/zjchain/third_party/libBLS/threshold_encryption
# Build directory: /root/zjchain/third_party/libBLS/build/threshold_encryption
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(te_tests "/root/zjchain/third_party/libBLS/build/threshold_encryption/te_unit_test")
set_tests_properties(te_tests PROPERTIES  _BACKTRACE_TRIPLES "/root/zjchain/third_party/libBLS/threshold_encryption/CMakeLists.txt;70;add_test;/root/zjchain/third_party/libBLS/threshold_encryption/CMakeLists.txt;0;")
add_test(te_wrap_tests "/root/zjchain/third_party/libBLS/build/threshold_encryption/te_unit_test")
set_tests_properties(te_wrap_tests PROPERTIES  _BACKTRACE_TRIPLES "/root/zjchain/third_party/libBLS/threshold_encryption/CMakeLists.txt;95;add_test;/root/zjchain/third_party/libBLS/threshold_encryption/CMakeLists.txt;0;")
