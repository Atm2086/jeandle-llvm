//===- CachedJavaTypeV2.h - Cached current Java type queries ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_CACHEDJAVATYPEV2_H
#define LLVM_ANALYSIS_JEANDLE_CACHEDJAVATYPEV2_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/Jeandle/CachedJavaType.h"

namespace llvm {
class Function;
}

namespace llvm::jeandle {

/// Caller-owned edge facts for one function. An epoch consists of serial
/// queries over fixed IR/CFG, attributes, metadata, DT and VM/type information.
/// Clear BEFORE any relevant mutation, RAUW or deletion, including instruction
/// moves and changes outside the queried value's SSA use graph. The Base cache
/// has a different invalidation policy and must be maintained separately.
///
/// GuardOnly caches condition tracing without changing oracle calls. Opting in
/// to StableOracleEpoch additionally promises that the supplied null oracle's
/// answers are stable: keep the same analysis instance and assumptions, and
/// clear before any oracle maintenance, clear/forget/threadEdge or replacement.
/// In particular, unchanged IR alone does not establish that contract for an
/// arbitrary oracle. Only completed edge results persist; traversal, budgets,
/// PHI stacks and partial block facts remain query-local.
class JavaTypeFactsCacheV2 {
public:
  enum class Mode { GuardOnly, StableOracleEpoch };
  using Key = std::pair<Value *, std::pair<BasicBlock *, BasicBlock *>>;

  /// Cumulative helper calls, not LVI cache misses or internal solve counts.
  /// NullComputed also includes structural fallback or unreachable-edge work
  /// which may not call the oracle. GuardComputed includes nested bypasses.
  struct Stats {
    uint64_t NullRequests = 0;
    uint64_t NullHits = 0;
    uint64_t NullComputed = 0;
    uint64_t GuardRequests = 0;
    uint64_t GuardHits = 0;
    uint64_t GuardComputed = 0;
    uint64_t GuardBypasses = 0;
  };

private:
  Function *Owner;
  Mode CacheMode;
  DenseMap<Key, bool> NullEdges;
  DenseMap<Key, JavaType> Guards;
  Stats Counters;

  friend struct JavaTypeFactsCacheV2Access;

public:
  explicit JavaTypeFactsCacheV2(Function &F, Mode M = Mode::GuardOnly)
      : Owner(&F), CacheMode(M) {}

  /// Begin a new epoch. Counters remain cumulative until resetStats(). No
  /// observer tracks IR or oracle changes; the owner must call this explicitly.
  void clear() {
    NullEdges.clear();
    Guards.clear();
  }
  Function *getFunction() const { return Owner; }
  Mode getMode() const { return CacheMode; }
  size_t nullSize() const { return NullEdges.size(); }
  size_t guardSize() const { return Guards.size(); }
  const Stats &getStats() const { return Counters; }
  void resetStats() { Counters = {}; }
};

/// Completed context-independent types for the isolated V2 query engine.
/// Definedness proofs and contextual traversal state stay query-local.
using JavaTypeBaseMemo = DenseMap<Value *, JavaType>;

/// Optional work counters for one top-level V2 query. Enabling these snapshots
/// the existing keys to distinguish reuse across queries from reuse within one
/// query. Leave disabled for timing measurements; no snapshot is then built.
/// Entry probes are reported separately in CachedJavaTypeQueryInfo::Base*.
struct JavaTypeCacheV2Diagnostics {
  uint64_t InputHits = 0;
  uint64_t InputMisses = 0; // Dependencies requiring discovery, not open SCCs.
  uint64_t PriorQueryHits = 0; // Entry + input hits present before this query.
  uint64_t CurrentQueryHits = 0; // Completed earlier in this query.
  uint64_t NodesEvaluated = 0;
  uint64_t InputEdgesVisited = 0; // DFS edges, excluding SCC refinement.
  uint64_t OpenNodeEdges = 0; // Cycle/cross edges to an unfinished component.
  uint64_t CyclicSCCs = 0;
  uint64_t RefinementSteps = 0;
  uint64_t EntriesAdded = 0;
};

/// Only complete base types survive a query.
/// Keep this cache separate from the old engine's JavaTypeCache. It belongs to
/// one function. Notify relevant mutations before changing IR, attributes or
/// metadata; VM/type-universe changes and unclassified changes require clear().
/// Definedness proofs and all contextual query state remain query-local.
class JavaTypeCacheV2 {
  JavaTypeBaseMemo BaseTypes;

  friend JavaType getCachedJavaTypeV2(Value *, DominatorTree *, Instruction *,
                                     IsNullEdgeOracle, JavaTypeCacheV2 &,
                                     CachedJavaTypeQueryInfo &,
                                     JavaTypeCacheV2Diagnostics *,
                                     JavaTypeFactsCacheV2 *);

public:
  void clear() { BaseTypes.clear(); }
  size_t baseSize() const { return BaseTypes.size(); }

  /// Invalidate Changed and its transitive SSA users BEFORE mutation/RAUW/
  /// deletion. Include non-pointer values: freeze typing also depends on
  /// definedness of select conditions and their integer dependencies.
  ///
  /// CFG edits need no blanket flush: notify PHIs whose incoming values change
  /// and every erased value. Merely changing incoming block labels, moving a
  /// definition or splitting an edge does not change a base type. If the edit
  /// cannot enumerate all changed/deleted values, use clear(). Notify argument
  /// attribute edits with the Argument; callee declaration attribute changes
  /// require all affected calls or clear() (callee Arguments have no call uses).
  /// Nonlocal roots (e.g. Function/global changes) and oversized use closures
  /// conservatively clear everything. Only Base/visit/FullClear fields of the
  /// returned shared statistics structure are populated; no table scan is used.
  LLVM_ABI JavaTypeCacheInvalidationInfo invalidate(ArrayRef<Value *> Changed);
};

/// Query through the isolated copy of the current JavaType query engine with
/// an external base memo. Public JavaType operations use the original helpers.
/// Parameter meanings match getCachedJavaType; V2 has its own cache type to
/// prevent mixing results from different query engines. Info and optional
/// Diagnostics are reset on each call. BaseHits/BaseMisses count getBaseJavaType
/// entry probes, including those made by contextual queries. They do not count
/// SCC bookkeeping or input-edge probes. Optional FactsCache shares completed
/// edge work under its separate epoch contract. Its cumulative counters are
/// available through getStats(); Info's block-Facts/Final counters remain zero.
/// SkipVerification=false is not a certificate of absence of truncation.
LLVM_ABI JavaType getCachedJavaTypeV2(Value *V, DominatorTree *DT,
                                     Instruction *Context,
                                     IsNullEdgeOracle IsNullEdge,
                                     JavaTypeCacheV2 &Cache,
                                     CachedJavaTypeQueryInfo &Info,
                                     JavaTypeCacheV2Diagnostics *Diagnostics =
                                         nullptr,
                                     JavaTypeFactsCacheV2 *FactsCache = nullptr);

} // namespace llvm::jeandle

#endif // LLVM_ANALYSIS_JEANDLE_CACHEDJAVATYPEV2_H
