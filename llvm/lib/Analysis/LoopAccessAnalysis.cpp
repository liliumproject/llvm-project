//===- LoopAccessAnalysis.cpp - Loop Access Analysis Implementation --------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The implementation for the loop memory dependence that was originally
// developed for the loop vectorizer.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/AssumeBundleQueries.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ScalarEvolutionPatternMatch.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <utility>
#include <variant>
#include <vector>

using namespace llvm;
using namespace llvm::SCEVPatternMatch;

#define DEBUG_TYPE "loop-accesses"

static cl::opt<unsigned, true>
VectorizationFactor("force-vector-width", cl::Hidden,
                    cl::desc("Sets the SIMD width. Zero is autoselect."),
                    cl::location(VectorizerParams::VectorizationFactor));
unsigned VectorizerParams::VectorizationFactor;

static cl::opt<unsigned, true>
VectorizationInterleave("force-vector-interleave", cl::Hidden,
                        cl::desc("Sets the vectorization interleave count. "
                                 "Zero is autoselect."),
                        cl::location(
                            VectorizerParams::VectorizationInterleave));
unsigned VectorizerParams::VectorizationInterleave;

static cl::opt<unsigned, true> RuntimeMemoryCheckThreshold(
    "runtime-memory-check-threshold", cl::Hidden,
    cl::desc("When performing memory disambiguation checks at runtime do not "
             "generate more than this number of comparisons (default = 8)."),
    cl::location(VectorizerParams::RuntimeMemoryCheckThreshold), cl::init(8));
unsigned VectorizerParams::RuntimeMemoryCheckThreshold;

/// The maximum iterations used to merge memory checks
static cl::opt<unsigned> MemoryCheckMergeThreshold(
    "memory-check-merge-threshold", cl::Hidden,
    cl::desc("Maximum number of comparisons done when trying to merge "
             "runtime memory checks. (default = 100)"),
    cl::init(100));

/// Maximum SIMD width.
const unsigned VectorizerParams::MaxVectorWidth = 64;

/// We collect dependences up to this threshold.
static cl::opt<unsigned>
    MaxDependences("max-dependences", cl::Hidden,
                   cl::desc("Maximum number of dependences collected by "
                            "loop-access analysis (default = 100)"),
                   cl::init(100));

/// This enables versioning on the strides of symbolically striding memory
/// accesses in code like the following.
///   for (i = 0; i < N; ++i)
///     A[i * Stride1] += B[i * Stride2] ...
///
/// Will be roughly translated to
///    if (Stride1 == 1 && Stride2 == 1) {
///      for (i = 0; i < N; i+=4)
///       A[i:i+3] += ...
///    } else
///      ...
static cl::opt<bool> EnableMemAccessVersioning(
    "enable-mem-access-versioning", cl::init(true), cl::Hidden,
    cl::desc("Enable symbolic stride memory access versioning"));

/// Enable store-to-load forwarding conflict detection. This option can
/// be disabled for correctness testing.
static cl::opt<bool> EnableForwardingConflictDetection(
    "store-to-load-forwarding-conflict-detection", cl::Hidden,
    cl::desc("Enable conflict detection in loop-access analysis"),
    cl::init(true));

static cl::opt<unsigned> MaxForkedSCEVDepth(
    "max-forked-scev-depth", cl::Hidden,
    cl::desc("Maximum recursion depth when finding forked SCEVs (default = 5)"),
    cl::init(5));

static cl::opt<bool> SpeculateUnitStride(
    "laa-speculate-unit-stride", cl::Hidden,
    cl::desc("Speculate that non-constant strides are unit in LAA"),
    cl::init(true));

static cl::opt<bool, true> HoistRuntimeChecks(
    "hoist-runtime-checks", cl::Hidden,
    cl::desc(
        "Hoist inner loop runtime memory checks to outer loop if possible"),
    cl::location(VectorizerParams::HoistRuntimeChecks), cl::init(true));
bool VectorizerParams::HoistRuntimeChecks;

bool VectorizerParams::isInterleaveForced() {
  return ::VectorizationInterleave.getNumOccurrences() > 0;
}

const SCEV *llvm::replaceSymbolicStrideSCEV(PredicatedScalarEvolution &PSE,
                                            const DenseMap<Value *, const SCEV *> &PtrToStride,
                                            Value *Ptr) {
  const SCEV *OrigSCEV = PSE.getSCEV(Ptr);

  // If there is an entry in the map return the SCEV of the pointer with the
  // symbolic stride replaced by one.
  const SCEV *StrideSCEV = PtrToStride.lookup(Ptr);
  if (!StrideSCEV)
    // For a non-symbolic stride, just return the original expression.
    return OrigSCEV;

  // Note: This assert is both overly strong and overly weak.  The actual
  // invariant here is that StrideSCEV should be loop invariant.  The only
  // such invariant strides we happen to speculate right now are unknowns
  // and thus this is a reasonable proxy of the actual invariant.
  assert(isa<SCEVUnknown>(StrideSCEV) && "shouldn't be in map");

  ScalarEvolution *SE = PSE.getSE();
  const SCEV *CT = SE->getOne(StrideSCEV->getType());
  PSE.addPredicate(*SE->getEqualPredicate(StrideSCEV, CT));
  const SCEV *Expr = PSE.getSCEV(Ptr);

  LLVM_DEBUG(dbgs() << "LAA: Replacing SCEV: " << *OrigSCEV
	     << " by: " << *Expr << "\n");
  return Expr;
}

RuntimeCheckingPtrGroup::RuntimeCheckingPtrGroup(
    unsigned Index, const RuntimePointerChecking &RtCheck)
    : High(RtCheck.Pointers[Index].End), Low(RtCheck.Pointers[Index].Start),
      AddressSpace(RtCheck.Pointers[Index]
                       .PointerValue->getType()
                       ->getPointerAddressSpace()),
      NeedsFreeze(RtCheck.Pointers[Index].NeedsFreeze) {
  Members.push_back(Index);
}

/// Returns \p A + \p B, if it is guaranteed not to unsigned wrap. Otherwise
/// return nullptr. \p A and \p B must have the same type.
static const SCEV *addSCEVNoOverflow(const SCEV *A, const SCEV *B,
                                     ScalarEvolution &SE) {
  if (!SE.willNotOverflow(Instruction::Add, /*IsSigned=*/false, A, B))
    return nullptr;
  return SE.getAddExpr(A, B);
}

/// Returns \p A * \p B, if it is guaranteed not to unsigned wrap. Otherwise
/// return nullptr. \p A and \p B must have the same type.
static const SCEV *mulSCEVOverflow(const SCEV *A, const SCEV *B,
                                   ScalarEvolution &SE) {
  if (!SE.willNotOverflow(Instruction::Mul, /*IsSigned=*/false, A, B))
    return nullptr;
  return SE.getMulExpr(A, B);
}

/// Return true, if evaluating \p AR at \p MaxBTC cannot wrap, because \p AR at
/// \p MaxBTC is guaranteed inbounds of the accessed object.
static bool
evaluatePtrAddRecAtMaxBTCWillNotWrap(const SCEVAddRecExpr *AR,
                                     const SCEV *MaxBTC, const SCEV *EltSize,
                                     ScalarEvolution &SE, const DataLayout &DL,
                                     DominatorTree *DT, AssumptionCache *AC) {
  auto *PointerBase = SE.getPointerBase(AR->getStart());
  auto *StartPtr = dyn_cast<SCEVUnknown>(PointerBase);
  if (!StartPtr)
    return false;
  const Loop *L = AR->getLoop();
  bool CheckForNonNull, CheckForFreed;
  Value *StartPtrV = StartPtr->getValue();
  uint64_t DerefBytes = StartPtrV->getPointerDereferenceableBytes(
      DL, CheckForNonNull, CheckForFreed);

  if (DerefBytes && (CheckForNonNull || CheckForFreed))
    return false;

  const SCEV *Step = AR->getStepRecurrence(SE);
  Type *WiderTy = SE.getWiderType(MaxBTC->getType(), Step->getType());
  const SCEV *DerefBytesSCEV = SE.getConstant(WiderTy, DerefBytes);

  // Check if we have a suitable dereferencable assumption we can use.
  if (!StartPtrV->canBeFreed()) {
    RetainedKnowledge DerefRK = getKnowledgeValidInContext(
        StartPtrV, {Attribute::Dereferenceable}, *AC,
        L->getLoopPredecessor()->getTerminator(), DT);
    if (DerefRK) {
      DerefBytesSCEV = SE.getUMaxExpr(
          DerefBytesSCEV, SE.getConstant(WiderTy, DerefRK.ArgValue));
    }
  }

  if (DerefBytesSCEV->isZero())
    return false;

  bool IsKnownNonNegative = SE.isKnownNonNegative(Step);
  if (!IsKnownNonNegative && !SE.isKnownNegative(Step))
    return false;

  Step = SE.getNoopOrSignExtend(Step, WiderTy);
  MaxBTC = SE.getNoopOrZeroExtend(MaxBTC, WiderTy);

  // For the computations below, make sure they don't unsigned wrap.
  if (!SE.isKnownPredicate(CmpInst::ICMP_UGE, AR->getStart(), StartPtr))
    return false;
  const SCEV *StartOffset = SE.getNoopOrZeroExtend(
      SE.getMinusSCEV(AR->getStart(), StartPtr), WiderTy);

  const SCEV *OffsetAtLastIter =
      mulSCEVOverflow(MaxBTC, SE.getAbsExpr(Step, /*IsNSW=*/false), SE);
  if (!OffsetAtLastIter)
    return false;

  const SCEV *OffsetEndBytes = addSCEVNoOverflow(
      OffsetAtLastIter, SE.getNoopOrZeroExtend(EltSize, WiderTy), SE);
  if (!OffsetEndBytes)
    return false;

  if (IsKnownNonNegative) {
    // For positive steps, check if
    //  (AR->getStart() - StartPtr) + (MaxBTC  * Step) + EltSize <= DerefBytes,
    // while making sure none of the computations unsigned wrap themselves.
    const SCEV *EndBytes = addSCEVNoOverflow(StartOffset, OffsetEndBytes, SE);
    if (!EndBytes)
      return false;
    return SE.isKnownPredicate(CmpInst::ICMP_ULE, EndBytes, DerefBytesSCEV);
  }

  // For negative steps check if
  //  * StartOffset >= (MaxBTC * Step + EltSize)
  //  * StartOffset <= DerefBytes.
  assert(SE.isKnownNegative(Step) && "must be known negative");
  return SE.isKnownPredicate(CmpInst::ICMP_SGE, StartOffset, OffsetEndBytes) &&
         SE.isKnownPredicate(CmpInst::ICMP_ULE, StartOffset, DerefBytesSCEV);
}

std::pair<const SCEV *, const SCEV *> llvm::getStartAndEndForAccess(
    const Loop *Lp, const SCEV *PtrExpr, Type *AccessTy, const SCEV *BTC,
    const SCEV *MaxBTC, ScalarEvolution *SE,
    DenseMap<std::pair<const SCEV *, Type *>,
             std::pair<const SCEV *, const SCEV *>> *PointerBounds,
    DominatorTree *DT, AssumptionCache *AC) {
  std::pair<const SCEV *, const SCEV *> *PtrBoundsPair;
  if (PointerBounds) {
    auto [Iter, Ins] = PointerBounds->insert(
        {{PtrExpr, AccessTy},
         {SE->getCouldNotCompute(), SE->getCouldNotCompute()}});
    if (!Ins)
      return Iter->second;
    PtrBoundsPair = &Iter->second;
  }

  const SCEV *ScStart;
  const SCEV *ScEnd;

  auto &DL = Lp->getHeader()->getDataLayout();
  Type *IdxTy = DL.getIndexType(PtrExpr->getType());
  const SCEV *EltSizeSCEV = SE->getStoreSizeOfExpr(IdxTy, AccessTy);
  if (SE->isLoopInvariant(PtrExpr, Lp)) {
    ScStart = ScEnd = PtrExpr;
  } else if (auto *AR = dyn_cast<SCEVAddRecExpr>(PtrExpr)) {
    ScStart = AR->getStart();
    if (!isa<SCEVCouldNotCompute>(BTC))
      // Evaluating AR at an exact BTC is safe: LAA separately checks that
      // accesses cannot wrap in the loop. If evaluating AR at BTC wraps, then
      // the loop either triggers UB when executing a memory access with a
      // poison pointer or the wrapping/poisoned pointer is not used.
      ScEnd = AR->evaluateAtIteration(BTC, *SE);
    else {
      // Evaluating AR at MaxBTC may wrap and create an expression that is less
      // than the start of the AddRec due to wrapping (for example consider
      // MaxBTC = -2). If that's the case, set ScEnd to -(EltSize + 1). ScEnd
      // will get incremented by EltSize before returning, so this effectively
      // sets ScEnd to the maximum unsigned value for the type. Note that LAA
      // separately checks that accesses cannot not wrap, so unsigned max
      // represents an upper bound.
      if (evaluatePtrAddRecAtMaxBTCWillNotWrap(AR, MaxBTC, EltSizeSCEV, *SE, DL,
                                               DT, AC)) {
        ScEnd = AR->evaluateAtIteration(MaxBTC, *SE);
      } else {
        ScEnd = SE->getAddExpr(
            SE->getNegativeSCEV(EltSizeSCEV),
            SE->getSCEV(ConstantExpr::getIntToPtr(
                ConstantInt::get(EltSizeSCEV->getType(), -1), AR->getType())));
      }
    }
    const SCEV *Step = AR->getStepRecurrence(*SE);

    // For expressions with negative step, the upper bound is ScStart and the
    // lower bound is ScEnd.
    if (const auto *CStep = dyn_cast<SCEVConstant>(Step)) {
      if (CStep->getValue()->isNegative())
        std::swap(ScStart, ScEnd);
    } else {
      // Fallback case: the step is not constant, but we can still
      // get the upper and lower bounds of the interval by using min/max
      // expressions.
      ScStart = SE->getUMinExpr(ScStart, ScEnd);
      ScEnd = SE->getUMaxExpr(AR->getStart(), ScEnd);
    }
  } else
    return {SE->getCouldNotCompute(), SE->getCouldNotCompute()};

  assert(SE->isLoopInvariant(ScStart, Lp) && "ScStart needs to be invariant");
  assert(SE->isLoopInvariant(ScEnd, Lp) && "ScEnd needs to be invariant");

  // Add the size of the pointed element to ScEnd.
  ScEnd = SE->getAddExpr(ScEnd, EltSizeSCEV);

  std::pair<const SCEV *, const SCEV *> Res = {ScStart, ScEnd};
  if (PointerBounds)
    *PtrBoundsPair = Res;
  return Res;
}

/// Calculate Start and End points of memory access using
/// getStartAndEndForAccess.
void RuntimePointerChecking::insert(Loop *Lp, Value *Ptr, const SCEV *PtrExpr,
                                    Type *AccessTy, bool WritePtr,
                                    unsigned DepSetId, unsigned ASId,
                                    PredicatedScalarEvolution &PSE,
                                    bool NeedsFreeze) {
  const SCEV *SymbolicMaxBTC = PSE.getSymbolicMaxBackedgeTakenCount();
  const SCEV *BTC = PSE.getBackedgeTakenCount();
  const auto &[ScStart, ScEnd] = getStartAndEndForAccess(
      Lp, PtrExpr, AccessTy, BTC, SymbolicMaxBTC, PSE.getSE(),
      &DC.getPointerBounds(), DC.getDT(), DC.getAC());
  assert(!isa<SCEVCouldNotCompute>(ScStart) &&
         !isa<SCEVCouldNotCompute>(ScEnd) &&
         "must be able to compute both start and end expressions");
  Pointers.emplace_back(Ptr, ScStart, ScEnd, WritePtr, DepSetId, ASId, PtrExpr,
                        NeedsFreeze);
}

bool RuntimePointerChecking::tryToCreateDiffCheck(
    const RuntimeCheckingPtrGroup &CGI, const RuntimeCheckingPtrGroup &CGJ) {
  // If either group contains multiple different pointers, bail out.
  // TODO: Support multiple pointers by using the minimum or maximum pointer,
  // depending on src & sink.
  if (CGI.Members.size() != 1 || CGJ.Members.size() != 1)
    return false;

  const PointerInfo *Src = &Pointers[CGI.Members[0]];
  const PointerInfo *Sink = &Pointers[CGJ.Members[0]];

  // If either pointer is read and written, multiple checks may be needed. Bail
  // out.
  if (!DC.getOrderForAccess(Src->PointerValue, !Src->IsWritePtr).empty() ||
      !DC.getOrderForAccess(Sink->PointerValue, !Sink->IsWritePtr).empty())
    return false;

  ArrayRef<unsigned> AccSrc =
      DC.getOrderForAccess(Src->PointerValue, Src->IsWritePtr);
  ArrayRef<unsigned> AccSink =
      DC.getOrderForAccess(Sink->PointerValue, Sink->IsWritePtr);
  // If either pointer is accessed multiple times, there may not be a clear
  // src/sink relation. Bail out for now.
  if (AccSrc.size() != 1 || AccSink.size() != 1)
    return false;

  // If the sink is accessed before src, swap src/sink.
  if (AccSink[0] < AccSrc[0])
    std::swap(Src, Sink);

  const SCEVConstant *Step;
  const SCEV *SrcStart;
  const SCEV *SinkStart;
  const Loop *InnerLoop = DC.getInnermostLoop();
  if (!match(Src->Expr,
             m_scev_AffineAddRec(m_SCEV(SrcStart), m_SCEVConstant(Step),
                                 m_SpecificLoop(InnerLoop))) ||
      !match(Sink->Expr,
             m_scev_AffineAddRec(m_SCEV(SinkStart), m_scev_Specific(Step),
                                 m_SpecificLoop(InnerLoop))))
    return false;

  SmallVector<Instruction *, 4> SrcInsts =
      DC.getInstructionsForAccess(Src->PointerValue, Src->IsWritePtr);
  SmallVector<Instruction *, 4> SinkInsts =
      DC.getInstructionsForAccess(Sink->PointerValue, Sink->IsWritePtr);
  Type *SrcTy = getLoadStoreType(SrcInsts[0]);
  Type *DstTy = getLoadStoreType(SinkInsts[0]);
  if (isa<ScalableVectorType>(SrcTy) || isa<ScalableVectorType>(DstTy))
    return false;

  const DataLayout &DL = InnerLoop->getHeader()->getDataLayout();
  unsigned AllocSize =
      std::max(DL.getTypeAllocSize(SrcTy), DL.getTypeAllocSize(DstTy));

  // Only matching constant steps matching the AllocSize are supported at the
  // moment. This simplifies the difference computation. Can be extended in the
  // future.
  if (Step->getAPInt().abs() != AllocSize)
    return false;

  IntegerType *IntTy =
      IntegerType::get(Src->PointerValue->getContext(),
                       DL.getPointerSizeInBits(CGI.AddressSpace));

  // When counting down, the dependence distance needs to be swapped.
  if (Step->getValue()->isNegative())
    std::swap(SinkStart, SrcStart);

  const SCEV *SinkStartInt = SE->getPtrToIntExpr(SinkStart, IntTy);
  const SCEV *SrcStartInt = SE->getPtrToIntExpr(SrcStart, IntTy);
  if (isa<SCEVCouldNotCompute>(SinkStartInt) ||
      isa<SCEVCouldNotCompute>(SrcStartInt))
    return false;

  // If the start values for both Src and Sink also vary according to an outer
  // loop, then it's probably better to avoid creating diff checks because
  // they may not be hoisted. We should instead let llvm::addRuntimeChecks
  // do the expanded full range overlap checks, which can be hoisted.
  if (HoistRuntimeChecks && InnerLoop->getParentLoop() &&
      isa<SCEVAddRecExpr>(SinkStartInt) && isa<SCEVAddRecExpr>(SrcStartInt)) {
    auto *SrcStartAR = cast<SCEVAddRecExpr>(SrcStartInt);
    auto *SinkStartAR = cast<SCEVAddRecExpr>(SinkStartInt);
    const Loop *StartARLoop = SrcStartAR->getLoop();
    if (StartARLoop == SinkStartAR->getLoop() &&
        StartARLoop == InnerLoop->getParentLoop() &&
        // If the diff check would already be loop invariant (due to the
        // recurrences being the same), then we prefer to keep the diff checks
        // because they are cheaper.
        SrcStartAR->getStepRecurrence(*SE) !=
            SinkStartAR->getStepRecurrence(*SE)) {
      LLVM_DEBUG(dbgs() << "LAA: Not creating diff runtime check, since these "
                           "cannot be hoisted out of the outer loop\n");
      return false;
    }
  }

  LLVM_DEBUG(dbgs() << "LAA: Creating diff runtime check for:\n"
                    << "SrcStart: " << *SrcStartInt << '\n'
                    << "SinkStartInt: " << *SinkStartInt << '\n');
  DiffChecks.emplace_back(SrcStartInt, SinkStartInt, AllocSize,
                          Src->NeedsFreeze || Sink->NeedsFreeze);
  return true;
}

SmallVector<RuntimePointerCheck, 4> RuntimePointerChecking::generateChecks() {
  SmallVector<RuntimePointerCheck, 4> Checks;

  for (unsigned I = 0; I < CheckingGroups.size(); ++I) {
    for (unsigned J = I + 1; J < CheckingGroups.size(); ++J) {
      const RuntimeCheckingPtrGroup &CGI = CheckingGroups[I];
      const RuntimeCheckingPtrGroup &CGJ = CheckingGroups[J];

      if (needsChecking(CGI, CGJ)) {
        CanUseDiffCheck = CanUseDiffCheck && tryToCreateDiffCheck(CGI, CGJ);
        Checks.emplace_back(&CGI, &CGJ);
      }
    }
  }
  return Checks;
}

void RuntimePointerChecking::generateChecks(
    MemoryDepChecker::DepCandidates &DepCands, bool UseDependencies) {
  assert(Checks.empty() && "Checks is not empty");
  groupChecks(DepCands, UseDependencies);
  Checks = generateChecks();
}

bool RuntimePointerChecking::needsChecking(
    const RuntimeCheckingPtrGroup &M, const RuntimeCheckingPtrGroup &N) const {
  for (const auto &I : M.Members)
    for (const auto &J : N.Members)
      if (needsChecking(I, J))
        return true;
  return false;
}

/// Compare \p I and \p J and return the minimum.
/// Return nullptr in case we couldn't find an answer.
static const SCEV *getMinFromExprs(const SCEV *I, const SCEV *J,
                                   ScalarEvolution *SE) {
  std::optional<APInt> Diff = SE->computeConstantDifference(J, I);
  if (!Diff)
    return nullptr;
  return Diff->isNegative() ? J : I;
}

bool RuntimeCheckingPtrGroup::addPointer(
    unsigned Index, const RuntimePointerChecking &RtCheck) {
  return addPointer(
      Index, RtCheck.Pointers[Index].Start, RtCheck.Pointers[Index].End,
      RtCheck.Pointers[Index].PointerValue->getType()->getPointerAddressSpace(),
      RtCheck.Pointers[Index].NeedsFreeze, *RtCheck.SE);
}

bool RuntimeCheckingPtrGroup::addPointer(unsigned Index, const SCEV *Start,
                                         const SCEV *End, unsigned AS,
                                         bool NeedsFreeze,
                                         ScalarEvolution &SE) {
  assert(AddressSpace == AS &&
         "all pointers in a checking group must be in the same address space");

  // Compare the starts and ends with the known minimum and maximum
  // of this set. We need to know how we compare against the min/max
  // of the set in order to be able to emit memchecks.
  const SCEV *Min0 = getMinFromExprs(Start, Low, &SE);
  if (!Min0)
    return false;

  const SCEV *Min1 = getMinFromExprs(End, High, &SE);
  if (!Min1)
    return false;

  // Update the low bound  expression if we've found a new min value.
  if (Min0 == Start)
    Low = Start;

  // Update the high bound expression if we've found a new max value.
  if (Min1 != End)
    High = End;

  Members.push_back(Index);
  this->NeedsFreeze |= NeedsFreeze;
  return true;
}

void RuntimePointerChecking::groupChecks(
    MemoryDepChecker::DepCandidates &DepCands, bool UseDependencies) {
  // We build the groups from dependency candidates equivalence classes
  // because:
  //    - We know that pointers in the same equivalence class share
  //      the same underlying object and therefore there is a chance
  //      that we can compare pointers
  //    - We wouldn't be able to merge two pointers for which we need
  //      to emit a memcheck. The classes in DepCands are already
  //      conveniently built such that no two pointers in the same
  //      class need checking against each other.

  // We use the following (greedy) algorithm to construct the groups
  // For every pointer in the equivalence class:
  //   For each existing group:
  //   - if the difference between this pointer and the min/max bounds
  //     of the group is a constant, then make the pointer part of the
  //     group and update the min/max bounds of that group as required.

  CheckingGroups.clear();

  // If we need to check two pointers to the same underlying object
  // with a non-constant difference, we shouldn't perform any pointer
  // grouping with those pointers. This is because we can easily get
  // into cases where the resulting check would return false, even when
  // the accesses are safe.
  //
  // The following example shows this:
  // for (i = 0; i < 1000; ++i)
  //   a[5000 + i * m] = a[i] + a[i + 9000]
  //
  // Here grouping gives a check of (5000, 5000 + 1000 * m) against
  // (0, 10000) which is always false. However, if m is 1, there is no
  // dependence. Not grouping the checks for a[i] and a[i + 9000] allows
  // us to perform an accurate check in this case.
  //
  // In the above case, we have a non-constant distance and an Unknown
  // dependence between accesses to the same underlying object, and could retry
  // with runtime checks. Therefore UseDependencies is false. In this case we
  // will use the fallback path and create separate checking groups for all
  // pointers.

  // If we don't have the dependency partitions, construct a new
  // checking pointer group for each pointer. This is also required
  // for correctness, because in this case we can have checking between
  // pointers to the same underlying object.
  if (!UseDependencies) {
    for (unsigned I = 0; I < Pointers.size(); ++I)
      CheckingGroups.emplace_back(I, *this);
    return;
  }

  unsigned TotalComparisons = 0;

  DenseMap<Value *, SmallVector<unsigned>> PositionMap;
  for (unsigned Index = 0; Index < Pointers.size(); ++Index)
    PositionMap[Pointers[Index].PointerValue].push_back(Index);

  // We need to keep track of what pointers we've already seen so we
  // don't process them twice.
  SmallSet<unsigned, 2> Seen;

  // Go through all equivalence classes, get the "pointer check groups"
  // and add them to the overall solution. We use the order in which accesses
  // appear in 'Pointers' to enforce determinism.
  for (unsigned I = 0; I < Pointers.size(); ++I) {
    // We've seen this pointer before, and therefore already processed
    // its equivalence class.
    if (Seen.contains(I))
      continue;

    MemoryDepChecker::MemAccessInfo Access(Pointers[I].PointerValue,
                                           Pointers[I].IsWritePtr);

    SmallVector<RuntimeCheckingPtrGroup, 2> Groups;

    // Because DepCands is constructed by visiting accesses in the order in
    // which they appear in alias sets (which is deterministic) and the
    // iteration order within an equivalence class member is only dependent on
    // the order in which unions and insertions are performed on the
    // equivalence class, the iteration order is deterministic.
    for (auto M : DepCands.members(Access)) {
      auto PointerI = PositionMap.find(M.getPointer());
      // If we can't find the pointer in PositionMap that means we can't
      // generate a memcheck for it.
      if (PointerI == PositionMap.end())
        continue;
      for (unsigned Pointer : PointerI->second) {
        bool Merged = false;
        // Mark this pointer as seen.
        Seen.insert(Pointer);

        // Go through all the existing sets and see if we can find one
        // which can include this pointer.
        for (RuntimeCheckingPtrGroup &Group : Groups) {
          // Don't perform more than a certain amount of comparisons.
          // This should limit the cost of grouping the pointers to something
          // reasonable.  If we do end up hitting this threshold, the algorithm
          // will create separate groups for all remaining pointers.
          if (TotalComparisons > MemoryCheckMergeThreshold)
            break;

          TotalComparisons++;

          if (Group.addPointer(Pointer, *this)) {
            Merged = true;
            break;
          }
        }

        if (!Merged)
          // We couldn't add this pointer to any existing set or the threshold
          // for the number of comparisons has been reached. Create a new group
          // to hold the current pointer.
          Groups.emplace_back(Pointer, *this);
      }
    }

    // We've computed the grouped checks for this partition.
    // Save the results and continue with the next one.
    llvm::append_range(CheckingGroups, Groups);
  }
}

bool RuntimePointerChecking::arePointersInSamePartition(
    const SmallVectorImpl<int> &PtrToPartition, unsigned PtrIdx1,
    unsigned PtrIdx2) {
  return (PtrToPartition[PtrIdx1] != -1 &&
          PtrToPartition[PtrIdx1] == PtrToPartition[PtrIdx2]);
}

bool RuntimePointerChecking::needsChecking(unsigned I, unsigned J) const {
  const PointerInfo &PointerI = Pointers[I];
  const PointerInfo &PointerJ = Pointers[J];

  // No need to check if two readonly pointers intersect.
  if (!PointerI.IsWritePtr && !PointerJ.IsWritePtr)
    return false;

  // Only need to check pointers between two different dependency sets.
  if (PointerI.DependencySetId == PointerJ.DependencySetId)
    return false;

  // Only need to check pointers in the same alias set.
  return PointerI.AliasSetId == PointerJ.AliasSetId;
}

/// Assign each RuntimeCheckingPtrGroup pointer an index for stable UTC output.
static DenseMap<const RuntimeCheckingPtrGroup *, unsigned>
getPtrToIdxMap(ArrayRef<RuntimeCheckingPtrGroup> CheckingGroups) {
  DenseMap<const RuntimeCheckingPtrGroup *, unsigned> PtrIndices;
  for (const auto &[Idx, CG] : enumerate(CheckingGroups))
    PtrIndices[&CG] = Idx;
  return PtrIndices;
}

void RuntimePointerChecking::printChecks(
    raw_ostream &OS, const SmallVectorImpl<RuntimePointerCheck> &Checks,
    unsigned Depth) const {
  unsigned N = 0;
  auto PtrIndices = getPtrToIdxMap(CheckingGroups);
  for (const auto &[Check1, Check2] : Checks) {
    const auto &First = Check1->Members, &Second = Check2->Members;
    OS.indent(Depth) << "Check " << N++ << ":\n";
    OS.indent(Depth + 2) << "Comparing group GRP" << PtrIndices.at(Check1)
                         << ":\n";
    for (unsigned K : First)
      OS.indent(Depth + 2) << *Pointers[K].PointerValue << "\n";
    OS.indent(Depth + 2) << "Against group GRP" << PtrIndices.at(Check2)
                         << ":\n";
    for (unsigned K : Second)
      OS.indent(Depth + 2) << *Pointers[K].PointerValue << "\n";
  }
}

void RuntimePointerChecking::print(raw_ostream &OS, unsigned Depth) const {

  OS.indent(Depth) << "Run-time memory checks:\n";
  printChecks(OS, Checks, Depth);

  OS.indent(Depth) << "Grouped accesses:\n";
  auto PtrIndices = getPtrToIdxMap(CheckingGroups);
  for (const auto &CG : CheckingGroups) {
    OS.indent(Depth + 2) << "Group GRP" << PtrIndices.at(&CG) << ":\n";
    OS.indent(Depth + 4) << "(Low: " << *CG.Low << " High: " << *CG.High
                         << ")\n";
    for (unsigned Member : CG.Members) {
      OS.indent(Depth + 6) << "Member: " << *Pointers[Member].Expr << "\n";
    }
  }
}

namespace {

/// Analyses memory accesses in a loop.
///
/// Checks whether run time pointer checks are needed and builds sets for data
/// dependence checking.
class AccessAnalysis {
public:
  /// Read or write access location.
  typedef PointerIntPair<Value *, 1, bool> MemAccessInfo;
  typedef SmallVector<MemAccessInfo, 8> MemAccessInfoList;

  AccessAnalysis(const Loop *TheLoop, AAResults *AA, const LoopInfo *LI,
                 MemoryDepChecker::DepCandidates &DA,
                 PredicatedScalarEvolution &PSE,
                 SmallPtrSetImpl<MDNode *> &LoopAliasScopes)
      : TheLoop(TheLoop), BAA(*AA), AST(BAA), LI(LI), DepCands(DA), PSE(PSE),
        LoopAliasScopes(LoopAliasScopes) {
    // We're analyzing dependences across loop iterations.
    BAA.enableCrossIterationMode();
  }

  /// Register a load  and whether it is only read from.
  void addLoad(const MemoryLocation &Loc, Type *AccessTy, bool IsReadOnly) {
    Value *Ptr = const_cast<Value *>(Loc.Ptr);
    AST.add(adjustLoc(Loc));
    Accesses[MemAccessInfo(Ptr, false)].insert(AccessTy);
    if (IsReadOnly)
      ReadOnlyPtr.insert(Ptr);
  }

  /// Register a store.
  void addStore(const MemoryLocation &Loc, Type *AccessTy) {
    Value *Ptr = const_cast<Value *>(Loc.Ptr);
    AST.add(adjustLoc(Loc));
    Accesses[MemAccessInfo(Ptr, true)].insert(AccessTy);
  }

  /// Check if we can emit a run-time no-alias check for \p Access.
  ///
  /// Returns true if we can emit a run-time no alias check for \p Access.
  /// If we can check this access, this also adds it to a dependence set and
  /// adds a run-time to check for it to \p RtCheck. If \p Assume is true,
  /// we will attempt to use additional run-time checks in order to get
  /// the bounds of the pointer.
  bool createCheckForAccess(RuntimePointerChecking &RtCheck,
                            MemAccessInfo Access, Type *AccessTy,
                            const DenseMap<Value *, const SCEV *> &Strides,
                            DenseMap<Value *, unsigned> &DepSetId,
                            Loop *TheLoop, unsigned &RunningDepId,
                            unsigned ASId, bool Assume);

  /// Check whether we can check the pointers at runtime for
  /// non-intersection.
  ///
  /// Returns true if we need no check or if we do and we can generate them
  /// (i.e. the pointers have computable bounds). A return value of false means
  /// we couldn't analyze and generate runtime checks for all pointers in the
  /// loop, but if \p AllowPartial is set then we will have checks for those
  /// pointers we could analyze.
  bool canCheckPtrAtRT(RuntimePointerChecking &RtCheck, Loop *TheLoop,
                       const DenseMap<Value *, const SCEV *> &Strides,
                       Value *&UncomputablePtr, bool AllowPartial);

  /// Goes over all memory accesses, checks whether a RT check is needed
  /// and builds sets of dependent accesses.
  void buildDependenceSets() {
    processMemAccesses();
  }

  /// Initial processing of memory accesses determined that we need to
  /// perform dependency checking.
  ///
  /// Note that this can later be cleared if we retry memcheck analysis without
  /// dependency checking (i.e. ShouldRetryWithRuntimeChecks).
  bool isDependencyCheckNeeded() const { return !CheckDeps.empty(); }

  /// We decided that no dependence analysis would be used.  Reset the state.
  void resetDepChecks(MemoryDepChecker &DepChecker) {
    CheckDeps.clear();
    DepChecker.clearDependences();
  }

  const MemAccessInfoList &getDependenciesToCheck() const { return CheckDeps; }

private:
  typedef MapVector<MemAccessInfo, SmallSetVector<Type *, 1>> PtrAccessMap;

  /// Adjust the MemoryLocation so that it represents accesses to this
  /// location across all iterations, rather than a single one.
  MemoryLocation adjustLoc(MemoryLocation Loc) const {
    // The accessed location varies within the loop, but remains within the
    // underlying object.
    Loc.Size = LocationSize::beforeOrAfterPointer();
    Loc.AATags.Scope = adjustAliasScopeList(Loc.AATags.Scope);
    Loc.AATags.NoAlias = adjustAliasScopeList(Loc.AATags.NoAlias);
    return Loc;
  }

  /// Drop alias scopes that are only valid within a single loop iteration.
  MDNode *adjustAliasScopeList(MDNode *ScopeList) const {
    if (!ScopeList)
      return nullptr;

    // For the sake of simplicity, drop the whole scope list if any scope is
    // iteration-local.
    if (any_of(ScopeList->operands(), [&](Metadata *Scope) {
          return LoopAliasScopes.contains(cast<MDNode>(Scope));
        }))
      return nullptr;

    return ScopeList;
  }

  /// Go over all memory access and check whether runtime pointer checks
  /// are needed and build sets of dependency check candidates.
  void processMemAccesses();

  /// Map of all accesses. Values are the types used to access memory pointed to
  /// by the pointer.
  PtrAccessMap Accesses;

  /// The loop being checked.
  const Loop *TheLoop;

  /// List of accesses that need a further dependence check.
  MemAccessInfoList CheckDeps;

  /// Set of pointers that are read only.
  SmallPtrSet<Value*, 16> ReadOnlyPtr;

  /// Batched alias analysis results.
  BatchAAResults BAA;

  /// An alias set tracker to partition the access set by underlying object and
  //intrinsic property (such as TBAA metadata).
  AliasSetTracker AST;

  /// The LoopInfo of the loop being checked.
  const LoopInfo *LI;

  /// Sets of potentially dependent accesses - members of one set share an
  /// underlying pointer. The set "CheckDeps" identfies which sets really need a
  /// dependence check.
  MemoryDepChecker::DepCandidates &DepCands;

  /// Initial processing of memory accesses determined that we may need
  /// to add memchecks.  Perform the analysis to determine the necessary checks.
  ///
  /// Note that, this is different from isDependencyCheckNeeded.  When we retry
  /// memcheck analysis without dependency checking
  /// (i.e. ShouldRetryWithRuntimeChecks), isDependencyCheckNeeded is
  /// cleared while this remains set if we have potentially dependent accesses.
  bool IsRTCheckAnalysisNeeded = false;

  /// The SCEV predicate containing all the SCEV-related assumptions.
  PredicatedScalarEvolution &PSE;

  DenseMap<Value *, SmallVector<const Value *, 16>> UnderlyingObjects;

  /// Alias scopes that are declared inside the loop, and as such not valid
  /// across iterations.
  SmallPtrSetImpl<MDNode *> &LoopAliasScopes;
};

} // end anonymous namespace

/// Try to compute a constant stride for \p AR. Used by getPtrStride and
/// isNoWrap.
static std::optional<int64_t>
getStrideFromAddRec(const SCEVAddRecExpr *AR, const Loop *Lp, Type *AccessTy,
                    Value *Ptr, PredicatedScalarEvolution &PSE) {
  // The access function must stride over the innermost loop.
  if (Lp != AR->getLoop()) {
    LLVM_DEBUG({
      dbgs() << "LAA: Bad stride - Not striding over innermost loop ";
      if (Ptr)
        dbgs() << *Ptr << " ";

      dbgs() << "SCEV: " << *AR << "\n";
    });
    return std::nullopt;
  }

  // Check the step is constant.
  const SCEV *Step = AR->getStepRecurrence(*PSE.getSE());

  // Calculate the pointer stride and check if it is constant.
  const APInt *APStepVal;
  if (!match(Step, m_scev_APInt(APStepVal))) {
    LLVM_DEBUG({
      dbgs() << "LAA: Bad stride - Not a constant strided ";
      if (Ptr)
        dbgs() << *Ptr << " ";
      dbgs() << "SCEV: " << *AR << "\n";
    });
    return std::nullopt;
  }

  const auto &DL = Lp->getHeader()->getDataLayout();
  TypeSize AllocSize = DL.getTypeAllocSize(AccessTy);
  int64_t Size = AllocSize.getFixedValue();

  // Huge step value - give up.
  std::optional<int64_t> StepVal = APStepVal->trySExtValue();
  if (!StepVal)
    return std::nullopt;

  // Strided access.
  return *StepVal % Size ? std::nullopt : std::make_optional(*StepVal / Size);
}

/// Check whether \p AR is a non-wrapping AddRec. If \p Ptr is not nullptr, use
/// informating from the IR pointer value to determine no-wrap.
static bool isNoWrap(PredicatedScalarEvolution &PSE, const SCEVAddRecExpr *AR,
                     Value *Ptr, Type *AccessTy, const Loop *L, bool Assume,
                     std::optional<int64_t> Stride = std::nullopt) {
  // FIXME: This should probably only return true for NUW.
  if (AR->getNoWrapFlags(SCEV::NoWrapMask))
    return true;

  if (Ptr && PSE.hasNoOverflow(Ptr, SCEVWrapPredicate::IncrementNUSW))
    return true;

  // An nusw getelementptr that is an AddRec cannot wrap. If it would wrap,
  // the distance between the previously accessed location and the wrapped
  // location will be larger than half the pointer index type space. In that
  // case, the GEP would be  poison and any memory access dependent on it would
  // be immediate UB when executed.
  if (auto *GEP = dyn_cast_if_present<GetElementPtrInst>(Ptr);
      GEP && GEP->hasNoUnsignedSignedWrap())
    return true;

  if (!Stride)
    Stride = getStrideFromAddRec(AR, L, AccessTy, Ptr, PSE);
  if (Stride) {
    // If the null pointer is undefined, then a access sequence which would
    // otherwise access it can be assumed not to unsigned wrap.  Note that this
    // assumes the object in memory is aligned to the natural alignment.
    unsigned AddrSpace = AR->getType()->getPointerAddressSpace();
    if (!NullPointerIsDefined(L->getHeader()->getParent(), AddrSpace) &&
        (Stride == 1 || Stride == -1))
      return true;
  }

  if (Ptr && Assume) {
    PSE.setNoOverflow(Ptr, SCEVWrapPredicate::IncrementNUSW);
    LLVM_DEBUG(dbgs() << "LAA: Pointer may wrap:\n"
                      << "LAA:   Pointer: " << *Ptr << "\n"
                      << "LAA:   SCEV: " << *AR << "\n"
                      << "LAA:   Added an overflow assumption\n");
    return true;
  }

  return false;
}

static void visitPointers(Value *StartPtr, const Loop &InnermostLoop,
                          function_ref<void(Value *)> AddPointer) {
  SmallPtrSet<Value *, 8> Visited;
  SmallVector<Value *> WorkList;
  WorkList.push_back(StartPtr);

  while (!WorkList.empty()) {
    Value *Ptr = WorkList.pop_back_val();
    if (!Visited.insert(Ptr).second)
      continue;
    auto *PN = dyn_cast<PHINode>(Ptr);
    // SCEV does not look through non-header PHIs inside the loop. Such phis
    // can be analyzed by adding separate accesses for each incoming pointer
    // value.
    if (PN && InnermostLoop.contains(PN->getParent()) &&
        PN->getParent() != InnermostLoop.getHeader()) {
      llvm::append_range(WorkList, PN->incoming_values());
    } else
      AddPointer(Ptr);
  }
}

// Walk back through the IR for a pointer, looking for a select like the
// following:
//
//  %offset = select i1 %cmp, i64 %a, i64 %b
//  %addr = getelementptr double, double* %base, i64 %offset
//  %ld = load double, double* %addr, align 8
//
// We won't be able to form a single SCEVAddRecExpr from this since the
// address for each loop iteration depends on %cmp. We could potentially
// produce multiple valid SCEVAddRecExprs, though, and check all of them for
// memory safety/aliasing if needed.
//
// If we encounter some IR we don't yet handle, or something obviously fine
// like a constant, then we just add the SCEV for that term to the list passed
// in by the caller. If we have a node that may potentially yield a valid
// SCEVAddRecExpr then we decompose it into parts and build the SCEV terms
// ourselves before adding to the list.
static void findForkedSCEVs(
    ScalarEvolution *SE, const Loop *L, Value *Ptr,
    SmallVectorImpl<PointerIntPair<const SCEV *, 1, bool>> &ScevList,
    unsigned Depth) {
  // If our Value is a SCEVAddRecExpr, loop invariant, not an instruction, or
  // we've exceeded our limit on recursion, just return whatever we have
  // regardless of whether it can be used for a forked pointer or not, along
  // with an indication of whether it might be a poison or undef value.
  const SCEV *Scev = SE->getSCEV(Ptr);
  if (isa<SCEVAddRecExpr>(Scev) || L->isLoopInvariant(Ptr) ||
      !isa<Instruction>(Ptr) || Depth == 0) {
    ScevList.emplace_back(Scev, !isGuaranteedNotToBeUndefOrPoison(Ptr));
    return;
  }

  Depth--;

  auto UndefPoisonCheck = [](PointerIntPair<const SCEV *, 1, bool> S) {
    return get<1>(S);
  };

  auto GetBinOpExpr = [&SE](unsigned Opcode, const SCEV *L, const SCEV *R) {
    switch (Opcode) {
    case Instruction::Add:
      return SE->getAddExpr(L, R);
    case Instruction::Sub:
      return SE->getMinusSCEV(L, R);
    default:
      llvm_unreachable("Unexpected binary operator when walking ForkedPtrs");
    }
  };

  Instruction *I = cast<Instruction>(Ptr);
  unsigned Opcode = I->getOpcode();
  switch (Opcode) {
  case Instruction::GetElementPtr: {
    auto *GEP = cast<GetElementPtrInst>(I);
    Type *SourceTy = GEP->getSourceElementType();
    // We only handle base + single offset GEPs here for now.
    // Not dealing with preexisting gathers yet, so no vectors.
    if (I->getNumOperands() != 2 || SourceTy->isVectorTy()) {
      ScevList.emplace_back(Scev, !isGuaranteedNotToBeUndefOrPoison(GEP));
      break;
    }
    SmallVector<PointerIntPair<const SCEV *, 1, bool>, 2> BaseScevs;
    SmallVector<PointerIntPair<const SCEV *, 1, bool>, 2> OffsetScevs;
    findForkedSCEVs(SE, L, I->getOperand(0), BaseScevs, Depth);
    findForkedSCEVs(SE, L, I->getOperand(1), OffsetScevs, Depth);

    // See if we need to freeze our fork...
    bool NeedsFreeze = any_of(BaseScevs, UndefPoisonCheck) ||
                       any_of(OffsetScevs, UndefPoisonCheck);

    // Check that we only have a single fork, on either the base or the offset.
    // Copy the SCEV across for the one without a fork in order to generate
    // the full SCEV for both sides of the GEP.
    if (OffsetScevs.size() == 2 && BaseScevs.size() == 1)
      BaseScevs.push_back(BaseScevs[0]);
    else if (BaseScevs.size() == 2 && OffsetScevs.size() == 1)
      OffsetScevs.push_back(OffsetScevs[0]);
    else {
      ScevList.emplace_back(Scev, NeedsFreeze);
      break;
    }

    Type *IntPtrTy = SE->getEffectiveSCEVType(GEP->getPointerOperandType());

    // Find the size of the type being pointed to. We only have a single
    // index term (guarded above) so we don't need to index into arrays or
    // structures, just get the size of the scalar value.
    const SCEV *Size = SE->getSizeOfExpr(IntPtrTy, SourceTy);

    for (auto [B, O] : zip(BaseScevs, OffsetScevs)) {
      const SCEV *Base = get<0>(B);
      const SCEV *Offset = get<0>(O);

      // Scale up the offsets by the size of the type, then add to the bases.
      const SCEV *Scaled =
          SE->getMulExpr(Size, SE->getTruncateOrSignExtend(Offset, IntPtrTy));
      ScevList.emplace_back(SE->getAddExpr(Base, Scaled), NeedsFreeze);
    }
    break;
  }
  case Instruction::Select: {
    SmallVector<PointerIntPair<const SCEV *, 1, bool>, 2> ChildScevs;
    // A select means we've found a forked pointer, but we currently only
    // support a single select per pointer so if there's another behind this
    // then we just bail out and return the generic SCEV.
    findForkedSCEVs(SE, L, I->getOperand(1), ChildScevs, Depth);
    findForkedSCEVs(SE, L, I->getOperand(2), ChildScevs, Depth);
    if (ChildScevs.size() == 2)
      append_range(ScevList, ChildScevs);
    else
      ScevList.emplace_back(Scev, !isGuaranteedNotToBeUndefOrPoison(Ptr));
    break;
  }
  case Instruction::PHI: {
    SmallVector<PointerIntPair<const SCEV *, 1, bool>, 2> ChildScevs;
    // A phi means we've found a forked pointer, but we currently only
    // support a single phi per pointer so if there's another behind this
    // then we just bail out and return the generic SCEV.
    if (I->getNumOperands() == 2) {
      findForkedSCEVs(SE, L, I->getOperand(0), ChildScevs, Depth);
      findForkedSCEVs(SE, L, I->getOperand(1), ChildScevs, Depth);
    }
    if (ChildScevs.size() == 2)
      append_range(ScevList, ChildScevs);
    else
      ScevList.emplace_back(Scev, !isGuaranteedNotToBeUndefOrPoison(Ptr));
    break;
  }
  case Instruction::Add:
  case Instruction::Sub: {
    SmallVector<PointerIntPair<const SCEV *, 1, bool>> LScevs;
    SmallVector<PointerIntPair<const SCEV *, 1, bool>> RScevs;
    findForkedSCEVs(SE, L, I->getOperand(0), LScevs, Depth);
    findForkedSCEVs(SE, L, I->getOperand(1), RScevs, Depth);

    // See if we need to freeze our fork...
    bool NeedsFreeze =
        any_of(LScevs, UndefPoisonCheck) || any_of(RScevs, UndefPoisonCheck);

    // Check that we only have a single fork, on either the left or right side.
    // Copy the SCEV across for the one without a fork in order to generate
    // the full SCEV for both sides of the BinOp.
    if (LScevs.size() == 2 && RScevs.size() == 1)
      RScevs.push_back(RScevs[0]);
    else if (RScevs.size() == 2 && LScevs.size() == 1)
      LScevs.push_back(LScevs[0]);
    else {
      ScevList.emplace_back(Scev, NeedsFreeze);
      break;
    }

    for (auto [L, R] : zip(LScevs, RScevs))
      ScevList.emplace_back(GetBinOpExpr(Opcode, get<0>(L), get<0>(R)),
                            NeedsFreeze);
    break;
  }
  default:
    // Just return the current SCEV if we haven't handled the instruction yet.
    LLVM_DEBUG(dbgs() << "ForkedPtr unhandled instruction: " << *I << "\n");
    ScevList.emplace_back(Scev, !isGuaranteedNotToBeUndefOrPoison(Ptr));
    break;
  }
}

static SmallVector<PointerIntPair<const SCEV *, 1, bool>>
findForkedPointer(PredicatedScalarEvolution &PSE,
                  const DenseMap<Value *, const SCEV *> &StridesMap, Value *Ptr,
                  const Loop *L) {
  ScalarEvolution *SE = PSE.getSE();
  assert(SE->isSCEVable(Ptr->getType()) && "Value is not SCEVable!");
  SmallVector<PointerIntPair<const SCEV *, 1, bool>> Scevs;
  findForkedSCEVs(SE, L, Ptr, Scevs, MaxForkedSCEVDepth);

  // For now, we will only accept a forked pointer with two possible SCEVs
  // that are either SCEVAddRecExprs or loop invariant.
  if (Scevs.size() == 2 &&
      (isa<SCEVAddRecExpr>(get<0>(Scevs[0])) ||
       SE->isLoopInvariant(get<0>(Scevs[0]), L)) &&
      (isa<SCEVAddRecExpr>(get<0>(Scevs[1])) ||
       SE->isLoopInvariant(get<0>(Scevs[1]), L))) {
    LLVM_DEBUG(dbgs() << "LAA: Found forked pointer: " << *Ptr << "\n");
    LLVM_DEBUG(dbgs() << "\t(1) " << *get<0>(Scevs[0]) << "\n");
    LLVM_DEBUG(dbgs() << "\t(2) " << *get<0>(Scevs[1]) << "\n");
    return Scevs;
  }

  return {{replaceSymbolicStrideSCEV(PSE, StridesMap, Ptr), false}};
}

bool AccessAnalysis::createCheckForAccess(
    RuntimePointerChecking &RtCheck, MemAccessInfo Access, Type *AccessTy,
    const DenseMap<Value *, const SCEV *> &StridesMap,
    DenseMap<Value *, unsigned> &DepSetId, Loop *TheLoop,
    unsigned &RunningDepId, unsigned ASId, bool Assume) {
  Value *Ptr = Access.getPointer();

  SmallVector<PointerIntPair<const SCEV *, 1, bool>> TranslatedPtrs =
      findForkedPointer(PSE, StridesMap, Ptr, TheLoop);
  assert(!TranslatedPtrs.empty() && "must have some translated pointers");

  /// Check whether all pointers can participate in a runtime bounds check. They
  /// must either be invariant or AddRecs. If ShouldCheckWrap is true, they also
  /// must not wrap.
  for (auto &P : TranslatedPtrs) {
    // The bounds for loop-invariant pointer is trivial.
    if (PSE.getSE()->isLoopInvariant(P.getPointer(), TheLoop))
      continue;

    const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(P.getPointer());
    if (!AR && Assume)
      AR = PSE.getAsAddRec(Ptr);
    if (!AR || !AR->isAffine())
      return false;

    // If there's only one option for Ptr, look it up after bounds and wrap
    // checking, because assumptions might have been added to PSE.
    if (TranslatedPtrs.size() == 1) {
      AR =
          cast<SCEVAddRecExpr>(replaceSymbolicStrideSCEV(PSE, StridesMap, Ptr));
      P.setPointer(AR);
    }

    // When we run after a failing dependency check we have to make sure
    // we don't have wrapping pointers.
    if (!isNoWrap(PSE, AR, TranslatedPtrs.size() == 1 ? Ptr : nullptr, AccessTy,
                  TheLoop, Assume)) {
      return false;
    }
  }

  for (auto [PtrExpr, NeedsFreeze] : TranslatedPtrs) {
    // The id of the dependence set.
    unsigned DepId;

    if (isDependencyCheckNeeded()) {
      Value *Leader = DepCands.getLeaderValue(Access).getPointer();
      unsigned &LeaderId = DepSetId[Leader];
      if (!LeaderId)
        LeaderId = RunningDepId++;
      DepId = LeaderId;
    } else
      // Each access has its own dependence set.
      DepId = RunningDepId++;

    bool IsWrite = Access.getInt();
    RtCheck.insert(TheLoop, Ptr, PtrExpr, AccessTy, IsWrite, DepId, ASId, PSE,
                   NeedsFreeze);
    LLVM_DEBUG(dbgs() << "LAA: Found a runtime check ptr:" << *Ptr << '\n');
  }

  return true;
}

bool AccessAnalysis::canCheckPtrAtRT(
    RuntimePointerChecking &RtCheck, Loop *TheLoop,
    const DenseMap<Value *, const SCEV *> &StridesMap, Value *&UncomputablePtr,
    bool AllowPartial) {
  // Find pointers with computable bounds. We are going to use this information
  // to place a runtime bound check.
  bool CanDoRT = true;

  bool MayNeedRTCheck = false;
  if (!IsRTCheckAnalysisNeeded) return true;

  bool IsDepCheckNeeded = isDependencyCheckNeeded();

  // We assign a consecutive id to access from different alias sets.
  // Accesses between different groups doesn't need to be checked.
  unsigned ASId = 0;
  for (const auto &AS : AST) {
    int NumReadPtrChecks = 0;
    int NumWritePtrChecks = 0;
    bool CanDoAliasSetRT = true;
    ++ASId;
    auto ASPointers = AS.getPointers();

    // We assign consecutive id to access from different dependence sets.
    // Accesses within the same set don't need a runtime check.
    unsigned RunningDepId = 1;
    DenseMap<Value *, unsigned> DepSetId;

    SmallVector<std::pair<MemAccessInfo, Type *>, 4> Retries;

    // First, count how many write and read accesses are in the alias set. Also
    // collect MemAccessInfos for later.
    SmallVector<MemAccessInfo, 4> AccessInfos;
    for (const Value *ConstPtr : ASPointers) {
      Value *Ptr = const_cast<Value *>(ConstPtr);
      bool IsWrite = Accesses.contains(MemAccessInfo(Ptr, true));
      if (IsWrite)
        ++NumWritePtrChecks;
      else
        ++NumReadPtrChecks;
      AccessInfos.emplace_back(Ptr, IsWrite);
    }

    // We do not need runtime checks for this alias set, if there are no writes
    // or a single write and no reads.
    if (NumWritePtrChecks == 0 ||
        (NumWritePtrChecks == 1 && NumReadPtrChecks == 0)) {
      assert((ASPointers.size() <= 1 ||
              all_of(ASPointers,
                     [this](const Value *Ptr) {
                       MemAccessInfo AccessWrite(const_cast<Value *>(Ptr),
                                                 true);
                       return !DepCands.contains(AccessWrite);
                     })) &&
             "Can only skip updating CanDoRT below, if all entries in AS "
             "are reads or there is at most 1 entry");
      continue;
    }

    for (auto &Access : AccessInfos) {
      for (const auto &AccessTy : Accesses[Access]) {
        if (!createCheckForAccess(RtCheck, Access, AccessTy, StridesMap,
                                  DepSetId, TheLoop, RunningDepId, ASId,
                                  false)) {
          LLVM_DEBUG(dbgs() << "LAA: Can't find bounds for ptr:"
                            << *Access.getPointer() << '\n');
          Retries.emplace_back(Access, AccessTy);
          CanDoAliasSetRT = false;
        }
      }
    }

    // Note that this function computes CanDoRT and MayNeedRTCheck
    // independently. For example CanDoRT=false, MayNeedRTCheck=false means that
    // we have a pointer for which we couldn't find the bounds but we don't
    // actually need to emit any checks so it does not matter.
    //
    // We need runtime checks for this alias set, if there are at least 2
    // dependence sets (in which case RunningDepId > 2) or if we need to re-try
    // any bound checks (because in that case the number of dependence sets is
    // incomplete).
    bool NeedsAliasSetRTCheck = RunningDepId > 2 || !Retries.empty();

    // We need to perform run-time alias checks, but some pointers had bounds
    // that couldn't be checked.
    if (NeedsAliasSetRTCheck && !CanDoAliasSetRT) {
      // Reset the CanDoSetRt flag and retry all accesses that have failed.
      // We know that we need these checks, so we can now be more aggressive
      // and add further checks if required (overflow checks).
      CanDoAliasSetRT = true;
      for (const auto &[Access, AccessTy] : Retries) {
        if (!createCheckForAccess(RtCheck, Access, AccessTy, StridesMap,
                                  DepSetId, TheLoop, RunningDepId, ASId,
                                  /*Assume=*/true)) {
          CanDoAliasSetRT = false;
          UncomputablePtr = Access.getPointer();
          if (!AllowPartial)
            break;
        }
      }
    }

    CanDoRT &= CanDoAliasSetRT;
    MayNeedRTCheck |= NeedsAliasSetRTCheck;
    ++ASId;
  }

  // If the pointers that we would use for the bounds comparison have different
  // address spaces, assume the values aren't directly comparable, so we can't
  // use them for the runtime check. We also have to assume they could
  // overlap. In the future there should be metadata for whether address spaces
  // are disjoint.
  unsigned NumPointers = RtCheck.Pointers.size();
  for (unsigned i = 0; i < NumPointers; ++i) {
    for (unsigned j = i + 1; j < NumPointers; ++j) {
      // Only need to check pointers between two different dependency sets.
      if (RtCheck.Pointers[i].DependencySetId ==
          RtCheck.Pointers[j].DependencySetId)
       continue;
      // Only need to check pointers in the same alias set.
      if (RtCheck.Pointers[i].AliasSetId != RtCheck.Pointers[j].AliasSetId)
        continue;

      Value *PtrI = RtCheck.Pointers[i].PointerValue;
      Value *PtrJ = RtCheck.Pointers[j].PointerValue;

      unsigned ASi = PtrI->getType()->getPointerAddressSpace();
      unsigned ASj = PtrJ->getType()->getPointerAddressSpace();
      if (ASi != ASj) {
        LLVM_DEBUG(
            dbgs() << "LAA: Runtime check would require comparison between"
                      " different address spaces\n");
        return false;
      }
    }
  }

  if (MayNeedRTCheck && (CanDoRT || AllowPartial))
    RtCheck.generateChecks(DepCands, IsDepCheckNeeded);

  LLVM_DEBUG(dbgs() << "LAA: We need to do " << RtCheck.getNumberOfChecks()
                    << " pointer comparisons.\n");

  // If we can do run-time checks, but there are no checks, no runtime checks
  // are needed. This can happen when all pointers point to the same underlying
  // object for example.
  RtCheck.Need = CanDoRT ? RtCheck.getNumberOfChecks() != 0 : MayNeedRTCheck;

  bool CanDoRTIfNeeded = !RtCheck.Need || CanDoRT;
  assert(CanDoRTIfNeeded == (CanDoRT || !MayNeedRTCheck) &&
         "CanDoRTIfNeeded depends on RtCheck.Need");
  if (!CanDoRTIfNeeded && !AllowPartial)
    RtCheck.reset();
  return CanDoRTIfNeeded;
}

void AccessAnalysis::processMemAccesses() {
  // We process the set twice: first we process read-write pointers, last we
  // process read-only pointers. This allows us to skip dependence tests for
  // read-only pointers.

  LLVM_DEBUG(dbgs() << "LAA: Processing memory accesses...\n");
  LLVM_DEBUG(dbgs() << "  AST: "; AST.dump());
  LLVM_DEBUG(dbgs() << "LAA:   Accesses(" << Accesses.size() << "):\n");
  LLVM_DEBUG({
    for (const auto &[A, _] : Accesses)
      dbgs() << "\t" << *A.getPointer() << " ("
             << (A.getInt()
                     ? "write"
                     : (ReadOnlyPtr.contains(A.getPointer()) ? "read-only"
                                                             : "read"))
             << ")\n";
  });

  // The AliasSetTracker has nicely partitioned our pointers by metadata
  // compatibility and potential for underlying-object overlap. As a result, we
  // only need to check for potential pointer dependencies within each alias
  // set.
  for (const auto &AS : AST) {
    // Note that both the alias-set tracker and the alias sets themselves used
    // ordered collections internally and so the iteration order here is
    // deterministic.
    auto ASPointers = AS.getPointers();

    bool SetHasWrite = false;

    // Map of (pointer to underlying objects, accessed address space) to last
    // access encountered.
    typedef DenseMap<std::pair<const Value *, unsigned>, MemAccessInfo>
        UnderlyingObjToAccessMap;
    UnderlyingObjToAccessMap ObjToLastAccess;

    // Set of access to check after all writes have been processed.
    PtrAccessMap DeferredAccesses;

    // Iterate over each alias set twice, once to process read/write pointers,
    // and then to process read-only pointers.
    for (int SetIteration = 0; SetIteration < 2; ++SetIteration) {
      bool UseDeferred = SetIteration > 0;
      PtrAccessMap &S = UseDeferred ? DeferredAccesses : Accesses;

      for (const Value *ConstPtr : ASPointers) {
        Value *Ptr = const_cast<Value *>(ConstPtr);

        // For a single memory access in AliasSetTracker, Accesses may contain
        // both read and write, and they both need to be handled for CheckDeps.
        for (const auto &[AC, _] : S) {
          if (AC.getPointer() != Ptr)
            continue;

          bool IsWrite = AC.getInt();

          // If we're using the deferred access set, then it contains only
          // reads.
          bool IsReadOnlyPtr = ReadOnlyPtr.contains(Ptr) && !IsWrite;
          if (UseDeferred && !IsReadOnlyPtr)
            continue;
          // Otherwise, the pointer must be in the PtrAccessSet, either as a
          // read or a write.
          assert(((IsReadOnlyPtr && UseDeferred) || IsWrite ||
                  S.contains(MemAccessInfo(Ptr, false))) &&
                 "Alias-set pointer not in the access set?");

          MemAccessInfo Access(Ptr, IsWrite);
          DepCands.insert(Access);

          // Memorize read-only pointers for later processing and skip them in
          // the first round (they need to be checked after we have seen all
          // write pointers). Note: we also mark pointer that are not
          // consecutive as "read-only" pointers (so that we check
          // "a[b[i]] +="). Hence, we need the second check for "!IsWrite".
          if (!UseDeferred && IsReadOnlyPtr) {
            // We only use the pointer keys, the types vector values don't
            // matter.
            DeferredAccesses.insert({Access, {}});
            continue;
          }

          // If this is a write - check other reads and writes for conflicts. If
          // this is a read only check other writes for conflicts (but only if
          // there is no other write to the ptr - this is an optimization to
          // catch "a[i] = a[i] + " without having to do a dependence check).
          if ((IsWrite || IsReadOnlyPtr) && SetHasWrite) {
            CheckDeps.push_back(Access);
            IsRTCheckAnalysisNeeded = true;
          }

          if (IsWrite)
            SetHasWrite = true;

          // Create sets of pointers connected by a shared alias set and
          // underlying object.
          SmallVector<const Value *, 16> &UOs = UnderlyingObjects[Ptr];
          UOs = {};
          ::getUnderlyingObjects(Ptr, UOs, LI);
          LLVM_DEBUG(dbgs()
                     << "Underlying objects for pointer " << *Ptr << "\n");
          for (const Value *UnderlyingObj : UOs) {
            // nullptr never alias, don't join sets for pointer that have "null"
            // in their UnderlyingObjects list.
            if (isa<ConstantPointerNull>(UnderlyingObj) &&
                !NullPointerIsDefined(
                    TheLoop->getHeader()->getParent(),
                    UnderlyingObj->getType()->getPointerAddressSpace()))
              continue;

            auto [It, Inserted] = ObjToLastAccess.try_emplace(
                {UnderlyingObj,
                 cast<PointerType>(Ptr->getType())->getAddressSpace()},
                Access);
            if (!Inserted) {
              DepCands.unionSets(Access, It->second);
              It->second = Access;
            }

            LLVM_DEBUG(dbgs() << "  " << *UnderlyingObj << "\n");
          }
        }
      }
    }
  }
}

/// Check whether the access through \p Ptr has a constant stride.
std::optional<int64_t>
llvm::getPtrStride(PredicatedScalarEvolution &PSE, Type *AccessTy, Value *Ptr,
                   const Loop *Lp,
                   const DenseMap<Value *, const SCEV *> &StridesMap,
                   bool Assume, bool ShouldCheckWrap) {
  const SCEV *PtrScev = replaceSymbolicStrideSCEV(PSE, StridesMap, Ptr);
  if (PSE.getSE()->isLoopInvariant(PtrScev, Lp))
    return 0;

  assert(Ptr->getType()->isPointerTy() && "Unexpected non-ptr");
  if (isa<ScalableVectorType>(AccessTy)) {
    LLVM_DEBUG(dbgs() << "LAA: Bad stride - Scalable object: " << *AccessTy
                      << "\n");
    return std::nullopt;
  }

  const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(PtrScev);
  if (Assume && !AR)
    AR = PSE.getAsAddRec(Ptr);

  if (!AR) {
    LLVM_DEBUG(dbgs() << "LAA: Bad stride - Not an AddRecExpr pointer " << *Ptr
                      << " SCEV: " << *PtrScev << "\n");
    return std::nullopt;
  }

  std::optional<int64_t> Stride =
      getStrideFromAddRec(AR, Lp, AccessTy, Ptr, PSE);
  if (!ShouldCheckWrap || !Stride)
    return Stride;

  if (isNoWrap(PSE, AR, Ptr, AccessTy, Lp, Assume, Stride))
    return Stride;

  LLVM_DEBUG(
      dbgs() << "LAA: Bad stride - Pointer may wrap in the address space "
             << *Ptr << " SCEV: " << *AR << "\n");
  return std::nullopt;
}

std::optional<int64_t> llvm::getPointersDiff(Type *ElemTyA, Value *PtrA,
                                             Type *ElemTyB, Value *PtrB,
                                             const DataLayout &DL,
                                             ScalarEvolution &SE,
                                             bool StrictCheck, bool CheckType) {
  assert(PtrA && PtrB && "Expected non-nullptr pointers.");

  // Make sure that A and B are different pointers.
  if (PtrA == PtrB)
    return 0;

  // Make sure that the element types are the same if required.
  if (CheckType && ElemTyA != ElemTyB)
    return std::nullopt;

  unsigned ASA = PtrA->getType()->getPointerAddressSpace();
  unsigned ASB = PtrB->getType()->getPointerAddressSpace();

  // Check that the address spaces match.
  if (ASA != ASB)
    return std::nullopt;
  unsigned IdxWidth = DL.getIndexSizeInBits(ASA);

  APInt OffsetA(IdxWidth, 0), OffsetB(IdxWidth, 0);
  const Value *PtrA1 = PtrA->stripAndAccumulateConstantOffsets(
      DL, OffsetA, /*AllowNonInbounds=*/true);
  const Value *PtrB1 = PtrB->stripAndAccumulateConstantOffsets(
      DL, OffsetB, /*AllowNonInbounds=*/true);

  std::optional<int64_t> Val;
  if (PtrA1 == PtrB1) {
    // Retrieve the address space again as pointer stripping now tracks through
    // `addrspacecast`.
    ASA = cast<PointerType>(PtrA1->getType())->getAddressSpace();
    ASB = cast<PointerType>(PtrB1->getType())->getAddressSpace();
    // Check that the address spaces match and that the pointers are valid.
    if (ASA != ASB)
      return std::nullopt;

    IdxWidth = DL.getIndexSizeInBits(ASA);
    OffsetA = OffsetA.sextOrTrunc(IdxWidth);
    OffsetB = OffsetB.sextOrTrunc(IdxWidth);

    OffsetB -= OffsetA;
    Val = OffsetB.trySExtValue();
  } else {
    // Otherwise compute the distance with SCEV between the base pointers.
    const SCEV *PtrSCEVA = SE.getSCEV(PtrA);
    const SCEV *PtrSCEVB = SE.getSCEV(PtrB);
    std::optional<APInt> Diff =
        SE.computeConstantDifference(PtrSCEVB, PtrSCEVA);
    if (!Diff)
      return std::nullopt;
    Val = Diff->trySExtValue();
  }

  if (!Val)
    return std::nullopt;

  int64_t Size = DL.getTypeStoreSize(ElemTyA);
  int64_t Dist = *Val / Size;

  // Ensure that the calculated distance matches the type-based one after all
  // the bitcasts removal in the provided pointers.
  if (!StrictCheck || Dist * Size == Val)
    return Dist;
  return std::nullopt;
}

bool llvm::sortPtrAccesses(ArrayRef<Value *> VL, Type *ElemTy,
                           const DataLayout &DL, ScalarEvolution &SE,
                           SmallVectorImpl<unsigned> &SortedIndices) {
  assert(llvm::all_of(
             VL, [](const Value *V) { return V->getType()->isPointerTy(); }) &&
         "Expected list of pointer operands.");
  // Walk over the pointers, and map each of them to an offset relative to
  // first pointer in the array.
  Value *Ptr0 = VL[0];

  using DistOrdPair = std::pair<int64_t, unsigned>;
  auto Compare = llvm::less_first();
  std::set<DistOrdPair, decltype(Compare)> Offsets(Compare);
  Offsets.emplace(0, 0);
  bool IsConsecutive = true;
  for (auto [Idx, Ptr] : drop_begin(enumerate(VL))) {
    std::optional<int64_t> Diff =
        getPointersDiff(ElemTy, Ptr0, ElemTy, Ptr, DL, SE,
                        /*StrictCheck=*/true);
    if (!Diff)
      return false;

    // Check if the pointer with the same offset is found.
    int64_t Offset = *Diff;
    auto [It, IsInserted] = Offsets.emplace(Offset, Idx);
    if (!IsInserted)
      return false;
    // Consecutive order if the inserted element is the last one.
    IsConsecutive &= std::next(It) == Offsets.end();
  }
  SortedIndices.clear();
  if (!IsConsecutive) {
    // Fill SortedIndices array only if it is non-consecutive.
    SortedIndices.resize(VL.size());
    for (auto [Idx, Off] : enumerate(Offsets))
      SortedIndices[Idx] = Off.second;
  }
  return true;
}

/// Returns true if the memory operations \p A and \p B are consecutive.
bool llvm::isConsecutiveAccess(Value *A, Value *B, const DataLayout &DL,
                               ScalarEvolution &SE, bool CheckType) {
  Value *PtrA = getLoadStorePointerOperand(A);
  Value *PtrB = getLoadStorePointerOperand(B);
  if (!PtrA || !PtrB)
    return false;
  Type *ElemTyA = getLoadStoreType(A);
  Type *ElemTyB = getLoadStoreType(B);
  std::optional<int64_t> Diff =
      getPointersDiff(ElemTyA, PtrA, ElemTyB, PtrB, DL, SE,
                      /*StrictCheck=*/true, CheckType);
  return Diff == 1;
}

void MemoryDepChecker::addAccess(StoreInst *SI) {
  visitPointers(SI->getPointerOperand(), *InnermostLoop,
                [this, SI](Value *Ptr) {
                  Accesses[MemAccessInfo(Ptr, true)].push_back(AccessIdx);
                  InstMap.push_back(SI);
                  ++AccessIdx;
                });
}

void MemoryDepChecker::addAccess(LoadInst *LI) {
  visitPointers(LI->getPointerOperand(), *InnermostLoop,
                [this, LI](Value *Ptr) {
                  Accesses[MemAccessInfo(Ptr, false)].push_back(AccessIdx);
                  InstMap.push_back(LI);
                  ++AccessIdx;
                });
}

MemoryDepChecker::VectorizationSafetyStatus
MemoryDepChecker::Dependence::isSafeForVectorization(DepType Type) {
  switch (Type) {
  case NoDep:
  case Forward:
  case BackwardVectorizable:
    return VectorizationSafetyStatus::Safe;

  case Unknown:
    return VectorizationSafetyStatus::PossiblySafeWithRtChecks;
  case ForwardButPreventsForwarding:
  case Backward:
  case BackwardVectorizableButPreventsForwarding:
  case IndirectUnsafe:
    return VectorizationSafetyStatus::Unsafe;
  }
  llvm_unreachable("unexpected DepType!");
}

bool MemoryDepChecker::Dependence::isBackward() const {
  switch (Type) {
  case NoDep:
  case Forward:
  case ForwardButPreventsForwarding:
  case Unknown:
  case IndirectUnsafe:
    return false;

  case BackwardVectorizable:
  case Backward:
  case BackwardVectorizableButPreventsForwarding:
    return true;
  }
  llvm_unreachable("unexpected DepType!");
}

bool MemoryDepChecker::Dependence::isPossiblyBackward() const {
  return isBackward() || Type == Unknown || Type == IndirectUnsafe;
}

bool MemoryDepChecker::Dependence::isForward() const {
  switch (Type) {
  case Forward:
  case ForwardButPreventsForwarding:
    return true;

  case NoDep:
  case Unknown:
  case BackwardVectorizable:
  case Backward:
  case BackwardVectorizableButPreventsForwarding:
  case IndirectUnsafe:
    return false;
  }
  llvm_unreachable("unexpected DepType!");
}

bool MemoryDepChecker::couldPreventStoreLoadForward(uint64_t Distance,
                                                    uint64_t TypeByteSize,
                                                    unsigned CommonStride) {
  // If loads occur at a distance that is not a multiple of a feasible vector
  // factor store-load forwarding does not take place.
  // Positive dependences might cause troubles because vectorizing them might
  // prevent store-load forwarding making vectorized code run a lot slower.
  //   a[i] = a[i-3] ^ a[i-8];
  //   The stores to a[i:i+1] don't align with the stores to a[i-3:i-2] and
  //   hence on your typical architecture store-load forwarding does not take
  //   place. Vectorizing in such cases does not make sense.
  // Store-load forwarding distance.

  // After this many iterations store-to-load forwarding conflicts should not
  // cause any slowdowns.
  const uint64_t NumItersForStoreLoadThroughMemory = 8 * TypeByteSize;
  // Maximum vector factor.
  uint64_t MaxVFWithoutSLForwardIssuesPowerOf2 =
      std::min(VectorizerParams::MaxVectorWidth * TypeByteSize,
               MaxStoreLoadForwardSafeDistanceInBits);

  // Compute the smallest VF at which the store and load would be misaligned.
  for (uint64_t VF = 2 * TypeByteSize;
       VF <= MaxVFWithoutSLForwardIssuesPowerOf2; VF *= 2) {
    // If the number of vector iteration between the store and the load are
    // small we could incur conflicts.
    if (Distance % VF && Distance / VF < NumItersForStoreLoadThroughMemory) {
      MaxVFWithoutSLForwardIssuesPowerOf2 = (VF >> 1);
      break;
    }
  }

  if (MaxVFWithoutSLForwardIssuesPowerOf2 < 2 * TypeByteSize) {
    LLVM_DEBUG(
        dbgs() << "LAA: Distance " << Distance
               << " that could cause a store-load forwarding conflict\n");
    return true;
  }

  if (CommonStride &&
      MaxVFWithoutSLForwardIssuesPowerOf2 <
          MaxStoreLoadForwardSafeDistanceInBits &&
      MaxVFWithoutSLForwardIssuesPowerOf2 !=
          VectorizerParams::MaxVectorWidth * TypeByteSize) {
    uint64_t MaxVF =
        bit_floor(MaxVFWithoutSLForwardIssuesPowerOf2 / CommonStride);
    uint64_t MaxVFInBits = MaxVF * TypeByteSize * 8;
    MaxStoreLoadForwardSafeDistanceInBits =
        std::min(MaxStoreLoadForwardSafeDistanceInBits, MaxVFInBits);
  }
  return false;
}

void MemoryDepChecker::mergeInStatus(VectorizationSafetyStatus S) {
  if (Status < S)
    Status = S;
}

/// Given a dependence-distance \p Dist between two memory accesses, that have
/// strides in the same direction whose absolute value of the maximum stride is
/// given in \p MaxStride, in a loop whose maximum backedge taken count is \p
/// MaxBTC, check if it is possible to prove statically that the dependence
/// distance is larger than the range that the accesses will travel through the
/// execution of the loop. If so, return true; false otherwise. This is useful
/// for example in loops such as the following (PR31098):
///
///     for (i = 0; i < D; ++i) {
///                = out[i];
///       out[i+D] =
///     }
static bool isSafeDependenceDistance(const DataLayout &DL, ScalarEvolution &SE,
                                     const SCEV &MaxBTC, const SCEV &Dist,
                                     uint64_t MaxStride) {

  // If we can prove that
  //      (**) |Dist| > MaxBTC * Step
  // where Step is the absolute stride of the memory accesses in bytes,
  // then there is no dependence.
  //
  // Rationale:
  // We basically want to check if the absolute distance (|Dist/Step|)
  // is >= the loop iteration count (or > MaxBTC).
  // This is equivalent to the Strong SIV Test (Practical Dependence Testing,
  // Section 4.2.1); Note, that for vectorization it is sufficient to prove
  // that the dependence distance is >= VF; This is checked elsewhere.
  // But in some cases we can prune dependence distances early, and
  // even before selecting the VF, and without a runtime test, by comparing
  // the distance against the loop iteration count. Since the vectorized code
  // will be executed only if LoopCount >= VF, proving distance >= LoopCount
  // also guarantees that distance >= VF.
  //
  const SCEV *Step = SE.getConstant(MaxBTC.getType(), MaxStride);
  const SCEV *Product = SE.getMulExpr(&MaxBTC, Step);

  const SCEV *CastedDist = &Dist;
  const SCEV *CastedProduct = Product;
  uint64_t DistTypeSizeBits = DL.getTypeSizeInBits(Dist.getType());
  uint64_t ProductTypeSizeBits = DL.getTypeSizeInBits(Product->getType());

  // The dependence distance can be positive/negative, so we sign extend Dist;
  // The multiplication of the absolute stride in bytes and the
  // backedgeTakenCount is non-negative, so we zero extend Product.
  if (DistTypeSizeBits > ProductTypeSizeBits)
    CastedProduct = SE.getZeroExtendExpr(Product, Dist.getType());
  else
    CastedDist = SE.getNoopOrSignExtend(&Dist, Product->getType());

  // Is  Dist - (MaxBTC * Step) > 0 ?
  // (If so, then we have proven (**) because |Dist| >= Dist)
  const SCEV *Minus = SE.getMinusSCEV(CastedDist, CastedProduct);
  if (SE.isKnownPositive(Minus))
    return true;

  // Second try: Is  -Dist - (MaxBTC * Step) > 0 ?
  // (If so, then we have proven (**) because |Dist| >= -1*Dist)
  const SCEV *NegDist = SE.getNegativeSCEV(CastedDist);
  Minus = SE.getMinusSCEV(NegDist, CastedProduct);
  return SE.isKnownPositive(Minus);
}

/// Check the dependence for two accesses with the same stride \p Stride.
/// \p Distance is the positive distance in bytes, and \p TypeByteSize is type
/// size in bytes.
///
/// \returns true if they are independent.
static bool areStridedAccessesIndependent(uint64_t Distance, uint64_t Stride,
                                          uint64_t TypeByteSize) {
  assert(Stride > 1 && "The stride must be greater than 1");
  assert(TypeByteSize > 0 && "The type size in byte must be non-zero");
  assert(Distance > 0 && "The distance must be non-zero");

  // Skip if the distance is not multiple of type byte size.
  if (Distance % TypeByteSize)
    return false;

  // No dependence if the distance is not multiple of the stride.
  // E.g.
  //      for (i = 0; i < 1024 ; i += 4)
  //        A[i+2] = A[i] + 1;
  //
  // Two accesses in memory (distance is 2, stride is 4):
  //     | A[0] |      |      |      | A[4] |      |      |      |
  //     |      |      | A[2] |      |      |      | A[6] |      |
  //
  // E.g.
  //      for (i = 0; i < 1024 ; i += 3)
  //        A[i+4] = A[i] + 1;
  //
  // Two accesses in memory (distance is 4, stride is 3):
  //     | A[0] |      |      | A[3] |      |      | A[6] |      |      |
  //     |      |      |      |      | A[4] |      |      | A[7] |      |
  return Distance % Stride;
}

bool MemoryDepChecker::areAccessesCompletelyBeforeOrAfter(const SCEV *Src,
                                                          Type *SrcTy,
                                                          const SCEV *Sink,
                                                          Type *SinkTy) {
  const SCEV *BTC = PSE.getBackedgeTakenCount();
  const SCEV *SymbolicMaxBTC = PSE.getSymbolicMaxBackedgeTakenCount();
  ScalarEvolution &SE = *PSE.getSE();
  const auto &[SrcStart_, SrcEnd_] =
      getStartAndEndForAccess(InnermostLoop, Src, SrcTy, BTC, SymbolicMaxBTC,
                              &SE, &PointerBounds, DT, AC);
  if (isa<SCEVCouldNotCompute>(SrcStart_) || isa<SCEVCouldNotCompute>(SrcEnd_))
    return false;

  const auto &[SinkStart_, SinkEnd_] =
      getStartAndEndForAccess(InnermostLoop, Sink, SinkTy, BTC, SymbolicMaxBTC,
                              &SE, &PointerBounds, DT, AC);
  if (isa<SCEVCouldNotCompute>(SinkStart_) ||
      isa<SCEVCouldNotCompute>(SinkEnd_))
    return false;

  if (!LoopGuards)
    LoopGuards.emplace(ScalarEvolution::LoopGuards::collect(InnermostLoop, SE));

  auto SrcEnd = SE.applyLoopGuards(SrcEnd_, *LoopGuards);
  auto SinkStart = SE.applyLoopGuards(SinkStart_, *LoopGuards);
  if (SE.isKnownPredicate(CmpInst::ICMP_ULE, SrcEnd, SinkStart))
    return true;

  auto SinkEnd = SE.applyLoopGuards(SinkEnd_, *LoopGuards);
  auto SrcStart = SE.applyLoopGuards(SrcStart_, *LoopGuards);
  return SE.isKnownPredicate(CmpInst::ICMP_ULE, SinkEnd, SrcStart);
}

std::variant<MemoryDepChecker::Dependence::DepType,
             MemoryDepChecker::DepDistanceStrideAndSizeInfo>
MemoryDepChecker::getDependenceDistanceStrideAndSize(
    const AccessAnalysis::MemAccessInfo &A, Instruction *AInst,
    const AccessAnalysis::MemAccessInfo &B, Instruction *BInst) {
  const auto &DL = InnermostLoop->getHeader()->getDataLayout();
  auto &SE = *PSE.getSE();
  const auto &[APtr, AIsWrite] = A;
  const auto &[BPtr, BIsWrite] = B;

  // Two reads are independent.
  if (!AIsWrite && !BIsWrite)
    return MemoryDepChecker::Dependence::NoDep;

  Type *ATy = getLoadStoreType(AInst);
  Type *BTy = getLoadStoreType(BInst);

  // We cannot check pointers in different address spaces.
  if (APtr->getType()->getPointerAddressSpace() !=
      BPtr->getType()->getPointerAddressSpace())
    return MemoryDepChecker::Dependence::Unknown;

  std::optional<int64_t> StrideAPtr =
      getPtrStride(PSE, ATy, APtr, InnermostLoop, SymbolicStrides, true, true);
  std::optional<int64_t> StrideBPtr =
      getPtrStride(PSE, BTy, BPtr, InnermostLoop, SymbolicStrides, true, true);

  const SCEV *Src = PSE.getSCEV(APtr);
  const SCEV *Sink = PSE.getSCEV(BPtr);

  // If the induction step is negative we have to invert source and sink of the
  // dependence when measuring the distance between them. We should not swap
  // AIsWrite with BIsWrite, as their uses expect them in program order.
  if (StrideAPtr && *StrideAPtr < 0) {
    std::swap(Src, Sink);
    std::swap(AInst, BInst);
    std::swap(ATy, BTy);
    std::swap(StrideAPtr, StrideBPtr);
  }

  const SCEV *Dist = SE.getMinusSCEV(Sink, Src);

  LLVM_DEBUG(dbgs() << "LAA: Src Scev: " << *Src << "Sink Scev: " << *Sink
                    << "\n");
  LLVM_DEBUG(dbgs() << "LAA: Distance for " << *AInst << " to " << *BInst
                    << ": " << *Dist << "\n");

  // Need accesses with constant strides and the same direction for further
  // dependence analysis. We don't want to vectorize "A[B[i]] += ..." and
  // similar code or pointer arithmetic that could wrap in the address space.

  // If either Src or Sink are not strided (i.e. not a non-wrapping AddRec) and
  // not loop-invariant (stride will be 0 in that case), we cannot analyze the
  // dependence further and also cannot generate runtime checks.
  if (!StrideAPtr || !StrideBPtr) {
    LLVM_DEBUG(dbgs() << "Pointer access with non-constant stride\n");
    return MemoryDepChecker::Dependence::IndirectUnsafe;
  }

  int64_t StrideAPtrInt = *StrideAPtr;
  int64_t StrideBPtrInt = *StrideBPtr;
  LLVM_DEBUG(dbgs() << "LAA:  Src induction step: " << StrideAPtrInt
                    << " Sink induction step: " << StrideBPtrInt << "\n");
  // At least Src or Sink are loop invariant and the other is strided or
  // invariant. We can generate a runtime check to disambiguate the accesses.
  if (!StrideAPtrInt || !StrideBPtrInt)
    return MemoryDepChecker::Dependence::Unknown;

  // Both Src and Sink have a constant stride, check if they are in the same
  // direction.
  if ((StrideAPtrInt > 0) != (StrideBPtrInt > 0)) {
    LLVM_DEBUG(
        dbgs() << "Pointer access with strides in different directions\n");
    return MemoryDepChecker::Dependence::Unknown;
  }

  TypeSize AStoreSz = DL.getTypeStoreSize(ATy);
  TypeSize BStoreSz = DL.getTypeStoreSize(BTy);

  // If store sizes are not the same, set TypeByteSize to zero, so we can check
  // it in the caller isDependent.
  uint64_t ASz = DL.getTypeAllocSize(ATy);
  uint64_t BSz = DL.getTypeAllocSize(BTy);
  uint64_t TypeByteSize = (AStoreSz == BStoreSz) ? BSz : 0;

  uint64_t StrideAScaled = std::abs(StrideAPtrInt) * ASz;
  uint64_t StrideBScaled = std::abs(StrideBPtrInt) * BSz;

  uint64_t MaxStride = std::max(StrideAScaled, StrideBScaled);

  std::optional<uint64_t> CommonStride;
  if (StrideAScaled == StrideBScaled)
    CommonStride = StrideAScaled;

  // TODO: Historically, we didn't retry with runtime checks when (unscaled)
  // strides were different but there is no inherent reason to.
  if (!isa<SCEVConstant>(Dist))
    ShouldRetryWithRuntimeChecks |= StrideAPtrInt == StrideBPtrInt;

  // If distance is a SCEVCouldNotCompute, return Unknown immediately.
  if (isa<SCEVCouldNotCompute>(Dist)) {
    LLVM_DEBUG(dbgs() << "LAA: Uncomputable distance.\n");
    return Dependence::Unknown;
  }

  return DepDistanceStrideAndSizeInfo(Dist, MaxStride, CommonStride,
                                      TypeByteSize, AIsWrite, BIsWrite);
}

MemoryDepChecker::Dependence::DepType
MemoryDepChecker::isDependent(const MemAccessInfo &A, unsigned AIdx,
                              const MemAccessInfo &B, unsigned BIdx) {
  assert(AIdx < BIdx && "Must pass arguments in program order");

  // Check if we can prove that Sink only accesses memory after Src's end or
  // vice versa. The helper is used to perform the checks only on the exit paths
  // where it helps to improve the analysis result.
  auto CheckCompletelyBeforeOrAfter = [&]() {
    auto *APtr = A.getPointer();
    auto *BPtr = B.getPointer();
    Type *ATy = getLoadStoreType(InstMap[AIdx]);
    Type *BTy = getLoadStoreType(InstMap[BIdx]);
    const SCEV *Src = PSE.getSCEV(APtr);
    const SCEV *Sink = PSE.getSCEV(BPtr);
    return areAccessesCompletelyBeforeOrAfter(Src, ATy, Sink, BTy);
  };

  // Get the dependence distance, stride, type size and what access writes for
  // the dependence between A and B.
  auto Res =
      getDependenceDistanceStrideAndSize(A, InstMap[AIdx], B, InstMap[BIdx]);
  if (std::holds_alternative<Dependence::DepType>(Res)) {
    if (std::get<Dependence::DepType>(Res) == Dependence::Unknown &&
        CheckCompletelyBeforeOrAfter())
      return Dependence::NoDep;
    return std::get<Dependence::DepType>(Res);
  }

  auto &[Dist, MaxStride, CommonStride, TypeByteSize, AIsWrite, BIsWrite] =
      std::get<DepDistanceStrideAndSizeInfo>(Res);
  bool HasSameSize = TypeByteSize > 0;

  ScalarEvolution &SE = *PSE.getSE();
  auto &DL = InnermostLoop->getHeader()->getDataLayout();

  // If the distance between the acecsses is larger than their maximum absolute
  // stride multiplied by the symbolic maximum backedge taken count (which is an
  // upper bound of the number of iterations), the accesses are independet, i.e.
  // they are far enough appart that accesses won't access the same location
  // across all loop ierations.
  if (HasSameSize &&
      isSafeDependenceDistance(
          DL, SE, *(PSE.getSymbolicMaxBackedgeTakenCount()), *Dist, MaxStride))
    return Dependence::NoDep;

  // The rest of this function relies on ConstDist being at most 64-bits, which
  // is checked earlier. Will assert if the calling code changes.
  const APInt *APDist = nullptr;
  uint64_t ConstDist =
      match(Dist, m_scev_APInt(APDist)) ? APDist->abs().getZExtValue() : 0;

  // Attempt to prove strided accesses independent.
  if (APDist) {
    // If the distance between accesses and their strides are known constants,
    // check whether the accesses interlace each other.
    if (ConstDist > 0 && CommonStride && CommonStride > 1 && HasSameSize &&
        areStridedAccessesIndependent(ConstDist, *CommonStride, TypeByteSize)) {
      LLVM_DEBUG(dbgs() << "LAA: Strided accesses are independent\n");
      return Dependence::NoDep;
    }
  } else {
    if (!LoopGuards)
      LoopGuards.emplace(
          ScalarEvolution::LoopGuards::collect(InnermostLoop, SE));
    Dist = SE.applyLoopGuards(Dist, *LoopGuards);
  }

  // Negative distances are not plausible dependencies.
  if (SE.isKnownNonPositive(Dist)) {
    if (SE.isKnownNonNegative(Dist)) {
      if (HasSameSize) {
        // Write to the same location with the same size.
        return Dependence::Forward;
      }
      LLVM_DEBUG(dbgs() << "LAA: possibly zero dependence difference but "
                           "different type sizes\n");
      return Dependence::Unknown;
    }

    bool IsTrueDataDependence = (AIsWrite && !BIsWrite);
    // Check if the first access writes to a location that is read in a later
    // iteration, where the distance between them is not a multiple of a vector
    // factor and relatively small.
    //
    // NOTE: There is no need to update MaxSafeVectorWidthInBits after call to
    // couldPreventStoreLoadForward, even if it changed MinDepDistBytes, since a
    // forward dependency will allow vectorization using any width.

    if (IsTrueDataDependence && EnableForwardingConflictDetection) {
      if (!ConstDist) {
        return CheckCompletelyBeforeOrAfter() ? Dependence::NoDep
                                              : Dependence::Unknown;
      }
      if (!HasSameSize ||
          couldPreventStoreLoadForward(ConstDist, TypeByteSize)) {
        LLVM_DEBUG(
            dbgs() << "LAA: Forward but may prevent st->ld forwarding\n");
        return Dependence::ForwardButPreventsForwarding;
      }
    }

    LLVM_DEBUG(dbgs() << "LAA: Dependence is negative\n");
    return Dependence::Forward;
  }

  int64_t MinDistance = SE.getSignedRangeMin(Dist).getSExtValue();
  // Below we only handle strictly positive distances.
  if (MinDistance <= 0) {
    return CheckCompletelyBeforeOrAfter() ? Dependence::NoDep
                                          : Dependence::Unknown;
  }

  if (!HasSameSize) {
    if (CheckCompletelyBeforeOrAfter())
      return Dependence::NoDep;
    LLVM_DEBUG(dbgs() << "LAA: ReadWrite-Write positive dependency with "
                         "different type sizes\n");
    return Dependence::Unknown;
  }
  // Bail out early if passed-in parameters make vectorization not feasible.
  unsigned ForcedFactor = (VectorizerParams::VectorizationFactor ?
                           VectorizerParams::VectorizationFactor : 1);
  unsigned ForcedUnroll = (VectorizerParams::VectorizationInterleave ?
                           VectorizerParams::VectorizationInterleave : 1);
  // The minimum number of iterations for a vectorized/unrolled version.
  unsigned MinNumIter = std::max(ForcedFactor * ForcedUnroll, 2U);

  // It's not vectorizable if the distance is smaller than the minimum distance
  // needed for a vectroized/unrolled version. Vectorizing one iteration in
  // front needs MaxStride. Vectorizing the last iteration needs TypeByteSize.
  // (No need to plus the last gap distance).
  //
  // E.g. Assume one char is 1 byte in memory and one int is 4 bytes.
  //      foo(int *A) {
  //        int *B = (int *)((char *)A + 14);
  //        for (i = 0 ; i < 1024 ; i += 2)
  //          B[i] = A[i] + 1;
  //      }
  //
  // Two accesses in memory (stride is 4 * 2):
  //     | A[0] |      | A[2] |      | A[4] |      | A[6] |      |
  //                              | B[0] |      | B[2] |      | B[4] |
  //
  // MinDistance needs for vectorizing iterations except the last iteration:
  // 4 * 2 * (MinNumIter - 1). MinDistance needs for the last iteration: 4.
  // So the minimum distance needed is: 4 * 2 * (MinNumIter - 1) + 4.
  //
  // If MinNumIter is 2, it is vectorizable as the minimum distance needed is
  // 12, which is less than distance.
  //
  // If MinNumIter is 4 (Say if a user forces the vectorization factor to be 4),
  // the minimum distance needed is 28, which is greater than distance. It is
  // not safe to do vectorization.
  //
  // We use MaxStride (maximum of src and sink strides) to get a conservative
  // lower bound on the MinDistanceNeeded in case of different strides.

  // We know that Dist is positive, but it may not be constant. Use the signed
  // minimum for computations below, as this ensures we compute the closest
  // possible dependence distance.
  uint64_t MinDistanceNeeded = MaxStride * (MinNumIter - 1) + TypeByteSize;
  if (MinDistanceNeeded > static_cast<uint64_t>(MinDistance)) {
    if (!ConstDist) {
      // For non-constant distances, we checked the lower bound of the
      // dependence distance and the distance may be larger at runtime (and safe
      // for vectorization). Classify it as Unknown, so we re-try with runtime
      // checks, unless we can prove both accesses cannot overlap.
      return CheckCompletelyBeforeOrAfter() ? Dependence::NoDep
                                            : Dependence::Unknown;
    }
    LLVM_DEBUG(dbgs() << "LAA: Failure because of positive minimum distance "
                      << MinDistance << '\n');
    return Dependence::Backward;
  }

  // Unsafe if the minimum distance needed is greater than smallest dependence
  // distance distance.
  if (MinDistanceNeeded > MinDepDistBytes) {
    LLVM_DEBUG(dbgs() << "LAA: Failure because it needs at least "
                      << MinDistanceNeeded << " size in bytes\n");
    return Dependence::Backward;
  }

  MinDepDistBytes =
      std::min(static_cast<uint64_t>(MinDistance), MinDepDistBytes);

  bool IsTrueDataDependence = (!AIsWrite && BIsWrite);
  if (IsTrueDataDependence && EnableForwardingConflictDetection && ConstDist &&
      couldPreventStoreLoadForward(MinDistance, TypeByteSize, *CommonStride))
    return Dependence::BackwardVectorizableButPreventsForwarding;

  uint64_t MaxVF = MinDepDistBytes / MaxStride;
  LLVM_DEBUG(dbgs() << "LAA: Positive min distance " << MinDistance
                    << " with max VF = " << MaxVF << '\n');

  uint64_t MaxVFInBits = MaxVF * TypeByteSize * 8;
  if (!ConstDist && MaxVFInBits < MaxTargetVectorWidthInBits) {
    // For non-constant distances, we checked the lower bound of the dependence
    // distance and the distance may be larger at runtime (and safe for
    // vectorization). Classify it as Unknown, so we re-try with runtime checks,
    // unless we can prove both accesses cannot overlap.
    return CheckCompletelyBeforeOrAfter() ? Dependence::NoDep
                                          : Dependence::Unknown;
  }

  if (CheckCompletelyBeforeOrAfter())
    return Dependence::NoDep;

  MaxSafeVectorWidthInBits = std::min(MaxSafeVectorWidthInBits, MaxVFInBits);
  return Dependence::BackwardVectorizable;
}

bool MemoryDepChecker::areDepsSafe(const DepCandidates &DepCands,
                                   const MemAccessInfoList &CheckDeps) {

  MinDepDistBytes = -1;
  SmallPtrSet<MemAccessInfo, 8> Visited;
  for (MemAccessInfo CurAccess : CheckDeps) {
    if (Visited.contains(CurAccess))
      continue;

    // Check accesses within this set.
    EquivalenceClasses<MemAccessInfo>::member_iterator AI =
        DepCands.findLeader(CurAccess);
    EquivalenceClasses<MemAccessInfo>::member_iterator AE =
        DepCands.member_end();

    // Check every access pair.
    while (AI != AE) {
      Visited.insert(*AI);
      bool AIIsWrite = AI->getInt();
      // Check loads only against next equivalent class, but stores also against
      // other stores in the same equivalence class - to the same address.
      EquivalenceClasses<MemAccessInfo>::member_iterator OI =
          (AIIsWrite ? AI : std::next(AI));
      while (OI != AE) {
        // Check every accessing instruction pair in program order.
        auto &Acc = Accesses[*AI];
        for (std::vector<unsigned>::iterator I1 = Acc.begin(), I1E = Acc.end();
             I1 != I1E; ++I1)
          // Scan all accesses of another equivalence class, but only the next
          // accesses of the same equivalent class.
          for (std::vector<unsigned>::iterator
                   I2 = (OI == AI ? std::next(I1) : Accesses[*OI].begin()),
                   I2E = (OI == AI ? I1E : Accesses[*OI].end());
               I2 != I2E; ++I2) {
            auto A = std::make_pair(&*AI, *I1);
            auto B = std::make_pair(&*OI, *I2);

            assert(*I1 != *I2);
            if (*I1 > *I2)
              std::swap(A, B);

            Dependence::DepType Type =
                isDependent(*A.first, A.second, *B.first, B.second);
            mergeInStatus(Dependence::isSafeForVectorization(Type));

            // Gather dependences unless we accumulated MaxDependences
            // dependences.  In that case return as soon as we find the first
            // unsafe dependence.  This puts a limit on this quadratic
            // algorithm.
            if (RecordDependences) {
              if (Type != Dependence::NoDep)
                Dependences.emplace_back(A.second, B.second, Type);

              if (Dependences.size() >= MaxDependences) {
                RecordDependences = false;
                Dependences.clear();
                LLVM_DEBUG(dbgs()
                           << "Too many dependences, stopped recording\n");
              }
            }
            if (!RecordDependences && !isSafeForVectorization())
              return false;
          }
        ++OI;
      }
      ++AI;
    }
  }

  LLVM_DEBUG(dbgs() << "Total Dependences: " << Dependences.size() << "\n");
  return isSafeForVectorization();
}

SmallVector<Instruction *, 4>
MemoryDepChecker::getInstructionsForAccess(Value *Ptr, bool IsWrite) const {
  MemAccessInfo Access(Ptr, IsWrite);
  auto &IndexVector = Accesses.find(Access)->second;

  SmallVector<Instruction *, 4> Insts;
  transform(IndexVector,
                 std::back_inserter(Insts),
                 [&](unsigned Idx) { return this->InstMap[Idx]; });
  return Insts;
}

const char *MemoryDepChecker::Dependence::DepName[] = {
    "NoDep",
    "Unknown",
    "IndirectUnsafe",
    "Forward",
    "ForwardButPreventsForwarding",
    "Backward",
    "BackwardVectorizable",
    "BackwardVectorizableButPreventsForwarding"};

void MemoryDepChecker::Dependence::print(
    raw_ostream &OS, unsigned Depth,
    const SmallVectorImpl<Instruction *> &Instrs) const {
  OS.indent(Depth) << DepName[Type] << ":\n";
  OS.indent(Depth + 2) << *Instrs[Source] << " -> \n";
  OS.indent(Depth + 2) << *Instrs[Destination] << "\n";
}

bool LoopAccessInfo::canAnalyzeLoop() {
  // We need to have a loop header.
  LLVM_DEBUG(dbgs() << "\nLAA: Checking a loop in '"
                    << TheLoop->getHeader()->getParent()->getName() << "' from "
                    << TheLoop->getLocStr() << "\n");

  // We can only analyze innermost loops.
  if (!TheLoop->isInnermost()) {
    LLVM_DEBUG(dbgs() << "LAA: loop is not the innermost loop\n");
    recordAnalysis("NotInnerMostLoop") << "loop is not the innermost loop";
    return false;
  }

  // We must have a single backedge.
  if (TheLoop->getNumBackEdges() != 1) {
    LLVM_DEBUG(
        dbgs() << "LAA: loop control flow is not understood by analyzer\n");
    recordAnalysis("CFGNotUnderstood")
        << "loop control flow is not understood by analyzer";
    return false;
  }

  // ScalarEvolution needs to be able to find the symbolic max backedge taken
  // count, which is an upper bound on the number of loop iterations. The loop
  // may execute fewer iterations, if it exits via an uncountable exit.
  const SCEV *ExitCount = PSE->getSymbolicMaxBackedgeTakenCount();
  if (isa<SCEVCouldNotCompute>(ExitCount)) {
    recordAnalysis("CantComputeNumberOfIterations")
        << "could not determine number of loop iterations";
    LLVM_DEBUG(dbgs() << "LAA: SCEV could not compute the loop exit count.\n");
    return false;
  }

  LLVM_DEBUG(dbgs() << "LAA: Found an analyzable loop: "
                    << TheLoop->getHeader()->getName() << "\n");
  return true;
}

bool LoopAccessInfo::analyzeLoop(AAResults *AA, const LoopInfo *LI,
                                 const TargetLibraryInfo *TLI,
                                 DominatorTree *DT) {
  // Holds the Load and Store instructions.
  SmallVector<LoadInst *, 16> Loads;
  SmallVector<StoreInst *, 16> Stores;
  SmallPtrSet<MDNode *, 8> LoopAliasScopes;

  // Holds all the different accesses in the loop.
  unsigned NumReads = 0;
  unsigned NumReadWrites = 0;

  bool HasComplexMemInst = false;

  // A runtime check is only legal to insert if there are no convergent calls.
  HasConvergentOp = false;

  PtrRtChecking->Pointers.clear();
  PtrRtChecking->Need = false;

  const bool IsAnnotatedParallel = TheLoop->isAnnotatedParallel();

  const bool EnableMemAccessVersioningOfLoop =
      EnableMemAccessVersioning &&
      !TheLoop->getHeader()->getParent()->hasOptSize();

  // Traverse blocks in fixed RPOT order, regardless of their storage in the
  // loop info, as it may be arbitrary.
  LoopBlocksRPO RPOT(TheLoop);
  RPOT.perform(LI);
  for (BasicBlock *BB : RPOT) {
    // Scan the BB and collect legal loads and stores. Also detect any
    // convergent instructions.
    for (Instruction &I : *BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (Call->isConvergent())
          HasConvergentOp = true;
      }

      // With both a non-vectorizable memory instruction and a convergent
      // operation, found in this loop, no reason to continue the search.
      if (HasComplexMemInst && HasConvergentOp)
        return false;

      // Avoid hitting recordAnalysis multiple times.
      if (HasComplexMemInst)
        continue;

      // Record alias scopes defined inside the loop.
      if (auto *Decl = dyn_cast<NoAliasScopeDeclInst>(&I))
        for (Metadata *Op : Decl->getScopeList()->operands())
          LoopAliasScopes.insert(cast<MDNode>(Op));

      // Many math library functions read the rounding mode. We will only
      // vectorize a loop if it contains known function calls that don't set
      // the flag. Therefore, it is safe to ignore this read from memory.
      auto *Call = dyn_cast<CallInst>(&I);
      if (Call && getVectorIntrinsicIDForCall(Call, TLI))
        continue;

      // If this is a load, save it. If this instruction can read from memory
      // but is not a load, we only allow it if it's a call to a function with a
      // vector mapping and no pointer arguments.
      if (I.mayReadFromMemory()) {
        auto hasPointerArgs = [](CallBase *CB) {
          return any_of(CB->args(), [](Value const *Arg) {
            return Arg->getType()->isPointerTy();
          });
        };

        // If the function has an explicit vectorized counterpart, and does not
        // take output/input pointers, we can safely assume that it can be
        // vectorized.
        if (Call && !Call->isNoBuiltin() && Call->getCalledFunction() &&
            !hasPointerArgs(Call) && !VFDatabase::getMappings(*Call).empty())
          continue;

        auto *Ld = dyn_cast<LoadInst>(&I);
        if (!Ld) {
          recordAnalysis("CantVectorizeInstruction", Ld)
            << "instruction cannot be vectorized";
          HasComplexMemInst = true;
          continue;
        }
        if (!Ld->isSimple() && !IsAnnotatedParallel) {
          recordAnalysis("NonSimpleLoad", Ld)
              << "read with atomic ordering or volatile read";
          LLVM_DEBUG(dbgs() << "LAA: Found a non-simple load.\n");
          HasComplexMemInst = true;
          continue;
        }
        NumLoads++;
        Loads.push_back(Ld);
        DepChecker->addAccess(Ld);
        if (EnableMemAccessVersioningOfLoop)
          collectStridedAccess(Ld);
        continue;
      }

      // Save 'store' instructions. Abort if other instructions write to memory.
      if (I.mayWriteToMemory()) {
        auto *St = dyn_cast<StoreInst>(&I);
        if (!St) {
          recordAnalysis("CantVectorizeInstruction", St)
              << "instruction cannot be vectorized";
          HasComplexMemInst = true;
          continue;
        }
        if (!St->isSimple() && !IsAnnotatedParallel) {
          recordAnalysis("NonSimpleStore", St)
              << "write with atomic ordering or volatile write";
          LLVM_DEBUG(dbgs() << "LAA: Found a non-simple store.\n");
          HasComplexMemInst = true;
          continue;
        }
        NumStores++;
        Stores.push_back(St);
        DepChecker->addAccess(St);
        if (EnableMemAccessVersioningOfLoop)
          collectStridedAccess(St);
      }
    } // Next instr.
  } // Next block.

  if (HasComplexMemInst)
    return false;

  // Now we have two lists that hold the loads and the stores.
  // Next, we find the pointers that they use.

  // Check if we see any stores. If there are no stores, then we don't
  // care if the pointers are *restrict*.
  if (!Stores.size()) {
    LLVM_DEBUG(dbgs() << "LAA: Found a read-only loop!\n");
    return true;
  }

  MemoryDepChecker::DepCandidates DepCands;
  AccessAnalysis Accesses(TheLoop, AA, LI, DepCands, *PSE, LoopAliasScopes);

  // Holds the analyzed pointers. We don't want to call getUnderlyingObjects
  // multiple times on the same object. If the ptr is accessed twice, once
  // for read and once for write, it will only appear once (on the write
  // list). This is okay, since we are going to check for conflicts between
  // writes and between reads and writes, but not between reads and reads.
  SmallSet<std::pair<Value *, Type *>, 16> Seen;

  // Record uniform store addresses to identify if we have multiple stores
  // to the same address.
  SmallPtrSet<Value *, 16> UniformStores;

  for (StoreInst *ST : Stores) {
    Value *Ptr = ST->getPointerOperand();

    if (isInvariant(Ptr)) {
      // Record store instructions to loop invariant addresses
      StoresToInvariantAddresses.push_back(ST);
      HasStoreStoreDependenceInvolvingLoopInvariantAddress |=
          !UniformStores.insert(Ptr).second;
    }

    // If we did *not* see this pointer before, insert it to  the read-write
    // list. At this phase it is only a 'write' list.
    Type *AccessTy = getLoadStoreType(ST);
    if (Seen.insert({Ptr, AccessTy}).second) {
      ++NumReadWrites;

      MemoryLocation Loc = MemoryLocation::get(ST);
      // The TBAA metadata could have a control dependency on the predication
      // condition, so we cannot rely on it when determining whether or not we
      // need runtime pointer checks.
      if (blockNeedsPredication(ST->getParent(), TheLoop, DT))
        Loc.AATags.TBAA = nullptr;

      visitPointers(const_cast<Value *>(Loc.Ptr), *TheLoop,
                    [&Accesses, AccessTy, Loc](Value *Ptr) {
                      MemoryLocation NewLoc = Loc.getWithNewPtr(Ptr);
                      Accesses.addStore(NewLoc, AccessTy);
                    });
    }
  }

  if (IsAnnotatedParallel) {
    LLVM_DEBUG(
        dbgs() << "LAA: A loop annotated parallel, ignore memory dependency "
               << "checks.\n");
    return true;
  }

  for (LoadInst *LD : Loads) {
    Value *Ptr = LD->getPointerOperand();
    // If we did *not* see this pointer before, insert it to the
    // read list. If we *did* see it before, then it is already in
    // the read-write list. This allows us to vectorize expressions
    // such as A[i] += x;  Because the address of A[i] is a read-write
    // pointer. This only works if the index of A[i] is consecutive.
    // If the address of i is unknown (for example A[B[i]]) then we may
    // read a few words, modify, and write a few words, and some of the
    // words may be written to the same address.
    bool IsReadOnlyPtr = false;
    Type *AccessTy = getLoadStoreType(LD);
    if (Seen.insert({Ptr, AccessTy}).second ||
        !getPtrStride(*PSE, AccessTy, Ptr, TheLoop, SymbolicStrides)) {
      ++NumReads;
      IsReadOnlyPtr = true;
    }

    // See if there is an unsafe dependency between a load to a uniform address and
    // store to the same uniform address.
    if (UniformStores.contains(Ptr)) {
      LLVM_DEBUG(dbgs() << "LAA: Found an unsafe dependency between a uniform "
                           "load and uniform store to the same address!\n");
      HasLoadStoreDependenceInvolvingLoopInvariantAddress = true;
    }

    MemoryLocation Loc = MemoryLocation::get(LD);
    // The TBAA metadata could have a control dependency on the predication
    // condition, so we cannot rely on it when determining whether or not we
    // need runtime pointer checks.
    if (blockNeedsPredication(LD->getParent(), TheLoop, DT))
      Loc.AATags.TBAA = nullptr;

    visitPointers(const_cast<Value *>(Loc.Ptr), *TheLoop,
                  [&Accesses, AccessTy, Loc, IsReadOnlyPtr](Value *Ptr) {
                    MemoryLocation NewLoc = Loc.getWithNewPtr(Ptr);
                    Accesses.addLoad(NewLoc, AccessTy, IsReadOnlyPtr);
                  });
  }

  // If we write (or read-write) to a single destination and there are no
  // other reads in this loop then is it safe to vectorize.
  if (NumReadWrites == 1 && NumReads == 0) {
    LLVM_DEBUG(dbgs() << "LAA: Found a write-only loop!\n");
    return true;
  }

  // Build dependence sets and check whether we need a runtime pointer bounds
  // check.
  Accesses.buildDependenceSets();

  // Find pointers with computable bounds. We are going to use this information
  // to place a runtime bound check.
  Value *UncomputablePtr = nullptr;
  HasCompletePtrRtChecking = Accesses.canCheckPtrAtRT(
      *PtrRtChecking, TheLoop, SymbolicStrides, UncomputablePtr, AllowPartial);
  if (!HasCompletePtrRtChecking) {
    const auto *I = dyn_cast_or_null<Instruction>(UncomputablePtr);
    recordAnalysis("CantIdentifyArrayBounds", I)
        << "cannot identify array bounds";
    LLVM_DEBUG(dbgs() << "LAA: We can't vectorize because we can't find "
                      << "the array bounds.\n");
    return false;
  }

  LLVM_DEBUG(
    dbgs() << "LAA: May be able to perform a memory runtime check if needed.\n");

  bool DepsAreSafe = true;
  if (Accesses.isDependencyCheckNeeded()) {
    LLVM_DEBUG(dbgs() << "LAA: Checking memory dependencies\n");
    DepsAreSafe =
        DepChecker->areDepsSafe(DepCands, Accesses.getDependenciesToCheck());

    if (!DepsAreSafe && DepChecker->shouldRetryWithRuntimeChecks()) {
      LLVM_DEBUG(dbgs() << "LAA: Retrying with memory checks\n");

      // Clear the dependency checks. We assume they are not needed.
      Accesses.resetDepChecks(*DepChecker);

      PtrRtChecking->reset();
      PtrRtChecking->Need = true;

      UncomputablePtr = nullptr;
      HasCompletePtrRtChecking =
          Accesses.canCheckPtrAtRT(*PtrRtChecking, TheLoop, SymbolicStrides,
                                   UncomputablePtr, AllowPartial);

      // Check that we found the bounds for the pointer.
      if (!HasCompletePtrRtChecking) {
        auto *I = dyn_cast_or_null<Instruction>(UncomputablePtr);
        recordAnalysis("CantCheckMemDepsAtRunTime", I)
            << "cannot check memory dependencies at runtime";
        LLVM_DEBUG(dbgs() << "LAA: Can't vectorize with memory checks\n");
        return false;
      }
      DepsAreSafe = true;
    }
  }

  if (HasConvergentOp) {
    recordAnalysis("CantInsertRuntimeCheckWithConvergent")
        << "cannot add control dependency to convergent operation";
    LLVM_DEBUG(dbgs() << "LAA: We can't vectorize because a runtime check "
                         "would be needed with a convergent operation\n");
    return false;
  }

  if (DepsAreSafe) {
    LLVM_DEBUG(
        dbgs() << "LAA: No unsafe dependent memory operations in loop.  We"
               << (PtrRtChecking->Need ? "" : " don't")
               << " need runtime memory checks.\n");
    return true;
  }

  emitUnsafeDependenceRemark();
  return false;
}

void LoopAccessInfo::emitUnsafeDependenceRemark() {
  const auto *Deps = getDepChecker().getDependences();
  if (!Deps)
    return;
  const auto *Found =
      llvm::find_if(*Deps, [](const MemoryDepChecker::Dependence &D) {
        return MemoryDepChecker::Dependence::isSafeForVectorization(D.Type) !=
               MemoryDepChecker::VectorizationSafetyStatus::Safe;
      });
  if (Found == Deps->end())
    return;
  MemoryDepChecker::Dependence Dep = *Found;

  LLVM_DEBUG(dbgs() << "LAA: unsafe dependent memory operations in loop\n");

  // Emit remark for first unsafe dependence
  bool HasForcedDistribution = false;
  std::optional<const MDOperand *> Value =
      findStringMetadataForLoop(TheLoop, "llvm.loop.distribute.enable");
  if (Value) {
    const MDOperand *Op = *Value;
    assert(Op && mdconst::hasa<ConstantInt>(*Op) && "invalid metadata");
    HasForcedDistribution = mdconst::extract<ConstantInt>(*Op)->getZExtValue();
  }

  const std::string Info =
      HasForcedDistribution
          ? "unsafe dependent memory operations in loop."
          : "unsafe dependent memory operations in loop. Use "
            "#pragma clang loop distribute(enable) to allow loop distribution "
            "to attempt to isolate the offending operations into a separate "
            "loop";
  OptimizationRemarkAnalysis &R =
      recordAnalysis("UnsafeDep", Dep.getDestination(getDepChecker())) << Info;

  switch (Dep.Type) {
  case MemoryDepChecker::Dependence::NoDep:
  case MemoryDepChecker::Dependence::Forward:
  case MemoryDepChecker::Dependence::BackwardVectorizable:
    llvm_unreachable("Unexpected dependence");
  case MemoryDepChecker::Dependence::Backward:
    R << "\nBackward loop carried data dependence.";
    break;
  case MemoryDepChecker::Dependence::ForwardButPreventsForwarding:
    R << "\nForward loop carried data dependence that prevents "
         "store-to-load forwarding.";
    break;
  case MemoryDepChecker::Dependence::BackwardVectorizableButPreventsForwarding:
    R << "\nBackward loop carried data dependence that prevents "
         "store-to-load forwarding.";
    break;
  case MemoryDepChecker::Dependence::IndirectUnsafe:
    R << "\nUnsafe indirect dependence.";
    break;
  case MemoryDepChecker::Dependence::Unknown:
    R << "\nUnknown data dependence.";
    break;
  }

  if (Instruction *I = Dep.getSource(getDepChecker())) {
    DebugLoc SourceLoc = I->getDebugLoc();
    if (auto *DD = dyn_cast_or_null<Instruction>(getPointerOperand(I)))
      SourceLoc = DD->getDebugLoc();
    if (SourceLoc)
      R << " Memory location is the same as accessed at "
        << ore::NV("Location", SourceLoc);
  }
}

bool LoopAccessInfo::blockNeedsPredication(BasicBlock *BB, Loop *TheLoop,
                                           DominatorTree *DT)  {
  assert(TheLoop->contains(BB) && "Unknown block used");

  // Blocks that do not dominate the latch need predication.
  const BasicBlock *Latch = TheLoop->getLoopLatch();
  return !DT->dominates(BB, Latch);
}

OptimizationRemarkAnalysis &
LoopAccessInfo::recordAnalysis(StringRef RemarkName, const Instruction *I) {
  assert(!Report && "Multiple reports generated");

  const BasicBlock *CodeRegion = TheLoop->getHeader();
  DebugLoc DL = TheLoop->getStartLoc();

  if (I) {
    CodeRegion = I->getParent();
    // If there is no debug location attached to the instruction, revert back to
    // using the loop's.
    if (I->getDebugLoc())
      DL = I->getDebugLoc();
  }

  Report = std::make_unique<OptimizationRemarkAnalysis>(DEBUG_TYPE, RemarkName,
                                                        DL, CodeRegion);
  return *Report;
}

bool LoopAccessInfo::isInvariant(Value *V) const {
  auto *SE = PSE->getSE();
  if (TheLoop->isLoopInvariant(V))
    return true;
  if (!SE->isSCEVable(V->getType()))
    return false;
  const SCEV *S = SE->getSCEV(V);
  return SE->isLoopInvariant(S, TheLoop);
}

/// If \p Ptr is a GEP, which has a loop-variant operand, return that operand.
/// Otherwise, return \p Ptr.
static Value *getLoopVariantGEPOperand(Value *Ptr, ScalarEvolution *SE,
                                       Loop *Lp) {
  auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
  if (!GEP)
    return Ptr;

  Value *V = Ptr;
  for (const Use &U : GEP->operands()) {
    if (!SE->isLoopInvariant(SE->getSCEV(U), Lp)) {
      if (V == Ptr)
        V = U;
      else
        // There must be exactly one loop-variant operand.
        return Ptr;
    }
  }
  return V;
}

/// Get the stride of a pointer access in a loop. Looks for symbolic
/// strides "a[i*stride]". Returns the symbolic stride, or null otherwise.
static const SCEV *getStrideFromPointer(Value *Ptr, ScalarEvolution *SE, Loop *Lp) {
  auto *PtrTy = dyn_cast<PointerType>(Ptr->getType());
  if (!PtrTy)
    return nullptr;

  // Try to remove a gep instruction to make the pointer (actually index at this
  // point) easier analyzable. If OrigPtr is equal to Ptr we are analyzing the
  // pointer, otherwise, we are analyzing the index.
  Value *OrigPtr = Ptr;

  Ptr = getLoopVariantGEPOperand(Ptr, SE, Lp);
  const SCEV *V = SE->getSCEV(Ptr);

  if (Ptr != OrigPtr)
    // Strip off casts.
    while (auto *C = dyn_cast<SCEVIntegralCastExpr>(V))
      V = C->getOperand();

  if (!match(V, m_scev_AffineAddRec(m_SCEV(), m_SCEV(V), m_SpecificLoop(Lp))))
    return nullptr;

  // Note that the restriction after this loop invariant check are only
  // profitability restrictions.
  if (!SE->isLoopInvariant(V, Lp))
    return nullptr;

  // Look for the loop invariant symbolic value.
  if (isa<SCEVUnknown>(V))
    return V;

  if (auto *C = dyn_cast<SCEVIntegralCastExpr>(V))
    if (isa<SCEVUnknown>(C->getOperand()))
      return V;

  return nullptr;
}

void LoopAccessInfo::collectStridedAccess(Value *MemAccess) {
  Value *Ptr = getLoadStorePointerOperand(MemAccess);
  if (!Ptr)
    return;

  // Note: getStrideFromPointer is a *profitability* heuristic.  We
  // could broaden the scope of values returned here - to anything
  // which happens to be loop invariant and contributes to the
  // computation of an interesting IV - but we chose not to as we
  // don't have a cost model here, and broadening the scope exposes
  // far too many unprofitable cases.
  const SCEV *StrideExpr = getStrideFromPointer(Ptr, PSE->getSE(), TheLoop);
  if (!StrideExpr)
    return;

  LLVM_DEBUG(dbgs() << "LAA: Found a strided access that is a candidate for "
                       "versioning:");
  LLVM_DEBUG(dbgs() << "  Ptr: " << *Ptr << " Stride: " << *StrideExpr << "\n");

  if (!SpeculateUnitStride) {
    LLVM_DEBUG(dbgs() << "  Chose not to due to -laa-speculate-unit-stride\n");
    return;
  }

  // Avoid adding the "Stride == 1" predicate when we know that
  // Stride >= Trip-Count. Such a predicate will effectively optimize a single
  // or zero iteration loop, as Trip-Count <= Stride == 1.
  //
  // TODO: We are currently not making a very informed decision on when it is
  // beneficial to apply stride versioning. It might make more sense that the
  // users of this analysis (such as the vectorizer) will trigger it, based on
  // their specific cost considerations; For example, in cases where stride
  // versioning does  not help resolving memory accesses/dependences, the
  // vectorizer should evaluate the cost of the runtime test, and the benefit
  // of various possible stride specializations, considering the alternatives
  // of using gather/scatters (if available).

  const SCEV *MaxBTC = PSE->getSymbolicMaxBackedgeTakenCount();

  // Match the types so we can compare the stride and the MaxBTC.
  // The Stride can be positive/negative, so we sign extend Stride;
  // The backedgeTakenCount is non-negative, so we zero extend MaxBTC.
  const DataLayout &DL = TheLoop->getHeader()->getDataLayout();
  uint64_t StrideTypeSizeBits = DL.getTypeSizeInBits(StrideExpr->getType());
  uint64_t BETypeSizeBits = DL.getTypeSizeInBits(MaxBTC->getType());
  const SCEV *CastedStride = StrideExpr;
  const SCEV *CastedBECount = MaxBTC;
  ScalarEvolution *SE = PSE->getSE();
  if (BETypeSizeBits >= StrideTypeSizeBits)
    CastedStride = SE->getNoopOrSignExtend(StrideExpr, MaxBTC->getType());
  else
    CastedBECount = SE->getZeroExtendExpr(MaxBTC, StrideExpr->getType());
  const SCEV *StrideMinusBETaken = SE->getMinusSCEV(CastedStride, CastedBECount);
  // Since TripCount == BackEdgeTakenCount + 1, checking:
  // "Stride >= TripCount" is equivalent to checking:
  // Stride - MaxBTC> 0
  if (SE->isKnownPositive(StrideMinusBETaken)) {
    LLVM_DEBUG(
        dbgs() << "LAA: Stride>=TripCount; No point in versioning as the "
                  "Stride==1 predicate will imply that the loop executes "
                  "at most once.\n");
    return;
  }
  LLVM_DEBUG(dbgs() << "LAA: Found a strided access that we can version.\n");

  // Strip back off the integer cast, and check that our result is a
  // SCEVUnknown as we expect.
  const SCEV *StrideBase = StrideExpr;
  if (const auto *C = dyn_cast<SCEVIntegralCastExpr>(StrideBase))
    StrideBase = C->getOperand();
  SymbolicStrides[Ptr] = cast<SCEVUnknown>(StrideBase);
}

LoopAccessInfo::LoopAccessInfo(Loop *L, ScalarEvolution *SE,
                               const TargetTransformInfo *TTI,
                               const TargetLibraryInfo *TLI, AAResults *AA,
                               DominatorTree *DT, LoopInfo *LI,
                               AssumptionCache *AC, bool AllowPartial)
    : PSE(std::make_unique<PredicatedScalarEvolution>(*SE, *L)),
      PtrRtChecking(nullptr), TheLoop(L), AllowPartial(AllowPartial) {
  unsigned MaxTargetVectorWidthInBits = std::numeric_limits<unsigned>::max();
  if (TTI && !TTI->enableScalableVectorization())
    // Scale the vector width by 2 as rough estimate to also consider
    // interleaving.
    MaxTargetVectorWidthInBits =
        TTI->getRegisterBitWidth(TargetTransformInfo::RGK_FixedWidthVector) * 2;

  DepChecker = std::make_unique<MemoryDepChecker>(
      *PSE, AC, DT, L, SymbolicStrides, MaxTargetVectorWidthInBits);
  PtrRtChecking = std::make_unique<RuntimePointerChecking>(*DepChecker, SE);
  if (canAnalyzeLoop())
    CanVecMem = analyzeLoop(AA, LI, TLI, DT);
}

void LoopAccessInfo::print(raw_ostream &OS, unsigned Depth) const {
  if (CanVecMem) {
    OS.indent(Depth) << "Memory dependences are safe";
    const MemoryDepChecker &DC = getDepChecker();
    if (!DC.isSafeForAnyVectorWidth())
      OS << " with a maximum safe vector width of "
         << DC.getMaxSafeVectorWidthInBits() << " bits";
    if (!DC.isSafeForAnyStoreLoadForwardDistances()) {
      uint64_t SLDist = DC.getStoreLoadForwardSafeDistanceInBits();
      OS << ", with a maximum safe store-load forward width of " << SLDist
         << " bits";
    }
    if (PtrRtChecking->Need)
      OS << " with run-time checks";
    OS << "\n";
  }

  if (HasConvergentOp)
    OS.indent(Depth) << "Has convergent operation in loop\n";

  if (Report)
    OS.indent(Depth) << "Report: " << Report->getMsg() << "\n";

  if (auto *Dependences = DepChecker->getDependences()) {
    OS.indent(Depth) << "Dependences:\n";
    for (const auto &Dep : *Dependences) {
      Dep.print(OS, Depth + 2, DepChecker->getMemoryInstructions());
      OS << "\n";
    }
  } else
    OS.indent(Depth) << "Too many dependences, not recorded\n";

  // List the pair of accesses need run-time checks to prove independence.
  PtrRtChecking->print(OS, Depth);
  if (PtrRtChecking->Need && !HasCompletePtrRtChecking)
    OS.indent(Depth) << "Generated run-time checks are incomplete\n";
  OS << "\n";

  OS.indent(Depth)
      << "Non vectorizable stores to invariant address were "
      << (HasStoreStoreDependenceInvolvingLoopInvariantAddress ||
                  HasLoadStoreDependenceInvolvingLoopInvariantAddress
              ? ""
              : "not ")
      << "found in loop.\n";

  OS.indent(Depth) << "SCEV assumptions:\n";
  PSE->getPredicate().print(OS, Depth);

  OS << "\n";

  OS.indent(Depth) << "Expressions re-written:\n";
  PSE->print(OS, Depth);
}

const LoopAccessInfo &LoopAccessInfoManager::getInfo(Loop &L,
                                                     bool AllowPartial) {
  const auto &[It, Inserted] = LoopAccessInfoMap.try_emplace(&L);

  // We need to create the LoopAccessInfo if either we don't already have one,
  // or if it was created with a different value of AllowPartial.
  if (Inserted || It->second->hasAllowPartial() != AllowPartial)
    It->second = std::make_unique<LoopAccessInfo>(&L, &SE, TTI, TLI, &AA, &DT,
                                                  &LI, AC, AllowPartial);

  return *It->second;
}
void LoopAccessInfoManager::clear() {
  // Collect LoopAccessInfo entries that may keep references to IR outside the
  // analyzed loop or SCEVs that may have been modified or invalidated. At the
  // moment, that is loops requiring memory or SCEV runtime checks, as those cache
  // SCEVs, e.g. for pointer expressions.
  for (const auto &[L, LAI] : LoopAccessInfoMap) {
    if (LAI->getRuntimePointerChecking()->getChecks().empty() &&
        LAI->getPSE().getPredicate().isAlwaysTrue())
      continue;
    LoopAccessInfoMap.erase(L);
  }
}

bool LoopAccessInfoManager::invalidate(
    Function &F, const PreservedAnalyses &PA,
    FunctionAnalysisManager::Invalidator &Inv) {
  // Check whether our analysis is preserved.
  auto PAC = PA.getChecker<LoopAccessAnalysis>();
  if (!PAC.preserved() && !PAC.preservedSet<AllAnalysesOn<Function>>())
    // If not, give up now.
    return true;

  // Check whether the analyses we depend on became invalid for any reason.
  // Skip checking TargetLibraryAnalysis as it is immutable and can't become
  // invalid.
  return Inv.invalidate<AAManager>(F, PA) ||
         Inv.invalidate<ScalarEvolutionAnalysis>(F, PA) ||
         Inv.invalidate<LoopAnalysis>(F, PA) ||
         Inv.invalidate<DominatorTreeAnalysis>(F, PA);
}

LoopAccessInfoManager LoopAccessAnalysis::run(Function &F,
                                              FunctionAnalysisManager &FAM) {
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &AA = FAM.getResult<AAManager>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &TTI = FAM.getResult<TargetIRAnalysis>(F);
  auto &TLI = FAM.getResult<TargetLibraryAnalysis>(F);
  auto &AC = FAM.getResult<AssumptionAnalysis>(F);
  return LoopAccessInfoManager(SE, AA, DT, LI, &TTI, &TLI, &AC);
}

AnalysisKey LoopAccessAnalysis::Key;
