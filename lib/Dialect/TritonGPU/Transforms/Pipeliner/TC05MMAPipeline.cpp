#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/IR/Dominance.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "triton/Dialect/Triton/IR/Utility.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/IR/Types.h"
#include "triton/Dialect/TritonGPU/Transforms/Passes.h"
#include "triton/Dialect/TritonGPU/Transforms/PipelineExpander.h"
#include "triton/Dialect/TritonGPU/Transforms/PipeliningUtility.h"
#include "triton/Dialect/TritonGPU/Transforms/Schedule.h"
#include "triton/Dialect/TritonGPU/Transforms/Utility.h"
#include "triton/Dialect/TritonNvidiaGPU/IR/Dialect.h"

using namespace mlir;
namespace tt = mlir::triton;
namespace ttg = mlir::triton::gpu;
namespace ttng = mlir::triton::nvidia_gpu;

namespace {

const char *kPipelineStageAttrName = "triton.pipeline_stage";
const char *kPipelineAttrName = "triton.pipeline";

// Utils:
void replaceAllUsesDominatedBy(Operation *domOp, Value newValue,
                               Value oldValue) {
  DominanceInfo domOpInfo(domOp->getParentOp());
  oldValue.replaceUsesWithIf(newValue, [&](OpOperand &use) {
    return domOpInfo.properlyDominates(domOp, use.getOwner());
  });
}

void annotateWithPipelineStage(IRRewriter &builder, Operation *op, int stage) {
  op->setAttr(kPipelineStageAttrName,
              IntegerAttr::get(builder.getI32Type(), stage));
}

struct MMAInfo {
  struct AccOverridePoint {
    Operation *op;
    Value condition = nullptr;
    Value initValue = nullptr;
    int distance = 0;
  };

  ttng::TMEMAllocOp accAlloc; // Directly precedes the dot, allocating tmem
                              // for the accumulator
  ttng::TMEMLoadOp
      accLoad; // Directly follows the dot, loading accumulator from tmem
  std::optional<AccOverridePoint> accDef;
  std::optional<int> yieldArgNo;
  bool accIsMultiBuffered;

  Value phase = nullptr;
  Value barrierIdx = nullptr;
  Value accInsertIdx = nullptr;
  Value accExtractIdx = nullptr;
  Value barrierAlloc = nullptr;
};

// Returns the TMEMAllocOp and TMEMLoadOp that are used to allocate and load the
// accumulator for the given MMA operation. The TMEMAllocOp and TMEMLoadOp must
// be in the same region as the MMA operation.
std::optional<std::pair<ttng::TMEMAllocOp, ttng::TMEMLoadOp>>
getTMemAllocAndLoad(Operation *mmaOp) {
  auto acc = mmaOp->getOperand(2).getDefiningOp<ttng::TMEMAllocOp>();
  if (!acc || acc->getParentRegion() != mmaOp->getParentRegion()) {
    return std::nullopt;
  }
  for (auto user : acc->getUsers()) {
    if (auto load = dyn_cast<ttng::TMEMLoadOp>(user)) {
      if (load->getParentRegion() == mmaOp->getParentRegion()) {
        return std::make_pair(acc, load);
      }
    }
  }
  return std::nullopt;
}

// Check if the accumulator is being used by the same MMA in the next iteration.
// If so, return the yield argument number that the accumulator is being used
// as. Also, check if accumulator has runtime divergent uses - uses that may not
// be known at the compile time.
std::optional<int> trackAccChain(scf::ForOp forOp, ttng::TMEMLoadOp accDef,
                                 ttng::TMEMAllocOp accAlloc,
                                 bool &hasDivergentUses) {
  hasDivergentUses = false;
  struct UseInfo {
    Value value = nullptr;
    std::optional<int> yieldArgNo = std::nullopt;
    bool divergentUse = false;
  };
  SmallVector<UseInfo> queue;
  std::optional<int> yieldArgNo = std::nullopt;
  queue.push_back({accDef.getResult(), std::nullopt, false});
  while (!queue.empty()) {
    UseInfo info = queue.pop_back_val();
    for (auto &use : info.value.getUses()) {
      if (auto yieldOp = dyn_cast<scf::YieldOp>(use.getOwner())) {
        if (yieldOp->getParentOp() == forOp) {
          queue.push_back({forOp.getRegionIterArg(use.getOperandNumber()),
                           use.getOperandNumber(), true}); // divergent use
          continue;
        }
        if (auto ifOp = dyn_cast<scf::IfOp>(yieldOp->getParentOp())) {
          queue.push_back({ifOp.getResult(use.getOperandNumber()),
                           info.yieldArgNo, true}); // divergent use
          continue;
        }
        assert(0 && "Unexpected use of accumulator");
      } else if (auto selectOp = dyn_cast<arith::SelectOp>(use.getOwner())) {
        queue.push_back({selectOp.getResult(), info.yieldArgNo, true});
      } else if (use.getOwner() == accAlloc) {
        yieldArgNo = info.yieldArgNo;
      } else {
        // Op other than yield or accAlloc. Mark as divergent use if
        // we had to go through selectOp or ifOp.
        hasDivergentUses = info.divergentUse;
      }
    }
  }
  return yieldArgNo;
}

SmallVector<Operation *> getDirectAccUses(ttng::TMEMLoadOp accDef) {
  SmallVector<Operation *> accUses;
  for (auto user : accDef.getResult().getUsers()) {
    if (!isa<arith::SelectOp>(user) && !isa<scf::YieldOp>(user)) {
      accUses.push_back(user);
    }
  }
  return accUses;
}

std::optional<MMAInfo::AccOverridePoint>
getAccOverridePointInLoop(scf::ForOp forOp, ttng::TMEMAllocOp accUse,
                          ttng::TMEMLoadOp accDef) {
  MMAInfo::AccOverridePoint accOverridePoint;
  DenseSet<Value> seen;
  Value v = accUse.getSrc();
  if (v == nullptr) {
    // Uninitialized accumulator means unused accumulator
    accOverridePoint.op = accUse;
    return accOverridePoint;
  }
  int dist = 0;
  while (auto blockArg = dyn_cast<BlockArgument>(v)) {
    if (!seen.insert(v).second) {
      return std::nullopt;
    }
    assert(blockArg.getOwner() == forOp.getBody());
    auto yieldOp = cast<scf::YieldOp>(blockArg.getOwner()->getTerminator());
    v = yieldOp.getOperand(blockArg.getArgNumber() - 1);
    dist++;
  }
  if (!v.getDefiningOp()) {
    return std::nullopt;
  }
  accOverridePoint.distance = dist;
  bool thenOverrides = false;
  if (auto selectOp = dyn_cast<arith::SelectOp>(v.getDefiningOp())) {
    accOverridePoint.op = selectOp;
    bool trueIsConst =
        (selectOp.getTrueValue().getDefiningOp<arith::ConstantOp>() != nullptr);
    bool falseIsConst =
        (selectOp.getFalseValue().getDefiningOp<arith::ConstantOp>() !=
         nullptr);
    if (trueIsConst && falseIsConst) {
      // Both values are constant, so the select overrides unconditionally
      accOverridePoint.initValue = v;
      return accOverridePoint;
    } else if (trueIsConst) {
      accOverridePoint.initValue = selectOp.getTrueValue();
      thenOverrides = true;
    } else if (falseIsConst) {
      accOverridePoint.initValue = selectOp.getFalseValue();
      thenOverrides = false;
    } else {
      return std::nullopt;
    }
    accOverridePoint.condition = selectOp.getCondition();
    if (!thenOverrides) {
      IRRewriter builder(selectOp);
      Value vTrue = builder.create<arith::ConstantOp>(
          selectOp.getLoc(), builder.getBoolAttr(true));
      accOverridePoint.condition = builder.create<arith::XOrIOp>(
          selectOp.getLoc(), accOverridePoint.condition, vTrue);
    }
  } else if (v.getDefiningOp() != accDef) {
    assert(!isa<scf::IfOp>(v.getDefiningOp()) &&
           "Expected unconditional override op");
    accOverridePoint.op = v.getDefiningOp();
    accOverridePoint.initValue = v;
  } else {
    return std::nullopt;
  }

  return accOverridePoint;
}

std::optional<MMAInfo::AccOverridePoint>
getAccUseFlagFalseInLoop(scf::ForOp forOp, Value useAccFlagUse) {
  DenseSet<Value> seen;
  Value v = useAccFlagUse;
  int dist = 0;
  while (auto blockArg = dyn_cast<BlockArgument>(v)) {
    if (!seen.insert(v).second) {
      return {};
    }
    assert(blockArg.getOwner() == forOp.getBody());
    auto yieldOp = cast<scf::YieldOp>(blockArg.getOwner()->getTerminator());
    v = yieldOp.getOperand(blockArg.getArgNumber() - 1);
    dist++;
  }
  if (!v.getDefiningOp() || !forOp->isAncestor(v.getDefiningOp())) {
    return std::nullopt;
  }
  assert(v.getType().isInteger(1));

  IRRewriter builder(v.getDefiningOp()->getNextNode());
  MMAInfo::AccOverridePoint accOverridePoint;
  accOverridePoint.distance = dist;
  Location loc = v.getDefiningOp()->getLoc();
  auto vTrue =
      builder.create<arith::ConstantOp>(loc, builder.getBoolAttr(true));
  accOverridePoint.op = v.getDefiningOp();
  accOverridePoint.condition = builder.create<arith::XOrIOp>(loc, v, vTrue);

  return accOverridePoint;
}

std::optional<MMAInfo::AccOverridePoint>
getAccOverrideOrFlagFalseInLoop(scf::ForOp forOp, Operation *mmaOp) {
  auto tmemAllocAndLoad = getTMemAllocAndLoad(mmaOp);
  assert(tmemAllocAndLoad.has_value() && "Expected tmem alloc and load");
  auto [accAlloc, accLoad] = tmemAllocAndLoad.value();
  auto accOverridePoint = getAccOverridePointInLoop(forOp, accAlloc, accLoad);

  if (!accOverridePoint.has_value()) {
    if (auto op = dyn_cast<ttng::TCGen5MMAOp>(mmaOp)) {
      auto useAccFlag = op.getUseD();
      accOverridePoint = getAccUseFlagFalseInLoop(forOp, useAccFlag);
    } else if (auto op = dyn_cast<ttng::TCGen5MMAScaledOp>(mmaOp)) {
      auto useAccFlag = op.getUseD();
      accOverridePoint = getAccUseFlagFalseInLoop(forOp, useAccFlag);
    }
  }

  return accOverridePoint;
}

// Given a result of MemDescSubview, or Alloca, create a MemDescSubview with a
// single buffer slice (leading dimension equal to 1), at the given index.
Value createSingleBufferView(IRRewriter &builder, Value alloc, Value idx) {
  assert(isa<ttg::MemDescType>(alloc.getType()) && "Expected MemDescType");
  auto allocDescType = cast<ttg::MemDescType>(alloc.getType());
  SmallVector<int64_t> shape;
  if (allocDescType.getShape().size() > 1) {
    shape.insert(shape.end(), allocDescType.getShape().begin() + 1,
                 allocDescType.getShape().end());
  } else {
    shape.push_back(1);
  }
  auto viewDescType = ttg::MemDescType::get(
      shape, allocDescType.getElementType(), allocDescType.getEncoding(),
      allocDescType.getMemorySpace(), allocDescType.getMutableMemory());
  SmallVector<Value> idxs = {idx};
  if (allocDescType.getShape().size() > 1) {
    Value zero = builder.create<arith::ConstantIntOp>(alloc.getLoc(), 0, 32);
    for (unsigned i = 1; i < allocDescType.getShape().size(); i++) {
      idxs.push_back(zero);
    }
  }
  return builder.create<ttg::MemDescSubviewOp>(alloc.getLoc(), viewDescType,
                                               alloc, idxs);
}

Value createSingleBufferView(IRRewriter &builder, Value alloc, int idx) {
  return createSingleBufferView(
      builder, alloc,
      builder.create<arith::ConstantIntOp>(alloc.getLoc(), idx, 32));
}

Value createBarrierAlloc(scf::ForOp forOp, Operation *mmaOp, int numStages) {
  IRRewriter rewriter(forOp->getContext());
  rewriter.setInsertionPoint(forOp);
  MLIRContext *ctx = forOp.getContext();
  Location loc = forOp.getLoc();
  unsigned numCTAs = triton::gpu::TritonGPUDialect::getNumCTAs(
      forOp->getParentOfType<ModuleOp>());
  Attribute sharedMemorySpace = ttg::SharedMemorySpaceAttr::get(ctx);
  auto barrierCTALayout = ttg::CTALayoutAttr::get(
      /*context=*/ctx, /*CTAsPerCGA=*/{numCTAs},
      /*CTASplitNum=*/{1}, /*CTAOrder=*/{0});
  auto barrierEncoding =
      ttg::SharedEncodingAttr::get(ctx, 1, 1, 1, {0}, barrierCTALayout);
  ttg::MemDescType barrierMemDescType =
      ttg::MemDescType::get({numStages}, rewriter.getI64Type(), barrierEncoding,
                            sharedMemorySpace, /*mutableMemory=*/true);
  Value barrierAlloc =
      rewriter.create<ttg::LocalAllocOp>(loc, barrierMemDescType, Value());
  for (unsigned i = 0; i < numStages; i++) {
    Value barrierView = createSingleBufferView(rewriter, barrierAlloc, i);
    rewriter.create<ttng::InitBarrierOp>(forOp->getLoc(), barrierView, 1);
  }
  return barrierAlloc;
}

ttng::TMEMAllocOp createTMemAlloc(IRRewriter &builder,
                                  ttng::TMEMAllocOp oldTMemAllocOp,
                                  bool multiBufferred, int numStages) {
  Location loc = oldTMemAllocOp.getLoc();
  auto oldRetType = oldTMemAllocOp.getType();
  SmallVector<int64_t> shape = {oldRetType.getShape().begin(),
                                oldRetType.getShape().end()};
  if (multiBufferred) {
    shape.insert(shape.begin(), numStages);
  }
  Type accMemDescType = triton::gpu::MemDescType::get(
      shape, oldRetType.getElementType(), oldRetType.getEncoding(),
      oldRetType.getMemorySpace(), /*mutableMemory=*/true);
  return builder.create<ttng::TMEMAllocOp>(oldTMemAllocOp.getLoc(),
                                           accMemDescType, nullptr);
}

void createInitStore(IRRewriter &builder, ttng::TMEMAllocOp allocOp,
                     Value initVal, bool multiBufferred) {
  Value bufferSlice = allocOp;
  if (multiBufferred) {
    bufferSlice = createSingleBufferView(builder, allocOp, 0);
  }
  Value vTrue = builder.create<arith::ConstantIntOp>(allocOp.getLoc(), 1, 1);
  builder.create<ttng::TMEMStoreOp>(allocOp.getLoc(), bufferSlice, initVal,
                                    vTrue);
}

Operation *findNearestCommonDominator(ArrayRef<Operation *> ops,
                                      DominanceInfo &domInfo) {
  if (ops.size() == 0) {
    return nullptr;
  }
  if (ops.size() == 1) {
    return ops[0];
  }
  llvm::SmallPtrSet<Block *, 16> blocks;
  for (auto op : ops) {
    blocks.insert(op->getBlock());
  }
  Block *domBlock = domInfo.findNearestCommonDominator(blocks);
  if (domBlock == nullptr) {
    return nullptr;
  }
  SmallVector<Operation *> ancestorOps;
  for (auto op : ops) {
    ancestorOps.push_back(domBlock->findAncestorOpInBlock(*op));
  }
  Operation *dom = ancestorOps[0];
  for (unsigned i = 1; i < ops.size(); i++) {
    if (ancestorOps[i]->isBeforeInBlock(dom)) {
      dom = ancestorOps[i];
    }
  }
  return dom;
}

void updateAccUsesInLoop(IRRewriter &builder, scf::ForOp forOp, MMAInfo &info,
                         ttng::TMEMAllocOp newAlloc, int numStages) {
  DominanceInfo domInfo(forOp);
  SmallVector<Operation *> directUses = getDirectAccUses(info.accLoad);
  if (!directUses.empty()) {
    Operation *domOp = findNearestCommonDominator(directUses, domInfo);
    assert(domOp != nullptr && "Could not find a common dominator");
    builder.setInsertionPoint(domOp);
    Value extractSlice = newAlloc;
    if (info.accIsMultiBuffered) {
      extractSlice =
          createSingleBufferView(builder, newAlloc, info.accExtractIdx);
    }
    auto load = builder.create<ttng::TMEMLoadOp>(
        domOp->getLoc(), info.accLoad.getType(), extractSlice);
    annotateWithPipelineStage(
        builder, forOp.getBody()->findAncestorOpInBlock(*load.getOperation()),
        numStages - 1);
    for (auto user : directUses) {
      user->replaceUsesOfWith(info.accLoad, load);
    }
  }
}

void updateAccUsesOutsideLoop(IRRewriter &builder, scf::ForOp forOp,
                              const MMAInfo &info, ttng::TMEMAllocOp newAlloc,
                              int extractIdxArgNo) {
  builder.setInsertionPointAfter(forOp);
  if (!info.yieldArgNo.has_value()) {
    return;
  }
  if (forOp.getResult(info.yieldArgNo.value()).getUsers().empty()) {
    return;
  }
  Value bufferSlice = newAlloc;
  if (info.accIsMultiBuffered) {
    Value extractIdxVal = forOp.getResult(extractIdxArgNo);
    bufferSlice = createSingleBufferView(builder, newAlloc, extractIdxVal);
  }
  auto load = builder.create<ttng::TMEMLoadOp>(
      forOp.getLoc(), forOp.getResult(info.yieldArgNo.value()).getType(),
      bufferSlice);
  forOp.getResult(info.yieldArgNo.value()).replaceAllUsesWith(load);
}

void updateAccDefsInLoop(IRRewriter &builder, scf::ForOp forOp, MMAInfo &info,
                         ttng::TMEMAllocOp newAlloc, int numStages) {
  assert(info.accDef.has_value());
  Operation *def = info.accDef->op;
  Value condition = info.accDef->condition;
  Location loc = def->getLoc();

  builder.setInsertionPointAfter(def);
  if (condition && condition.getDefiningOp()) {
    builder.setInsertionPointAfter(condition.getDefiningOp());
  }
  // if insertion point is outside the loop body, move it inside
  if (builder.getBlock() != forOp.getBody()) {
    builder.setInsertionPointAfter(&forOp.getBody()->front());
  }
  Value numStagesVal = builder.create<arith::ConstantIntOp>(loc, numStages, 32);

  Value newInsertIdx = builder.create<arith::AddIOp>(
      loc, info.accInsertIdx, builder.create<arith::ConstantIntOp>(loc, 1, 32));
  Value insWrap = builder.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                                newInsertIdx, numStagesVal);
  newInsertIdx = builder.create<arith::SelectOp>(
      loc, newInsertIdx.getType(), insWrap,
      builder.create<arith::ConstantIntOp>(loc, 0, 32), newInsertIdx);
  if (condition) {
    newInsertIdx =
        builder.create<arith::SelectOp>(loc, newInsertIdx.getType(), condition,
                                        newInsertIdx, info.accInsertIdx);
  }
  annotateWithPipelineStage(builder, newInsertIdx.getDefiningOp(), 0);

  Value newExtractIdx = builder.create<arith::AddIOp>(
      loc, info.accExtractIdx,
      builder.create<arith::ConstantIntOp>(loc, 1, 32));
  auto extWrap = builder.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                               newExtractIdx, numStagesVal);
  newExtractIdx = builder.create<arith::SelectOp>(
      loc, newExtractIdx.getType(), extWrap,
      builder.create<arith::ConstantIntOp>(loc, 0, 32), newExtractIdx);
  if (info.accDef->condition) {
    newExtractIdx = builder.create<arith::SelectOp>(
        loc, newExtractIdx.getType(), info.accDef->condition, newExtractIdx,
        info.accExtractIdx);
  }
  annotateWithPipelineStage(builder, newExtractIdx.getDefiningOp(), 1);

  if (info.accDef->initValue) {
    Value bufferSlice = createSingleBufferView(builder, newAlloc, newInsertIdx);
    Value vTrue = builder.create<arith::ConstantIntOp>(loc, 1, 1);
    auto tmemStore = builder.create<ttng::TMEMStoreOp>(
        loc, bufferSlice, info.accDef->initValue,
        condition ? condition : vTrue);
    annotateWithPipelineStage(builder, tmemStore, 0);
  }

  // Always update the for yield with the new insert and extract indices
  auto forYield = cast<scf::YieldOp>(forOp.getBody()->getTerminator());
  forYield->replaceUsesOfWith(info.accInsertIdx, newInsertIdx);
  forYield->replaceUsesOfWith(info.accExtractIdx, newExtractIdx);

  // Only update rest of the uses if the override is dist 0 (the same
  // loop iteration)
  if (info.accDef->distance == 0) {
    replaceAllUsesDominatedBy(newInsertIdx.getDefiningOp(), newInsertIdx,
                              info.accInsertIdx);
    replaceAllUsesDominatedBy(newExtractIdx.getDefiningOp(), newExtractIdx,
                              info.accExtractIdx);
  }

  if (info.accDef->initValue && condition) {
    assert(isa<arith::SelectOp>(info.accDef->op));
    info.accDef->op->erase();
  }

  info.accInsertIdx = newInsertIdx;
  info.accExtractIdx = newExtractIdx;
}

// Hoist tmem_allocs outside of the loop and update the mma ops to use the
// hoisted tmem allocs. Also, update the acc loads and stores to use the new
// tmem allocs.
void hoistAndUseTMemAlloc(IRRewriter &builder, scf::ForOp forOp,
                          Operation *mmaOp, MMAInfo &info, int numStages) {
  builder.setInsertionPoint(forOp);
  Value zero = builder.create<arith::ConstantIntOp>(forOp.getLoc(), 0, 32);
  Value one = builder.create<arith::ConstantIntOp>(forOp.getLoc(), 1, 32);
  Value numStagesVal =
      builder.create<arith::ConstantIntOp>(forOp.getLoc(), numStages, 32);
  Value vTrue = builder.create<arith::ConstantIntOp>(forOp.getLoc(), 1, 1);

  builder.setInsertionPoint(forOp);
  ttng::TMEMAllocOp newAlloc = createTMemAlloc(
      builder, info.accAlloc, info.accIsMultiBuffered, numStages);
  bool chainedAcc = info.yieldArgNo.has_value();
  if (chainedAcc) {
    Value accInitValue = forOp.getInitArgs()[info.yieldArgNo.value()];
    createInitStore(builder, newAlloc, accInitValue, info.accIsMultiBuffered);
  }

  // Update mma ops to use the hoisted tmem allocs
  Value insertSlice = newAlloc;
  if (info.accIsMultiBuffered) {
    builder.setInsertionPoint(mmaOp);
    insertSlice =
        createSingleBufferView(builder, insertSlice, info.accInsertIdx);
  }

  if (auto op = dyn_cast<ttng::TCGen5MMAOp>(mmaOp)) {
    op.getDMutable().assign(insertSlice);
  } else if (auto op = dyn_cast<ttng::TCGen5MMAScaledOp>(mmaOp)) {
    op.getDMutable().assign(insertSlice);
  }

  updateAccUsesInLoop(builder, forOp, info, newAlloc, numStages);
  assert(isa<BlockArgument>(info.accExtractIdx));
  int extractIdxArgNo =
      cast<BlockArgument>(info.accExtractIdx).getArgNumber() - 1;
  updateAccUsesOutsideLoop(builder, forOp, info, newAlloc, extractIdxArgNo);

  // Short circuit loop carry value that was holding the accumulator value,
  // removing the last reference to the loaded accumulator.
  if (info.yieldArgNo.has_value()) {
    forOp.getBody()->getTerminator()->setOperand(
        info.yieldArgNo.value(), forOp.getInitArgs()[info.yieldArgNo.value()]);
  }

  if (info.accIsMultiBuffered) {
    updateAccDefsInLoop(builder, forOp, info, newAlloc, numStages);
  }

  info.accLoad.erase();
  info.accAlloc.erase();
  info.accAlloc = newAlloc;
}

// Create multi-buffered barrier allocs and lower the MMA to MMA + wait barrier
void createBarrierAndWaitOps(IRRewriter &builder, scf::ForOp forOp,
                             Operation *mmaOp, MMAInfo &info, int numStages) {
  builder.setInsertionPoint(forOp);
  Value zero = builder.create<arith::ConstantIntOp>(forOp.getLoc(), 0, 32);
  Value one = builder.create<arith::ConstantIntOp>(forOp.getLoc(), 1, 32);
  Value numStagesVal =
      builder.create<arith::ConstantIntOp>(forOp.getLoc(), numStages, 32);

  info.barrierAlloc = createBarrierAlloc(forOp, mmaOp, numStages);

  Location loc = mmaOp->getLoc();
  builder.setInsertionPoint(mmaOp);

  Value barrierSlice =
      createSingleBufferView(builder, info.barrierAlloc, info.barrierIdx);

  if (auto op = dyn_cast<ttng::TCGen5MMAOp>(mmaOp)) {
    op.getBarrierMutable().assign(barrierSlice);
  } else if (auto op = dyn_cast<ttng::TCGen5MMAScaledOp>(mmaOp)) {
    op.getBarrierMutable().assign(barrierSlice);
  }

  builder.setInsertionPointAfter(mmaOp);
  auto waitOp =
      builder.create<ttng::WaitBarrierOp>(loc, barrierSlice, info.phase);
  annotateWithPipelineStage(builder, waitOp, numStages - 1);

  Value newBarrierIdx =
      builder.create<arith::AddIOp>(loc, info.barrierIdx, one);
  auto barWrap = builder.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                               newBarrierIdx, numStagesVal);

  // New barrierIdx and phase are in the first stage, so they can be used by
  // the ops that are ahead of them in either order or stages.
  newBarrierIdx = builder.create<arith::SelectOp>(loc, newBarrierIdx.getType(),
                                                  barWrap, zero, newBarrierIdx);
  replaceAllUsesDominatedBy(newBarrierIdx.getDefiningOp(), newBarrierIdx,
                            info.barrierIdx);
  info.barrierIdx = newBarrierIdx;
  annotateWithPipelineStage(builder, info.barrierIdx.getDefiningOp(), 0);

  Value newPhase = builder.create<arith::SelectOp>(
      loc, info.phase.getType(), barWrap,
      builder.create<arith::XOrIOp>(loc, info.phase, one), info.phase);
  replaceAllUsesDominatedBy(newPhase.getDefiningOp(), newPhase, info.phase);
  info.phase = newPhase;
  annotateWithPipelineStage(builder, info.phase.getDefiningOp(), 0);
}

// Find MMAs eligible for pipelining and lower them by:
// 1. Hoisting the accumulator allocation outside of the loop.
// 2. Creating a barrier alloc and lowering the MMA to MMA + wait barrier.
// 3. Updating the uses of the accumulator in the loop to use the new tmem
// alloc.
FailureOr<scf::ForOp> preProcessLoopForTC05MMAPipelining(scf::ForOp forOp,
                                                         int numStages) {
  SmallVector<Operation *> mmaOps;
  forOp.walk([&](Operation *op) {
    // Skip MMA nested in another forOp
    if (isa<ttng::TCGen5MMAOp>(op) &&
        op->getParentOfType<scf::ForOp>() == forOp) {
      mmaOps.push_back(op);
    }
  });

  if (mmaOps.empty()) {
    return failure();
  }

  mmaOps = getMMAsWithMultiBufferredOperands(forOp, mmaOps);

  if (mmaOps.empty()) {
    return failure();
  }

  IRRewriter builder(forOp->getContext());
  for (auto mmaOp : mmaOps) {
    // Avoid pipelining if in the backward slice of the mmaOp there is an
    // operation that is already assigned a stage, as it would make the pipeline
    // deeper than we are prepared for.
    SetVector<Operation *> backwardSlice;
    BackwardSliceOptions opt;
    opt.omitBlockArguments = true;
    getBackwardSlice(mmaOp, &backwardSlice, opt);
    if (llvm::any_of(backwardSlice, [&](Operation *op) {
          return op->hasAttr(kPipelineStageAttrName);
        })) {
      continue;
    }

    auto allocAndLoadOpt = getTMemAllocAndLoad(mmaOp);
    if (!allocAndLoadOpt) {
      continue;
    }
    auto [accAlloc, accLoad] = allocAndLoadOpt.value();
    bool hasDivergentUses = false;
    std::optional<int> yieldArgNo =
        trackAccChain(forOp, accLoad, accAlloc, hasDivergentUses);
    if (hasDivergentUses) {
      // If we can't tell for sure that the value is coming from the mma
      // accumulator, skip.
      continue;
    }
    if (yieldArgNo.has_value()) {
      int accInitArgNo =
          cast<BlockArgument>(accAlloc.getSrc()).getArgNumber() - 1;
      assert(yieldArgNo.value() == accInitArgNo);
    }

    std::optional<MMAInfo::AccOverridePoint> accOverridePoint =
        getAccOverrideOrFlagFalseInLoop(forOp, mmaOp);

    if (accOverridePoint.has_value() && accOverridePoint->distance > 1) {
      // We only support an override up to 1 iteration back.
      continue;
    }

    // Check pipelining and multi-buffering constraints
    // 1. If the acc is used by an op in the loop (other than the dot) it
    // requires multi-buffering to pipeline, as different stages cannot operate
    // on the same buffer.
    bool requiresMultiBuffer = !getDirectAccUses(accLoad).empty();

    // 2. If the acc is not owerwritten in the loop (by op other than the dot),
    // it cannot be multi-buffered. This is because the overwrite is the only
    // way to initialize next buffer without incurring a copy.
    bool canMultiBuffer = accOverridePoint.has_value();

    if (requiresMultiBuffer && !canMultiBuffer) {
      continue;
    }

    MMAInfo mmaInfo = {.accAlloc = accAlloc,
                       .accLoad = accLoad,
                       .accDef = accOverridePoint,
                       .yieldArgNo = yieldArgNo,
                       .accIsMultiBuffered = requiresMultiBuffer};

    builder.setInsertionPoint(forOp);
    Value zero = builder.create<arith::ConstantIntOp>(forOp.getLoc(), 0, 32);

    // Update for loop with new arguments
    SmallVector<Value> newOperands;
    const int argsPerMMA = 4;
    newOperands.push_back(zero); // phase
    newOperands.push_back(zero); // barrierIdx
    newOperands.push_back(zero); // accInsertIdx
    newOperands.push_back(zero); // accExtractIdx
    assert(newOperands.size() == argsPerMMA);

    int firstNewOperandIndex = forOp.getInitArgs().size();
    auto newForOp = replaceForOpWithNewSignature(builder, forOp, newOperands);
    forOp.erase();
    forOp = newForOp;

    mmaInfo.phase = forOp.getRegionIterArg(firstNewOperandIndex + 0);
    mmaInfo.barrierIdx = forOp.getRegionIterArg(firstNewOperandIndex + 1);
    mmaInfo.accInsertIdx = forOp.getRegionIterArg(firstNewOperandIndex + 2);
    mmaInfo.accExtractIdx = forOp.getRegionIterArg(firstNewOperandIndex + 3);

    SmallVector<Value> newYieldOperands;
    newYieldOperands.push_back(mmaInfo.phase);
    newYieldOperands.push_back(mmaInfo.barrierIdx);
    newYieldOperands.push_back(mmaInfo.accInsertIdx);
    newYieldOperands.push_back(mmaInfo.accExtractIdx);

    appendToForOpYield(forOp, newYieldOperands);

    annotateWithPipelineStage(builder, mmaOp, 0);
    hoistAndUseTMemAlloc(builder, forOp, mmaOp, mmaInfo, numStages);
    createBarrierAndWaitOps(builder, forOp, mmaOp, mmaInfo, numStages);

    // Invalidate and dealloc barrier
    builder.setInsertionPointAfter(forOp);
    Location loc = mmaOp->getLoc();
    for (int i = 0; i < numStages; i++) {
      Value barrierView =
          createSingleBufferView(builder, mmaInfo.barrierAlloc, i);
      builder.create<ttng::InvalBarrierOp>(loc, barrierView);
    }
    builder.create<ttg::LocalDeallocOp>(loc, mmaInfo.barrierAlloc);
  }

  return forOp;
}

bool insertUsersOfOp(tt::CoarseSchedule &coarseSchedule, Operation *op,
                     int stage, tt::CoarseSchedule::Cluster cluster) {
  bool changed = false;
  for (auto user : op->getUsers()) {
    if (coarseSchedule.count(user) == 0) {
      changed = true;
      coarseSchedule.insert(user, stage, cluster);
      insertUsersOfOp(coarseSchedule, user, stage, cluster);
    }
  }
  return changed;
}

bool getTC05MMASchedule(scf::ForOp &forOp, int numStages,
                        tt::PipeliningOption &options) {
  tt::CoarseSchedule coarseSchedule(numStages);
  tt::CoarseSchedule::Cluster cluster = coarseSchedule.clusters.newAtFront();
  for (auto &op : forOp.getBody()->without_terminator()) {
    if (op.hasAttr(kPipelineStageAttrName)) {
      int stage =
          op.getAttrOfType<IntegerAttr>(kPipelineStageAttrName).getInt();
      coarseSchedule.insert(&op, stage, cluster);
    }
  }

  auto scheduleDependencies = [&]() {
    bool fixedPoint = false;
    while (!fixedPoint) {
      fixedPoint = true;
      // Schedule upstream dependencies
      for (int stage = 0; stage < numStages; stage++) {
        for (auto &op : forOp.getBody()->without_terminator()) {
          if (coarseSchedule.count(&op) && coarseSchedule[&op].first == stage) {
            bool changed = coarseSchedule.insertDepsOfOp(&op, stage, cluster,
                                                         /*includeArg=*/false);
            fixedPoint &= !changed;
          }
        }
      }
      // Schedule downstream dependencies
      for (int stage = numStages - 1; stage >= 0; stage--) {
        for (auto &op : forOp.getBody()->without_terminator()) {
          if (coarseSchedule.count(&op) && coarseSchedule[&op].first == stage) {
            bool changed = insertUsersOfOp(coarseSchedule, &op, stage, cluster);
            fixedPoint &= !changed;
          }
        }
      }
    }
  };

  scheduleDependencies();

  // Make sure that async loads are scheduled in the same stage they are used.
  DenseMap<ttg::LocalAllocOp, int> allocToStage;
  DenseMap<ttg::LocalAllocOp, ttng::WaitBarrierOp> allocToBarrierWait;
  for (auto &op : forOp.getBody()->without_terminator()) {
    if (auto barrierWait = dyn_cast<ttng::WaitBarrierOp>(op)) {
      auto localAlloc = findShmemAlloc(barrierWait.getAlloc());
      assert(localAlloc);
      assert(allocToBarrierWait.count(localAlloc) == 0);
      allocToBarrierWait[localAlloc] = barrierWait;
      continue;
    }
    if (!coarseSchedule.count(&op))
      continue;

    auto [stage, cluster] = coarseSchedule[&op];
    for (auto arg : op.getOperands()) {
      auto memDescTy = dyn_cast<ttg::MemDescType>(arg.getType());
      if (!memDescTy)
        continue;

      auto localAlloc = findShmemAlloc(arg);
      if (!localAlloc)
        continue;

      allocToStage[localAlloc] = stage;
    }
  }

  for (auto &op : forOp.getBody()->without_terminator()) {
    Value memDesc;
    Value barrier;
    if (auto copyOp = dyn_cast<ttg::AsyncCopyGlobalToLocalOp>(op)) {
      memDesc = copyOp.getResult();
    } else if (auto copyOp = dyn_cast<ttng::AsyncTMACopyGlobalToLocalOp>(op)) {
      memDesc = copyOp.getResult();
      barrier = copyOp.getBarrier();
    } else if (auto gatherOp = dyn_cast<ttng::AsyncTMAGatherOp>(op)) {
      memDesc = gatherOp.getResult();
      barrier = gatherOp.getBarrier();
    } else if (auto storeOp = dyn_cast<ttng::AsyncTMACopyLocalToGlobalOp>(op)) {
      memDesc = storeOp.getSrc();
    } else if (auto scatterOp = dyn_cast<ttng::AsyncTMAScatterOp>(op)) {
      memDesc = scatterOp.getSrc();
    } else {
      continue;
    }
    auto localAlloc = findShmemAlloc(memDesc);
    assert(localAlloc);
    int stage = allocToStage[localAlloc];
    coarseSchedule.insert(&op, stage, cluster);

    // Schedule any barrier wait in the same stage as well, otherwise we will
    // change the loop distance to the wait.
    if (!barrier)
      continue;
    auto barrierAlloc = findShmemAlloc(barrier);
    assert(barrierAlloc);
    auto waitOp = allocToBarrierWait[barrierAlloc];
    // NOTE: barriers can be grouped onto multiple loads, so schedule into the
    // eariest stage where the result is used. This means we reduce the distance
    // between the tma issue and wait, but it is at least correct.
    coarseSchedule.insertMinimum(waitOp, stage, cluster);
  }

  scheduleDependencies();

  // Schedule everything else to stage 0
  for (auto &op : forOp.getBody()->without_terminator()) {
    op.removeAttr(kPipelineStageAttrName);
    if (coarseSchedule.count(&op) == 0) {
      coarseSchedule.insert(&op, 0, cluster);
    }
  }

  std::vector<std::pair<Operation *, unsigned>> schedule =
      coarseSchedule.createFinalSchedule(forOp);

  options.getScheduleFn =
      [schedule](scf::ForOp forOp,
                 std::vector<std::pair<Operation *, unsigned>> &s) {
        s = std::move(schedule);
      };
  options.peelEpilogue = false;
  options.predicateFn = tt::predicateOp;
  options.supportDynamicLoops = true;

  return true;
}

} // namespace

void mlir::triton::pipelineTC05MMALoops(ModuleOp module,
                                        const SmallVector<scf::ForOp> &forOps,
                                        int numStages, bool disableExpander) {
  for (auto forOp : forOps) {
    FailureOr<scf::ForOp> newForOp =
        preProcessLoopForTC05MMAPipelining(forOp, numStages);
    if (succeeded(newForOp)) {
      (*newForOp)->setAttr(kPipelineAttrName,
                           UnitAttr::get(module.getContext()));
    }
  }
  // Run canonicalization to clean up the short-circuited loop carried values.
  mlir::RewritePatternSet patterns(module.getContext());
  scf::ForOp::getCanonicalizationPatterns(patterns, module.getContext());
  if (applyPatternsGreedily(module, std::move(patterns)).failed()) {
    llvm::errs() << "Failed to canonicalize the module\n";
    return;
  }

  if (!disableExpander) {
    SmallVector<scf::ForOp> loops;
    module->walk([&](scf::ForOp forOp) {
      if (forOp->getAttr(kPipelineAttrName))
        loops.push_back(forOp);
    });

    for (auto forOp : loops) {
      mlir::triton::PipeliningOption options;
      bool foundSchedule = getTC05MMASchedule(forOp, /*numStages=*/2, options);
      assert(foundSchedule && "Failed to find a schedule for TC05MMA");

      IRRewriter rewriter(forOp->getContext());
      rewriter.setInsertionPoint(forOp);
      FailureOr<scf::ForOp> newForOp =
          mlir::triton::pipelineForLoop(rewriter, forOp, options);
    }
  }
}