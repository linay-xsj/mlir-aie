//===-VectorToAIEVecConversions.cpp - Vector to AIEVec convs. ---*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023, Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//
// This file contains conversions from the Vector dialect into the AIEVec
// dialect. Conversions assume that the Vector dialect has been rectricted
// to ops that can be translated to a sequence of valid AIEVec ops.
//===----------------------------------------------------------------------===//
#include "VectorToAIEVecConversions.h"

#include "aie/Dialect/AIEVec/AIEVecUtils.h"
#include "aie/Dialect/AIEVec/IR/AIEVecOps.h"
#include "aie/Dialect/AIEVec/Pipelines/Passes.h"

#include "aie/Dialect/AIEVec/Utils/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/EmitC/IR/EmitC.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/TypeSwitch.h"

#include <bitset>
#include <optional>
#include <tuple>

#define DEBUG_TYPE "lower-vector-to-aievec"

using namespace mlir;
using namespace arith;
using namespace vector;
using namespace xilinx;
using namespace xilinx::aievec;

//===----------------------------------------------------------------------===//
// Utility functions
//===----------------------------------------------------------------------===//

// Given the LHS and RHS of an `arith::AddIOp`, if one of them is defined by an
// `arith::MulIOp`, return a tuple with the `lhs`, `rhs`, and `acc` of the MAC
// operation that can replace them.
static std::optional<std::tuple<Value, Value, Value>>
extractMACOperandsFromAddOperands(Value addLhs, Value addRhs) {
  auto lhsDefOp = addLhs.getDefiningOp();
  auto rhsDefOp = addRhs.getDefiningOp();
  arith::MulIOp mulOp = nullptr;
  Value acc;
  if (lhsDefOp) {
    mulOp = dyn_cast<arith::MulIOp>(lhsDefOp);
    acc = addRhs;
  }
  if (!mulOp && rhsDefOp) {
    mulOp = dyn_cast<arith::MulIOp>(rhsDefOp);
    acc = addLhs;
  }
  if (mulOp)
    return std::make_tuple(mulOp.getLhs(), mulOp.getRhs(), acc);

  // If the MulIOp has been already translated to aievec::MulOp:
  aievec::SRSOp lhsSrsOp = addLhs.getDefiningOp<aievec::SRSOp>();
  aievec::SRSOp rhsSrsOp = addRhs.getDefiningOp<aievec::SRSOp>();
  aievec::MulOp aieMulOp = nullptr;
  if (lhsSrsOp) {
    aieMulOp = lhsSrsOp.getSource().getDefiningOp<aievec::MulOp>();
    acc = addRhs;
  }
  if (!aieMulOp && rhsSrsOp) {
    aieMulOp = rhsSrsOp.getSource().getDefiningOp<aievec::MulOp>();
    acc = addLhs;
  }
  if (aieMulOp)
    return std::make_tuple(aieMulOp.getLhs(), aieMulOp.getRhs(), acc);
  return {};
}

// Convert a input value to a target vector type. This function can insert
// multiple aievec ops depending on the combination of input and output vector
// types.
static std::optional<Value>
convertValueToTargetTypeAieML(ConversionPatternRewriter &rewriter, Location loc,
                              Value inputVal, VectorType tgtType) {
  VectorType srcType = cast<VectorType>(inputVal.getType());
  auto srcElemType = srcType.getElementType();
  unsigned srcBitWidth = srcElemType.getIntOrFloatBitWidth();
  unsigned srcLaneSize = getVectorLaneSize(srcType);

  auto tgtElemType = tgtType.getElementType();
  unsigned tgtBitWidth = tgtElemType.getIntOrFloatBitWidth();
  unsigned tgtLaneSize = getVectorLaneSize(tgtType);

  if (srcType == tgtType)
    return inputVal;

  if ((srcElemType == tgtElemType) && (srcLaneSize != tgtLaneSize)) {
    // TODO: relax the condition below?
    if ((srcLaneSize == 16 && tgtLaneSize == 32 &&
         isa<FloatType>(srcElemType)) ||
        (srcLaneSize == 32 && tgtLaneSize == 64 &&
         isa<IntegerType>(srcElemType))) {
      auto zeroConstOp = rewriter.create<arith::ConstantOp>(
          loc, srcType.getElementType(),
          rewriter.getZeroAttr(srcType.getElementType()));
      auto broadcastZeroOp = rewriter.create<aievec::BroadcastScalarOp>(
          loc, tgtType, zeroConstOp->getResult(0));
      auto extOp = rewriter.create<aievec::ExtOp>(
          loc, srcType, broadcastZeroOp->getResult(0), 0);

      SmallVector<Value> inputSources = {inputVal, extOp->getResult(0)};
      aievec::ConcatOp concatOp =
          rewriter.create<aievec::ConcatOp>(loc, tgtType, inputSources);

      return concatOp.getResult();
    }
  } else if ((srcElemType != tgtElemType) && (srcLaneSize == tgtLaneSize) &&
             isa<IntegerType>(srcElemType) && isa<IntegerType>(tgtElemType)) {
    if (srcBitWidth == 16 && tgtBitWidth == 32 && srcLaneSize == 16) {
      // Case 1: vector<16xi16> to vector<16xi32> conversion by aievec.ups +
      // aievec.cast
      auto accType = getVectorOpDestType(srcType, /*AIEML =*/true);
      auto upsOp = rewriter.create<aievec::UPSOp>(loc, accType, inputVal);
      auto castOp = rewriter.create<aievec::CastOp>(
          loc, tgtType, upsOp.getResult(), /*isResAcc*/ false);
      return castOp.getResult();
    } else if (srcBitWidth == 8 && tgtBitWidth == 32 && srcLaneSize == 16) {
      // Case 2: vector<16xi8> to vector<16xi32> conversion by aievec.concat +
      // aievec.ups + aievec.cast + aievec.ext
      auto concatOutType = createVectorType(32, srcElemType);
      auto concatOp = rewriter.create<aievec::ConcatOp>(
          loc, concatOutType, SmallVector<Value>({inputVal, inputVal}));
      auto accType = getVectorOpDestType(concatOutType, /*AIEML =*/true);
      auto upsOp =
          rewriter.create<aievec::UPSOp>(loc, accType, concatOp.getResult());
      auto castType = createVectorType(32, tgtElemType);
      auto castOp = rewriter.create<aievec::CastOp>(
          loc, castType, upsOp.getResult(), /*isResAcc*/ false);
      auto extOp =
          rewriter.create<aievec::ExtOp>(loc, tgtType, castOp.getResult(), 0);
      return extOp.getResult();
    } else if (srcBitWidth == 8 && tgtBitWidth == 16 && srcLaneSize == 32) {
      // Case 3: vector<32xi8> to vector<32xi16> conversion by aievec.unpack
      auto unpackOp = rewriter.create<aievec::UnpackOp>(loc, tgtType, inputVal);
      return unpackOp.getResult();
    }
  }

  return std::nullopt;
}

// Return the list of attributes that configure an `aievec.select` op to
// perform a rotation of the input vector by `rotation` number of elements.
// The attribute values depend on the vector type of the select operation.
static SmallVector<NamedAttribute>
buildAttributeListForRotationSelectOp(PatternRewriter &rewriter, VectorType vTy,
                                      int64_t rotation) {
  unsigned width = 0;
  auto elemTy = vTy.getElementType();
  auto intTy = dyn_cast<IntegerType>(elemTy);
  if (intTy)
    width = intTy.getWidth();
  StringAttr attr0 = rewriter.getStringAttr("0");
  StringAttr attr0x06040200 = rewriter.getStringAttr("0x06040200");
  StringAttr attr0x0e0c0a08 = rewriter.getStringAttr("0x0e0c0a08");
  StringAttr attr0x2103 = rewriter.getStringAttr("0x2103");
  StringAttr attr0x3210 = rewriter.getStringAttr("0x3210");
  StringAttr selectAttrName = rewriter.getStringAttr("select");
  StringAttr xoffsetsAttrName = rewriter.getStringAttr("xoffsets");
  StringAttr xoffsetsHiAttrName = rewriter.getStringAttr("xoffsets_hi");
  StringAttr xsquareAttrName = rewriter.getStringAttr("xsquare");
  StringAttr xstartAttrName = rewriter.getStringAttr("xstart");
  StringAttr yoffsetsAttrName = rewriter.getStringAttr("yoffsets");
  StringAttr yoffsetsHiAttrName = rewriter.getStringAttr("yoffsets_hi");
  StringAttr ysquareAttrName = rewriter.getStringAttr("ysquare");
  StringAttr ystartAttrName = rewriter.getStringAttr("ystart");

  switch (width) {
  case 16:
    if (rotation % 2) {
      int64_t xstart = rotation + 1;
      int64_t ystart = rotation - 1;
      return SmallVector<NamedAttribute, 9>(
          {{selectAttrName, rewriter.getStringAttr("0x11111111")},
           {xoffsetsAttrName, attr0x06040200},
           {xoffsetsHiAttrName, attr0x0e0c0a08},
           {xsquareAttrName, attr0x2103},
           {xstartAttrName, rewriter.getStringAttr(std::to_string(xstart))},
           {yoffsetsAttrName, rewriter.getStringAttr("0x0503010f")},
           {yoffsetsHiAttrName, rewriter.getStringAttr("0x0d0b0907")},
           {ysquareAttrName, attr0x2103},
           {ystartAttrName, rewriter.getStringAttr(std::to_string(ystart))}});
    } else {
      return SmallVector<NamedAttribute, 9>(
          {{selectAttrName, attr0},
           {xoffsetsAttrName, attr0x06040200},
           {xoffsetsHiAttrName, attr0x0e0c0a08},
           {xsquareAttrName, attr0x3210},
           {xstartAttrName, rewriter.getStringAttr(std::to_string(rotation))},
           {yoffsetsAttrName, attr0},
           {yoffsetsHiAttrName, attr0},
           {ysquareAttrName, attr0},
           {ystartAttrName, attr0}});
    }
    break;
  case 32:
    return SmallVector<NamedAttribute, 7>(
        {{selectAttrName, attr0},
         {xoffsetsAttrName, rewriter.getStringAttr("0x76543210")},
         {xsquareAttrName, attr0x3210},
         {xstartAttrName, rewriter.getStringAttr(std::to_string(rotation))},
         {yoffsetsAttrName, attr0},
         {ysquareAttrName, attr0},
         {ystartAttrName, attr0}});
  }
  return {};
}

namespace xilinx {
namespace aievec {

SmallVector<NamedAttribute> buildFMAOpSplatAttrForElemTy(aievec::FMAOp fmaOp,
                                                         int64_t bcastPos,
                                                         int64_t step = 1) {
  unsigned width = 0;
  auto elemTy = fmaOp.getLhs().getType().getElementType();
  auto intTy = dyn_cast<IntegerType>(elemTy);
  if (intTy)
    width = intTy.getWidth();
  auto ctx = fmaOp.getContext();
  switch (width) {
  case 16:
    // NOTE: The pattern is:
    //       acc[0]  = x[0]  * z[bcastPos] + x[16] * z[bcastPos+step]
    //       acc[1]  = x[1]  * z[bcastPos] + x[17] * z[bcastPos+step]
    //       acc[2]  = x[2]  * z[bcastPos] + x[18] * z[bcastPos+step]
    //       acc[3]  = x[3]  * z[bcastPos] + x[19] * z[bcastPos+step]
    //       acc[4]  = x[4]  * z[bcastPos] + x[20] * z[bcastPos+step]
    //       acc[5]  = x[5]  * z[bcastPos] + x[21] * z[bcastPos+step]
    //       acc[6]  = x[6]  * z[bcastPos] + x[22] * z[bcastPos+step]
    //       acc[7]  = x[7]  * z[bcastPos] + x[23] * z[bcastPos+step]
    //       acc[8]  = x[8]  * z[bcastPos] + x[24] * z[bcastPos+step]
    //       acc[9]  = x[9]  * z[bcastPos] + x[25] * z[bcastPos+step]
    //       acc[10] = x[10] * z[bcastPos] + x[26] * z[bcastPos+step]
    //       acc[11] = x[11] * z[bcastPos] + x[27] * z[bcastPos+step]
    //       acc[12] = x[12] * z[bcastPos] + x[28] * z[bcastPos+step]
    //       acc[13] = x[13] * z[bcastPos] + x[29] * z[bcastPos+step]
    //       acc[14] = x[14] * z[bcastPos] + x[30] * z[bcastPos+step]
    //       acc[15] = x[15] * z[bcastPos] + x[31] * z[bcastPos+step]
    return SmallVector<NamedAttribute, 11>(
        {{fmaOp.getXstartAttrName(), StringAttr::get(ctx, "0")},
         {fmaOp.getXoffsetsAttrName(), StringAttr::get(ctx, "0x73727170")},
         {fmaOp.getXoffsetsHiAttrName(), StringAttr::get(ctx, "0x77767574")},
         {fmaOp.getXstepAttrName(), fmaOp.getXstepAttr()},
         {fmaOp.getXsquareAttrName(), StringAttr::get(ctx, "0x3120")},
         {fmaOp.getZstartAttrName(),
          StringAttr::get(ctx, std::to_string(bcastPos))},
         {fmaOp.getZoffsetsAttrName(), StringAttr::get(ctx, "0")},
         {fmaOp.getZoffsetsHiAttrName(), StringAttr::get(ctx, "0")},
         {fmaOp.getZstepAttrName(), StringAttr::get(ctx, std::to_string(step))},
         {fmaOp.getZsquareAttrName(), fmaOp.getZsquareAttr()},
         {fmaOp.getFmsubAttrName(), fmaOp.getFmsubAttr()}});
  case 32:
    return SmallVector<NamedAttribute, 11>(
        {{fmaOp.getXstartAttrName(), StringAttr::get(ctx, "0")},
         {fmaOp.getXoffsetsAttrName(), StringAttr::get(ctx, "0x76543210")},
         {fmaOp.getXoffsetsHiAttrName(), fmaOp.getXoffsetsHiAttr()},
         {fmaOp.getXstepAttrName(), fmaOp.getXstepAttr()},
         {fmaOp.getXsquareAttrName(), fmaOp.getXsquareAttr()},
         {fmaOp.getZstartAttrName(),
          StringAttr::get(ctx, std::to_string(bcastPos))},
         {fmaOp.getZoffsetsAttrName(), StringAttr::get(ctx, "0x00000000")},
         {fmaOp.getZoffsetsHiAttrName(), fmaOp.getZoffsetsHiAttr()},
         {fmaOp.getZstepAttrName(), fmaOp.getZstepAttr()},
         {fmaOp.getZsquareAttrName(), fmaOp.getZsquareAttr()},
         {fmaOp.getFmsubAttrName(), fmaOp.getFmsubAttr()}});
  }
  return {};
}

} // namespace aievec
} // namespace xilinx

template <typename SrcOpTy, typename AIEv2ElemOp>
static LogicalResult genAddElemAieML(ConversionPatternRewriter &rewriter,
                                     Value lval, Value rval, VectorType srcType,
                                     SrcOpTy srcOp) {
  auto lCastOp = rewriter.create<aievec::CastOp>(srcOp.getLoc(), srcType, lval,
                                                 /*isResAcc*/ true);
  auto rCastOp = rewriter.create<aievec::CastOp>(srcOp.getLoc(), srcType, rval,
                                                 /*isResAcc*/ true);
  auto elemOp = rewriter.create<AIEv2ElemOp>(
      srcOp.getLoc(), lCastOp->getResult(0).getType(), lCastOp->getResult(0),
      rCastOp->getResult(0));
  rewriter.replaceOpWithNewOp<aievec::CastOp>(
      srcOp, srcOp.getType(), elemOp.getResult(), /*isResAcc*/ false);
  return success();
}

static arith::CmpIPredicate
convertToIntegerPredicate(arith::CmpFPredicate pred) {
  switch (pred) {
  case CmpFPredicate::UEQ:
  case CmpFPredicate::OEQ:
    return CmpIPredicate::eq;
  case CmpFPredicate::UGT:
    return CmpIPredicate::ugt;
  case CmpFPredicate::OGT:
    return CmpIPredicate::sgt;
  case CmpFPredicate::UGE:
    return CmpIPredicate::uge;
  case CmpFPredicate::OGE:
    return CmpIPredicate::sge;
  case CmpFPredicate::ULT:
    return CmpIPredicate::ult;
  case CmpFPredicate::OLT:
    return CmpIPredicate::slt;
  case CmpFPredicate::ULE:
    return CmpIPredicate::ule;
  case CmpFPredicate::OLE:
    return CmpIPredicate::sle;
  case CmpFPredicate::UNE:
  case CmpFPredicate::ONE:
    return CmpIPredicate::ne;
  default:
    llvm_unreachable("Unexpected predicate!");
  }
}

static arith::CmpIPredicate
convertToIntegerPredicate(arith::CmpIPredicate pred) {
  return pred;
}

static aievec::CmpOp createCmpOpAieML(ConversionPatternRewriter &rewriter,
                                      CmpIPredicate pred, Location loc,
                                      Type type, Value lhs, Value rhs) {
  switch (pred) {
  case CmpIPredicate::eq:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "eq");
  case CmpIPredicate::ne:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "ne");
  case CmpIPredicate::slt:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "slt");
  case CmpIPredicate::ult:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "ult");
  case CmpIPredicate::sle:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "sle");
  case CmpIPredicate::ule:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "ule");
  case CmpIPredicate::sgt:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "sgt");
  case CmpIPredicate::ugt:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "ugt");
  case CmpIPredicate::sge:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "sge");
  case CmpIPredicate::uge:
    return rewriter.create<aievec::CmpOp>(loc, type, lhs, rhs, "uge");
  }
  return nullptr;
}

template <typename DstOpTy>
static void generateAIEVecOpsForReductionOp(ConversionPatternRewriter &rewriter,
                                            vector::ReductionOp srcOp,
                                            int shiftIndex, Value curValue) {
  assert(shiftIndex > 0 && (shiftIndex & (shiftIndex - 1)) == 0 &&
         "shiftIndex must be power of 2");

  Location loc = srcOp.getLoc();
  VectorType vType = dyn_cast<VectorType>(curValue.getType());
  Type scalarType = vType.getElementType();
  SmallVector<Value> sources = {curValue};
  Type vecType = curValue.getType();
  DstOpTy curOp = nullptr;
  unsigned elWidth = scalarType.getIntOrFloatBitWidth();

  for (int id = shiftIndex; id > 0; id /= 2) {
    arith::ConstantOp constOp = rewriter.create<arith::ConstantOp>(
        loc, rewriter.getI32IntegerAttr(id * elWidth / 8));

    auto shiftBytesOp = rewriter.create<aievec::ShiftOp>(
        loc, vecType, curValue, curValue, constOp.getResult());

    curOp = rewriter.create<DstOpTy>(loc, vecType, curValue,
                                     shiftBytesOp.getResult());

    curValue = curOp.getResult();
  }

  arith::ConstantOp zeroConstOp =
      rewriter.create<arith::ConstantOp>(loc, rewriter.getI32IntegerAttr(0));
  rewriter.replaceOpWithNewOp<aievec::ExtElemOp>(srcOp, scalarType, curOp,
                                                 zeroConstOp.getResult());
  return;
}

//===----------------------------------------------------------------------===//
// Rewrite patterns
//===----------------------------------------------------------------------===//

// This pattern fold `vector.extract` and `vector.broadcast` into
// `aievec.broadcast` for aie-ml
struct FoldVectorExtractAndBroadcastToAIEBroadcast
    : public OpConversionPattern<vector::BroadcastOp> {
  using OpConversionPattern<vector::BroadcastOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::BroadcastOp bcastOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    auto extOp =
        dyn_cast<vector::ExtractOp>(adaptor.getSource().getDefiningOp());

    if (!extOp)
      return failure();

    auto src = extOp.getVector();
    auto pos = extOp.getStaticPosition();
    int64_t posVal = pos[0];
    VectorType srcVecType = cast<VectorType>(src.getType());
    VectorType resultType = cast<VectorType>(bcastOp.getResult().getType());
    if (srcVecType != resultType) {
      if (srcVecType.getNumElements() != 2 * resultType.getNumElements())
        return failure();
      int8_t half = static_cast<int8_t>(posVal / resultType.getNumElements());
      posVal -= half * resultType.getNumElements();
      src = rewriter
                .create<aievec::ExtOp>(extOp.getLoc(), resultType, src,
                                       rewriter.getI8IntegerAttr(half))
                .getResult();
    }

    unsigned elWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    if (laneSize * elWidth == 512) {
      // Common use case for the broadcast_elem intrinsic
      rewriter.replaceOpWithNewOp<aievec::BroadcastOp>(bcastOp, resultType, src,
                                                       posVal);
    } else if (laneSize * elWidth == 256) {
      // e.g. need v16bf16 due to the subsequent v16accfloat operation
      VectorType aievecBcastType =
          createVectorType(512 / elWidth, resultType.getElementType());
      auto concatOp = rewriter.create<aievec::ConcatOp>(
          bcastOp.getLoc(), aievecBcastType, SmallVector<Value>({src, src}));
      auto aieBcastOp = rewriter.create<aievec::BroadcastOp>(
          bcastOp.getLoc(), aievecBcastType, concatOp.getResult(), posVal);
      rewriter.replaceOpWithNewOp<aievec::ExtOp>(bcastOp, resultType,
                                                 aieBcastOp.getResult(), 0);
    } else if (laneSize * elWidth == 1024) {
      // e.g. need v32int32 due to the subsequent v32acc32 operation
      VectorType aievecBcastType =
          createVectorType(512 / elWidth, resultType.getElementType());
      int8_t half = static_cast<int8_t>(posVal / resultType.getNumElements());
      posVal -= half * resultType.getNumElements();
      auto extOp =
          rewriter.create<aievec::ExtOp>(bcastOp.getLoc(), aievecBcastType, src,
                                         rewriter.getI8IntegerAttr(half));
      auto aieBcastOp = rewriter.create<aievec::BroadcastOp>(
          bcastOp.getLoc(), aievecBcastType, extOp.getResult(), posVal);
      rewriter.replaceOpWithNewOp<aievec::ConcatOp>(
          bcastOp, resultType,
          SmallVector<Value>({aieBcastOp.getResult(), aieBcastOp.getResult()}));
    } else {
      return failure();
    }

    return success();
  }
};

struct ConvertBroadcastToAIEBroadcast
    : public OpConversionPattern<vector::BroadcastOp> {
  using OpConversionPattern<vector::BroadcastOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::BroadcastOp bcastOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    if (auto extOp = adaptor.getSource().getDefiningOp<vector::ExtractOp>())
      return failure();

    // Only support broadcasting a single element for now
    if (!isa<IntegerType, IndexType, FloatType>(adaptor.getSource().getType()))
      return failure();

    VectorType resultType = cast<VectorType>(bcastOp.getResult().getType());
    Type scalarType = resultType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);
    auto src = bcastOp.getSource();

    if (laneSize * elWidth == 512) {
      rewriter.replaceOpWithNewOp<aievec::BroadcastScalarOp>(bcastOp,
                                                             resultType, src);
      return success();
    } else if (laneSize * elWidth == 256) {
      VectorType vecType = createVectorType(512 / elWidth, scalarType);
      auto aieBcastOp = rewriter.create<aievec::BroadcastScalarOp>(
          bcastOp.getLoc(), vecType, src);
      rewriter.replaceOpWithNewOp<aievec::ExtOp>(bcastOp, resultType,
                                                 aieBcastOp.getResult(), 0);
      return success();
    } else if (laneSize * elWidth == 1024) {
      VectorType vecType = createVectorType(512 / elWidth, scalarType);
      auto aieBcastOp = rewriter.create<aievec::BroadcastScalarOp>(
          bcastOp.getLoc(), vecType, src);
      rewriter.replaceOpWithNewOp<aievec::ConcatOp>(
          bcastOp, resultType,
          SmallVector<Value>({aieBcastOp.getResult(), aieBcastOp.getResult()}));
      return success();
    }

    return failure();
  }
};

// This pattern replaces `arith.muli`+`arith.addi` on vectors with
// `aievec.mac_elem`. This pattern works for aie-ml.
struct ConvertMulAddToAIEVecFMAElemOpPattern
    : public OpConversionPattern<arith::AddIOp> {
  using OpConversionPattern<arith::AddIOp>::OpConversionPattern;

  ConvertMulAddToAIEVecFMAElemOpPattern(MLIRContext *context,
                                        unsigned shiftParam = 0)
      : OpConversionPattern<arith::AddIOp>(context), shiftParam(shiftParam) {}

  LogicalResult
  matchAndRewrite(arith::AddIOp addOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Verify it's a vector operation
    VectorType resultType = dyn_cast<VectorType>(addOp.getType());
    if (!resultType)
      return failure();

    // Verify it can be replaced by a MAC
    auto res =
        extractMACOperandsFromAddOperands(adaptor.getLhs(), adaptor.getRhs());
    if (!res)
      return failure();
    auto [lhs, rhs, acc] = *res;

    // Verify the vector type is supported by AIEML
    unsigned resultElWidth =
        resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    if ((laneSize != 32 || resultElWidth != 16) &&
        (laneSize != 16 || resultElWidth != 32))
      return failure();

    Type accType = getVectorOpDestType(cast<VectorType>(acc.getType()),
                                       /*AIEML =*/true);
    auto upsOp = rewriter.create<aievec::UPSOp>(addOp.getLoc(), accType, acc,
                                                shiftParam);
    auto fmaElemOp = rewriter.create<aievec::FMAElemOp>(
        addOp.getLoc(), accType, lhs, rhs, upsOp.getResult(),
        /*fmsub=*/false);

    auto shiftParamOp = rewriter.create<arith::ConstantOp>(
        addOp.getLoc(), rewriter.getI32IntegerAttr(shiftParam));
    rewriter.replaceOpWithNewOp<aievec::SRSOp>(
        addOp, resultType, fmaElemOp.getResult(), shiftParamOp.getResult());

    return success();
  }

  unsigned shiftParam;
};

// This pattern replaces `arith.mulf` on vectors with
// `aievec.mul_elem`. This pattern works for aie-ml.
struct ConvertMulFToAIEVecMulElemOpPattern
    : public OpConversionPattern<arith::MulFOp> {
  using OpConversionPattern<arith::MulFOp>::OpConversionPattern;

  ConvertMulFToAIEVecMulElemOpPattern(MLIRContext *context,
                                      unsigned shiftParam = 0)
      : OpConversionPattern<arith::MulFOp>(context), shiftParam(shiftParam) {}

  LogicalResult
  matchAndRewrite(arith::MulFOp mulOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Verify it's a vector operation
    VectorType resultType = dyn_cast<VectorType>(mulOp.getType());
    if (!resultType)
      return failure();

    // FIXME: Verify it is not a part of FMA
    auto isAddOp = [&](Operation *op) { return isa<arith::AddFOp>(op); };
    if (mulOp->hasOneUse() && llvm::any_of(mulOp->getUsers(), isAddOp))
      return failure();

    unsigned resultElWidth =
        resultType.getElementType().getIntOrFloatBitWidth();

    unsigned laneSize = getVectorLaneSize(resultType);

    // bfloat16 type
    if (laneSize != 16 || (resultElWidth != 16 && resultElWidth != 32))
      return failure();

    // Decide the accType for aievec.mul_elem based on mulOp's lhs & rhs
    auto lval = adaptor.getLhs();
    auto rval = adaptor.getRhs();
    if (auto lvalExtOp = lval.getDefiningOp<arith::ExtFOp>()) {
      lval = lvalExtOp->getOperand(0);
    }
    if (auto rvalExtOp = rval.getDefiningOp<arith::ExtFOp>()) {
      rval = rvalExtOp->getOperand(0);
    }
    VectorType lSrcType = cast<VectorType>(lval.getType());
    VectorType rSrcType = cast<VectorType>(rval.getType());
    unsigned lBitWidth = lSrcType.getElementType().getIntOrFloatBitWidth();
    unsigned rBitWidth = rSrcType.getElementType().getIntOrFloatBitWidth();
    Type accType = getVectorOpDestType(lSrcType, /*AIEML =*/true);
    if (rBitWidth > lBitWidth) {
      accType = getVectorOpDestType(rSrcType, /*AIEML =*/true);
    }
    // Only support the same lhs/rhs type at the moment
    if (lSrcType != rSrcType) {
      return failure();
    }
    // Only support two bfloat16 inputs at the moment
    if (lBitWidth != 16 || rBitWidth != 16) {
      return failure();
    }

    // Prepare lhr/rhs for the aievec.mul_elem op
    VectorType targetInputType =
        createVectorType(512 / lBitWidth, lSrcType.getElementType());
    if (rBitWidth > lBitWidth) {
      targetInputType =
          createVectorType(512 / rBitWidth, rSrcType.getElementType());
    }
    auto lValConverted = convertValueToTargetTypeAieML(rewriter, mulOp.getLoc(),
                                                       lval, targetInputType);
    auto rValConverted = convertValueToTargetTypeAieML(rewriter, mulOp.getLoc(),
                                                       rval, targetInputType);
    if (!lValConverted || !rValConverted)
      return failure();

    // Create an aievec.mul_elem op
    aievec::MulElemOp mulElemOp = rewriter.create<aievec::MulElemOp>(
        mulOp.getLoc(), accType, *lValConverted, *rValConverted);

    // Create an aievec.cast or an aievec.srs op
    auto mulElemResultType = mulElemOp.getType();
    auto mulElemResultElWidth =
        mulElemResultType.getElementType().getIntOrFloatBitWidth();

    if (mulElemResultElWidth == resultElWidth) {
      rewriter.replaceOpWithNewOp<aievec::CastOp>(
          mulOp, resultType, mulElemOp.getResult(), /*isResAcc*/ false);
    } else if (mulElemResultElWidth > resultElWidth) {
      auto shiftParamOp = rewriter.create<arith::ConstantOp>(
          mulOp.getLoc(), rewriter.getI32IntegerAttr(shiftParam));
      rewriter.replaceOpWithNewOp<aievec::SRSOp>(
          mulOp, resultType, mulElemOp.getResult(), shiftParamOp.getResult());
    } else {
      return failure();
    }

    return success();
  }

  unsigned shiftParam;
};

// This pattern replaces `arith.muli` on vectors with
// `aievec.mul_elem`. This pattern works for aie-ml.
struct ConvertMulIToAIEVecMulElemOpPattern
    : public OpConversionPattern<arith::MulIOp> {
  using OpConversionPattern<arith::MulIOp>::OpConversionPattern;

  ConvertMulIToAIEVecMulElemOpPattern(MLIRContext *context,
                                      unsigned shiftParam = 0)
      : OpConversionPattern<arith::MulIOp>(context), shiftParam(shiftParam) {}

  LogicalResult
  matchAndRewrite(arith::MulIOp mulOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Verify it's a vector operation
    VectorType resultType = dyn_cast<VectorType>(mulOp.getType());
    if (!resultType)
      return failure();

    // FIXME: Verify it is not a part of MAC
    auto isAddOp = [&](Operation *op) { return isa<arith::AddIOp>(op); };
    if (mulOp->hasOneUse() && llvm::any_of(mulOp->getUsers(), isAddOp))
      return failure();

    // Verify the vector type is supported by AIEML
    unsigned resultElWidth =
        resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    if ((laneSize != 32 || (resultElWidth != 16 && resultElWidth != 8)) &&
        ((laneSize != 16 && laneSize != 32) || resultElWidth != 32))
      return failure();

    // Decide the accType for aievec.mul_elem based on mulOp's lhs & rhs
    auto lval = adaptor.getLhs();
    auto rval = adaptor.getRhs();
    if (auto lvalExtOp = lval.getDefiningOp<arith::ExtSIOp>()) {
      lval = lvalExtOp->getOperand(0);
    }
    if (auto rvalExtOp = rval.getDefiningOp<arith::ExtSIOp>()) {
      rval = rvalExtOp->getOperand(0);
    }
    VectorType lSrcType = cast<VectorType>(lval.getType());
    VectorType rSrcType = cast<VectorType>(rval.getType());
    unsigned lBitWidth = lSrcType.getElementType().getIntOrFloatBitWidth();
    unsigned rBitWidth = rSrcType.getElementType().getIntOrFloatBitWidth();
    Type accType = getVectorOpDestType(lSrcType, /*AIEML =*/true);
    if (rBitWidth > lBitWidth) {
      accType = getVectorOpDestType(rSrcType, /*AIEML =*/true);
    }

    // Prepare lhr/rhs for the aievec.mul_elem op
    VectorType targetInputType =
        createVectorType(512 / lBitWidth, lSrcType.getElementType());
    if (rBitWidth > lBitWidth) {
      targetInputType =
          createVectorType(512 / rBitWidth, rSrcType.getElementType());
    }
    auto lValConverted = convertValueToTargetTypeAieML(rewriter, mulOp.getLoc(),
                                                       lval, targetInputType);
    auto rValConverted = convertValueToTargetTypeAieML(rewriter, mulOp.getLoc(),
                                                       rval, targetInputType);
    if (!lValConverted || !rValConverted)
      return failure();

    // Create an aievec.mul_elem op
    aievec::MulElemOp mulElemOp = rewriter.create<aievec::MulElemOp>(
        mulOp.getLoc(), accType, *lValConverted, *rValConverted);

    // Create an aievec.cast or an aievec.srs op
    auto mulElemResultType = mulElemOp.getType();
    auto mulElemResultElWidth =
        mulElemResultType.getElementType().getIntOrFloatBitWidth();

    if (mulElemResultElWidth == resultElWidth) {
      rewriter.replaceOpWithNewOp<aievec::CastOp>(
          mulOp, resultType, mulElemOp.getResult(), /*isResAcc*/ false);
    } else if (mulElemResultElWidth > resultElWidth) {
      auto shiftParamOp = rewriter.create<arith::ConstantOp>(
          mulOp.getLoc(), rewriter.getI32IntegerAttr(shiftParam));
      rewriter.replaceOpWithNewOp<aievec::SRSOp>(
          mulOp, resultType, mulElemOp.getResult(), shiftParamOp.getResult());
    } else {
      return failure();
    }

    return success();
  }

  unsigned shiftParam;
};

// This pattern folds an extract + broadcast feeding into an `aievec::FMAOp`
// into the op, using the shuffle attributes.
struct FoldBroadcastToFMAOp : public OpConversionPattern<aievec::FMAOp> {
  using OpConversionPattern<aievec::FMAOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(aievec::FMAOp fmaOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto concatOp =
        dyn_cast<aievec::ConcatOp>(adaptor.getLhs().getDefiningOp());
    if (!concatOp)
      return failure();
    vector::BroadcastOp bcastOp = nullptr;
    auto concatDefOp = concatOp.getSources()[0].getDefiningOp();
    if (concatDefOp)
      bcastOp = dyn_cast<vector::BroadcastOp>(concatDefOp);
    Value lhs = adaptor.getRhs();
    if (!bcastOp) {
      bcastOp = dyn_cast<vector::BroadcastOp>(adaptor.getRhs().getDefiningOp());
      if (!bcastOp)
        return failure();
      lhs = concatOp.getSources()[0];
    }
    auto extOp =
        dyn_cast<vector::ExtractOp>(bcastOp.getSource().getDefiningOp());
    if (!extOp)
      return failure();

    auto rhs = extOp.getVector();
    auto concatVecType = cast<VectorType>(concatOp.getResult().getType());
    auto zvec = rewriter.create<arith::ConstantOp>(
        concatOp.getLoc(), lhs.getType(), rewriter.getZeroAttr(lhs.getType()));
    auto lhsX2 =
        rewriter
            .create<aievec::ConcatOp>(concatOp.getLoc(), concatVecType,
                                      SmallVector<Value, 2>({lhs, zvec}))
            .getResult();
    // XXX: We assume a 1D vector
    auto pos = extOp.getStaticPosition();
    int64_t zstart = pos[0];
    auto fmaOpAttr = buildFMAOpSplatAttrForElemTy(fmaOp, zstart);
    rewriter.replaceOpWithNewOp<aievec::FMAOp>(
        fmaOp, TypeRange({fmaOp.getResult().getType()}),
        ValueRange({lhsX2, rhs, adaptor.getAcc()}), fmaOpAttr);

    return success();
  }
};

struct ConvertMulAddToAIEVecFMAOpPattern
    : public OpConversionPattern<aievec::AddOp> {
  using OpConversionPattern<aievec::AddOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(aievec::AddOp addOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType vecType = cast<VectorType>(addOp.getType());

    auto res =
        extractMACOperandsFromAddOperands(adaptor.getLhs(), adaptor.getRhs());
    if (!res)
      return failure();
    auto [lhs, rhs, acc] = *res;

    SmallVector<int64_t, 4> concatVecShape(vecType.getShape().begin(),
                                           vecType.getShape().end());
    concatVecShape[vecType.getRank() - 1] *= 2;
    auto concatVecType =
        VectorType::get(concatVecShape, vecType.getElementType());
    Type accType = getVectorOpDestType(cast<VectorType>(acc.getType()),
                                       /*AIEML =*/false);
    auto lhsX2 = rewriter
                     .create<aievec::ConcatOp>(addOp.getLoc(), concatVecType,
                                               SmallVector<Value, 2>(2, lhs))
                     .getResult();
    auto upsOp = rewriter.create<aievec::UPSOp>(addOp.getLoc(), accType, acc);
    auto fmaOp = rewriter.create<aievec::FMAOp>(
        addOp.getLoc(), accType, lhsX2, rhs, upsOp.getResult(),
        /*xstart=*/"", /*xoffsets=*/"", /*xoffsets_hi=*/"", /*xstep=*/"",
        /*xsquare=*/"", /*zstart=*/"", /*zoffsets=*/"", /*zoffsets_hi=*/"",
        /*zstep=*/"", /*zsquare=*/"", /*fmsub=*/false);
    auto shiftParamOp = rewriter.create<arith::ConstantOp>(
        addOp.getLoc(), rewriter.getI32IntegerAttr(0));
    rewriter.replaceOpWithNewOp<aievec::SRSOp>(
        addOp, vecType, fmaOp.getResult(), shiftParamOp.getResult());
    return success();
  }
};

// This pattern replaces `vector.transfer_read` with `aievec.upd`. Right now,
// it performs a naïve direct translation. This needs to be expanded to
// support more complex scenarios.
struct LowerVectorTransferReadToAIEUPD
    : public OpConversionPattern<vector::TransferReadOp> {
  using OpConversionPattern<vector::TransferReadOp>::OpConversionPattern;

  LowerVectorTransferReadToAIEUPD(MLIRContext *context, int64_t minVectorSize,
                                  int64_t maxVectorSize, int64_t alignment,
                                  int64_t maxLoadSize)
      : OpConversionPattern<vector::TransferReadOp>(context),
        minVectorSize(minVectorSize), maxVectorSize(maxVectorSize),
        vectorAlignment(alignment), maxLoadSize(maxLoadSize) {}

  LogicalResult
  matchAndRewrite(vector::TransferReadOp readOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Masked loads
    if (readOp.getMask())
      return readOp.emitError() << "AIE doesn't support masked loads.";

    // Non-contiguous loads
    AffineMap map = readOp.getPermutationMap();
    if (!map.isMinorIdentity())
      return failure();

    // Splats
    if (map.isConstant())
      return failure();

    // Misaligned accesses
    auto vType = readOp.getVectorType();
    if (getTransferReadAlignmentOffset(adaptor, vType, vectorAlignment)
            .value_or(0) != 0)
      return failure();

    // Invalid vector size.
    // We can handle cases where the vector size is:
    //   1) the minimum vector size
    //   2) a square multiple of the alignment size and up to the maximum
    //      vector size.
    int64_t vSize = vType.getNumElements() * vType.getElementTypeBitWidth();
    if (vSize > maxVectorSize ||
        (vSize % vectorAlignment && vSize != minVectorSize))
      return failure();
    // We can deal with linked update instructions when the vector size is
    // exactly twice the load size. This could change in future architectures
    if (vSize > maxLoadSize && vSize != maxLoadSize * 2)
      return failure();
    int64_t multiplicity = vSize / vectorAlignment;
    if ((vSize > minVectorSize) && std::bitset<8>(multiplicity).count() != 1)
      return failure();

    auto updOp = rewriter.create<xilinx::aievec::UPDOp>(
        readOp.getLoc(), vType, adaptor.getSource(), adaptor.getIndices(), 0, 0,
        TypedValue<VectorType>(nullptr));
    if (vSize > maxLoadSize) {
      updOp = rewriter.create<xilinx::aievec::UPDOp>(
          readOp.getLoc(), vType, adaptor.getSource(), adaptor.getIndices(),
          maxLoadSize, 1, updOp.getResult());
    }
    rewriter.replaceOp(readOp, updOp.getResult());

    return success();
  }

  int64_t minVectorSize, maxVectorSize, vectorAlignment, maxLoadSize;
};

// XXX: Notice that this template doesn't verify that the vector element type
// XXX: is supported by the target architecture.
template <typename SrcOpTy, typename DstOpTy>
struct OneToOneVectorOpToAIEVecOpPattern : public OpConversionPattern<SrcOpTy> {
  using OpConversionPattern<SrcOpTy>::OpConversionPattern;
  using OpAdaptor = typename SrcOpTy::Adaptor;

  LogicalResult
  matchAndRewrite(SrcOpTy srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<DstOpTy>(
        srcOp, srcOp.getResult().getType(), adaptor.getLhs(), adaptor.getRhs(),
        /*xstart=*/"", /*xoffsets=*/"", /*xoffsets_hi=*/"", /*xsquare=*/"",
        /*zstart=*/"", /*zoffsets=*/"", /*zoffsets_hi=*/"", /*zsquare=*/"");
    return success();
  }
};

struct LowerVectorAddIOpToAIEVecAddOp
    : public OpConversionPattern<arith::AddIOp> {
  using OpConversionPattern<arith::AddIOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(arith::AddIOp addOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto resType = addOp.getType();
    if (!isa<VectorType>(resType))
      return failure();

    auto lhs = adaptor.getLhs();
    auto rhs = adaptor.getRhs();
    auto lhsDefOp = lhs.getDefiningOp();
    auto rhsDefOp = rhs.getDefiningOp();
    if ((lhsDefOp && isa<arith::MulIOp>(lhsDefOp)) ||
        (rhsDefOp && isa<arith::MulIOp>(rhsDefOp)))
      return failure();

    rewriter.replaceOpWithNewOp<aievec::AddOp>(
        addOp, resType, lhs, rhs,
        /*xstart=*/"", /*xoffsets=*/"", /*xoffsets_hi=*/"", /*xsquare=*/"",
        /*zstart=*/"", /*zoffsets=*/"", /*zoffsets_hi=*/"", /*zsquare=*/"");
    return success();
  }
};

using LowerVectorAddFOpToAIEVecAddOp =
    OneToOneVectorOpToAIEVecOpPattern<arith::AddFOp, aievec::AddOp>;
using LowerVectorMulFOpToAIEVecMulOp =
    OneToOneVectorOpToAIEVecOpPattern<arith::MulFOp, aievec::MulOp>;
using LowerVectorSubIOpToAIEVecSubOp =
    OneToOneVectorOpToAIEVecOpPattern<arith::SubIOp, aievec::SubOp>;
using LowerVectorSubFOpToAIEVecSubOp =
    OneToOneVectorOpToAIEVecOpPattern<arith::SubFOp, aievec::SubOp>;

struct LowerVectorMulIOpToAIEVecMulOp
    : public OpConversionPattern<arith::MulIOp> {
  using OpConversionPattern<arith::MulIOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(arith::MulIOp mulOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto resTy = dyn_cast<VectorType>(mulOp.getType());
    if (!resTy)
      return failure();
    auto accTy = getVectorOpDestType(resTy, /*AIEML =*/false);
    auto newMulOp = rewriter.create<aievec::MulOp>(
        mulOp.getLoc(), accTy, adaptor.getLhs(), adaptor.getRhs());
    auto shiftParamOp = rewriter.create<arith::ConstantOp>(
        mulOp.getLoc(), rewriter.getI32IntegerAttr(0));
    rewriter.replaceOpWithNewOp<aievec::SRSOp>(
        mulOp, resTy, newMulOp.getResult(), shiftParamOp.getResult());
    return success();
  }
};

template <typename SrcOpTy, typename DstOpTy>
struct LowerVectorAddOrSubOpToAIEVecAddElemOrSubElemOp
    : public OpConversionPattern<SrcOpTy> {
  using OpConversionPattern<SrcOpTy>::OpConversionPattern;
  using OpAdaptor = typename SrcOpTy::Adaptor;

  LogicalResult
  matchAndRewrite(SrcOpTy srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType resultType = dyn_cast<VectorType>(srcOp.getType());
    if (!resultType)
      return failure();

    // A set recording the vector lane size and element width we are supporting
    // for aie-ml.
    llvm::SmallSet<std::pair<unsigned, signed>, 16> laneSizeElWidthPairSet;
    laneSizeElWidthPairSet.insert({64, 8});
    laneSizeElWidthPairSet.insert({32, 16});
    laneSizeElWidthPairSet.insert({16, 32});
    laneSizeElWidthPairSet.insert({32, 32});

    auto lhs = adaptor.getLhs();
    auto rhs = adaptor.getRhs();
    auto lhsDefOp = lhs.getDefiningOp();
    auto rhsDefOp = rhs.getDefiningOp();
    if ((lhsDefOp && isa<arith::MulIOp>(lhsDefOp)) ||
        (rhsDefOp && isa<arith::MulIOp>(rhsDefOp)) ||
        (lhsDefOp && isa<arith::MulFOp>(lhsDefOp)) ||
        (rhsDefOp && isa<arith::MulFOp>(rhsDefOp)))
      return failure();

    Type scalarType = resultType.getElementType();
    unsigned resultElWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    // Integer cases
    if (scalarType.isa<IntegerType>()) {
      if (!laneSizeElWidthPairSet.count(
              std::make_pair(laneSize, resultElWidth)))
        return failure();

      // If the ops are defined without extension ops and with supported data
      // type, the arith::AddI or arith::SubI can be directly replaced with
      // aievec::AddElem or aievec::SubElem.
      if (!lhsDefOp && !rhsDefOp) {
        if (laneSize * resultElWidth == 512) {
          rewriter.replaceOpWithNewOp<DstOpTy>(srcOp, srcOp.getType(), lhs,
                                               rhs);
          return success();
        } else {
          return genAddElemAieML<SrcOpTy, DstOpTy>(rewriter, lhs, rhs,
                                                   resultType, srcOp);
        }
      }

      // If element width is 32, we need to consider sign extension cases
      if (resultElWidth == 32) {
        auto lhsExt = lhsDefOp ? dyn_cast<arith::ExtSIOp>(lhsDefOp) : nullptr;
        auto rhsExt = rhsDefOp ? dyn_cast<arith::ExtSIOp>(rhsDefOp) : nullptr;

        if (!lhsExt && !rhsExt) {
          if (laneSize * resultElWidth == 512) {
            rewriter.replaceOpWithNewOp<DstOpTy>(srcOp, srcOp.getType(), lhs,
                                                 rhs);
            return success();
          } else {
            return genAddElemAieML<SrcOpTy, DstOpTy>(rewriter, lhs, rhs,
                                                     resultType, srcOp);
          }
        }

        if (lhsExt && rhsExt) {
          auto lval = lhsExt->getOperand(0);
          auto rval = rhsExt->getOperand(0);
          VectorType lSrcType = cast<VectorType>(lval.getType());

          Type accType = getVectorOpDestType(lSrcType, /*AIEML =*/true);
          auto lUpsOp =
              rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, lval);
          auto rUpsOp =
              rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, rval);
          auto elemOp = rewriter.create<DstOpTy>(
              srcOp.getLoc(), lUpsOp->getResult(0).getType(),
              lUpsOp->getResult(0), rUpsOp->getResult(0));
          rewriter.replaceOpWithNewOp<aievec::CastOp>(
              srcOp, srcOp.getType(), elemOp.getResult(), /*isResAcc*/ false);
          return success();
        }

        if (!lhsExt || !rhsExt) {
          auto lval = lhsExt ? lhsExt->getOperand(0) : lhs;
          auto rval = rhsExt ? rhsExt->getOperand(0) : rhs;
          auto extVal = lhsExt ? lval : rval;
          VectorType vType = cast<VectorType>(extVal.getType());
          unsigned bitWidth = vType.getElementType().getIntOrFloatBitWidth();

          if (bitWidth != 8 && bitWidth != 16) {
            return genAddElemAieML<SrcOpTy, DstOpTy>(rewriter, lhs, rhs,
                                                     resultType, srcOp);
          }

          if (bitWidth * laneSize != 256) {
            return genAddElemAieML<SrcOpTy, DstOpTy>(rewriter, lhs, rhs,
                                                     resultType, srcOp);
          }

          Type accType = nullptr;

          if (bitWidth == 8) {
            accType = getVectorOpDestType(vType, /*AIEML =*/true);
            Value valToUps = lhsExt ? lval : rval;
            Value valToCast = lhsExt ? rval : lval;
            auto upsOp = rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType,
                                                        valToUps);
            auto castOp = rewriter.create<aievec::CastOp>(
                srcOp.getLoc(), resultType, valToCast, /*isResAcc*/ true);
            Value lhsToElemOp =
                lhsExt ? upsOp->getResult(0) : castOp->getResult(0);
            Value rhsToElemOp =
                lhsExt ? castOp->getResult(0) : upsOp->getResult(0);
            auto elemOp = rewriter.create<DstOpTy>(
                srcOp.getLoc(), upsOp->getResult(0).getType(), lhsToElemOp,
                rhsToElemOp);
            rewriter.replaceOpWithNewOp<aievec::CastOp>(
                srcOp, srcOp.getType(), elemOp.getResult(), /*isResAcc*/ false);
            return success();
          } else if (bitWidth == 16) {
            accType = getVectorOpDestType(resultType, /*AIEML =*/true);
            auto lUpsOp =
                rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, lval);
            auto rUpsOp =
                rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, rval);

            auto elemOp = rewriter.create<DstOpTy>(
                srcOp.getLoc(), lUpsOp->getResult(0).getType(),
                lUpsOp->getResult(0), rUpsOp->getResult(0));

            auto shiftParamOp = rewriter.create<arith::ConstantOp>(
                srcOp.getLoc(), rewriter.getI32IntegerAttr(0));
            rewriter.replaceOpWithNewOp<aievec::SRSOp>(
                srcOp, srcOp.getType(), elemOp.getResult(),
                shiftParamOp.getResult());
            return success();
          }
        }
      } else {
        rewriter.replaceOpWithNewOp<DstOpTy>(srcOp, srcOp.getType(), lhs, rhs);
        return success();
      }
    }
    // Float types
    else {
      if (laneSize != 16)
        return failure();

      // v16float or v16bf16 with extension op case
      if (resultElWidth == 32) {
        if (!lhsDefOp && !rhsDefOp) {
          return genAddElemAieML<SrcOpTy, DstOpTy>(rewriter, lhs, rhs,
                                                   resultType, srcOp);
        }

        auto lhsExt = lhsDefOp ? dyn_cast<arith::ExtFOp>(lhsDefOp) : nullptr;
        auto rhsExt = rhsDefOp ? dyn_cast<arith::ExtFOp>(rhsDefOp) : nullptr;
        // v16float
        if (!lhsExt && !rhsExt) {
          return genAddElemAieML<SrcOpTy, DstOpTy>(rewriter, lhs, rhs,
                                                   resultType, srcOp);
        }

        // v16bf16 with two extension ops
        if (lhsExt && rhsExt) {
          auto lval = lhsExt->getOperand(0);
          auto rval = rhsExt->getOperand(0);
          VectorType vType = cast<VectorType>(lval.getType());

          Type accType = getVectorOpDestType(vType, /*AIEML =*/true);
          auto lUpsOp =
              rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, lval);
          auto rUpsOp =
              rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, rval);
          auto elemOp = rewriter.create<DstOpTy>(
              srcOp.getLoc(), lUpsOp->getResult(0).getType(),
              lUpsOp->getResult(0), rUpsOp->getResult(0));
          rewriter.replaceOpWithNewOp<aievec::CastOp>(srcOp, srcOp.getType(),
                                                      elemOp.getResult());
          return success();
        }

        // v16bf16 with one extension op
        if (!lhsExt || !rhsExt) {
          auto lval = lhsExt ? lhsExt->getOperand(0) : lhs;
          auto rval = rhsExt ? rhsExt->getOperand(0) : rhs;
          auto extVal = lhsExt ? lval : rval;
          VectorType vType = cast<VectorType>(extVal.getType());
          Type accType = getVectorOpDestType(vType, /*AIEML =*/true);
          aievec::UPSOp upsOp = nullptr;
          aievec::CastOp castOp = nullptr;

          if (lhsExt) {
            upsOp =
                rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, lval);
            castOp = rewriter.create<aievec::CastOp>(srcOp.getLoc(), resultType,
                                                     rval,
                                                     /*isResAcc*/ true);
          } else {
            upsOp =
                rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, rval);
            castOp = rewriter.create<aievec::CastOp>(srcOp.getLoc(), resultType,
                                                     lval,
                                                     /*isResAcc*/ true);
          }
          auto elemOp = rewriter.create<DstOpTy>(
              srcOp.getLoc(), upsOp->getResult(0).getType(),
              upsOp->getResult(0), castOp->getResult(0));

          rewriter.replaceOpWithNewOp<aievec::CastOp>(
              srcOp, srcOp.getType(), elemOp.getResult(), /*isResAcc*/ false);
          return success();
        }
      }
      // v16bfloat16
      Type accType = getVectorOpDestType(resultType, /*AIEML =*/true);
      auto lUpsOp =
          rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, lhs);
      auto rUpsOp =
          rewriter.create<aievec::UPSOp>(srcOp.getLoc(), accType, rhs);
      auto elemOp = rewriter.create<DstOpTy>(
          srcOp.getLoc(), lUpsOp->getResult(0).getType(), lUpsOp->getResult(0),
          rUpsOp->getResult(0));
      auto shiftParamOp = rewriter.create<arith::ConstantOp>(
          srcOp.getLoc(), rewriter.getI32IntegerAttr(0));
      rewriter.replaceOpWithNewOp<aievec::SRSOp>(
          srcOp, srcOp.getType(), elemOp.getResult(), shiftParamOp.getResult());
      return success();
    }
    return failure();
  }
};

using LowerVectorAddIOpToAIEVecAddElemOp =
    LowerVectorAddOrSubOpToAIEVecAddElemOrSubElemOp<arith::AddIOp,
                                                    aievec::AddElemOp>;
using LowerVectorSubIOpToAIEVecSubElemOp =
    LowerVectorAddOrSubOpToAIEVecAddElemOrSubElemOp<arith::SubIOp,
                                                    aievec::SubElemOp>;
using LowerVectorAddFOpToAIEVecAddElemOp =
    LowerVectorAddOrSubOpToAIEVecAddElemOrSubElemOp<arith::AddFOp,
                                                    aievec::AddElemOp>;
using LowerVectorSubFOpToAIEVecSubElemOp =
    LowerVectorAddOrSubOpToAIEVecAddElemOrSubElemOp<arith::SubFOp,
                                                    aievec::SubElemOp>;

template <typename SrcOpTy, typename DstOpTy>
struct LowerVectorMinMaxOpToAIEVecMinMaxOp
    : public OpConversionPattern<SrcOpTy> {
  using OpConversionPattern<SrcOpTy>::OpConversionPattern;
  using OpAdaptor = typename SrcOpTy::Adaptor;

  LogicalResult
  matchAndRewrite(SrcOpTy srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType resultType = dyn_cast<VectorType>(srcOp.getType());
    if (!resultType)
      return failure();

    // A set recording the element width we are supporting for aie-ml.
    llvm::SmallSet<unsigned, 16> elWidthSet;
    elWidthSet.insert(8);
    elWidthSet.insert(16);
    elWidthSet.insert(32);

    Type scalarType = resultType.getElementType();
    unsigned resultElWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    if (!(elWidthSet.count(resultElWidth) && laneSize * resultElWidth == 512))
      return failure();

    rewriter.replaceOpWithNewOp<DstOpTy>(srcOp, srcOp.getType(),
                                         adaptor.getLhs(), adaptor.getRhs());
    return success();
  }
};

using LowerVectorMinSIOpToAIEVecMinOp =
    LowerVectorMinMaxOpToAIEVecMinMaxOp<arith::MinSIOp, aievec::MinOp>;
using LowerVectorMaxSIOpToAIEVecMaxOp =
    LowerVectorMinMaxOpToAIEVecMinMaxOp<arith::MaxSIOp, aievec::MaxOp>;
using LowerVectorMinimumFOpToAIEVecMinOp =
    LowerVectorMinMaxOpToAIEVecMinMaxOp<arith::MinimumFOp, aievec::MinOp>;
using LowerVectorMaximumFOpToAIEVecMaxOp =
    LowerVectorMinMaxOpToAIEVecMinMaxOp<arith::MaximumFOp, aievec::MaxOp>;

template <typename SrcOpTy, typename CmpTy>
struct LowerVectorCmpOpToAIEVecCmpOp : public OpConversionPattern<SrcOpTy> {
  using OpConversionPattern<SrcOpTy>::OpConversionPattern;
  using OpAdaptor = typename SrcOpTy::Adaptor;

  LogicalResult
  matchAndRewrite(SrcOpTy srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType lhsType = dyn_cast<VectorType>(srcOp.getLhs().getType());
    if (!lhsType)
      return failure();

    llvm::SmallSet<unsigned, 16> elWidthSet;
    elWidthSet.insert(8);
    elWidthSet.insert(16);
    elWidthSet.insert(32);

    Type scalarType = lhsType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(lhsType);

    if (!(elWidthSet.count(elWidth) && laneSize * elWidth == 512))
      return failure();

    // Unsigned int and unsigned long long are acceptable type.
    Type type =
        mlir::IntegerType::get(srcOp.getContext(), laneSize <= 32 ? 32 : 64,
                               mlir::IntegerType::Unsigned);

    Location loc = srcOp.getLoc();
    Value lhs = srcOp.getLhs();
    Value rhs = srcOp.getRhs();
    CmpTy pred = srcOp.getPredicate();

    arith::CmpIPredicate ipred = convertToIntegerPredicate(pred);

    aievec::CmpOp aieCmpOp =
        createCmpOpAieML(rewriter, ipred, loc, type, lhs, rhs);

    if (!aieCmpOp)
      return failure();

    VectorType resultType = dyn_cast<VectorType>(srcOp.getResult().getType());
    // Convert vector i1 type to unsigned interger type by built-in unrealized
    // conversion cast op.
    rewriter.replaceOpWithNewOp<UnrealizedConversionCastOp>(
        srcOp, resultType, aieCmpOp.getResult());

    return success();
  }
};

using LowerVectorCmpIOpToAIEVecCmpOp =
    LowerVectorCmpOpToAIEVecCmpOp<arith::CmpIOp, CmpIPredicate>;
using LowerVectorCmpFOpToAIEVecCmpOp =
    LowerVectorCmpOpToAIEVecCmpOp<arith::CmpFOp, CmpFPredicate>;

struct LowerVectorSelectOpToAIEVecSelOp
    : public OpConversionPattern<arith::SelectOp> {
  using OpConversionPattern<arith::SelectOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(arith::SelectOp srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType resultType = dyn_cast<VectorType>(srcOp.getType());
    if (!resultType)
      return failure();

    llvm::SmallSet<unsigned, 16> elWidthSet;
    elWidthSet.insert(8);
    elWidthSet.insert(16);
    elWidthSet.insert(32);

    Type scalarType = resultType.getElementType();
    unsigned resultElWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    if (!(elWidthSet.count(resultElWidth) && laneSize * resultElWidth == 512))
      return failure();

    Type type =
        mlir::IntegerType::get(srcOp.getContext(), laneSize <= 32 ? 32 : 64,
                               mlir::IntegerType::Unsigned);

    auto convertOp = rewriter.create<UnrealizedConversionCastOp>(
        srcOp.getLoc(), type, adaptor.getCondition());

    rewriter.replaceOpWithNewOp<aievec::SelOp>(
        srcOp, srcOp.getResult().getType(), srcOp.getTrueValue(),
        srcOp.getFalseValue(), convertOp.getResult(0));

    return success();
  }
};

struct LowerVectorReductionMinOp
    : public OpConversionPattern<vector::ReductionOp> {
  using OpConversionPattern<vector::ReductionOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::ReductionOp srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto kind = srcOp.getKind();
    if (kind != vector::CombiningKind::MINSI &&
        kind != vector::CombiningKind::MINUI &&
        kind != vector::CombiningKind::MINF)
      return failure();

    VectorType vType = cast<VectorType>(srcOp.getVector().getType());
    Type scalarType = vType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(vType);

    if (laneSize * elWidth != 512)
      return failure();

    int shiftIndex = laneSize / 2;
    generateAIEVecOpsForReductionOp<aievec::MinOp>(rewriter, srcOp, shiftIndex,
                                                   srcOp.getVector());
    return success();
  }
};

struct LowerVectorReductionMaxOp
    : public OpConversionPattern<vector::ReductionOp> {
  using OpConversionPattern<vector::ReductionOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::ReductionOp srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto kind = srcOp.getKind();
    if (kind != vector::CombiningKind::MAXSI &&
        kind != vector::CombiningKind::MAXUI &&
        kind != vector::CombiningKind::MAXF)
      return failure();

    VectorType vType = cast<VectorType>(srcOp.getVector().getType());
    Type scalarType = vType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(vType);

    if (laneSize * elWidth != 512)
      return failure();

    int shiftIndex = laneSize / 2;
    generateAIEVecOpsForReductionOp<aievec::MaxOp>(rewriter, srcOp, shiftIndex,
                                                   srcOp.getVector());
    return success();
  }
};

struct LowerVectorReductionAddIntOp
    : public OpConversionPattern<vector::ReductionOp> {
  using OpConversionPattern<vector::ReductionOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::ReductionOp srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto kind = srcOp.getKind();
    if (kind != vector::CombiningKind::ADD)
      return failure();

    VectorType vType = cast<VectorType>(srcOp.getVector().getType());
    Type scalarType = vType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(vType);
    llvm::SmallSet<std::pair<unsigned, signed>, 16> laneSizeElWidthPairSet;
    laneSizeElWidthPairSet.insert({64, 8});
    laneSizeElWidthPairSet.insert({32, 16});
    laneSizeElWidthPairSet.insert({32, 32});
    laneSizeElWidthPairSet.insert({16, 32});

    if (!isa<IntegerType>(scalarType) ||
        !laneSizeElWidthPairSet.count(std::make_pair(laneSize, elWidth)))
      return failure();

    int shiftIndex = laneSize / 2;
    if (laneSize == 32 && elWidth == 32) {
      Location loc = srcOp.getLoc();
      VectorType vecType = createVectorType(laneSize / 2, scalarType);

      auto lExtOp =
          rewriter.create<aievec::ExtOp>(loc, vecType, srcOp.getVector(), 0);
      auto rExtOp =
          rewriter.create<aievec::ExtOp>(loc, vecType, srcOp.getVector(), 1);
      auto addElemOp = rewriter.create<aievec::AddElemOp>(
          loc, lExtOp.getResult().getType(), lExtOp.getResult(),
          rExtOp.getResult());
      shiftIndex /= 2;
      generateAIEVecOpsForReductionOp<aievec::AddElemOp>(
          rewriter, srcOp, shiftIndex, addElemOp.getResult());
    } else {
      generateAIEVecOpsForReductionOp<aievec::AddElemOp>(
          rewriter, srcOp, shiftIndex, srcOp.getVector());
    }
    return success();
  }
};

struct LowerVectorReductionAddFloatOp
    : public OpConversionPattern<vector::ReductionOp> {
  using OpConversionPattern<vector::ReductionOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::ReductionOp srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto kind = srcOp.getKind();
    if (kind != vector::CombiningKind::ADD)
      return failure();

    VectorType vType = cast<VectorType>(srcOp.getVector().getType());
    Type scalarType = vType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(vType);

    if (!isa<FloatType>(scalarType) || laneSize != 16 || elWidth != 32)
      return failure();

    int shiftIndex = laneSize / 2;
    assert(shiftIndex > 0 && (shiftIndex & (shiftIndex - 1)) == 0 &&
           "shiftIndex must be power of 2");

    Location loc = srcOp.getLoc();
    Value curValue = srcOp.getVector();
    aievec::CastOp curOp = nullptr;

    for (int id = shiftIndex; id > 0; id /= 2) {
      arith::ConstantOp constOp = rewriter.create<arith::ConstantOp>(
          loc, rewriter.getI32IntegerAttr(id * elWidth / 8));

      auto shiftBytesOp = rewriter.create<aievec::ShiftOp>(
          loc, vType, curValue, curValue, constOp.getResult());

      auto lCastOp = rewriter.create<aievec::CastOp>(loc, vType, curValue,
                                                     /*isResAcc*/ true);
      auto rCastOp =
          rewriter.create<aievec::CastOp>(loc, vType, shiftBytesOp.getResult(),
                                          /*isResAcc*/ true);
      auto elemOp = rewriter.create<aievec::AddElemOp>(
          loc, lCastOp.getResult().getType(), lCastOp.getResult(),
          rCastOp.getResult());
      curOp = rewriter.create<aievec::CastOp>(loc, vType, elemOp.getResult(),
                                              /*isResAcc*/ false);

      curValue = curOp.getResult();
    }

    arith::ConstantOp zeroConstOp =
        rewriter.create<arith::ConstantOp>(loc, rewriter.getI32IntegerAttr(0));
    rewriter.replaceOpWithNewOp<aievec::ExtElemOp>(srcOp, scalarType, curOp,
                                                   zeroConstOp.getResult());
    return success();
  }
};

struct LowerVectorReductionAddBfloat16Op
    : public OpConversionPattern<vector::ReductionOp> {
  using OpConversionPattern<vector::ReductionOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::ReductionOp srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto kind = srcOp.getKind();
    if (kind != vector::CombiningKind::ADD)
      return failure();

    VectorType vType = cast<VectorType>(srcOp.getVector().getType());
    Type scalarType = vType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(vType);

    if (!isa<FloatType>(scalarType) || laneSize != 16 || elWidth != 16)
      return failure();

    int shiftIndex = laneSize / 2;
    assert(shiftIndex > 0 && (shiftIndex & (shiftIndex - 1)) == 0 &&
           "shiftIndex must be power of 2");

    Value curValue = srcOp.getVector();
    Location loc = srcOp.getLoc();
    Type accType = getVectorOpDestType(vType, /*AIEML =*/true);
    unsigned accWidth =
        dyn_cast<VectorType>(accType).getElementType().getIntOrFloatBitWidth();

    auto upsOp =
        rewriter.create<aievec::UPSOp>(loc, accType, srcOp.getVector());

    curValue = upsOp.getResult();

    VectorType vecType = createVectorType(2 * laneSize, scalarType);
    aievec::AddElemOp curOp = nullptr;

    for (int id = shiftIndex; id > 0; id /= 2) {
      arith::ConstantOp constOp = rewriter.create<arith::ConstantOp>(
          loc, rewriter.getI32IntegerAttr(id * accWidth / 8));
      auto shiftBytesOp = rewriter.create<aievec::ShiftOp>(
          loc, accType, curValue, curValue, constOp, true);
      curOp = rewriter.create<aievec::AddElemOp>(loc, accType, curValue,
                                                 shiftBytesOp.getResult());
      curValue = curOp.getResult();
    }

    auto shiftParamOp = rewriter.create<arith::ConstantOp>(
        srcOp.getLoc(), rewriter.getI32IntegerAttr(0));
    auto srsOp = rewriter.create<aievec::SRSOp>(loc, vType, curOp.getResult(),
                                                shiftParamOp.getResult());
    SmallVector<Value> concatSources = {srsOp.getResult(), srsOp.getResult()};
    auto concatOp =
        rewriter.create<aievec::ConcatOp>(loc, vecType, concatSources);

    arith::ConstantOp zeroConstOp =
        rewriter.create<arith::ConstantOp>(loc, rewriter.getI32IntegerAttr(0));
    rewriter.replaceOpWithNewOp<aievec::ExtElemOp>(srcOp, scalarType, concatOp,
                                                   zeroConstOp.getResult());
    return success();
  }
};

// Convert a `vector.extract_strided_slice` op on 1D vectors into an
// `aievec.select` + `aievec.ext` op.
struct LowerVectorExtractStridedSliceOpAIEv1Pattern
    : public OpConversionPattern<vector::ExtractStridedSliceOp> {
  using OpConversionPattern<vector::ExtractStridedSliceOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::ExtractStridedSliceOp extractOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto vType = extractOp.getSourceVectorType();
    if (vType.getRank() != 1)
      return failure();

    int64_t stride = cast<IntegerAttr>(adaptor.getStrides()[0]).getInt();
    if (stride != 1)
      return failure();

    // AIE doesn't support select operations on i8
    if (getElementSizeInBits(vType) == 8)
      return extractOp.emitError()
             << "AIEv1 doesn't support select ops on int8 types";

    // We only accept the case where we are extracting a slice half the size of
    // the input vector.
    int64_t size = cast<IntegerAttr>(adaptor.getSizes()[0]).getInt();
    if (vType.getNumElements() != 2 * size)
      return failure();

    int64_t offset = cast<IntegerAttr>(adaptor.getOffsets()[0]).getInt();
    auto selectOp = rewriter.create<aievec::SelectOp>(
        extractOp.getLoc(), vType, adaptor.getVector(),
        buildAttributeListForRotationSelectOp(rewriter, vType, offset));
    rewriter.replaceOpWithNewOp<aievec::ExtOp>(extractOp, extractOp.getType(),
                                               selectOp.getResult(),
                                               rewriter.getI8IntegerAttr(0));

    return success();
  }
};

// Convert a `vector.extract_strided_slice` op on 1D vectors into an
// `aievec.shift` op.
struct LowerVectorExtractStridedSliceOpAIEMLPattern
    : public OpConversionPattern<vector::ExtractStridedSliceOp> {
  using OpConversionPattern<vector::ExtractStridedSliceOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(vector::ExtractStridedSliceOp extractOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto vType = cast<VectorType>(adaptor.getVector().getType());
    if (vType.getRank() != 1)
      return failure();

    int64_t stride = cast<IntegerAttr>(adaptor.getStrides()[0]).getInt();
    if (stride != 1)
      return failure();

    // We only accept the case where we are extracting a slice half the size of
    // the input vector.
    int64_t size = cast<IntegerAttr>(adaptor.getSizes()[0]).getInt();
    if (vType.getNumElements() != 2 * size)
      return failure();

    auto shortVecType = cast<VectorType>(extractOp.getResult().getType());
    auto bottomHalf = rewriter
                          .create<aievec::ExtOp>(
                              extractOp.getLoc(), shortVecType,
                              adaptor.getVector(), rewriter.getI8IntegerAttr(0))
                          .getResult();
    auto topHalf = rewriter
                       .create<aievec::ExtOp>(extractOp.getLoc(), shortVecType,
                                              adaptor.getVector(),
                                              rewriter.getI8IntegerAttr(1))
                       .getResult();
    int64_t offset = cast<IntegerAttr>(adaptor.getOffsets()[0]).getInt();
    int32_t shiftBytes = offset * getElementSizeInBits(vType) / 8;
    auto shiftBytesConstOp = rewriter.create<arith::ConstantOp>(
        extractOp.getLoc(), rewriter.getIntegerType(32),
        rewriter.getI32IntegerAttr(shiftBytes));
    rewriter.replaceOpWithNewOp<aievec::ShiftOp>(
        extractOp, shortVecType, bottomHalf, topHalf, shiftBytesConstOp);

    return success();
  }
};

// Replaces a short UPD op with a wide one followed by an ext op of the bottom
// half.
struct ExpandUPDToUPDAndExtPattern : public OpConversionPattern<aievec::UPDOp> {
  using OpConversionPattern<aievec::UPDOp>::OpConversionPattern;

  ExpandUPDToUPDAndExtPattern(MLIRContext *context)
      : OpConversionPattern<aievec::UPDOp>(context) {}

  LogicalResult
  matchAndRewrite(aievec::UPDOp updOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Verify that we haven't already expanded this one
    if (updOp->hasOneUse() && isa<aievec::ExtOp>(*updOp->getUsers().begin()))
      return failure();

    auto vecType = cast<VectorType>(updOp.getType());
    SmallVector<int64_t, 4> vecShape(vecType.getShape().begin(),
                                     vecType.getShape().end());
    vecShape[vecType.getRank() - 1] *= 2;
    auto longVecType = VectorType::get(vecShape, vecType.getElementType());
    auto newUpdOp = rewriter.create<aievec::UPDOp>(
        updOp.getLoc(), longVecType, adaptor.getSource(), adaptor.getIndices(),
        adaptor.getOffset(), adaptor.getIndex(), adaptor.getVector());
    rewriter.replaceOpWithNewOp<aievec::ExtOp>(
        updOp, vecType, newUpdOp.getResult(), rewriter.getI8IntegerAttr(0));

    return success();
  }
};

// Replaces a wide UPD op followed by an ext op of the bottom half with a short
// UPD op.
struct FuseExtIntoUPDPattern : public OpConversionPattern<aievec::ExtOp> {
  using OpConversionPattern<aievec::ExtOp>::OpConversionPattern;

  FuseExtIntoUPDPattern(MLIRContext *context)
      : OpConversionPattern<aievec::ExtOp>(context) {}

  LogicalResult
  matchAndRewrite(aievec::ExtOp extOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Verify we are extracting the lower half...
    if (extOp.getIndex() != 0)
      return failure();
    // ...of a UPDOp
    auto updOp = dyn_cast<aievec::UPDOp>(extOp.getSource().getDefiningOp());
    if (!updOp)
      return failure();

    // Verify that this is a direct upd -> ext pattern
    if (!updOp->hasOneUse())
      return failure();

    rewriter.replaceOpWithNewOp<aievec::UPDOp>(
        extOp, extOp.getType(), updOp.getSource(), updOp.getIndices(),
        updOp.getOffset(), updOp.getIndex(), updOp.getVector());

    return success();
  }
};

// Lower ExpOp to function call
struct ComputeExpOpByLUTPattern : public OpConversionPattern<math::ExpOp> {
  using OpConversionPattern<math::ExpOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(math::ExpOp expOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(adaptor.getOperand().getType());

    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(srcType);
    if (!isa<FloatType>(scalarType) || laneSize != 16 || elWidth != 16)
      return failure();

    StringRef includeName = "lut_based_ops.h";
    ModuleOp moduleOp = expOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    SmallVector<Value> expOperands = {adaptor.getOperand()};

    rewriter.setInsertionPoint(expOp);
    Type accType = getVectorOpDestType(srcType, /*AIEML =*/true);
    auto funcOp = rewriter.create<emitc::CallOp>(
        expOp.getLoc(), TypeRange{accType}, "getExpBf16", nullptr, nullptr,
        expOperands);
    auto shiftParamOp = rewriter.create<arith::ConstantOp>(
        expOp.getLoc(), rewriter.getI32IntegerAttr(0));
    rewriter.replaceOpWithNewOp<aievec::SRSOp>(
        expOp, srcType, funcOp.getResult(0), shiftParamOp.getResult());

    return success();
  }
};

// Lower the inverse of a float to a function call
// Convert the pattern-
//  %cst = arith.constant 1.000000e+00 : f32
//  %0 = arith.divf %cst, %arg1 : f32
//  %1 = arith.truncf %0 : f32 to bf16
// to -
//  %0 = emitc.call "getInvBf16"(%0) : f32 -> bf16;
struct ComputeInvOpByLUTPattern : public OpConversionPattern<arith::DivFOp> {
  using OpConversionPattern<arith::DivFOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(arith::DivFOp divOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Type srcType = adaptor.getLhs().getType();
    if (!divOp->hasOneUse() || isa<VectorType>(srcType) ||
        !isa<FloatType>(srcType)) {
      return failure();
    }

    if (!isa<arith::TruncFOp>(*divOp->getUsers().begin())) {
      return failure();
    }

    FloatType fType = cast<FloatType>(srcType);

    if (fType.getWidth() != 32) {
      return failure();
    }

    auto constOp = dyn_cast<arith::ConstantOp>(divOp.getLhs().getDefiningOp());
    if (!constOp ||
        constOp.getValue().cast<FloatAttr>().getValue().convertToDouble() !=
            1.0f) {
      return failure();
    }

    StringRef includeName = "lut_based_ops.h";
    ModuleOp moduleOp = divOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    SmallVector<Value> invOperands = {adaptor.getRhs()};
    arith::TruncFOp truncOp = cast<arith::TruncFOp>(*divOp->getUsers().begin());

    rewriter.setInsertionPoint(truncOp);
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        truncOp, TypeRange{truncOp.getResult().getType()}, "getInvBf16",
        nullptr, nullptr, invOperands);
    rewriter.eraseOp(divOp);
    return success();
  }
};

// Convert math.tanh to a function call to compute tanh(x) by look up tables
struct ComputeTanhOpByLUTPattern : public OpConversionPattern<math::TanhOp> {
  using OpConversionPattern<math::TanhOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(math::TanhOp tanhOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(tanhOp.getOperand().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return failure();
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (elWidth != 16 || laneSize != 16) {
      return failure();
    }

    StringRef includeName = "lut_based_ops.h";
    ModuleOp moduleOp = tanhOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    rewriter.setInsertionPoint(tanhOp);
    SmallVector<Value> tanhOperands = {adaptor.getOperand()};
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        tanhOp, TypeRange{tanhOp.getResult().getType()}, "getTanhBf16", nullptr,
        nullptr, tanhOperands);
    return success();
  }
};

// Convert math.sqrt to a function call to compute sqrt(x) for v16bfloat16 and
// v32bfloat16 types
struct ComputeSqrtOpPattern : public OpConversionPattern<math::SqrtOp> {
  using OpConversionPattern<math::SqrtOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(math::SqrtOp sqrtOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(sqrtOp.getOperand().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return failure();
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return failure();
    }

    StringRef includeName = "vec_math.h";
    ModuleOp moduleOp = sqrtOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    rewriter.setInsertionPoint(sqrtOp);
    SmallVector<Value> sqrtOperands = {adaptor.getOperand()};
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        sqrtOp, TypeRange{sqrtOp.getResult().getType()}, "getSqrtBf16", nullptr,
        nullptr, sqrtOperands);
    return success();
  }
};

// Convert math.rsqrt to a function call to compute 1.0f / sqrt(x) for
// v16bfloat16 and v32bfloat16 types
struct ComputeRsqrtOpPattern : public OpConversionPattern<math::RsqrtOp> {
  using OpConversionPattern<math::RsqrtOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(math::RsqrtOp rsqrtOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(rsqrtOp.getOperand().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return failure();
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return failure();
    }

    StringRef includeName = "vec_math.h";
    ModuleOp moduleOp = rsqrtOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    rewriter.setInsertionPoint(rsqrtOp);
    SmallVector<Value> rsqrtOperands = {adaptor.getOperand()};
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        rsqrtOp, TypeRange{rsqrtOp.getResult().getType()}, "getRsqrtBf16",
        nullptr, nullptr, rsqrtOperands);
    return success();
  }
};

// Convert math.erf to a function call to compute erf(x) for v16bfloat16 and
// v32bfloat16 types
struct ComputeErfOpPattern : public OpConversionPattern<math::ErfOp> {
  using OpConversionPattern<math::ErfOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(math::ErfOp erfOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(erfOp.getOperand().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return failure();
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return failure();
    }

    StringRef includeName = "vec_math.h";
    ModuleOp moduleOp = erfOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    rewriter.setInsertionPoint(erfOp);
    SmallVector<Value> erfOperands = {adaptor.getOperand()};
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        erfOp, TypeRange{erfOp.getResult().getType()}, "getErfBf16", nullptr,
        nullptr, erfOperands);
    return success();
  }
};

// Convert math.absf and math.absi to a function call to compute abs(x) for
// v16bfloat16, v32bfloat16, v16float, v16int32, v32int16 and v64int8 types
template <typename SrcOpTy>
struct ComputeAbsOpPattern : public OpConversionPattern<SrcOpTy> {
  using OpConversionPattern<SrcOpTy>::OpConversionPattern;
  using OpAdaptor = typename SrcOpTy::Adaptor;

  LogicalResult
  matchAndRewrite(SrcOpTy absOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    StringRef includeName = "vec_math.h";
    ModuleOp moduleOp = absOp->template getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    rewriter.setInsertionPoint(absOp);
    SmallVector<Value> absOperands = {adaptor.getOperand()};
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        absOp, TypeRange{absOp.getResult().getType()}, "getAbs", nullptr,
        nullptr, absOperands);
    return success();
  }
};

using ComputeAbsFOpPattern = ComputeAbsOpPattern<math::AbsFOp>;
using ComputeAbsIOpPattern = ComputeAbsOpPattern<math::AbsIOp>;

template <typename SrcOpTy>
struct LowerExtOpPattern : public OpConversionPattern<SrcOpTy> {
  using OpConversionPattern<SrcOpTy>::OpConversionPattern;
  using OpAdaptor = typename SrcOpTy::Adaptor;

  LogicalResult
  matchAndRewrite(SrcOpTy extOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(extOp.getIn().getType());
    VectorType dstType = dyn_cast<VectorType>(extOp.getOut().getType());

    auto accType = getVectorOpDestType(srcType, /*AIEML =*/true);
    auto upsOp =
        rewriter.create<aievec::UPSOp>(extOp.getLoc(), accType, extOp.getIn());

    if (dstType.getElementType().getIntOrFloatBitWidth() == 16) {
      auto shiftParamOp = rewriter.create<arith::ConstantOp>(
          extOp.getLoc(), rewriter.getI32IntegerAttr(0));
      rewriter.replaceOpWithNewOp<aievec::SRSOp>(
          extOp, dstType, upsOp.getResult(), shiftParamOp.getResult());
    } else {
      rewriter.replaceOpWithNewOp<aievec::CastOp>(
          extOp, dstType, upsOp.getResult(), /*isResAcc*/ false);
    }
    return success();
  }
};

using LowerExtFOpPattern = LowerExtOpPattern<arith::ExtFOp>;
using LowerExtSIOpPattern = LowerExtOpPattern<arith::ExtSIOp>;

template <typename SrcOpTy>
struct LowerTruncOpPattern : public OpConversionPattern<SrcOpTy> {
  using OpConversionPattern<SrcOpTy>::OpConversionPattern;
  using OpAdaptor = typename SrcOpTy::Adaptor;

  LogicalResult
  matchAndRewrite(SrcOpTy truncOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(truncOp.getIn().getType());
    VectorType dstType = dyn_cast<VectorType>(truncOp.getOut().getType());
    Type scalarType = srcType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    unsigned laneSize = getVectorLaneSize(srcType);
    auto accType = isa<IntegerType>(scalarType) && (elWidth == 32)
                       ? createVectorType(laneSize, scalarType)
                       : getVectorOpDestType(srcType, /*AIEML =*/true);

    auto shiftParamOp = rewriter.create<arith::ConstantOp>(
        truncOp.getLoc(), rewriter.getI32IntegerAttr(0));
    if (elWidth == 16) {
      auto upsOp = rewriter.create<aievec::UPSOp>(truncOp.getLoc(), accType,
                                                  truncOp.getIn());
      rewriter.replaceOpWithNewOp<aievec::SRSOp>(
          truncOp, dstType, upsOp.getResult(), shiftParamOp.getResult());
    } else {
      auto castOp = rewriter.create<aievec::CastOp>(truncOp.getLoc(), accType,
                                                    truncOp.getIn(), true);
      rewriter.replaceOpWithNewOp<aievec::SRSOp>(
          truncOp, dstType, castOp.getResult(), shiftParamOp.getResult());
    }
    return success();
  }
};

using LowerTruncFOpPattern = LowerTruncOpPattern<arith::TruncFOp>;
using LowerTruncIOpPattern = LowerTruncOpPattern<arith::TruncIOp>;

// Check there is an operation chain like-
//
//      %cst_0 = arith.constant dense<1.000000e+00> : vector<16xbf16>
//      %cst_1 = arith.constant 0.000000e+00 : bf16
//      %0 = vector.transfer_read %arg0[%arg2], %cst_1 : memref<1024xbf16>,
//      vector<16xbf16>
//      %1 = arith.negf %0 : vector<16xbf16>
//      %2 = math.exp %1 : vector<16xbf16>
//      %3 = arith.addf %2, %cst_0 : vector<16xbf16>
//      %4 = arith.divf %cst_0, %3 : vector<16xbf16>
//
// so that this operation chain can be converted to a function call to compute
// sigmoid value for v16bfloat16 and v32bfloat16 types
template <typename DivFOpTy>
static bool hasSigmoidComputationChain(DivFOpTy divfOp, arith::NegFOp &negOp) {
  auto constOp = dyn_cast<arith::ConstantOp>(divfOp.getLhs().getDefiningOp());
  if (!constOp) {
    return false;
  }

  auto cstDense = dyn_cast<DenseFPElementsAttr>(constOp.getValue());
  if (!cstDense) {
    return false;
  }

  if (cstDense.template getSplatValue<APFloat>().convertToFloat() != 1.0f) {
    return false;
  }

  Operation *addLvalOp = nullptr;
  Operation *addRvalOp = nullptr;
  // divfOp's rval could be an arith::AddFOp or the pattern like-
  // %1 = aievec.ups %a
  // %2 = aievec.ups %b;
  // %3 = aievec.add_elem %1, %2
  // %4 = aievec.srs %3;
  auto addOp = dyn_cast<arith::AddFOp>(divfOp.getRhs().getDefiningOp());
  if (!addOp) {
    auto srsOp = dyn_cast<aievec::SRSOp>(divfOp.getRhs().getDefiningOp());
    if (!srsOp) {
      return false;
    }
    auto addElemOp =
        dyn_cast<aievec::AddElemOp>(srsOp.getSource().getDefiningOp());
    if (!addElemOp) {
      return false;
    }
    auto lUpsOp = dyn_cast<aievec::UPSOp>(addElemOp.getLhs().getDefiningOp());
    auto rUpsOp = dyn_cast<aievec::UPSOp>(addElemOp.getRhs().getDefiningOp());
    if (!lUpsOp || !rUpsOp) {
      return false;
    }
    addLvalOp = lUpsOp.getSource().getDefiningOp();
    addRvalOp = rUpsOp.getSource().getDefiningOp();
    // One of add operation's operand is a constant op and another operand could
    // be arith::ExpOp or the combination of emitc.call and aievec.srs
    auto addDefOp = isa<arith::ConstantOp>(addLvalOp)
                        ? dyn_cast<aievec::SRSOp>(addRvalOp)
                        : dyn_cast<aievec::SRSOp>(addLvalOp);
    if (!addDefOp) {
      addLvalOp = isa<arith::ConstantOp>(addLvalOp)
                      ? dyn_cast<math::ExpOp>(addRvalOp)
                      : dyn_cast<math::ExpOp>(addLvalOp);
    } else {
      addLvalOp = addDefOp.getSource().getDefiningOp();
    }
    addRvalOp = isa<arith::ConstantOp>(addLvalOp)
                    ? lUpsOp.getSource().getDefiningOp()
                    : rUpsOp.getSource().getDefiningOp();
  } else {
    addLvalOp = addOp.getLhs().getDefiningOp();
    addRvalOp = addOp.getRhs().getDefiningOp();
  }

  if (!addLvalOp || !addRvalOp) {
    return false;
  }

  if (!((isa<math::ExpOp>(addLvalOp) && isa<arith::ConstantOp>(addRvalOp)) ||
        (isa<math::ExpOp>(addRvalOp) && isa<arith::ConstantOp>(addLvalOp)) ||
        (isa<emitc::CallOp>(addLvalOp) &&
         cast<emitc::CallOp>(addLvalOp).getCallee() == "getExpBf16" &&
         isa<arith::ConstantOp>(addRvalOp)) ||
        (isa<emitc::CallOp>(addRvalOp) &&
         cast<emitc::CallOp>(addRvalOp).getCallee() == "getExpBf16" &&
         isa<arith::ConstantOp>(addLvalOp)))) {
    return false;
  }

  constOp = isa<arith::ConstantOp>(addLvalOp)
                ? cast<arith::ConstantOp>(addLvalOp)
                : cast<arith::ConstantOp>(addRvalOp);

  cstDense = dyn_cast<DenseFPElementsAttr>(constOp.getValue());
  if (!cstDense) {
    return false;
  }

  if (cstDense.template getSplatValue<APFloat>().convertToFloat() != 1.0f) {
    return false;
  }

  auto expOp = isa<math::ExpOp>(addLvalOp)
                   ? cast<math::ExpOp>(addLvalOp)
                   : (isa<emitc::CallOp>(addLvalOp)
                          ? cast<emitc::CallOp>(addLvalOp)
                          : (isa<math::ExpOp>(addRvalOp)
                                 ? cast<math::ExpOp>(addRvalOp)
                                 : cast<emitc::CallOp>(addRvalOp)));

  auto expOperand = isa<math::ExpOp>(expOp)
                        ? cast<math::ExpOp>(expOp).getOperand()
                        : *(cast<emitc::CallOp>(expOp).getOperands().begin());
  negOp = dyn_cast<arith::NegFOp>(expOperand.getDefiningOp());

  if (!negOp) {
    return false;
  }

  return true;
}

// Convert the operation chain like-
//
//      %cst_0 = arith.constant dense<1.000000e+00> : vector<16xbf16>
//      %cst_1 = arith.constant 0.000000e+00 : bf16
//      %0 = vector.transfer_read %arg0[%arg2], %cst_1 : memref<1024xbf16>,
//      vector<16xbf16>
//      %1 = arith.negf %0 : vector<16xbf16>
//      %2 = math.exp %1 :vector<16xbf16>
//      %3 = arith.addf %2, %cst_0 : vector<16xbf16>
//      %4 = arith.divf %cst_0, %3 : vector<16xbf16>
//
// to a function call to compute sigmoid value for v16bfloat16 and
// v32bfloat16 types
struct ComputeSigmoidOpPattern : public OpConversionPattern<arith::DivFOp> {
  using OpConversionPattern<arith::DivFOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(arith::DivFOp divfOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(adaptor.getLhs().getType());

    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();
    if (!isa<FloatType>(scalarType)) {
      return failure();
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return failure();
    }

    arith::NegFOp negOp = nullptr;
    if (!hasSigmoidComputationChain(adaptor, negOp)) {
      return failure();
    }

    StringRef includeName = "vec_math.h";
    ModuleOp moduleOp = divfOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    rewriter.setInsertionPoint(divfOp);
    SmallVector<Value> sigmoidOperands = {negOp.getOperand()};
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        divfOp, TypeRange{adaptor.getLhs().getType()}, "getSigmoidBf16",
        nullptr, nullptr, sigmoidOperands);

    return success();
  }
};

// Convert math.ceil to a function call to compute ceil(x) for v16bfloat16
struct ComputeCeilOpPattern : public OpConversionPattern<math::CeilOp> {
  using OpConversionPattern<math::CeilOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(math::CeilOp ceilOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(ceilOp.getOperand().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return failure();
    }
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return failure();
    }

    StringRef includeName = "vec_math.h";
    ModuleOp moduleOp = ceilOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    rewriter.setInsertionPoint(ceilOp);
    SmallVector<Value> ceilOperands = {adaptor.getOperand()};
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        ceilOp, TypeRange{ceilOp.getResult().getType()}, "getCeilBf16", nullptr,
        nullptr, ceilOperands);
    return success();
  }
};

// Convert math.floor to a function call to compute floor(x) for v16bfloat16
struct ComputeFloorOpPattern : public OpConversionPattern<math::FloorOp> {
  using OpConversionPattern<math::FloorOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(math::FloorOp floorOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(floorOp.getOperand().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return failure();
    }
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return failure();
    }

    StringRef includeName = "vec_math.h";
    ModuleOp moduleOp = floorOp->getParentOfType<mlir::ModuleOp>();
    rewriter.setInsertionPointToStart(
        &moduleOp.getRegion().getBlocks().front());
    rewriter.create<emitc::IncludeOp>(moduleOp.getLoc(), includeName, false);

    rewriter.setInsertionPoint(floorOp);
    SmallVector<Value> floorOperands = {adaptor.getOperand()};
    rewriter.replaceOpWithNewOp<emitc::CallOp>(
        floorOp, TypeRange{floorOp.getResult().getType()}, "getFloorBf16",
        nullptr, nullptr, floorOperands);
    return success();
  }
};

// Convert arith.negf to aievec.neg to negate the vector for v16bfloat16 and
// v16float types.
struct ComputeNegOpPattern : public OpConversionPattern<arith::NegFOp> {
  using OpConversionPattern<arith::NegFOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(arith::NegFOp negOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(negOp.getOperand().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return failure();
    }
    unsigned laneSize = getVectorLaneSize(srcType);

    if (laneSize != 16) {
      return failure();
    }

    Location loc = negOp.getLoc();
    auto accType = getVectorOpDestType(srcType, /*AIEML =*/true);

    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth == 16) {
      auto upsOp =
          rewriter.create<aievec::UPSOp>(loc, accType, adaptor.getOperand());

      auto aieNegOp =
          rewriter.create<aievec::NegOp>(loc, accType, upsOp.getResult());

      auto shiftParamOp = rewriter.create<arith::ConstantOp>(
          negOp.getLoc(), rewriter.getI32IntegerAttr(0));
      rewriter.replaceOpWithNewOp<aievec::SRSOp>(
          negOp, srcType, aieNegOp.getResult(), shiftParamOp.getResult());
    } else {
      auto castOp = rewriter.create<aievec::CastOp>(
          loc, accType, adaptor.getOperand(), /*isResAcc*/ true);
      auto aieNegOp =
          rewriter.create<aievec::NegOp>(loc, accType, castOp.getResult());
      rewriter.replaceOpWithNewOp<aievec::CastOp>(
          negOp, srcType, aieNegOp.getResult(), /*isResAcc*/ false);
    }
    return success();
  }
};

// Check whether the value of constant operation is int type and the dense value
// is -1.
static bool hasConstNegOneValue(arith::ConstantOp constOp, unsigned elWidth) {
  if (!constOp) {
    return false;
  }
  auto cstDense = dyn_cast<DenseIntElementsAttr>(constOp.getValue());
  if (!cstDense) {
    return false;
  }

  if (elWidth == 32) {
    return cstDense.getSplatValue<int32_t>() == -1;
  } else if (elWidth == 16) {
    return cstDense.getSplatValue<int16_t>() == -1;
  } else if (elWidth == 8) {
    return cstDense.getSplatValue<int8_t>() == -1;
  }
  return false;
}

// Convert arith.xori to aievec.bxor to compute bitwise xor of two vectors for
// integer types
struct ComputeBxorAndBnegOpPattern : public OpConversionPattern<arith::XOrIOp> {
  using OpConversionPattern<arith::XOrIOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(arith::XOrIOp xorOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(xorOp.getLhs().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    if (!isa<IntegerType>(scalarType)) {
      return failure();
    }
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (laneSize * elWidth != 512) {
      return failure();
    }

    auto lhsConstOp =
        dyn_cast<arith::ConstantOp>(xorOp.getLhs().getDefiningOp());
    auto rhsConstOp =
        dyn_cast<arith::ConstantOp>(xorOp.getRhs().getDefiningOp());

    // If one of operands in xorOp is a constant -1, xorOp will be replaced with
    // aievec::BnegOp.
    if ((lhsConstOp && hasConstNegOneValue(lhsConstOp, elWidth)) ||
        (rhsConstOp && hasConstNegOneValue(rhsConstOp, elWidth))) {
      Value val = hasConstNegOneValue(lhsConstOp, elWidth) ? adaptor.getRhs()
                                                           : adaptor.getLhs();
      rewriter.replaceOpWithNewOp<aievec::BnegOp>(xorOp, srcType, val);
    } else {
      rewriter.replaceOpWithNewOp<aievec::BxorOp>(
          xorOp, srcType, adaptor.getLhs(), adaptor.getRhs());
    }
    return success();
  }
};

template <typename SrcOpTy, typename DstOpTy>
struct ComputeBandAndBorOpPattern : public OpConversionPattern<SrcOpTy> {
  using OpConversionPattern<SrcOpTy>::OpConversionPattern;
  using OpAdaptor = typename SrcOpTy::Adaptor;

  LogicalResult
  matchAndRewrite(SrcOpTy srcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(srcOp.getLhs().getType());
    if (!srcType) {
      return failure();
    }
    Type scalarType = srcType.getElementType();
    if (!isa<IntegerType>(scalarType)) {
      return failure();
    }
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (laneSize * elWidth != 512) {
      return failure();
    }

    rewriter.replaceOpWithNewOp<DstOpTy>(srcOp, srcOp.getResult().getType(),
                                         adaptor.getLhs(), adaptor.getRhs());

    return success();
  }
};

using ComputeBorOpPattern =
    ComputeBandAndBorOpPattern<arith::OrIOp, aievec::BorOp>;
using ComputeBandOpPattern =
    ComputeBandAndBorOpPattern<arith::AndIOp, aievec::BandOp>;

// Convert arith.shrsi to a combination of aievec.ups and aievec.srs to compute
// arithmetic right shift for integer types. Currently, only support the shift
// value with a broadcast vector.
struct ComputeSignedIntRightShiftOpPattern
    : public OpConversionPattern<arith::ShRSIOp> {
  using OpConversionPattern<arith::ShRSIOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(arith::ShRSIOp rsOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    VectorType srcType = dyn_cast<VectorType>(adaptor.getLhs().getType());
    if (!srcType) {
      return failure();
    }

    Type scalarType = srcType.getElementType();

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    if (laneSize * elWidth != 512) {
      return failure();
    }

    auto bcastOp =
        dyn_cast<aievec::BroadcastOp>(adaptor.getRhs().getDefiningOp());

    if (!bcastOp) {
      return failure();
    }

    arith::ConstantOp constOp = rewriter.create<arith::ConstantOp>(
        bcastOp.getLoc(), rewriter.getI32IntegerAttr(bcastOp.getIdx()));

    auto extElemOp = rewriter.create<aievec::ExtElemOp>(
        bcastOp.getLoc(), scalarType, bcastOp, constOp.getResult());

    Location loc = rsOp.getLoc();

    // The vector with v64int8 type can be divided into two v32int8 vectors and
    // be processed individually and be concatenated at the end.
    if (elWidth == 8) {
      VectorType halfSrcType = createVectorType(laneSize / 2, scalarType);

      auto rsOpLow =
          rewriter.create<aievec::ExtOp>(loc, halfSrcType, adaptor.getLhs(), 0);
      auto rsOpHigh =
          rewriter.create<aievec::ExtOp>(loc, halfSrcType, adaptor.getLhs(), 1);

      Type accType = getVectorOpDestType(halfSrcType, /*AIEML =*/true);

      auto upsOpLow =
          rewriter.create<aievec::UPSOp>(loc, accType, rsOpLow.getResult());

      auto srsOpLow = rewriter.create<aievec::SRSOp>(
          loc, halfSrcType, upsOpLow.getResult(), extElemOp.getResult());

      auto upsOpHigh =
          rewriter.create<aievec::UPSOp>(loc, accType, rsOpHigh.getResult());

      auto srsOpHigh = rewriter.create<aievec::SRSOp>(
          loc, halfSrcType, upsOpHigh.getResult(), extElemOp.getResult());

      SmallVector<Value> inputSources = {srsOpLow.getResult(),
                                         srsOpHigh.getResult()};
      rewriter.replaceOpWithNewOp<aievec::ConcatOp>(rsOp, srcType,
                                                    inputSources);
    } else {
      Type accType = getVectorOpDestType(srcType, /*AIEML =*/true);

      auto upsOp =
          rewriter.create<aievec::UPSOp>(loc, accType, adaptor.getLhs());

      rewriter.replaceOpWithNewOp<aievec::SRSOp>(
          rsOp, srcType, upsOp.getResult(), extElemOp.getResult());
    }
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Pattern collection
//===----------------------------------------------------------------------===//

static void populateAIEVecV1ConversionPatterns(RewritePatternSet &patterns,
                                               AnalysisManager &am) {
  patterns.add<LowerVectorTransferReadToAIEUPD>(patterns.getContext(), 128, 512,
                                                128, 256);
  // clang-format off
  patterns.add<LowerVectorAddIOpToAIEVecAddOp,
               LowerVectorSubIOpToAIEVecSubOp,
               LowerVectorMulIOpToAIEVecMulOp,
               LowerVectorAddFOpToAIEVecAddOp,
               LowerVectorSubFOpToAIEVecSubOp,
               LowerVectorMulFOpToAIEVecMulOp,
               ConvertMulAddToAIEVecFMAOpPattern,
               FoldBroadcastToFMAOp,
               LowerVectorExtractStridedSliceOpAIEv1Pattern>(patterns.getContext());
  // clang-format on
}

static void populateAIEVecV2ConversionPatterns(RewritePatternSet &patterns,
                                               AnalysisManager &am) {
  patterns.add<LowerVectorTransferReadToAIEUPD>(patterns.getContext(), 128,
                                                1024, 256, 1024);
  // clang-format off
  patterns.add<
      LowerVectorAddIOpToAIEVecAddElemOp,
      LowerVectorSubIOpToAIEVecSubElemOp,
      ComputeExpOpByLUTPattern,
      ComputeInvOpByLUTPattern,
      ComputeTanhOpByLUTPattern,
      ComputeSqrtOpPattern,
      ComputeRsqrtOpPattern,
      ComputeErfOpPattern,
      ComputeAbsFOpPattern,
      ComputeAbsIOpPattern,
      ComputeSigmoidOpPattern,
      ComputeCeilOpPattern,
      ComputeFloorOpPattern,
      ComputeNegOpPattern,
      ComputeBxorAndBnegOpPattern,
      ComputeBorOpPattern,
      ComputeBandOpPattern,
      ComputeSignedIntRightShiftOpPattern,
      ConvertMulIToAIEVecMulElemOpPattern,
      LowerVectorAddFOpToAIEVecAddElemOp,
      LowerVectorSubFOpToAIEVecSubElemOp,
      ConvertMulFToAIEVecMulElemOpPattern,
      LowerVectorMinSIOpToAIEVecMinOp,
      LowerVectorMinimumFOpToAIEVecMinOp,
      LowerVectorMaxSIOpToAIEVecMaxOp,
      LowerVectorMaximumFOpToAIEVecMaxOp,
      LowerVectorCmpIOpToAIEVecCmpOp,
      LowerVectorCmpFOpToAIEVecCmpOp,
      LowerVectorSelectOpToAIEVecSelOp,
      LowerVectorReductionMinOp,
      LowerVectorReductionMaxOp,
      LowerVectorReductionAddIntOp,
      LowerVectorReductionAddFloatOp,
      LowerVectorReductionAddBfloat16Op,
      FoldVectorExtractAndBroadcastToAIEBroadcast,
      ConvertBroadcastToAIEBroadcast,
      ConvertMulAddToAIEVecFMAElemOpPattern,
      LowerVectorExtractStridedSliceOpAIEMLPattern>(patterns.getContext());
  // clang-format on
}

//===----------------------------------------------------------------------===//
// Legalizations
//===----------------------------------------------------------------------===//

// TODO: Review the validity of these legalizations beyond basic cases.

static bool isInSigmoidOperationChain(math::ExpOp expOp) {
  auto negOp = dyn_cast<arith::NegFOp>(expOp.getOperand().getDefiningOp());

  if (!negOp) {
    return false;
  }

  arith::AddFOp addOp = nullptr;
  for (Operation *user : expOp->getUsers()) {
    addOp = dyn_cast<arith::AddFOp>(user);
    if (addOp) {
      break;
    }
  }

  if (!addOp) {
    return false;
  }

  auto addLvalOp = addOp.getLhs().getDefiningOp();
  auto addRvalOp = addOp.getRhs().getDefiningOp();

  if (!((isa<math::ExpOp>(addLvalOp) && isa<arith::ConstantOp>(addRvalOp)) ||
        (isa<math::ExpOp>(addRvalOp) && isa<arith::ConstantOp>(addLvalOp)))) {
    return false;
  }

  auto constOp = isa<arith::ConstantOp>(addLvalOp)
                     ? cast<arith::ConstantOp>(addLvalOp)
                     : cast<arith::ConstantOp>(addRvalOp);

  auto cstDense = dyn_cast<DenseFPElementsAttr>(constOp.getValue());
  if (!cstDense) {
    return false;
  }

  if (cstDense.getSplatValue<APFloat>().convertToFloat() != 1.0f) {
    return false;
  }

  arith::DivFOp divOp = nullptr;
  for (Operation *user : addOp->getUsers()) {
    divOp = dyn_cast<arith::DivFOp>(user);
    if (divOp) {
      break;
    }
  }

  if (!divOp) {
    return false;
  }

  constOp = dyn_cast<arith::ConstantOp>(divOp.getLhs().getDefiningOp());
  if (!constOp) {
    return false;
  }

  cstDense = dyn_cast<DenseFPElementsAttr>(constOp.getValue());
  if (!cstDense) {
    return false;
  }

  if (cstDense.getSplatValue<APFloat>().convertToFloat() != 1.0f) {
    return false;
  }
  return true;
}

static void configureAIEVecCommonLegalizations(ConversionTarget &target,
                                               AnalysisManager &am) {
  target.addLegalDialect<xilinx::aievec::AIEVecDialect, arith::ArithDialect,
                         emitc::EmitCDialect>();
  target.addIllegalOp<vector::TransferReadOp>();
  target.addIllegalOp<vector::ExtractStridedSliceOp>();
  target.addDynamicallyLegalOp<math::ExpOp>([](math::ExpOp expOp) {
    VectorType srcType = dyn_cast<VectorType>(expOp.getOperand().getType());
    if (!srcType) {
      return true;
    }
    Type scalarType = srcType.getElementType();
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(srcType);
    if (!isa<FloatType>(scalarType) || laneSize != 16 || elWidth != 16)
      return true;

    if (expOp->hasOneUse() && isInSigmoidOperationChain(expOp)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<math::TanhOp>([](math::TanhOp tanhOp) {
    VectorType srcType = dyn_cast<VectorType>(tanhOp.getOperand().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return true;
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth != 16 || laneSize != 16) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<math::SqrtOp>([](math::SqrtOp sqrtOp) {
    VectorType srcType = dyn_cast<VectorType>(sqrtOp.getOperand().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return true;
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<math::RsqrtOp>([](math::RsqrtOp rsqrtOp) {
    VectorType srcType = dyn_cast<VectorType>(rsqrtOp.getOperand().getType());
    Type scalarType = srcType.getElementType();
    if (!srcType || !isa<FloatType>(scalarType)) {
      return true;
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<math::ErfOp>([](math::ErfOp erfOp) {
    VectorType srcType = dyn_cast<VectorType>(erfOp.getOperand().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return true;
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<math::AbsFOp>([](math::AbsFOp absfOp) {
    VectorType srcType = dyn_cast<VectorType>(absfOp.getOperand().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth * laneSize != 512 && elWidth * laneSize != 256) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<math::AbsIOp>([](math::AbsIOp absiOp) {
    VectorType srcType = dyn_cast<VectorType>(absiOp.getOperand().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth * laneSize != 512 && elWidth * laneSize != 256) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<arith::DivFOp>([](arith::DivFOp divfOp) {
    VectorType srcType = dyn_cast<VectorType>(divfOp.getLhs().getType());

    if (!srcType) {
      Type scalarType = divfOp.getLhs().getType();
      if (!divfOp->hasOneUse() || !isa<FloatType>(scalarType)) {
        return true;
      }

      if (!isa<arith::TruncFOp>(*divfOp->getUsers().begin())) {
        return true;
      }

      FloatType fType = cast<FloatType>(scalarType);

      if (fType.getWidth() != 32) {
        return true;
      }

      auto constOp =
          dyn_cast<arith::ConstantOp>(divfOp.getLhs().getDefiningOp());
      if (!constOp ||
          constOp.getValue().cast<FloatAttr>().getValue().convertToDouble() !=
              1.0f) {
        return true;
      }
    } else {

      Type scalarType = srcType.getElementType();
      if (!isa<FloatType>(scalarType)) {
        return true;
      }

      unsigned laneSize = getVectorLaneSize(srcType);
      unsigned elWidth = scalarType.getIntOrFloatBitWidth();

      if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
        return true;
      }

      arith::NegFOp negOp = nullptr;
      if (!hasSigmoidComputationChain(divfOp, negOp)) {
        return true;
      }
    }
    return false;
  });

  target.addDynamicallyLegalOp<math::CeilOp>([](math::CeilOp ceilOp) {
    VectorType srcType = dyn_cast<VectorType>(ceilOp.getOperand().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return true;
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<math::FloorOp>([](math::FloorOp floorOp) {
    VectorType srcType = dyn_cast<VectorType>(floorOp.getOperand().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return true;
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();
    if (elWidth != 16 || (laneSize != 16 && laneSize != 32)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<arith::NegFOp>([](arith::NegFOp negOp) {
    VectorType srcType = dyn_cast<VectorType>(negOp.getOperand().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<FloatType>(scalarType)) {
      return true;
    }

    unsigned laneSize = getVectorLaneSize(srcType);
    return laneSize != 16;
  });

  target.addDynamicallyLegalOp<arith::XOrIOp>([](arith::XOrIOp xorOp) {
    VectorType srcType = dyn_cast<VectorType>(xorOp.getLhs().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<IntegerType>(scalarType)) {
      return true;
    }
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    return laneSize * elWidth != 512;
  });

  target.addDynamicallyLegalOp<arith::OrIOp>([](arith::OrIOp orOp) {
    VectorType srcType = dyn_cast<VectorType>(orOp.getLhs().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<IntegerType>(scalarType)) {
      return true;
    }
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    return laneSize * elWidth != 512;
  });

  target.addDynamicallyLegalOp<arith::ShRSIOp>([](arith::ShRSIOp rsOp) {
    VectorType srcType = dyn_cast<VectorType>(rsOp.getLhs().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    return laneSize * elWidth != 512;
  });

  target.addDynamicallyLegalOp<arith::AndIOp>([](arith::AndIOp andOp) {
    VectorType srcType = dyn_cast<VectorType>(andOp.getLhs().getType());
    if (!srcType) {
      return true;
    }

    Type scalarType = srcType.getElementType();

    if (!isa<IntegerType>(scalarType)) {
      return true;
    }
    unsigned laneSize = getVectorLaneSize(srcType);
    unsigned elWidth = scalarType.getIntOrFloatBitWidth();

    return laneSize * elWidth != 512;
  });

  target.addDynamicallyLegalOp<arith::AddIOp>(
      [](arith::AddIOp op) { return !isa<VectorType>(op.getType()); });
  target.addDynamicallyLegalOp<arith::AddFOp>(
      [](arith::AddFOp op) { return !isa<VectorType>(op.getType()); });
  target.addDynamicallyLegalOp<arith::SubIOp>(
      [](arith::SubIOp op) { return !isa<VectorType>(op.getType()); });
  target.addDynamicallyLegalOp<arith::SubFOp>(
      [](arith::SubFOp op) { return !isa<VectorType>(op.getType()); });
}

static void configureAIEVecV1Legalizations(ConversionTarget &target,
                                           AnalysisManager &am) {
  target.addDynamicallyLegalOp<arith::MulIOp>(
      [](arith::MulIOp op) { return !isa<VectorType>(op.getType()); });
  target.addDynamicallyLegalOp<arith::MulFOp>(
      [](arith::MulFOp op) { return !isa<VectorType>(op.getType()); });
  target.addDynamicallyLegalOp<aievec::FMAOp>([](xilinx::aievec::FMAOp op) {
    auto lhsDefOp = op.getLhs().getDefiningOp();
    aievec::ConcatOp concatOp = nullptr;
    if (lhsDefOp)
      concatOp = dyn_cast<aievec::ConcatOp>(op.getLhs().getDefiningOp());
    if (!concatOp)
      return true;
    vector::BroadcastOp srcBcast = nullptr;
    auto lhsOp = concatOp.getSources()[0].getDefiningOp();
    if (lhsOp)
      srcBcast = dyn_cast<vector::BroadcastOp>(lhsOp);
    if (!srcBcast) {
      auto rhsOp = op.getRhs().getDefiningOp();
      if (!rhsOp)
        return true;
      srcBcast = dyn_cast<vector::BroadcastOp>(rhsOp);
    }
    if (srcBcast) {
      auto srcOp = srcBcast.getSource().getDefiningOp();
      if (srcOp)
        return !isa<vector::ExtractOp>(srcOp);
    }
    return true;
  });
  target.addDynamicallyLegalOp<aievec::AddOp>([](aievec::AddOp op) {
    auto lSrsOp = op.getLhs().getDefiningOp<aievec::SRSOp>();
    auto rSrsOp = op.getRhs().getDefiningOp<aievec::SRSOp>();
    return (!lSrsOp || !lSrsOp.getSource().getDefiningOp<aievec::MulOp>()) &&
           (!rSrsOp || !rSrsOp.getSource().getDefiningOp<aievec::MulOp>());
  });
  target.addLegalDialect<memref::MemRefDialect>();
}

static void configureAIEVecV2Legalizations(ConversionTarget &target,
                                           AnalysisManager &am) {
  target.addLegalOp<UnrealizedConversionCastOp>();

  // A set recording the vector lane size and element width supported
  llvm::SmallSet<std::pair<unsigned, unsigned>, 16> laneSizeElWidthPairSet;
  laneSizeElWidthPairSet.insert({64, 8});
  laneSizeElWidthPairSet.insert({32, 16});
  laneSizeElWidthPairSet.insert({16, 32});
  laneSizeElWidthPairSet.insert({32, 32});

  // A set recording the element width supported
  llvm::SmallSet<unsigned, 16> elWidthSet;
  elWidthSet.insert(8);
  elWidthSet.insert(16);
  elWidthSet.insert(32);

  target.addDynamicallyLegalOp<arith::AddIOp>([=](arith::AddIOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    return !laneSizeElWidthPairSet.count(
        std::make_pair(laneSize, resultElWidth));
  });

  target.addDynamicallyLegalOp<arith::SubIOp>([=](arith::SubIOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    return !laneSizeElWidthPairSet.count(
        std::make_pair(laneSize, resultElWidth));
  });

  target.addDynamicallyLegalOp<arith::AddFOp>([](arith::AddFOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    unsigned laneSize = getVectorLaneSize(resultType);
    return laneSize != 16;
  });

  target.addDynamicallyLegalOp<arith::SubFOp>([](arith::SubFOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    unsigned laneSize = getVectorLaneSize(resultType);
    return laneSize != 16;
  });

  target.addDynamicallyLegalOp<arith::MulIOp>([](arith::MulIOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto isAddOp = [&](Operation *op) { return isa<arith::AddIOp>(op); };
    // Verify it is not a part of MAC
    if (op->hasOneUse() && llvm::any_of(op->getUsers(), isAddOp))
      return true;

    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    return (laneSize != 32 || (resultElWidth != 16 && resultElWidth != 8)) &&
           ((laneSize != 16 && laneSize != 32) || resultElWidth != 32);
  });

  target.addDynamicallyLegalOp<arith::MulFOp>([](arith::MulFOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto isAddOp = [&](Operation *op) { return isa<arith::AddFOp>(op); };
    // Verify it is not a part of FMA
    if (op->hasOneUse() && llvm::any_of(op->getUsers(), isAddOp))
      return true;

    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    return (laneSize != 16 || (resultElWidth != 16 && resultElWidth != 32));
  });

  target.addDynamicallyLegalOp<arith::MinSIOp>([=](arith::MinSIOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    return !(elWidthSet.count(resultElWidth) &&
             laneSize * resultElWidth == 512);
  });

  target.addDynamicallyLegalOp<arith::MaxSIOp>([=](arith::MaxSIOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    return !(elWidthSet.count(resultElWidth) &&
             laneSize * resultElWidth == 512);
  });

  target.addDynamicallyLegalOp<arith::MinimumFOp>([=](arith::MinimumFOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    return !(elWidthSet.count(resultElWidth) &&
             laneSize * resultElWidth == 512);
  });

  target.addDynamicallyLegalOp<arith::MaximumFOp>([=](arith::MaximumFOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    return !(elWidthSet.count(resultElWidth) &&
             laneSize * resultElWidth == 512);
  });

  target.addDynamicallyLegalOp<arith::CmpIOp>([=](arith::CmpIOp op) {
    auto lhsType = dyn_cast<VectorType>(op.getLhs().getType());
    if (!lhsType) {
      return true;
    }
    auto lhsElWidth = lhsType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(lhsType);

    if (!(elWidthSet.count(lhsElWidth) && laneSize * lhsElWidth == 512)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<arith::CmpFOp>([=](arith::CmpFOp op) {
    auto lhsType = dyn_cast<VectorType>(op.getLhs().getType());
    if (!lhsType) {
      return true;
    }
    auto lhsElWidth = lhsType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(lhsType);

    if (!(elWidthSet.count(lhsElWidth) && laneSize * lhsElWidth == 512)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<arith::SelectOp>([=](arith::SelectOp op) {
    auto resultType = dyn_cast<VectorType>(op.getType());
    if (!resultType) {
      return true;
    }
    auto resultElWidth = resultType.getElementType().getIntOrFloatBitWidth();
    unsigned laneSize = getVectorLaneSize(resultType);

    if (!(elWidthSet.count(resultElWidth) && laneSize * resultElWidth == 512)) {
      return true;
    }

    return false;
  });

  target.addDynamicallyLegalOp<vector::ReductionOp>(
      [=](vector::ReductionOp op) {
        auto kind = op.getKind();

        if (kind != vector::CombiningKind::ADD &&
            kind != vector::CombiningKind::MINSI &&
            kind != vector::CombiningKind::MINUI &&
            kind != vector::CombiningKind::MINF &&
            kind != vector::CombiningKind::MAXSI &&
            kind != vector::CombiningKind::MAXUI &&
            kind != vector::CombiningKind::MAXF) {
          return true;
        }

        VectorType vType = dyn_cast<VectorType>(op.getVector().getType());
        if (!vType)
          return true;

        llvm::SmallSet<std::pair<unsigned, signed>, 16> laneSizeElWidthPairSet;
        laneSizeElWidthPairSet.insert({64, 8});
        laneSizeElWidthPairSet.insert({32, 16});
        laneSizeElWidthPairSet.insert({32, 32});
        laneSizeElWidthPairSet.insert({16, 32});

        Type scalarType = vType.getElementType();
        unsigned elWidth = scalarType.getIntOrFloatBitWidth();
        unsigned laneSize = getVectorLaneSize(vType);

        if (scalarType.isa<IntegerType>() &&
            !laneSizeElWidthPairSet.count(std::make_pair(laneSize, elWidth)))
          return true;

        if (scalarType.isa<FloatType>() && laneSize != 16 && laneSize != 32)
          return true;

        return false;
      });
}

//===----------------------------------------------------------------------===//
// Lowering passes
//===----------------------------------------------------------------------===//

/// Lower incoming vector operations into their corresponding AIE vector
/// intrinsics.
struct LowerVectorToAIEVec
    : public PassWrapper<LowerVectorToAIEVec, OperationPass<>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerVectorToAIEVec)

  LowerVectorToAIEVec() = default;
  LowerVectorToAIEVec(const LowerVectorToAIEVec &pass) : PassWrapper(pass) {}

  LowerVectorToAIEVec(const LowerVectorToAIEVecOptions &options)
      : LowerVectorToAIEVec() {
    aieTarget = options.aieTarget;
  }

  // In case we want to register this pass as a standalone pass for test
  // purposes.
  StringRef getArgument() const final { return "test-lower-vector-to-aievec"; }
  StringRef getDescription() const final {
    return "Lower vector operations to AIE vector intrinsics";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<affine::AffineDialect, xilinx::aievec::AIEVecDialect,
                    arith::ArithDialect, memref::MemRefDialect, scf::SCFDialect,
                    vector::VectorDialect, emitc::EmitCDialect>();
  }

  Option<std::string> aieTarget{
      *this, "aie-target",
      llvm::cl::desc("Select AIE version: \"aie\" or \"aieml\". This will "
                     "determine the vector size and available operations."),
      llvm::cl::init("aie")};

  void runOnOperation() override {
    auto op = getOperation();
    MLIRContext *context = &getContext();
    RewritePatternSet patterns(context);
    ConversionTarget target(*context);
    AIEArch aieVersion = AIEArch::AIE;
    if (!aieTarget.empty()) {
      std::string target = aieTarget;
      if (target == "aieml") {
        aieVersion = AIEArch::AIE_ML;
      } else if (target != "aie") {
        op->emitError() << "unknown AIE target '" << aieTarget << "'";
        signalPassFailure();
        return;
      }
    }

    AnalysisManager am = getAnalysisManager();
    configureAIEVecCommonLegalizations(target, am);
    if (aieVersion == AIEArch::AIE) {
      populateAIEVecV1ConversionPatterns(patterns, am);
      configureAIEVecV1Legalizations(target, am);
    } else {
      populateAIEVecV2ConversionPatterns(patterns, am);
      configureAIEVecV2Legalizations(target, am);
    }

    if (failed(applyPartialConversion(op, target, std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

static std::unique_ptr<::mlir::Pass>
createLowerVectorToAIEVec(const LowerVectorToAIEVecOptions &options) {
  return std::make_unique<LowerVectorToAIEVec>(options);
}

//===---------------------------------------------------------------------------
// Custom canonicalization passes
//===---------------------------------------------------------------------------

struct ProcessExtOpsPass
    : public PassWrapper<ProcessExtOpsPass, OperationPass<>> {

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    RewritePatternSet patterns(context);
    ConversionTarget target(*context);
    patterns.add<LowerExtFOpPattern, LowerExtSIOpPattern, LowerTruncFOpPattern,
                 LowerTruncIOpPattern>(patterns.getContext());
    target.addLegalDialect<aievec::AIEVecDialect, arith::ArithDialect>();
    target.addDynamicallyLegalOp<arith::ExtFOp>([](arith::ExtFOp extfOp) {
      VectorType srcType = dyn_cast<VectorType>(extfOp.getIn().getType());
      VectorType dstType = dyn_cast<VectorType>(extfOp.getOut().getType());
      if (!srcType || !dstType) {
        return true;
      }

      Type srcScalarType = srcType.getElementType();
      Type dstScalarType = dstType.getElementType();

      if (!isa<FloatType>(srcScalarType) || !isa<FloatType>(dstScalarType)) {
        return true;
      }

      unsigned srcLaneSize = getVectorLaneSize(srcType);
      unsigned dstLaneSize = getVectorLaneSize(dstType);

      unsigned srcElWidth = srcScalarType.getIntOrFloatBitWidth();
      unsigned dstElWidth = dstScalarType.getIntOrFloatBitWidth();

      if (srcElWidth != 16 || srcLaneSize != 16 || dstElWidth != 32 ||
          dstLaneSize != 16) {
        return true;
      }

      return false;
    });

    target.addDynamicallyLegalOp<arith::ExtSIOp>([](arith::ExtSIOp extsiOp) {
      VectorType srcType = dyn_cast<VectorType>(extsiOp.getIn().getType());
      VectorType dstType = dyn_cast<VectorType>(extsiOp.getOut().getType());
      if (!srcType || !dstType) {
        return true;
      }

      Type srcScalarType = srcType.getElementType();
      Type dstScalarType = dstType.getElementType();

      if (!isa<IntegerType>(srcScalarType) ||
          !isa<IntegerType>(dstScalarType)) {
        return true;
      }

      unsigned srcLaneSize = getVectorLaneSize(srcType);
      unsigned dstLaneSize = getVectorLaneSize(dstType);

      unsigned srcElWidth = srcScalarType.getIntOrFloatBitWidth();
      unsigned dstElWidth = dstScalarType.getIntOrFloatBitWidth();

      if (!(srcLaneSize == 32 && (dstElWidth > srcElWidth) &&
            (dstLaneSize == srcLaneSize))) {
        return true;
      }

      return false;
    });

    target.addDynamicallyLegalOp<arith::TruncFOp>([](arith::TruncFOp truncfOp) {
      VectorType srcType = dyn_cast<VectorType>(truncfOp.getIn().getType());
      VectorType dstType = dyn_cast<VectorType>(truncfOp.getOut().getType());
      if (!srcType || !dstType) {
        return true;
      }

      Type srcScalarType = srcType.getElementType();
      Type dstScalarType = dstType.getElementType();

      if (!isa<FloatType>(srcScalarType) || !isa<FloatType>(dstScalarType)) {
        return true;
      }

      unsigned srcLaneSize = getVectorLaneSize(srcType);
      unsigned dstLaneSize = getVectorLaneSize(dstType);

      unsigned srcElWidth = srcScalarType.getIntOrFloatBitWidth();
      unsigned dstElWidth = dstScalarType.getIntOrFloatBitWidth();

      if (srcElWidth != 32 || srcLaneSize != 16 || dstElWidth != 16 ||
          dstLaneSize != 16) {
        return true;
      }

      return false;
    });

    target.addDynamicallyLegalOp<arith::TruncIOp>([](arith::TruncIOp trunciOp) {
      VectorType srcType = dyn_cast<VectorType>(trunciOp.getIn().getType());
      VectorType dstType = dyn_cast<VectorType>(trunciOp.getOut().getType());
      if (!srcType || !dstType) {
        return true;
      }

      Type srcScalarType = srcType.getElementType();
      Type dstScalarType = dstType.getElementType();

      if (!isa<IntegerType>(srcScalarType) ||
          !isa<IntegerType>(dstScalarType)) {
        return true;
      }

      unsigned srcLaneSize = getVectorLaneSize(srcType);
      unsigned dstLaneSize = getVectorLaneSize(dstType);

      unsigned srcElWidth = srcScalarType.getIntOrFloatBitWidth();
      unsigned dstElWidth = dstScalarType.getIntOrFloatBitWidth();

      if (!(srcLaneSize == 32 && (dstElWidth < srcElWidth) &&
            (dstLaneSize == srcLaneSize))) {
        return true;
      }

      return false;
    });

    auto op = getOperation();
    if (failed(applyPartialConversion(op, target, std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

// This pass widens UPD ops to twice the width followed by an ext op of the
// bottom half. This can be used together with SimplifyUPDOpsPass to find
// additional common subexpressions with UPDs generated from unaligned
// `transfer_read` ops.
struct ExtendUPDOpsPass
    : public PassWrapper<ExtendUPDOpsPass, OperationPass<>> {

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    RewritePatternSet patterns(context);
    ConversionTarget target(*context);
    patterns.add<ExpandUPDToUPDAndExtPattern>(patterns.getContext());
    target.addLegalDialect<aievec::AIEVecDialect>();
    target.addDynamicallyLegalOp<aievec::UPDOp>([](aievec::UPDOp op) {
      return op.getVector() ||
             (op->hasOneUse() && isa<aievec::UPDOp>(*op->getUsers().begin())) ||
             llvm::all_of(op->getUsers(),
                          [](Operation *op) { return isa<aievec::ExtOp>(op); });
    });
    auto op = getOperation();
    if (failed(applyPartialConversion(op, target, std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

// This pass replaces wide UPD ops that are only used by a single ext op of the
// bottom half. This pass undos the work of ExtendUPDOpsPass.
// TODO: This pass can be extended to work with wide UPD ops that are used by
// TODO: a single ext op of the top half, which might be a good opportunity to
// TODO: further optimize wide UPDs.
struct SimplifyUPDOpsPass
    : public PassWrapper<SimplifyUPDOpsPass, OperationPass<>> {

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    RewritePatternSet patterns(context);
    ConversionTarget target(*context);
    patterns.add<FuseExtIntoUPDPattern>(patterns.getContext());
    target.addLegalDialect<aievec::AIEVecDialect>();
    target.addDynamicallyLegalOp<aievec::ExtOp>([](aievec::ExtOp op) {
      auto defOp = op.getSource().getDefiningOp();
      return !defOp || !isa<aievec::UPDOp>(defOp) || !defOp->hasOneUse() ||
             op.getIndex() != 0;
    });
    auto op = getOperation();
    if (failed(applyPartialConversion(op, target, std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

//============================================================================//
//=============== Main Vector2AIEVec Pipeline Configuration ==================//
//============================================================================//

void xilinx::aievec::buildLowerVectorToAIEVec(
    OpPassManager &pm, const LowerVectorToAIEVecOptions &options) {
  // Add lowering from `Vector` to `AIEVec`
  pm.addPass(createLowerVectorToAIEVec(options));
  pm.addPass(std::make_unique<ProcessExtOpsPass>());
  pm.addPass(createCanonicalizerPass());

  // Simplify UPD ops
  pm.addPass(std::make_unique<ExtendUPDOpsPass>());
  pm.addPass(createCSEPass());
  pm.addPass(std::make_unique<SimplifyUPDOpsPass>());
  pm.addPass(createCanonicalizerPass());
}
