//===- aiert_insts.mlir -----------------------------------------*- MLIR -*-===//
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aie-opt --aie-dma-to-ipu %s | FileCheck %s
// CHECK: AIEX.ipu.writebd_shimtile {bd_id = 1 : i32, buffer_length = 32 : i32, buffer_offset = 0 : i32, column = 0 : i32, column_num = 1 : i32, d0_stepsize = 0 : i32, d0_wrap = 0 : i32, d1_stepsize = 0 : i32, d1_wrap = 0 : i32, d2_stepsize = 0 : i32, ddr_id = 2 : i32, enable_packet = 0 : i32, iteration_current = 0 : i32, iteration_stepsize = 0 : i32, iteration_wrap = 0 : i32, lock_acq_enable = 0 : i32, lock_acq_id = 0 : i32, lock_acq_val = 0 : i32, lock_rel_id = 0 : i32, lock_rel_val = 0 : i32, next_bd = 0 : i32, out_of_order_id = 0 : i32, packet_id = 0 : i32, packet_type = 0 : i32, use_next_bd = 0 : i32, valid_bd = 1 : i32}
// CHECK: AIEX.ipu.write32 {address = 119300 : ui32, column = 0 : i32, row = 0 : i32, value = 2147483649 : ui32}
// CHECK: AIEX.ipu.writebd_shimtile {bd_id = 0 : i32, buffer_length = 32 : i32, buffer_offset = 128 : i32, column = 0 : i32, column_num = 1 : i32, d0_stepsize = 0 : i32, d0_wrap = 8 : i32, d1_stepsize = 7 : i32, d1_wrap = 2 : i32, d2_stepsize = 15 : i32, ddr_id = 0 : i32, enable_packet = 0 : i32, iteration_current = 0 : i32, iteration_stepsize = 0 : i32, iteration_wrap = 0 : i32, lock_acq_enable = 0 : i32, lock_acq_id = 0 : i32, lock_acq_val = 0 : i32, lock_rel_id = 0 : i32, lock_rel_val = 0 : i32, next_bd = 0 : i32, out_of_order_id = 0 : i32, packet_id = 0 : i32, packet_type = 0 : i32, use_next_bd = 0 : i32, valid_bd = 1 : i32}
// CHECK: AIEX.ipu.write32 {address = 119316 : ui32, column = 0 : i32, row = 0 : i32, value = 0 : ui32}

module {
  AIE.device(ipu) {
    memref.global "public" @of_toMem : memref<32xi32>
    memref.global "public" @of_fromMem : memref<32xi32>
    func.func @sequence(%in : memref<4x2x8xi32>, %buf : memref<32xi32>, %out : memref<64xi32>) {
      %c0 = arith.constant 0 : i32
      %c1 = arith.constant 1 : i32
      %c2 = arith.constant 2 : i32
      %c4 = arith.constant 4 : i32
      %c8 = arith.constant 8 : i32
      %c16 = arith.constant 16 : i32
      %c32 = arith.constant 32 : i32
      AIEX.ipu.dma_memcpy_nd (%c0, %c0, %out[%c0,%c0,%c0,%c0][%c1,%c1,%c1,%c32][%c0,%c0,%c0]) { metadata = @of_toMem, id = 1 : i32 } : (i32, i32, memref<64xi32>, [i32,i32,i32,i32], [i32,i32,i32,i32], [i32,i32,i32])
      AIEX.ipu.dma_memcpy_nd (%c0, %c0, %in[%c0,%c2,%c0,%c0][%c1,%c2,%c2,%c8][%c0,%c16,%c8]) { metadata = @of_fromMem, id = 0 : i32 } : (i32, i32, memref<4x2x8xi32>, [i32,i32,i32,i32], [i32,i32,i32,i32], [i32,i32,i32])
      return
    }
    AIE.shimDMAAllocation @of_fromMem (MM2S, 0, 0)
    AIE.shimDMAAllocation @of_toMem (S2MM, 0, 0)
  }
}
