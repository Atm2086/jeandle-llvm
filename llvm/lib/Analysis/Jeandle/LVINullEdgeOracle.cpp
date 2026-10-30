//===- LVINullEdgeOracle.cpp - LVI-backed Java null-edge queries ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/LVINullEdgeOracle.h"
#include "llvm/Analysis/Jeandle/JavaTypeAnalysisStats.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/JavaType.h"
#include "llvm/Support/CommandLine.h"

#include <chrono>

using namespace llvm;

namespace {

/// E0 experiment: answer every null-edge query negatively, i.e. keep every
/// edge. The optimization stays sound (keeping an edge is always sound) and
/// the run measures how much the oracle actually costs. It is also the
/// "budget = 0" shape of the oracle-call budget.
cl::opt<bool> DisableNullEdgeOracle(
    "jeandle-disable-null-edge-oracle",
    cl::desc("Answer every null-edge oracle query negatively (keep every edge)"),
    cl::Hidden, cl::init(false));

/// S4 level 2: block-level null proof.
///
/// The edge-level query (level 3) asks LVI for the value of V on the edge
/// FromBB -> ToBB. V is an SSA value, so a null constant at the *end* of
/// FromBB already proves V null on every edge out of FromBB; the block-level
/// query (getConstant) needs one getValueInBlock instead of the
/// getEdgeValueLocal + getBlockValue pair the edge query performs.
///
/// The switch is off by default: the block-level proof is strictly stronger
/// than the edge-level one (it can answer "null" where the edge query answers
/// "unknown"), and the plan only allows changes that make the oracle answer
/// false more often, never true more often. It stays available for the A/B
/// measurement of the two levels' cost ratio.
cl::opt<bool> EnableBlockLevelNullCheck(
    "jeandle-null-edge-block-lvi",
    cl::desc("Prove nullness per (value, block) with a block-level LVI "
             "constant query before the per-edge predicate query"),
    cl::Hidden, cl::init(false));

/// The oracle is asked once per non-back predecessor of every joined block,
/// so its counters must not cost an atomic each. They accumulate into
/// thread-local plain counters and are flushed into the process-wide totals
/// once per top-level query.
thread_local NullEdgeOracleStats LocalStats;

/// Elapsed nanoseconds between two steady_clock reads.
uint64_t elapsedNanoseconds(std::chrono::steady_clock::time_point Start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - Start)
          .count());
}

} // namespace

bool llvm::isNullEdgeOracleDisabled() { return DisableNullEdgeOracle; }

NullEdgeOracleStats llvm::getLocalNullEdgeOracleStats() { return LocalStats; }

void llvm::flushLocalNullEdgeOracleStats() {
  if (LocalStats.Calls == 0 && LocalStats.Nanoseconds == 0)
    return;
  jeandle::addNullEdgeOracleStats(LocalStats);
  LocalStats = NullEdgeOracleStats();
}

/// LazyValueInfo-backed null-edge oracle. This fork's getPredicateOnEdge
/// returns a Constant*: the constant result of the comparison, or nullptr when
/// unknown. Unknown answers false (the edge is kept) — only positive proof may
/// skip an edge. Pointer casts and aliases are stripped; a freeze is stripped
/// only when its operand is proven defined, so null proofs describe the
/// queried value even when it is produced by freezing poison.
bool LVINullEdgeOracle::operator()(Value *V, BasicBlock *FromBB,
                                   BasicBlock *ToBB) const {
  ++LocalStats.Calls;
  if (DisableNullEdgeOracle)
    return false;

  while (true) {
    V = V->stripPointerCastsAndAliases();
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
      if (!jeandle::isProvenDefined(FI->getOperand(0)))
        break;
      V = FI->getOperand(0);
      continue;
    }
    break;
  }
  auto *PT = dyn_cast<PointerType>(V->getType());
  if (!PT)
    return false;

  // Structural fast path for the edge's own terminator: a conditional branch
  // on `icmp eq/ne TESTED, null` answers the query directly, for either
  // operand order and through pointer casts on the tested value — forms the
  // LVI query below does not reliably recover. A conclusive answer on the
  // tested edge is also final: the non-null side of a direct null test can
  // never be a null edge.
  if (auto *BI = dyn_cast<BranchInst>(FromBB->getTerminator()))
    if (BI->isConditional() && BI->getSuccessor(0) != BI->getSuccessor(1))
      if (auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition()))
        if (Cmp->getPredicate() == ICmpInst::ICMP_EQ ||
            Cmp->getPredicate() == ICmpInst::ICMP_NE) {
          Value *Op0 = Cmp->getOperand(0);
          Value *Op1 = Cmp->getOperand(1);
          Value *Tested =
              isa<ConstantPointerNull>(Op0->stripPointerCastsAndAliases())
                  ? Op1
                  : (isa<ConstantPointerNull>(
                         Op1->stripPointerCastsAndAliases())
                         ? Op0
                         : nullptr);
          if (Tested && Tested->stripPointerCastsAndAliases() == V) {
            // eq: the true successor is the null side; ne: the false one.
            BasicBlock *NullBB = Cmp->getPredicate() == ICmpInst::ICMP_EQ
                                     ? BI->getSuccessor(0)
                                     : BI->getSuccessor(1);
            ++LocalStats.Structural;
            bool IsNullEdge = NullBB == ToBB;
            if (IsNullEdge)
              ++LocalStats.True;
            return IsNullEdge;
          }
        }

  // S4 level 2: block-level proof. A null constant for V at the end of FromBB
  // proves V null on every edge out of FromBB (V is an SSA value, its
  // null-ness cannot change inside the block). The block value is cached by
  // LVI, so the repeated queries for the other successors of the same block
  // are cache hits.
  if (EnableBlockLevelNullCheck) {
    auto BlockStart = std::chrono::steady_clock::now();
    Constant *BlockC = LVI.getConstant(V, FromBB->getTerminator());
    LocalStats.BlockNanoseconds += elapsedNanoseconds(BlockStart);
    ++LocalStats.BlockCalls;
    if (BlockC && isa<ConstantPointerNull>(BlockC)) {
      ++LocalStats.BlockTrue;
      ++LocalStats.True;
      return true;
    }
  }

  // Only the LVI-backed part of the oracle is timed: the structural fast path
  // above is a handful of instructions, and two clock reads would cost more
  // than the work they measure.
  auto Start = std::chrono::steady_clock::now();
  Constant *C = LVI.getPredicateOnEdge(
      ICmpInst::ICMP_EQ, V, ConstantPointerNull::get(PT), FromBB, ToBB);
  LocalStats.Nanoseconds += elapsedNanoseconds(Start);
  bool IsNullEdge = C && C->isOneValue();
  if (IsNullEdge)
    ++LocalStats.True;
  return IsNullEdge;
}
