//===- packet_shim_header.mlir ---------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aie-translate --aie-generate-xaie %s | FileCheck %s

// CHECK: mlir_aie_configure_shimdma_70
// CHECK: XAie_DmaDesc [[bd0:.*]];
// CHECK: __mlir_aie_try(XAie_DmaSetPkt(&([[bd0]]), XAie_PacketInit(10,6)));

// CHECK: mlir_aie_configure_switchboxes
// CHECK: x = 7;
// CHECK: y = 0;
// CHECK: __mlir_aie_try(XAie_StrmPktSwMstrPortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), NORTH, 0, {{.*}} XAIE_SS_PKT_DONOT_DROP_HEADER, {{.*}} 0, {{.*}} 0x1));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlavePortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 3));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 3, {{.*}} 0, {{.*}} XAie_PacketInit(10,0), {{.*}} 0x1F, {{.*}} 0, {{.*}} 0));
// CHECK: x = 7;
// CHECK: y = 1;
// CHECK: __mlir_aie_try(XAie_StrmPktSwMstrPortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), DMA, 0, {{.*}} XAIE_SS_PKT_DROP_HEADER, {{.*}} 0, {{.*}} 0x1));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlavePortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 0, {{.*}} 0, {{.*}} XAie_PacketInit(10,0), {{.*}} 0x1F, {{.*}} 0, {{.*}} 0));
// CHECK: x = 7;
// CHECK: y = 0;
// CHECK: __mlir_aie_try(XAie_EnableShimDmaToAieStrmPort(&(ctx->DevInst), XAie_TileLoc(x,y), 3));

//
// This tests the shim DMA BD configuration lowering for packet switched routing
// to insert packet headers for shim DMA BDs.
//
module @aie_module {
  AIE.device(xcvc1902) {
    %0 = AIE.tile(7, 0)
    %1 = AIE.shimmux(%0) {
      AIE.connect<DMA : 0, North : 3>
    }
    %2 = AIE.switchbox(%0) {
      %10 = AIE.amsel<0> (0)
      %11 = AIE.masterset(North : 0, %10)
      AIE.packetrules(South : 3) {
        AIE.rule(31, 10, %10)
      }
    }
    %3 = AIE.tile(7, 1)
    %4 = AIE.switchbox(%3) {
      %10 = AIE.amsel<0> (0)
      %11 = AIE.masterset(DMA : 0, %10)
      AIE.packetrules(South : 0) {
        AIE.rule(31, 10, %10)
      }
    }
    %5 = AIE.lock(%3, 1)
    %6 = AIE.buffer(%3) {address = 3072 : i32, sym_name = "buf1"} : memref<32xi32, 2>
    %7 = AIE.external_buffer {sym_name = "buf"} : memref<32xi32>
    %8 = AIE.mem(%3) {
      %10 = AIE.dmaStart(S2MM, 0, ^bb1, ^bb2)
    ^bb1:  // 2 preds: ^bb0, ^bb1
      AIE.useLock(%5, Acquire, 0)
      AIE.dmaBd(<%6 : memref<32xi32, 2>, 0, 32>, 0)
      AIE.useLock(%5, Release, 1)
      AIE.nextBd ^bb1
    ^bb2:  // pred: ^bb0
      AIE.end
    }
    %9 = AIE.shimDMA(%0) {
      %10 = AIE.lock(%0, 1)
      %11 = AIE.dmaStart(MM2S, 0, ^bb1, ^bb2)
    ^bb1:  // 2 preds: ^bb0, ^bb1
      AIE.useLock(%10, Acquire, 1)
      AIE.dmaBdPacket(6, 10)
      AIE.dmaBd(<%7 : memref<32xi32>, 0, 32>, 0)
      AIE.useLock(%10, Release, 0)
      AIE.nextBd ^bb1
    ^bb2:  // pred: ^bb0
      AIE.end
    }
  }
}
