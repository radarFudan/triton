#include "mlir/IR/TypeUtilities.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "triton/Analysis/Utility.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/Transforms/Passes.h"
#include "triton/Dialect/TritonGPU/Transforms/Utility.h"
#include "TritonAMDGPUTransforms/Passes.h"
#include "TritonAMDGPUTransforms/MfmaGroup.h"
#include "triton/Tools/Sys/GetEnv.hpp"
#include "llvm/Support/Debug.h"
#include <memory>

using namespace mlir;
namespace tt = mlir::triton;
namespace ttg = mlir::triton::gpu;
namespace {
using tt::DotOp;
using ttg::BlockedEncodingAttr;
using ttg::ConvertLayoutOp;
using ttg::DotOperandEncodingAttr;
using ttg::AMDMfmaEncodingAttr;
using ttg::SliceEncodingAttr;

SmallVector<unsigned, 2>
warpsPerTileMFMA(tt::DotOp dotOp, const ArrayRef<int64_t> shape, int numWarps,
                 SmallVector<int64_t, 2> shapePerWarp) {
  // TODO: needs to be updated with appropriate shapePerWarp etc.
  auto filter = [&dotOp](Operation *op) {
    return op->getParentRegion() == dotOp->getParentRegion();
  };
  mlir::ForwardSliceOptions fwdOpt;
  fwdOpt.filter = filter;
  mlir::BackwardSliceOptions bwdOpt;
  bwdOpt.omitBlockArguments = true;
  bwdOpt.filter = filter;
  auto slices = mlir::getSlice(dotOp, bwdOpt, fwdOpt);
  for (Operation *op : slices)
    if (isa<tt::DotOp>(op) && (op != dotOp))
      return {(unsigned)numWarps, 1};

  SmallVector<int64_t, 2> tensorShape = {shape[0], shape[1]};
  SmallVector<unsigned, 2> ret = {1, 1};
  bool changed = false;

  do {
    changed = false;
    if (ret[0] * ret[1] >= numWarps)
      break;
    if (tensorShape[0] / (shapePerWarp[0] * 2) / ret[0] >=
        tensorShape[1] / shapePerWarp[1] / ret[1]) {
      if (ret[0] < tensorShape[0] / shapePerWarp[0]) {
        ret[0] *= 2;
      } else
        ret[1] *= 2;
    } else {
      ret[1] *= 2;
    }
  } while (true);

  if (ret[1] * shapePerWarp[1] > tensorShape[1]) {
    return {ret[1], ret[0]};
  }

  return ret;
}

class BlockedToMFMA : public mlir::RewritePattern {
  int mfmaVersion;
  int enforcedNonKDim;

public:
  BlockedToMFMA(mlir::MLIRContext *context, int mfmaVersion, int nonKDim)
      : mlir::RewritePattern(tt::DotOp::getOperationName(), 2, context),
        mfmaVersion(mfmaVersion), enforcedNonKDim(nonKDim) {}

  bool isChainDot(tt::DotOp &dotOp) const {
    auto filter = [&dotOp](Operation *op) {
      return op->getParentRegion() == dotOp->getParentRegion();
    };
    mlir::ForwardSliceOptions fwdOpt;
    fwdOpt.filter = filter;
    mlir::BackwardSliceOptions bwdOpt;
    bwdOpt.omitBlockArguments = true;
    bwdOpt.filter = filter;
    auto slices = mlir::getSlice(dotOp, bwdOpt, fwdOpt);
    for (Operation *op : slices) {
      if (isa<tt::DotOp>(op) && (op != dotOp))
        return true;
    }
    return false;
  }

  /// @brief Choose MFMA instruction parameters
  /// @param dot target dot operation
  /// @return pair {mDim, nDim, kDim} sizes of one MFMA instruction arguments
    std::tuple<unsigned, unsigned, unsigned>
    chooseMfmaDimensions(tt::DotOp dot) const {
    // number of matrix elements along k dim per one MFMA intruction
    unsigned kDim = 0;
    auto opType = dot.getA().getType().cast<RankedTensorType>();
    auto dataTypeA = opType.getElementType();
    auto dataTypeB =
        dot.getB().getType().cast<RankedTensorType>().getElementType();

    auto resType = dot.getD().getType().cast<RankedTensorType>();
    auto resShape = resType.getShape();

    unsigned mDim = 0;
    unsigned nDim = 0;
    if (enforcedNonKDim != 0) {
      mDim = enforcedNonKDim;
      nDim = enforcedNonKDim;
    } else {
      int minSize = std::min(resShape[0], resShape[1]);
      if (minSize >= 32) {
          mDim = 32;
          nDim = 32;
      }
      if (minSize >= 16 && minSize < 32) {
          mDim = 16;
          nDim = 16;
      }
      if (minSize < 16) {
          if (resShape[0] < 16 && resShape[1] >= 64) {
              mDim = 4;
              nDim = 64;
          } else if (resShape[0] >= 64 && resShape[1] < 16) {
              mDim = 64;
              nDim = 4;
          } else {
              assert(opType.getShape()[1] >= 64 &&
                     "k should be at least 64 to use this layout");
              mDim = 4;
              nDim = 4;
          }
      }
    }
    assert(mDim != 0 && nDim != 0);

    auto maybeMfmaInsn =
        MfmaInsn::selectMfma(mDim, nDim, dataTypeA, dataTypeB, mfmaVersion);
    if (failed(maybeMfmaInsn))
        llvm::report_fatal_error("No match found in MFMA database\n");
    else
        kDim = (*maybeMfmaInsn).getKDim();
    assert(kDim != 0);

    assert(resShape[0] % mDim == 0 && resShape[1] % nDim == 0);
    assert(opType.getShape()[1] % kDim == 0);
    return {mDim, nDim, kDim};
  }

  /**
   * @brief Convert layout and cast element type of a given tensor
   *
   * If old element type is different from new element type, this function
   * creates two new operations:
   * 1. %converted_value = layout_convert %value, newEncoding
   * 2. %casted_value = cast(fext, ftrunc, etc.) %value, newElemType
   *
   * If old element type is same as new element type, this function creates only
   * one operation: %converted_value = layout_convert %value, newEncoding
   *
   * @param rewriter
   * @param value original tensor value, which we need to convert and cast
   * @param newEncoding new encoding for the tenosr
   * @param newElemType new element type for the tensor
   * @return converted and optionaly casted tensor value
   */
  Value convertAndCastTensor(mlir::PatternRewriter &rewriter, Value value,
                             ::mlir::Attribute newEncoding,
                             Type newElemType) const {
    assert(newElemType.isIntOrFloat());

    auto loc = value.getLoc();
    auto oldType = value.getType().cast<RankedTensorType>();
    auto oldElemType = oldType.getElementType();

    assert(oldElemType.isIntOrFloat());
    assert(oldElemType.isIntOrIndex() == newElemType.isIntOrIndex());

    auto convertedType =
        RankedTensorType::get(oldType.getShape(), oldElemType, newEncoding);

    Value convertedTensor =
        rewriter.create<ttg::ConvertLayoutOp>(loc, convertedType, value);

    if (newElemType == oldElemType)
      return convertedTensor;

    Type castedType = convertedType.cloneWith(std::nullopt, newElemType);

    Value castedTensor;

    if (newElemType.isIntOrIndex()) {
      unsigned oldWidth = oldElemType.getIntOrFloatBitWidth();
      unsigned newWidth = newElemType.getIntOrFloatBitWidth();
      if (oldWidth == newWidth)
        castedTensor = rewriter.create<mlir::arith::BitcastOp>(
            loc, convertedType, convertedTensor);
      else if (oldWidth > newWidth)
        castedTensor = rewriter.create<mlir::arith::TruncIOp>(loc, castedType,
                                                              convertedTensor);
      else if (oldElemType.isSignedInteger())
        castedTensor = rewriter.create<mlir::arith::ExtSIOp>(loc, castedType,
                                                             convertedTensor);
      else
        castedTensor = rewriter.create<mlir::arith::ExtUIOp>(loc, castedType,
                                                             convertedTensor);
    } else {
      if (oldElemType.isF16() && newElemType.isF32())
        castedTensor = rewriter.create<mlir::arith::ExtFOp>(loc, castedType,
                                                            convertedTensor);
      else if (oldElemType.isF32() && newElemType.isF16())
        castedTensor = rewriter.create<mlir::arith::TruncFOp>(loc, castedType,
                                                              convertedTensor);
      else
        castedTensor =
            rewriter.create<tt::FpToFpOp>(loc, castedType, convertedTensor);
    }
    return castedTensor;
  }

  mlir::LogicalResult
  matchAndRewrite(mlir::Operation *op,
                  mlir::PatternRewriter &rewriter) const override {
    auto dotOp = cast<tt::DotOp>(op);

    RankedTensorType oldRetType = dotOp.getType();
    if (!oldRetType.getEncoding() ||
        !oldRetType.getEncoding().isa<ttg::BlockedEncodingAttr>())
      return failure();

    if (!supportMFMA(dotOp))
      return failure();

    auto CTALayout = ttg::getCTALayout(oldRetType.getEncoding());

    // get MFMA encoding for the given number of warps
    auto retShape = oldRetType.getShape();
    auto mod = op->getParentOfType<mlir::ModuleOp>();
    int numWarps = ttg::TritonGPUDialect::getNumWarps(mod);

    // operands
    Value a = dotOp.getA();
    Value b = dotOp.getB();
    auto oldAType = a.getType().cast<RankedTensorType>();
    auto oldBType = b.getType().cast<RankedTensorType>();
    auto ctx = oldAType.getContext();

    ttg::AMDMfmaEncodingAttr mfmaEnc;

    auto [mDim, nDim, kDim] = chooseMfmaDimensions(dotOp);

    auto warpsPerTile = warpsPerTileMFMA(dotOp, retShape, numWarps, {mDim, nDim});

    bool isTransposed = isChainDot(dotOp);
    mfmaEnc = ttg::AMDMfmaEncodingAttr::get(
        oldRetType.getContext(),
        /*versionMajor*/ mfmaVersion, /*versionMinor*/ 0, warpsPerTile,
        /*instrShape*/ mDim, nDim, isTransposed, CTALayout);

    Type mfmaAccType;
    if (oldRetType.getElementType().isIntOrIndex())
      mfmaAccType = rewriter.getIntegerType(32);
    else
      mfmaAccType = rewriter.getF32Type();

    // convert accumulator
    auto oldAcc = dotOp.getOperand(2);
    auto newAcc = convertAndCastTensor(rewriter, oldAcc, mfmaEnc, mfmaAccType);

    // kWidth is a number of consecutive elements per one instruction per one
    // thread
    auto kWidth = -1;
    // in mfma 32x32 case argument matrix groups elements in 2 groups
    // in mfma 16x16 case argument matrix groups elements in 4 groups
    // in mfma 4x4 case argument matrix groups in 16 groups
    if (mDim == 32 && nDim == 32)
        kWidth = kDim / 2;
    if (mDim == 16 && nDim == 16)
        kWidth = kDim / 4;
    if (mDim == 4 && nDim == 4)
        kWidth = kDim / 16;
    if (mDim == 4 && nDim == 64 || mDim == 64 && nDim == 4)
        kWidth = kDim;
    assert(kWidth != -1);
    auto newAType = RankedTensorType::get(
        oldAType.getShape(), oldAType.getElementType(),
        ttg::DotOperandEncodingAttr::get(ctx, 0, mfmaEnc, kWidth));
    auto newBType = RankedTensorType::get(
        oldBType.getShape(), oldBType.getElementType(),
        ttg::DotOperandEncodingAttr::get(ctx, 1, mfmaEnc, kWidth));
    a = rewriter.create<ttg::ConvertLayoutOp>(a.getLoc(), newAType, a);
    b = rewriter.create<ttg::ConvertLayoutOp>(b.getLoc(), newBType, b);
    auto newDot = rewriter.create<tt::DotOp>(dotOp.getLoc(), newAcc.getType(),
                                             a, b, newAcc, dotOp.getAllowTF32(),
                                             dotOp.getMaxNumImpreciseAcc());

    Value dotOutput =
        convertAndCastTensor(rewriter, newDot, oldRetType.getEncoding(),
                             oldRetType.getElementType());

    rewriter.replaceOp(op, dotOutput);

    return success();
  }
};

} // namespace

#define GEN_PASS_CLASSES
#include "TritonAMDGPUTransforms/Passes.h.inc"

class TritonAMDGPUAccelerateMatmulPass
    : public TritonAMDGPUAccelerateMatmulBase<
          TritonAMDGPUAccelerateMatmulPass> {
public:
  TritonAMDGPUAccelerateMatmulPass() = default;
  TritonAMDGPUAccelerateMatmulPass(int matrixCoreVersion,
                                   int matrixInstructionSize) {
    this->matrixCoreVersion = matrixCoreVersion;
    this->matrixInstructionSize = matrixInstructionSize;
  }
  void runOnOperation() override {
    MLIRContext *context = &getContext();
    ModuleOp m = getOperation();

    mlir::RewritePatternSet patterns(context);
    if (matrixCoreVersion == 1 || matrixCoreVersion == 2 ||
        matrixCoreVersion == 3)
      patterns.add<::BlockedToMFMA>(context, matrixCoreVersion,
                                    matrixInstructionSize);
    if (applyPatternsAndFoldGreedily(m, std::move(patterns)).failed()) {
      signalPassFailure();
    }
  }
};

std::unique_ptr<Pass>
mlir::createTritonAMDGPUAccelerateMatmulPass(int matrixCoreVersion,
                                             int matrixInstructionSize) {
  return std::make_unique<TritonAMDGPUAccelerateMatmulPass>(
      matrixCoreVersion, matrixInstructionSize);
}
