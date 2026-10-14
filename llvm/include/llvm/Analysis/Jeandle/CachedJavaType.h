//===- CachedJavaType.h - Cached Java type queries --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_CACHEDJAVATYPE_H
#define LLVM_ANALYSIS_JEANDLE_CACHEDJAVATYPE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Jeandle/JavaType.h"
#include <cstddef>
#include <cstdint>
#include <utility>

namespace llvm::jeandle {

/// Observations from one query, reset by getCachedJavaType.
/// SkipVerification means strict equality with a cold query is not required
/// after truncation or a PHI cycle; it does not certify the result.
struct CachedJavaTypeQueryInfo {
  bool SkipVerification = false;
  uint64_t BaseHits = 0;
  uint64_t BaseMisses = 0;
  uint64_t FactsHits = 0;
  uint64_t FactsMisses = 0;
  uint64_t FinalHits = 0;
  uint64_t FinalMisses = 0;
};

/// Work performed by one invalidation. Counts are entries, not bytes.
struct JavaTypeCacheInvalidationInfo {
  size_t BaseRemoved = 0;
  size_t FactsRemoved = 0;
  size_t FinalRemoved = 0;
  size_t ValuesVisited = 0;
  bool FullClear = false;
};

/// Completed results reusable while IR, type information and query dependencies
/// remain unchanged. Recursion stacks and budgets stay private to each query.
/// Notify mutations before replacing uses or deleting values, and update
/// DT/oracle before the next query. Unclassified changes still require clear().
class JavaTypeCache {
  using ContextKey = std::pair<Value *, BasicBlock *>;
  DenseMap<Value *, JavaType> BaseTypes;
  DenseMap<ContextKey, JavaType> PathFacts;
  DenseMap<ContextKey, JavaType> FinalTypes;

  friend JavaType getCachedJavaType(Value *, DominatorTree *, Instruction *,
                                   IsNullEdgeOracle, JavaTypeCache &,
                                   CachedJavaTypeQueryInfo &);

public:
  void clear() {
    BaseTypes.clear();
    PathFacts.clear();
    FinalTypes.clear();
  }
  /// Invalidate Changed and its transitive SSA users before mutation. Changes
  /// reaching calls, memory accesses or terminators also discard all contextual
  /// results: they may affect guards and LVI facts about unrelated values.
  /// CFGChanged additionally discards all derived base results; direct leaf
  /// types survive unless affected by Changed. Every erased value must be
  /// notified while still live, including values in erased blocks. Structural
  /// CFG edits must set CFGChanged, even when the changed terminator has no
  /// SSA users. VM/type-universe changes require clear(). Unsupported roots
  /// or an oversized use closure fall back to clearing all entries.
  LLVM_ABI JavaTypeCacheInvalidationInfo
  invalidate(ArrayRef<Value *> Changed, bool CFGChanged = false);
  size_t baseSize() const { return BaseTypes.size(); }
  size_t pathFactsSize() const { return PathFacts.size(); }
  size_t finalTypesSize() const { return FinalTypes.size(); }
};

/// @brief 
/// Query through the isolated cached engine. Use the original getJavaType in
/// JavaType.h when caching is disabled or an independent reference is needed.
/// An empty Cache starts a cold evaluation; Info is reset for every query.
/// @param V 
/// @param DT 
/// @param Context 
/// @param IsNullEdge 
/// @param Cache 
/// @param Info 统计
/// @return 
LLVM_ABI JavaType getCachedJavaType(Value *V, 
                                    DominatorTree *DT,
                                    Instruction *Context,
                                    IsNullEdgeOracle IsNullEdge,
                                    JavaTypeCache &Cache,
                                    CachedJavaTypeQueryInfo &Info);

} // namespace llvm::jeandle

#endif
