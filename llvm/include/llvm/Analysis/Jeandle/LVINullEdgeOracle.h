//===- LVINullEdgeOracle.h - LVI-backed Java null-edge queries ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_LVINULLEDGEORACLE_H
#define LLVM_ANALYSIS_JEANDLE_LVINULLEDGEORACLE_H

#include "llvm/Support/Compiler.h"

#include <cstdint>

namespace llvm {
class Value;
class BasicBlock;

class LazyValueInfo;

/// A jeandle::IsNullEdgeOracle backed by LazyValueInfo: answers "is V provably
/// null on edge FromBB -> ToBB?" via LVI's edge-predicate query. The
/// referenced LVI must stay valid for the queried CFG state. After changing
/// blocks or edges, callers must update or clear LVI before the next query.
struct LVINullEdgeOracle {
  LazyValueInfo &LVI;
  LLVM_ABI bool operator()(Value *V, BasicBlock *FromBB, BasicBlock *ToBB) const;
};

/// Process-wide counters for the null-edge oracle. The oracle is the dominant
/// cost of the path-facts join, so its call volume and answer distribution
/// decide which null-edge optimization is worth implementing.
///   Calls      - oracle invocations
///   Structural - calls resolved by the structural fast path (a direct
///                `icmp eq/ne V, null` test on the edge's own terminator)
///   True       - calls answered "this edge is a null edge" (the only answer
///                that removes an edge from the join)
///   Nanoseconds - time spent inside the LVI-backed part of the oracle; the
///                structural fast path is a handful of instructions and is not
///                timed, because two clock reads would cost more than it does.
///   BlockCalls  - calls that ran the block-level (level 2) LVI query
///   BlockTrue   - calls the block-level query answered "this edge is a null
///                 edge"
///   BlockNanoseconds - time spent inside the block-level LVI query
struct NullEdgeOracleStats {
  uint64_t Calls = 0;
  uint64_t Structural = 0;
  uint64_t True = 0;
  uint64_t Nanoseconds = 0;
  uint64_t BlockCalls = 0;
  uint64_t BlockTrue = 0;
  uint64_t BlockNanoseconds = 0;
};

/// Returns the null-edge oracle counters this thread accumulated since the
/// last flush.
LLVM_ABI NullEdgeOracleStats getLocalNullEdgeOracleStats();

/// Flushes the thread-local null-edge oracle counters into the process-wide
/// totals and resets them. Called once per top-level Java type query, so the
/// oracle's hot path never touches a shared atomic.
LLVM_ABI void flushLocalNullEdgeOracleStats();

/// Returns whether the null-edge oracle is disabled, i.e. whether every
/// query is answered negatively (every edge is kept). This is the E0
/// experiment of the null-edge optimization plan: it keeps the whole
/// optimization sound and measures how much the oracle actually costs.
LLVM_ABI bool isNullEdgeOracleDisabled();

} // namespace llvm

#endif
