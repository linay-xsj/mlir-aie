//===- test_ps1_xaie.mlir --------------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aie-translate --aie-generate-xaie %s | FileCheck %s

// CHECK: mlir_aie_configure_switchboxes
// CHECK: x = 0;
// CHECK: y = 1;
// CHECK: __mlir_aie_try(XAie_StrmConnCctEnable(&(ctx->DevInst), XAie_TileLoc(x,y), DMA, 0, EAST, 0));
// CHECK: x = 1;
// CHECK: y = 1;
// CHECK: __mlir_aie_try(XAie_StrmPktSwMstrPortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), CORE, 0, {{.*}} XAIE_SS_PKT_DONOT_DROP_HEADER, {{.*}} 0, {{.*}} 0x1));
// CHECK: __mlir_aie_try(XAie_StrmPktSwMstrPortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), CORE, 1, {{.*}} XAIE_SS_PKT_DONOT_DROP_HEADER, {{.*}} 1, {{.*}} 0x1));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlavePortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), WEST, 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable(&(ctx->DevInst), XAie_TileLoc(x,y), WEST, 0, {{.*}} 0, {{.*}} XAie_PacketInit(0,0), {{.*}} 0x1F, {{.*}} 0, {{.*}} 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlavePortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), WEST, 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable(&(ctx->DevInst), XAie_TileLoc(x,y), WEST, 0, {{.*}} 1, {{.*}} XAie_PacketInit(1,0), {{.*}} 0x1F, {{.*}} 0, {{.*}} 1));

// one-to-many, multiple arbiter
module @test_ps1_xaie {
 AIE.device(xcvc1902) {
  %t01 = AIE.tile(0, 1)
  %t11 = AIE.tile(1, 1)

  AIE.switchbox(%t01) {
    AIE.connect<DMA : 0, East : 0>
  }

  AIE.switchbox(%t11) {
    %a0_0 = AIE.amsel<0>(0)
    %a1_0 = AIE.amsel<1>(0)

    AIE.masterset(Core : 0, %a0_0)
    AIE.masterset(Core : 1, %a1_0)

    AIE.packetrules(West : 0) {
      AIE.rule(0x1F, 0x0, %a0_0)
      AIE.rule(0x1F, 0x1, %a1_0)
    }
  }
 }
}

//module @test_ps1_logical {
//  %t01 = AIE.tile(0, 1)
//  %t11 = AIE.tile(1, 1)
//
//  AIE.packet_flow(0x0) {
//    AIE.packet_source<%t01, DMA : 0>
//    AIE.packet_dest<%t11, Core : 0>
//  }
//
//  AIE.packet_flow(0x1) {
//    AIE.packet_source<%t01, DMA : 0>
//    AIE.packet_dest<%t11, Core : 1>
//  }
//}
