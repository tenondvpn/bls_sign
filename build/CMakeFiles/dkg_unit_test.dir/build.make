# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.25

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Disable VCS-based implicit rules.
% : %,v

# Disable VCS-based implicit rules.
% : RCS/%

# Disable VCS-based implicit rules.
% : RCS/%,v

# Disable VCS-based implicit rules.
% : SCCS/s.%

# Disable VCS-based implicit rules.
% : s.%

.SUFFIXES: .hpux_make_needs_suffix_list

# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /root/tools/cmake-3.25.1-linux-x86_64/bin/cmake

# The command to remove a file.
RM = /root/tools/cmake-3.25.1-linux-x86_64/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /root/zjchain/third_party/libBLS

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /root/zjchain/third_party/libBLS/build

# Include any dependencies generated for this target.
include CMakeFiles/dkg_unit_test.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include CMakeFiles/dkg_unit_test.dir/compiler_depend.make

# Include the progress variables for this target.
include CMakeFiles/dkg_unit_test.dir/progress.make

# Include the compile flags for this target's objects.
include CMakeFiles/dkg_unit_test.dir/flags.make

CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o: CMakeFiles/dkg_unit_test.dir/flags.make
CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o: /root/zjchain/third_party/libBLS/test/unit_tests_dkg.cpp
CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o: CMakeFiles/dkg_unit_test.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/root/zjchain/third_party/libBLS/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o"
	/usr/local/gcc-8.3.0/bin/g++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o -MF CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o.d -o CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o -c /root/zjchain/third_party/libBLS/test/unit_tests_dkg.cpp

CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.i"
	/usr/local/gcc-8.3.0/bin/g++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /root/zjchain/third_party/libBLS/test/unit_tests_dkg.cpp > CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.i

CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.s"
	/usr/local/gcc-8.3.0/bin/g++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /root/zjchain/third_party/libBLS/test/unit_tests_dkg.cpp -o CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.s

# Object files for target dkg_unit_test
dkg_unit_test_OBJECTS = \
"CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o"

# External object files for target dkg_unit_test
dkg_unit_test_EXTERNAL_OBJECTS =

dkg_unit_test: CMakeFiles/dkg_unit_test.dir/test/unit_tests_dkg.cpp.o
dkg_unit_test: CMakeFiles/dkg_unit_test.dir/build.make
dkg_unit_test: libbls.a
dkg_unit_test: /root/zjchain/third_party/libBLS/deps/deps_inst/x86_or_x64/lib/libcrypto.so
dkg_unit_test: /root/zjchain/third_party/libBLS/deps/deps_inst/x86_or_x64/lib/libgmp.a
dkg_unit_test: /root/zjchain/third_party/libBLS/deps/deps_inst/x86_or_x64/lib/libgmpxx.a
dkg_unit_test: CMakeFiles/dkg_unit_test.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/root/zjchain/third_party/libBLS/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Linking CXX executable dkg_unit_test"
	$(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/dkg_unit_test.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
CMakeFiles/dkg_unit_test.dir/build: dkg_unit_test
.PHONY : CMakeFiles/dkg_unit_test.dir/build

CMakeFiles/dkg_unit_test.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/dkg_unit_test.dir/cmake_clean.cmake
.PHONY : CMakeFiles/dkg_unit_test.dir/clean

CMakeFiles/dkg_unit_test.dir/depend:
	cd /root/zjchain/third_party/libBLS/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /root/zjchain/third_party/libBLS /root/zjchain/third_party/libBLS /root/zjchain/third_party/libBLS/build /root/zjchain/third_party/libBLS/build /root/zjchain/third_party/libBLS/build/CMakeFiles/dkg_unit_test.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/dkg_unit_test.dir/depend

