/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file implements logic for lowering HLO dialect to LHLO dialect.

#include "absl/memory/memory.h"
#include "llvm/ADT/APInt.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"  // TF:local_config_mlir
#include "mlir/Dialect/Linalg/IR/LinalgTypes.h"  // TF:local_config_mlir
#include "mlir/Dialect/StandardOps/Ops.h"  // TF:local_config_mlir
#include "mlir/IR/AffineExpr.h"  // TF:local_config_mlir
#include "mlir/IR/Attributes.h"  // TF:local_config_mlir
#include "mlir/IR/Builders.h"  // TF:local_config_mlir
#include "mlir/IR/Function.h"  // TF:local_config_mlir
#include "mlir/IR/Location.h"  // TF:local_config_mlir
#include "mlir/IR/MLIRContext.h"  // TF:local_config_mlir
#include "mlir/IR/Operation.h"  // TF:local_config_mlir
#include "mlir/IR/PatternMatch.h"  // TF:local_config_mlir
#include "mlir/IR/StandardTypes.h"  // TF:local_config_mlir
#include "mlir/Pass/Pass.h"  // TF:local_config_mlir
#include "mlir/Transforms/DialectConversion.h"  // TF:local_config_mlir
#include "tensorflow/compiler/mlir/xla/ir/lhlo_ops.h"
#include "tensorflow/compiler/mlir/xla/transforms/map_lhlo_to_scalar_op.h"

namespace mlir {
namespace xla_lhlo {
namespace {

ArrayAttr GetNParallelLoopsAttrs(unsigned nParallelLoops, Builder b) {
  auto parallelLoopTypeAttr = b.getStringAttr("parallel");
  SmallVector<Attribute, 3> iteratorTypes;
  for (int i = 0; i < nParallelLoops; ++i) {
    iteratorTypes.push_back(parallelLoopTypeAttr);
  }
  return b.getArrayAttr(iteratorTypes);
}

template <typename LhloOp>
class PointwiseToLinalgConverter : public OpConversionPattern<LhloOp> {
 public:
  using OpConversionPattern<LhloOp>::OpConversionPattern;

  PatternMatchResult matchAndRewrite(
      LhloOp lhlo_op, ArrayRef<Value*> args,
      ConversionPatternRewriter& rewriter) const final {
    auto loc = lhlo_op.getLoc();
    auto argType =
        lhlo_op.getOperand(0)->getType().template dyn_cast<ShapedType>();
    if (!argType || !argType.hasStaticShape()) {
      emitError(loc,
                "lhlo to linalg conversion expects statically shaped args");
      return ConversionPattern::matchFailure();
    }
    if (!argType || !argType.getElementType().isIntOrFloat()) {
      return ConversionPattern::matchFailure();
    }

    // Construct the indexing maps needed for linalg.generic ops.
    SmallVector<Attribute, 2> indexingMaps;
    SmallVector<Type, 4> bodyArgTypes, bodyResultTypes;
    unsigned nloops = 0;
    int operandCount = args.size() - 1;
    for (const auto& arg : llvm::enumerate(args)) {
      auto memrefType = arg.value()->getType().dyn_cast<MemRefType>();
      if (!memrefType) return ConversionPattern::matchFailure();
      unsigned rank = memrefType.getRank();
      if (!rank || (nloops && nloops != rank)) {
        return ConversionPattern::matchFailure();
      }
      nloops = std::max(nloops, rank);
      indexingMaps.emplace_back(
          AffineMapAttr::get(rewriter.getMultiDimIdentityMap(nloops)));
      auto& result_or_body_arg =
          arg.index() < operandCount ? bodyArgTypes : bodyResultTypes;
      result_or_body_arg.emplace_back(memrefType.getElementType());
    }

    // Define the number of input memref/output memrefs.
    SmallVector<Attribute, 2> nmemrefs{
        rewriter.getI64IntegerAttr(bodyArgTypes.size()),
        rewriter.getI64IntegerAttr(bodyResultTypes.size())};

    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, args, rewriter.getArrayAttr(indexingMaps),
        GetNParallelLoopsAttrs(nloops, rewriter),
        rewriter.getArrayAttr(nmemrefs),
        /*doc=*/nullptr, /*fun=*/nullptr, /*library_call=*/nullptr);

    // Add a block to the region.
    auto* region = &linalgOp.region();
    auto* block = rewriter.createBlock(region, region->end());
    block->addArguments(bodyArgTypes);
    block->addArguments(bodyResultTypes);

    SmallVector<Value*, 4> bodyArgs;
    for (int i = 0, e = bodyArgTypes.size(); i < e; ++i) {
      bodyArgs.push_back(block->getArgument(i));
    }

    rewriter.setInsertionPointToEnd(block);
    Operation* op = MapLhloOpToStdScalarOp<LhloOp>(
        llvm::cast<LhloOp>(lhlo_op), bodyResultTypes, bodyArgs, rewriter);
    rewriter.create<linalg::YieldOp>(loc, llvm::to_vector<1>(op->getResults()));
    rewriter.eraseOp(lhlo_op);
    return ConversionPattern::matchSuccess();
  }
};

class BroadcastInDimConverter : public OpConversionPattern<BroadcastInDimOp> {
 public:
  using OpConversionPattern<BroadcastInDimOp>::OpConversionPattern;

  PatternMatchResult matchAndRewrite(
      BroadcastInDimOp broadcastOp, ArrayRef<Value*> args,
      ConversionPatternRewriter& rewriter) const final {
    unsigned operandIndex = 0;
    unsigned resultIndex = 1;
    auto operandMemrefType =
        broadcastOp.getOperand(operandIndex)->getType().dyn_cast<MemRefType>();
    auto resultMemrefType =
        broadcastOp.getOperand(resultIndex)->getType().dyn_cast<MemRefType>();
    if (!operandMemrefType || !resultMemrefType) return matchFailure();

    SmallVector<Type, 4> bodyArgTypes{operandMemrefType.getElementType()};

    unsigned nloops = resultMemrefType.getRank();

    SmallVector<AffineExpr, 4> dimExprs;
    {
      dimExprs.reserve(nloops);

      auto operandShape = operandMemrefType.getShape();
      int index = 0;
      if (!broadcastOp.broadcast_dimensions().hasValue()) {
        return matchFailure();
      }
      for (const auto& broadcastSize :
           broadcastOp.broadcast_dimensions().getValue().getIntValues()) {
        int size = broadcastSize.getSExtValue();
        dimExprs.push_back(
            operandShape[index++] == 1
                ? mlir::getAffineConstantExpr(0, broadcastOp.getContext())
                : mlir::getAffineDimExpr(size, broadcastOp.getContext()));
      }
    }

    // Construct the indexing maps needed for linalg.generic ops.
    SmallVector<Attribute, 2> indexingMaps;
    indexingMaps.emplace_back(AffineMapAttr::get(
        AffineMap::get(nloops, /*symbolCount=*/0, dimExprs)));
    indexingMaps.emplace_back(
        AffineMapAttr::get(rewriter.getMultiDimIdentityMap(nloops)));

    // Define the number of input memref/output memrefs.
    SmallVector<Attribute, 2> nmemrefs{
        rewriter.getI64IntegerAttr(bodyArgTypes.size()),
        rewriter.getI64IntegerAttr(1)};

    auto loc = broadcastOp.getLoc();
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, args, rewriter.getArrayAttr(indexingMaps),
        GetNParallelLoopsAttrs(nloops, rewriter),
        rewriter.getArrayAttr(nmemrefs),
        /*doc=*/nullptr, /*fun=*/nullptr, /*library_call=*/nullptr);

    // Add a block to the region.
    auto* region = &linalgOp.region();
    auto* block = rewriter.createBlock(region, region->end());
    block->addArguments(bodyArgTypes);
    block->addArguments(resultMemrefType.getElementType());

    rewriter.setInsertionPointToEnd(block);
    rewriter.create<linalg::YieldOp>(loc, block->getArgument(operandIndex));
    rewriter.eraseOp(broadcastOp);
    return matchSuccess();
  }
};

class IotaConverter : public OpConversionPattern<IotaOp> {
 public:
  using OpConversionPattern<IotaOp>::OpConversionPattern;

  PatternMatchResult matchAndRewrite(
      IotaOp iotaOp, ArrayRef<Value*> args,
      ConversionPatternRewriter& rewriter) const final {
    auto resultMemrefType =
        iotaOp.getOperand()->getType().dyn_cast<MemRefType>();
    if (!resultMemrefType) return matchFailure();

    auto resultElementType = resultMemrefType.getElementType();
    if (!resultElementType.isIntOrFloat()) return matchFailure();

    // Construct the indexing maps needed for linalg.generic ops.
    unsigned nloops = resultMemrefType.getRank();
    SmallVector<Attribute, 2> indexingMaps;
    indexingMaps.emplace_back(
        AffineMapAttr::get(rewriter.getMultiDimIdentityMap(nloops)));

    // Define the number of input memref/output memrefs.
    SmallVector<Attribute, 2> nmemrefs{rewriter.getI64IntegerAttr(0),
                                       rewriter.getI64IntegerAttr(1)};

    auto loc = iotaOp.getLoc();
    auto linalgOp = rewriter.create<linalg::IndexedGenericOp>(
        loc, args, rewriter.getArrayAttr(indexingMaps),
        GetNParallelLoopsAttrs(nloops, rewriter),
        rewriter.getArrayAttr(nmemrefs),
        /*doc=*/nullptr, /*fun=*/nullptr, /*library_call=*/nullptr);

    // Add a block to the region.
    auto* region = &linalgOp.region();
    auto* block = rewriter.createBlock(region, region->end());
    for (unsigned i = 0; i < nloops; ++i) {
      block->addArgument(rewriter.getIndexType());
    }
    block->addArguments(llvm::makeArrayRef(resultElementType));

    rewriter.setInsertionPointToEnd(block);
    Operation* castOp = rewriter.create<IndexCastOp>(
        loc, block->getArgument(iotaOp.iota_dimension().getZExtValue()),
        rewriter.getIntegerType(resultElementType.getIntOrFloatBitWidth()));
    if (resultElementType.isa<FloatType>()) {
      castOp = rewriter.create<SIToFPOp>(loc, castOp->getResult(0),
                                         resultElementType);
    }
    rewriter.create<linalg::YieldOp>(loc, castOp->getResult(0));
    rewriter.eraseOp(iotaOp);
    return matchSuccess();
  }
};

void populateLHLOToLinalgConversionPattern(MLIRContext* context,
                                           OwningRewritePatternList* patterns) {
  // clang-format off
  patterns->insert<BroadcastInDimConverter,
                   IotaConverter,
                   PointwiseToLinalgConverter<xla_lhlo::AddOp>,
                   PointwiseToLinalgConverter<xla_lhlo::AndOp>,
                   PointwiseToLinalgConverter<xla_lhlo::CompareOp>,
                   PointwiseToLinalgConverter<xla_lhlo::DivOp>,
                   PointwiseToLinalgConverter<xla_lhlo::ExpOp>,
                   PointwiseToLinalgConverter<xla_lhlo::MaxOp>,
                   PointwiseToLinalgConverter<xla_lhlo::MinOp>,
                   PointwiseToLinalgConverter<xla_lhlo::MulOp>,
                   PointwiseToLinalgConverter<xla_lhlo::SelectOp>,
                   PointwiseToLinalgConverter<xla_lhlo::SubOp>>(context);
  // clang-format on
}

// Converts LHLO ops to Linalg generic.
// Sample result for xla_lhlo::AddOp.
//
// "xla_lhlo.add"(%arg1, %arg2, %out) :
//      (memref<2x2xf32>, memref<2x2xf32>, memref<2x2xf32>) -> ()
//
// will be converted to
//
// #map0 = (d0, d1) -> (d0, d1)
// "linalg.generic"(%arg1, %arg2, %out) ( {
//   ^bb0(%arg4: f32, %arg5: f32):
//     %0 = addf %arg4, %arg5 : f32
//     "linalg.yield"(%0) : (f32) -> ()
//   }) {
//     indexing_maps = [#map0, #map0, #map0],
//     iterator_types = ["parallel", "parallel"],
//     n_views = [2, 1]
//   } : (memref<2x2xf32>, memref<2x2xf32>, memref<2x2xf32>) -> ()
// }
struct LhloLegalizeToLinalg : public FunctionPass<LhloLegalizeToLinalg> {
  void runOnFunction() override {
    OwningRewritePatternList patterns;
    ConversionTarget target(getContext());
    target.addLegalDialect<linalg::LinalgDialect, StandardOpsDialect>();

    auto func = getFunction();
    populateLHLOToLinalgConversionPattern(func.getContext(), &patterns);
    if (failed(applyPartialConversion(func, target, patterns, nullptr))) {
      signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<OpPassBase<FuncOp>> createLegalizeToLinalgPass() {
  return absl::make_unique<LhloLegalizeToLinalg>();
}

static PassRegistration<LhloLegalizeToLinalg> legalize_pass(
    "lhlo-legalize-to-linalg", "Legalize from LHLO dialect to Linalg dialect");

}  // namespace xla_lhlo
}  // namespace mlir
