//===- aie.mlir ------------------------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (C) 2022, Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// REQUIRES: valid_xchess_license
// XFAIL: *
// RUN: aiecc.py -j4 --sysroot=%VITIS_SYSROOT% --host-target=aarch64-linux-gnu %s -I%host_runtime_lib%/  %extraAieCcFlags% %host_runtime_lib%/test_library.cpp %S/../test.cpp -o tutorial-1.exe
// RUN: %run_on_board ./tutorial-1.exe


// Declare this MLIR module. A block encapsulates all 
// AIE tiles, buffers, and communication in an AI Engine design
module @tutorial_1 {

    // Declare tile object of the AIE class located at position col 1, row 4
    %tile14 = AIE.tile(1, 4)
    %tile24 = AIE.tile(2, 4)

    // Declare buffer for tile(1, 4) with symbolic name "a14" and 
    // size 256 deep x int32 wide. By default, the address of 
    // this buffer begins after the stack (1024 Bytes offset) and 
    // all subsequent buffers are allocated one after another in memory.
    %buf = AIE.buffer(%tile14) { sym_name = "a14" } : memref<256xi32>
    %buf2 = AIE.buffer(%tile24) { sym_name = "a24" } : memref<8192xi32>

    // Define the algorithm for the core of tile(1, 4)
    // buf[3] = 14
    %core14 = AIE.core(%tile14) {
		%val = arith.constant 14 : i32 // declare a constant (int32)
		%idx = arith.constant 3 : index // declare a constant (index)
		memref.store %val, %buf2[%idx] : memref<8192xi32> // store val in buf[3]
        AIE.end
    }

}
