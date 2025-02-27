//===- AIEDmaToIpu.cpp ------------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

#include "aie/Dialect/AIEX/IR/AIEXDialect.h"
#include "aie/Dialect/AIEX/Transforms/AIEXPasses.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

using namespace mlir;
using namespace xilinx;
using namespace xilinx::AIEX;

struct RtpToIpuPattern : public OpConversionPattern<IpuWriteRTPOp> {
  using OpConversionPattern<IpuWriteRTPOp>::OpConversionPattern;

  RtpToIpuPattern(MLIRContext *context, PatternBenefit benefit = 1)
      : OpConversionPattern<IpuWriteRTPOp>(context, benefit) {}

  LogicalResult
  matchAndRewrite(IpuWriteRTPOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto ctx = op->getContext();
    auto i32ty = IntegerType::get(ctx, 32);
    auto zero = IntegerAttr::get(i32ty, 0);
    auto ui32ty =
        IntegerType::get(ctx, 32, IntegerType::SignednessSemantics::Unsigned);
    auto uzero = IntegerAttr::get(ui32ty, 0);

    auto device = op->getParentOfType<AIE::DeviceOp>();

    // initialize fields to zero
    auto column = zero;
    auto row = zero;
    auto address = uzero;
    auto value = zero;

    uint32_t rtp_buffer_addr = UINT_MAX;
    int c = op.getCol();
    int r = op.getRow();
    uint32_t v = op.getValue();
    uint32_t idx = op.getIndex();

    if (AIE::BufferOp buffer =
            device.lookupSymbol<AIE::BufferOp>(op.getBufferSymName())) {
      AIE::TileOp tile = buffer.getTileOp();
      if ((tile.colIndex() == c) && (tile.rowIndex() == r)) {
        rtp_buffer_addr = uint32_t(buffer.address());
      }
    }

    if (rtp_buffer_addr == UINT_MAX) {
      return op.emitOpError("RTP buffer address cannot be found. Has an RTP "
                            "buffer been allocated?\n");
    }

    rtp_buffer_addr += idx * sizeof(uint32_t);

    // column
    column = IntegerAttr::get(i32ty, c);

    // row
    row = IntegerAttr::get(i32ty, r);

    // address
    address = IntegerAttr::get(ui32ty, rtp_buffer_addr);

    // value
    value = IntegerAttr::get(i32ty, v);

    rewriter.create<IpuWrite32Op>(op->getLoc(), column.getInt(), row.getInt(),
                                  address.getUInt(), value.getInt());

    rewriter.eraseOp(op);
    return success();
  }
};

std::optional<AIE::ShimDMAAllocationOp>
getAllocOpForSymbol(AIE::DeviceOp dev, StringRef sym_name) {
  auto sym = dev.lookupSymbol(sym_name);
  if (!sym)
    return std::nullopt;

  auto uses = SymbolTable::getSymbolUses(sym, dev);
  for (auto use : *uses)
    if (auto infoOp = dyn_cast<AIE::ShimDMAAllocationOp>(use.getUser()))
      return infoOp;

  return std::nullopt;
}

struct PushToIpuPattern : public OpConversionPattern<IpuShimTilePushQueueOp> {
  using OpConversionPattern<IpuShimTilePushQueueOp>::OpConversionPattern;

  PushToIpuPattern(MLIRContext *context, PatternBenefit benefit = 1)
      : OpConversionPattern<IpuShimTilePushQueueOp>(context, benefit) {}

  LogicalResult
  matchAndRewrite(IpuShimTilePushQueueOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto ctx = op->getContext();
    auto i32ty = IntegerType::get(ctx, 32);
    auto zero = IntegerAttr::get(i32ty, 0);
    auto ui32ty =
        IntegerType::get(ctx, 32, IntegerType::SignednessSemantics::Unsigned);
    auto uzero = IntegerAttr::get(ui32ty, 0);

    bool send_tct = op.getIssueToken();
    uint32_t channel_num = 0;

    // initialize fields to zero
    auto column = zero;
    auto row = zero;
    auto address = uzero;
    auto value = uzero;

    auto dev = op->getParentOfType<AIE::DeviceOp>();
    if (!dev)
      return failure();

    auto infoOp = getAllocOpForSymbol(dev, op.getMetadata());
    if (!infoOp)
      return failure();

    auto channelDir = infoOp->getChannelDir();
    bool isMM2S = channelDir == AIE::DMAChannelDir::MM2S;
    channel_num += infoOp->getChannelIndex();

    // column
    column = IntegerAttr::get(i32ty, infoOp->getCol());

    // address
    uint32_t queue_offset = 0;
    if (isMM2S)
      queue_offset = 0x1D214;
    else
      queue_offset = 0x1D204;
    if (channel_num == 1)
      queue_offset += 0x8;
    address = IntegerAttr::get(ui32ty, queue_offset);

    // value
    uint32_t bd_id = op.getBdId();
    uint32_t repeat_cnt = op.getRepeatCount();
    uint32_t cmd = 0;
    cmd |= (bd_id & 0xF);
    cmd |= ((repeat_cnt & 0xFF) << 16);
    if (send_tct)
      cmd |= 0x80000000;
    value = IntegerAttr::get(ui32ty, cmd);

    rewriter.create<IpuWrite32Op>(op->getLoc(), column.getInt(), row.getInt(),
                                  address.getUInt(), value.getUInt());

    rewriter.eraseOp(op);
    return success();
  }
};

struct DmaToIpuPattern : public OpConversionPattern<IpuDmaMemcpyNdOp> {
  using OpConversionPattern<IpuDmaMemcpyNdOp>::OpConversionPattern;

  DmaToIpuPattern(MLIRContext *context, PatternBenefit benefit = 1)
      : OpConversionPattern<IpuDmaMemcpyNdOp>(context, benefit) {}

  LogicalResult
  matchAndRewrite(IpuDmaMemcpyNdOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto ctx = op->getContext();
    auto i32ty = IntegerType::get(ctx, 32);
    auto zero = IntegerAttr::get(i32ty, 0);
    auto memref = adaptor.getMemref();

    auto dev = op->getParentOfType<AIE::DeviceOp>();
    if (!dev)
      return failure();

    auto infoOp = getAllocOpForSymbol(dev, op.getMetadata());
    if (!infoOp)
      return failure();

    auto channelDir = infoOp->getChannelDir();
    bool isMM2S = channelDir == AIE::DMAChannelDir::MM2S;
    int col = infoOp->getCol();

    // initialize fields to zero
    auto column = zero;
    auto column_num = zero;
    auto ddr_id = zero;
    auto bd_id = zero;
    auto buffer_length = zero;
    auto buffer_offset = zero;
    auto enable_packet = zero;
    auto out_of_order_id = zero;
    auto packet_id = zero;
    auto packet_type = zero;
    auto d0_wrap = zero;
    auto d0_stepsize = zero;
    auto d1_wrap = zero;
    auto d1_stepsize = zero;
    auto d2_stepsize = zero;
    auto iteration_current = zero;
    auto iteration_wrap = zero;
    auto iteration_stepsize = zero;
    auto next_bd = zero;
    auto use_next_bd = zero;
    auto valid_bd = zero;
    auto lock_rel_val = zero;
    auto lock_rel_id = zero;
    auto lock_acq_enable = zero;
    auto lock_acq_val = zero;
    auto lock_acq_id = zero;

    auto issue_token = BoolAttr::get(ctx, false);
    auto repeat_count = zero;

    SmallVector<uint32_t, 4> offsets(4, 0);
    SmallVector<uint32_t, 4> lengths(4, 1);
    SmallVector<uint32_t, 3> strides(3, 0);

    if (auto c = op.getOffset0().getDefiningOp<arith::ConstantIntOp>())
      offsets[0] = static_cast<uint32_t>(c.value());
    if (auto c = op.getOffset1().getDefiningOp<arith::ConstantIntOp>())
      offsets[1] = static_cast<uint32_t>(c.value());
    if (auto c = op.getOffset2().getDefiningOp<arith::ConstantIntOp>())
      offsets[2] = static_cast<uint32_t>(c.value());
    if (auto c = op.getOffset3().getDefiningOp<arith::ConstantIntOp>())
      offsets[3] = static_cast<uint32_t>(c.value());
    if (auto c = op.getLength0().getDefiningOp<arith::ConstantIntOp>())
      lengths[0] = static_cast<uint32_t>(c.value());
    if (auto c = op.getLength1().getDefiningOp<arith::ConstantIntOp>())
      lengths[1] = static_cast<uint32_t>(c.value());
    if (auto c = op.getLength2().getDefiningOp<arith::ConstantIntOp>())
      lengths[2] = static_cast<uint32_t>(c.value());
    if (auto c = op.getLength3().getDefiningOp<arith::ConstantIntOp>())
      lengths[3] = static_cast<uint32_t>(c.value());
    if (auto c = op.getStride1().getDefiningOp<arith::ConstantIntOp>())
      strides[0] = static_cast<uint32_t>(c.value());
    if (auto c = op.getStride2().getDefiningOp<arith::ConstantIntOp>())
      strides[1] = static_cast<uint32_t>(c.value());
    if (auto c = op.getStride3().getDefiningOp<arith::ConstantIntOp>())
      strides[2] = static_cast<uint32_t>(c.value());

    // column
    column = IntegerAttr::get(i32ty, col);

    // column_num
    column_num = IntegerAttr::get(i32ty, 1);

    // ddr_id
    Block &entryBB = op->getParentOfType<func::FuncOp>().getBody().front();
    int arg_idx = -1;
    for (int i = 0, e = entryBB.getNumArguments(); i < e; i++) {
      if (entryBB.getArgument(i) == memref) {
        arg_idx = i;
        break;
      }
    }
    if (arg_idx < 0)
      return failure();
    ddr_id = IntegerAttr::get(i32ty, arg_idx);

    // bd_id
    bd_id = IntegerAttr::get(i32ty, op.getId());

    // buffer_length
    uint32_t repeat_length = 0;
    for (uint32_t index_3d = 0; index_3d < lengths[2]; index_3d++)
      for (uint32_t index_2d = 0; index_2d < lengths[1]; index_2d++)
        repeat_length += lengths[0];
    buffer_length = IntegerAttr::get(i32ty, repeat_length);

    // buffer_offset
    size_t stride = 1;
    size_t offset = 0;
    MemRefType my_memref = op.getMemref().getType();
    auto shape = my_memref.getShape();
    size_t R = shape.size();
    size_t S = my_memref.getElementType().getIntOrFloatBitWidth() / 8;
    for (size_t i = 0; i < R; i++) {
      offset += offsets[i] * stride * S;
      stride *= shape[R - i - 1];
    }
    buffer_offset = IntegerAttr::get(i32ty, offset);

    // enable_packet

    // out_of_order_id

    // packet_id

    // packet_type

    // d0_wrap
    if (strides[0])
      d0_wrap = IntegerAttr::get(i32ty, lengths[0]);

    // d0_stepsize
    d0_stepsize = IntegerAttr::get(i32ty, 0);

    // d1_wrap
    if (strides[1])
      d1_wrap = IntegerAttr::get(i32ty, lengths[1]);

    // d1_stepsize
    if (strides[0])
      d1_stepsize = IntegerAttr::get(i32ty, strides[0] - 1);

    // d2_stepsize
    if (strides[1])
      d2_stepsize = IntegerAttr::get(i32ty, strides[1] - 1);

    // iteration_current

    // iteration_wrap
    if (strides[2])
      iteration_wrap = IntegerAttr::get(i32ty, lengths[3] - 1);

    // iteration_stepsize
    if (strides[2])
      iteration_stepsize = IntegerAttr::get(i32ty, strides[2] - 1);

    //// next_bd
    // if (lengths[3] > 1)
    //  next_bd = IntegerAttr::get(i32ty, op.getId());

    //// use_next_bd
    // if (lengths[3] > 1)
    //  use_next_bd = IntegerAttr::get(i32ty, 1);

    // valid_bd
    valid_bd = IntegerAttr::get(i32ty, 1);

    // lock_rel_val

    // lock_rel_id

    // lock_acq_enable

    // lock_acq_val

    // lock_acq_id

    // repeat_count
    repeat_count = IntegerAttr::get(i32ty, lengths[3] - 1);

    // issue_token
    if (!isMM2S)
      issue_token = BoolAttr::get(ctx, true);

    auto new_op = rewriter.create<IpuWriteBdExShimTileOp>(
        op->getLoc(), column, column_num, ddr_id, bd_id, buffer_length,
        buffer_offset, enable_packet, out_of_order_id, packet_id, packet_type,
        d0_wrap, d0_stepsize, d1_wrap, d1_stepsize, d2_stepsize,
        iteration_current, iteration_wrap, iteration_stepsize, next_bd,
        use_next_bd, valid_bd, lock_rel_val, lock_rel_id, lock_acq_enable,
        lock_acq_val, lock_acq_id);

    rewriter.create<IpuShimTilePushQueueOp>(op->getLoc(), op.getMetadataAttr(),
                                            issue_token, repeat_count, bd_id);

    rewriter.eraseOp(op);
    return success();
  }
};

struct AIEDmaToIpuPass : public AIEDmaToIpuBase<AIEDmaToIpuPass> {
  void runOnOperation() override {

    AIE::DeviceOp device = getOperation();

    ConversionTarget target(getContext());
    target.addLegalDialect<AIEXDialect>();
    target.addLegalOp<AIE::BufferOp>();
    target.addLegalOp<AIE::ShimDMAAllocationOp>();
    target.addIllegalOp<IpuWriteRTPOp>();
    target.addIllegalOp<IpuDmaMemcpyNdOp>();
    target.addIllegalOp<IpuShimTilePushQueueOp>();

    RewritePatternSet patterns(&getContext());
    patterns.insert<DmaToIpuPattern>(&getContext());
    patterns.insert<PushToIpuPattern>(&getContext());
    patterns.insert<RtpToIpuPattern>(&getContext());

    if (failed(applyPartialConversion(device, target, std::move(patterns))))
      signalPassFailure();
  }
};

std::unique_ptr<OperationPass<AIE::DeviceOp>>
xilinx::AIEX::createAIEDmaToIpuPass() {
  return std::make_unique<AIEDmaToIpuPass>();
}
