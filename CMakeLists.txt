#
# This file is licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# (c) Copyright 2021 Xilinx Inc.

# optional parameters
#  -DTOOLCHAINFILES_PATH: path to cross compilation toolchain files, default: <mlir-aie-root-dir>/cmake/toolchainFiles
#  -DLibXAIE_x86_64_DIR: search path for x86_64 libxaiengine
#  -DLibXAIE_aarch64_DIR: search path for aarch64 libxaiengine
#  -DSysrootAarch64: sysroot for aarch crosscompile, if not specified and aarch64 in AIE_RUNTIME_TARGETS then default to  Vitis aarch64 sysroot
#  -DAIE_RUNTIME_TARGETS: list of targets (x86_64,aarch64) to build runtime libs for, default: x86_64; cross compilation for aarch64 against default Vitis Sysroot
#  -DAIE_RUNTIME_TEST_TARGET: runtime test target (x86_64 or aarch64) used for running unit test and tutorials, default x86_64

cmake_minimum_required(VERSION 3.21)

set(TOOLCHAINFILES_PATH ${CMAKE_SOURCE_DIR}/cmake/modulesXilinx)

set(AIE_RUNTIME_TARGETS
    "x86_64"
    CACHE STRING "Architectures to compile the runtime libraries for.")
set(AIE_RUNTIME_TEST_TARGET
    "x86_64"
    CACHE STRING "Runtime architecture to test with.")

if(POLICY CMP0068)
  cmake_policy(SET CMP0068 NEW)
  set(CMAKE_BUILD_WITH_INSTALL_NAME_DIR ON)
endif()

if(POLICY CMP0075)
  cmake_policy(SET CMP0075 NEW)
endif()

if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
endif()

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

if(POLICY CMP0116)
  cmake_policy(SET CMP0116 OLD)
endif()

project(AIE LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

set(AIE_COMPILER
    "XCHESSCC"
    CACHE STRING "Backend compiler selection")
set(AIE_LINKER
    "XCHESSCC"
    CACHE STRING "Backend linker selection")
set(HOST_COMPILER
    "NONE"
    CACHE STRING "Host program compiler selection")
set(PEANO_INSTALL_DIR
    "<unset>"
    CACHE STRING "Location of Peano compiler")

option(LLVM_INCLUDE_TOOLS "Generate build targets for the LLVM tools." ON)
option(LLVM_BUILD_TOOLS
       "Build the LLVM tools. If OFF, just generate build targets." ON)
option(AIE_INCLUDE_INTEGRATION_TESTS
       "Generate build targets for the mlir-aie integration tests." OFF)

find_package(MLIR REQUIRED CONFIG)

message(STATUS "Using MLIRConfig.cmake in: ${MLIR_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# These are a bit of a hack, because we're hijacking alot of LLVM machinery
set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/lib)
set(MLIR_BINARY_DIR ${CMAKE_BINARY_DIR})

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(AIE_TOOLS_BINARY_DIR ${AIE_BINARY_DIR}/bin)

find_package(Vitis 2023.2 COMPONENTS AIE AIE2)
configure_file(./utils/vitisVariables.config.in
               ${CMAKE_BINARY_DIR}/utils/vitisVariables.config @ONLY)

# Set up default Vitis Sysroot as sysroot when testing on aarch64
list(FIND AIE_RUNTIME_TARGETS "aarch64" indexAarch64)
if(NOT DEFINED SysrootAarch64
   AND DEFINED VITIS_ROOT
   AND ${indexAarch64} GREATER -1)
  set(Sysroot ${VITIS_ROOT}/gnu/aarch64/lin/aarch64-linux/aarch64-xilinx-linux/)
  if(AIE_RUNTIME_TEST_TARGET STREQUAL "aarch64")
    set(LIBCXX_VERSION "11.2.0")
    set(extraAieCcFlags
        "-I${CMAKE_INSTALL_PREFIX}/runtime_lib/aarch64/xaiengine/include -I${Sysroot}/usr/include -I${Sysroot}/usr/include/c++/${LIBCXX_VERSION} -I${Sysroot}/usr/include/c++/${LIBCXX_VERSION}/aarch64-xilinx-linux -L${Sysroot}/usr/lib/aarch64-xilinx-linux/${LIBCXX_VERSION} -L${CMAKE_INSTALL_PREFIX}/runtime_lib/aarch64/xaiengine/lib -B${Sysroot}/usr/lib/aarch64-xilinx-linux/${LIBCXX_VERSION}"
    )
  endif()
  message(
    STATUS
      "aarch64 specified as runtime target but no sysroot set, using default Vitis Sysroot for cross compilation: ${Sysroot}"
  )
elseif(SysrootAarch64 AND ${indexAarch64} GREATER -1)
  set(Sysroot ${SysrootAarch64})
endif()

# Define the default arguments to use with 'lit', and an option for the user to
# override.
set(LIT_ARGS_DEFAULT "-j 8 -sv")
if(MSVC_IDE OR XCODE)
  set(LIT_ARGS_DEFAULT "${LIT_ARGS_DEFAULT} --no-progress-bar")
endif()
set(LLVM_LIT_ARGS
    "${LIT_ARGS_DEFAULT}"
    CACHE STRING "Default options for lit")

list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)
include(ExternalProject)

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${MLIR_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_BINARY_DIR}/include)
add_definitions(${LLVM_DEFINITIONS})

# Silence a false positive GCC -Wunused-but-set-parameter warning in constexpr
# cases, by marking SelectedCase as used. See
# https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85827 for details. The issue is
# fixed in GCC 10.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "10.0")
  check_cxx_compiler_flag("-Wno-unused-but-set-parameter" CXX_SUPPORTS_WNO_UNUSED_BUT_SET_PARAMETER)
  append_if(CXX_SUPPORTS_WNO_UNUSED_BUT_SET_PARAMETER "-Wno-unused-but-set-parameter" CMAKE_CXX_FLAGS)
endif()

# Make sure we generate the headers
add_custom_target(aie-headers)
set_target_properties(aie-headers PROPERTIES FOLDER "Misc")
add_dependencies(aie-headers mlir-headers)

option(AIE_INCLUDE_DOCS "Generate build targets for the MLIR AIE docs." OFF)
if(AIE_INCLUDE_DOCS)
  # Make sure we build the docs
  add_custom_target(docs ALL)
  add_subdirectory(docs)
  add_dependencies(docs mlir-doc)
endif()

# setup python
include(MLIRDetectPythonEnv)
mlir_configure_python_dev_packages()

# python install directory
if(NOT AIE_PYTHON_PACKAGES_DIR)
  set(AIE_PYTHON_PACKAGES_DIR "${CMAKE_CURRENT_BINARY_DIR}/python")
endif()

# python install directory
if(NOT AIE_PYTHON_INSTALL_DIR)
  set(AIE_PYTHON_INSTALL_DIR "python")
endif()

add_subdirectory(include)
add_subdirectory(lib)
add_subdirectory(python)
add_subdirectory(runtime_lib)
add_subdirectory(aie_runtime_lib)
add_subdirectory(reference_designs)
add_subdirectory(test)
add_subdirectory(tutorials)
add_subdirectory(tools)
add_subdirectory(data)

if(NOT LLVM_INSTALL_TOOLCHAIN_ONLY)
  install(
    DIRECTORY include/aie include/aie-c
    DESTINATION include
    COMPONENT aie-headers
    FILES_MATCHING
    PATTERN "*.def"
    PATTERN "*.h"
    PATTERN "*.inc"
    PATTERN "*.td"
    PATTERN "LICENSE.TXT")

  install(
    DIRECTORY ${PROJECT_BINARY_DIR}/include/aie
    DESTINATION include
    COMPONENT aie-headers
    FILES_MATCHING
    PATTERN "*.def"
    PATTERN "*.h"
    PATTERN "*.gen"
    PATTERN "*.inc"
    PATTERN "*.td"
    PATTERN "CMakeFiles" EXCLUDE
    PATTERN "config.h" EXCLUDE)

  if(NOT LLVM_ENABLE_IDE)
    add_llvm_install_targets(install-aie-headers DEPENDS aie-headers COMPONENT
                             aie-headers)
  endif()
endif()

add_subdirectory(cmake/modules)
