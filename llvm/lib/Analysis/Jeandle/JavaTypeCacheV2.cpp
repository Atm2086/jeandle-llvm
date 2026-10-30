//===- JavaTypeCacheV2.cpp - Owned Java type query caches -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/JavaTypeCacheV2.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/JeandleUtils.h"

using namespace llvm;
using namespace llvm::jeandle;

namespace {

constexpr unsigned MaxDependencyValues = 4096;
constexpr unsigned MaxDependencyEdges = 4 * MaxDependencyValues;

template <typename MapT, typename LeafPredicate>
size_t invalidateValues(MapT &Memo, ArrayRef<Value *> Affected,
                        bool CFGChanged, JavaTypeBaseInvalidationMode Mode,
                        size_t &SlotsScanned, LeafPredicate IsLeaf) {
  size_t OldSize = Memo.size();
  if (!CFGChanged && Mode == JavaTypeBaseInvalidationMode::SelectiveKeys) {
    for (Value *V : Affected)
      Memo.erase(V);
  } else {
    SmallPtrSet<Value *, 32> Changed(Affected.begin(), Affected.end());
    for (auto It = Memo.begin(); It != Memo.end();) {
      auto Current = It++;
      ++SlotsScanned;
      if (Changed.contains(Current->first) ||
          (CFGChanged && !IsLeaf(Current->first)))
        Memo.erase(Current);
    }
  }
  return OldSize - Memo.size();
}

/// This is a conservative dependency region, not a second JavaType solver.
/// Start from the query and all observed oracle values, walk their operand
/// graphs, then close every definition/context/oracle block over predecessors.
/// Blocks represent *all* their instructions, including negative guards and
/// instructions read implicitly by LVI. Invalidation expands changed values
/// through users, so an operand defined outside this region still reaches its
/// dependent instruction's block. No warm memo can hide this dependency walk.
bool collectFinalDependencies(Value *V, BasicBlock *BB,
                              ArrayRef<JavaTypeFinalCacheV2::NullObservation> Obs,
                              JavaTypeFinalCacheV2::Entry &Entry) {
  SmallPtrSet<Value *, 32> Values;
  SmallPtrSet<BasicBlock *, 32> Blocks;
  SmallVector<Value *, 32> ValueWork{V};
  SmallVector<BasicBlock *, 32> BlockWork;
  if (BB)
    BlockWork.push_back(BB);
  for (const auto &O : Obs) {
    ValueWork.push_back(O.V);
    BlockWork.push_back(O.From);
    BlockWork.push_back(O.To);
  }
  unsigned Edges = 0;
  while (!ValueWork.empty()) {
    Value *Current = ValueWork.pop_back_val();
    if (!Current || !Values.insert(Current).second)
      continue;
    if (Values.size() > MaxDependencyValues)
      return false;
    Entry.Values.push_back(Current);
    if (auto *I = dyn_cast<Instruction>(Current)) {
      BlockWork.push_back(I->getParent());
      if (auto *PN = dyn_cast<PHINode>(I))
        for (BasicBlock *Incoming : PN->blocks()) {
          if (++Edges > MaxDependencyEdges)
            return false;
          BlockWork.push_back(Incoming);
        }
    }
    // Do not traverse into a callee/global initializer or follow terminator
    // successor operands. Nonlocal mutations require aggregate clear().
    if (isa<GlobalValue>(Current))
      continue;
    if (auto *U = dyn_cast<User>(Current))
      for (Value *Operand : U->operands()) {
        if (++Edges > MaxDependencyEdges)
          return false;
        if (!isa<BasicBlock>(Operand))
          ValueWork.push_back(Operand);
      }
  }
  while (!BlockWork.empty()) {
    BasicBlock *Current = BlockWork.pop_back_val();
    if (!Current || !Blocks.insert(Current).second)
      continue;
    if (Values.size() + Blocks.size() > MaxDependencyValues)
      return false;
    Entry.Blocks.push_back(Current);
    for (BasicBlock *Pred : predecessors(Current)) {
      if (++Edges > MaxDependencyEdges)
        return false;
      BlockWork.push_back(Pred);
    }
  }
  return true;
}

} // namespace

void JavaTypeBaseCacheV2::beginQuery() {
  InitialSize = Memo.size();
  PriorKeys.clear();
  if (StatsEnabled && DetailedStats)
    for (const auto &E : Memo)
      PriorKeys.insert(E.first);
}

void JavaTypeBaseCacheV2::endQuery() {
  assert(Memo.size() >= InitialSize && "cache invalidated during query");
  if (StatsEnabled)
    Counters.EntriesAdded += Memo.size() - InitialSize;
  PriorKeys.clear();
}

void JavaTypeBaseCacheV2::recordReuse(Value *V) {
  if (StatsEnabled && DetailedStats) {
    if (PriorKeys.contains(V))
      ++Counters.PriorQueryHits;
    else
      ++Counters.CurrentQueryHits;
  }
}

void JavaTypeBaseCacheV2::recordHit(Value *V) {
  if (StatsEnabled)
    ++Counters.Hits;
  recordReuse(V);
}

void JavaTypeBaseCacheV2::recordMiss() {
  if (StatsEnabled)
    ++Counters.Misses;
}

void JavaTypeBaseCacheV2::recordNode() {
  if (StatsEnabled)
    ++Counters.NodesEvaluated;
}

void JavaTypeBaseCacheV2::recordInputHit(Value *V) {
  if (StatsEnabled) {
    ++Counters.InputHits;
    ++Counters.InputEdgesVisited;
  }
  recordReuse(V);
}

void JavaTypeBaseCacheV2::recordInputMiss() {
  if (StatsEnabled) {
    ++Counters.InputMisses;
    ++Counters.InputEdgesVisited;
  }
}

void JavaTypeBaseCacheV2::recordOpenEdge() {
  if (StatsEnabled) {
    ++Counters.OpenNodeEdges;
    ++Counters.InputEdgesVisited;
  }
}

void JavaTypeBaseCacheV2::recordCycle() {
  if (StatsEnabled)
    ++Counters.CyclicSCCs;
}

void JavaTypeBaseCacheV2::recordRefinement() {
  if (StatsEnabled)
    ++Counters.RefinementSteps;
}

void JavaTypeBaseCacheV2::clear() {
  if (StatsEnabled) {
    ++Counters.Clears;
    Counters.EntriesRemoved += Memo.size();
  }
  Memo.clear();
  PriorKeys.clear();
}

size_t JavaTypeBaseCacheV2::invalidate(ArrayRef<Value *> Affected,
                                     bool CFGChanged,
                                     JavaTypeBaseInvalidationMode Mode,
                                     size_t &SlotsScanned) {
  size_t Removed = invalidateValues(
      Memo, Affected, CFGChanged, Mode, SlotsScanned, [](Value *V) {
        // A returned-argument call is an identity expression, not a leaf.
        return isa<Argument, Constant, LoadInst>(V) ||
               (isa<CallBase>(V) && !getJavaIdentityInput(V));
      });
  if (StatsEnabled) {
    ++Counters.Invalidations;
    Counters.EntriesRemoved += Removed;
  }
  return Removed;
}

void JavaTypeDefinedCacheV2::recordHit() {
  if (StatsEnabled)
    ++Counters.Hits;
}

void JavaTypeDefinedCacheV2::recordMiss() {
  if (StatsEnabled)
    ++Counters.Misses;
}

void JavaTypeDefinedCacheV2::recordNode() {
  if (StatsEnabled)
    ++Counters.NodesEvaluated;
}

void JavaTypeDefinedCacheV2::recordInsertion(size_t Count) {
  if (StatsEnabled)
    Counters.EntriesAdded += Count;
}

void JavaTypeDefinedCacheV2::clear() {
  if (StatsEnabled) {
    ++Counters.Clears;
    Counters.EntriesRemoved += Memo.size();
  }
  Memo.clear();
}

size_t JavaTypeDefinedCacheV2::invalidate(ArrayRef<Value *> Affected,
                                        bool CFGChanged,
                                        JavaTypeBaseInvalidationMode Mode,
                                        size_t &SlotsScanned) {
  size_t Removed = invalidateValues(
      Memo, Affected, CFGChanged, Mode, SlotsScanned, [](Value *V) {
        return isa<Argument, Constant, LoadInst, CallBase, FreezeInst,
                   AllocaInst>(V);
      });
  if (StatsEnabled) {
    ++Counters.Invalidations;
    Counters.EntriesRemoved += Removed;
  }
  return Removed;
}

void JavaTypeFactsCacheV2::bind(Function &F) {
  assert((!Owner || Owner == &F) && "cache belongs to another function");
  Owner = &F;
}

void JavaTypeFactsCacheV2::recordNullRequest() {
  if (StatsEnabled)
    ++Counters.NullRequests;
}

void JavaTypeFactsCacheV2::recordNullHit() {
  if (StatsEnabled) {
    ++Counters.Hits;
    ++Counters.NullHits;
  }
}

void JavaTypeFactsCacheV2::recordNullComputed() {
  if (StatsEnabled) {
    ++Counters.Misses;
    ++Counters.NullComputed;
  }
}

void JavaTypeFactsCacheV2::recordGuardRequest() {
  if (StatsEnabled)
    ++Counters.GuardRequests;
}

void JavaTypeFactsCacheV2::recordGuardHit() {
  if (StatsEnabled) {
    ++Counters.Hits;
    ++Counters.GuardHits;
  }
}

void JavaTypeFactsCacheV2::recordGuardComputed() {
  if (StatsEnabled) {
    ++Counters.Misses;
    ++Counters.GuardComputed;
  }
}

void JavaTypeFactsCacheV2::recordGuardBypass() {
  if (StatsEnabled)
    ++Counters.GuardBypasses;
}

void JavaTypeFactsCacheV2::recordNull(bool Hit) {
  recordNullRequest();
  if (Hit)
    recordNullHit();
  else
    recordNullComputed();
}

void JavaTypeFactsCacheV2::recordGuard(bool Hit, bool Bypass) {
  recordGuardRequest();
  if (Hit)
    recordGuardHit();
  else
    recordGuardComputed();
  if (Bypass)
    recordGuardBypass();
}

void JavaTypeFactsCacheV2::recordInsertion(size_t Count) {
  if (StatsEnabled)
    Counters.EntriesAdded += Count;
}

void JavaTypeFactsCacheV2::clear() {
  if (StatsEnabled) {
    ++Counters.Clears;
    Counters.EntriesRemoved += size();
  }
  NullEdges.clear();
  Guards.clear();
}

size_t JavaTypeFactsCacheV2::invalidate() {
  size_t Removed = size();
  if (StatsEnabled)
    ++Counters.Invalidations;
  clear();
  return Removed;
}

JavaTypeFinalCacheV2::Entry *
JavaTypeFinalCacheV2::lookup(Value *V, BasicBlock *BB) {
  auto It = Memo.find(Key{V, BB});
  return It == Memo.end() ? nullptr : &It->second;
}

bool JavaTypeFinalCacheV2::insert(Value *V, BasicBlock *BB, const JavaType &Type,
                                ArrayRef<NullObservation> Observations) {
  Entry NewEntry;
  NewEntry.Type = Type;
  NewEntry.NullEdges.append(Observations.begin(), Observations.end());
  if (!collectFinalDependencies(V, BB, Observations, NewEntry)) {
    recordBypass();
    return false;
  }
  Key K{V, BB};
  erase(K);
  for (Value *Dependency : NewEntry.Values)
    ByValue[Dependency].insert(K);
  for (BasicBlock *Dependency : NewEntry.Blocks)
    ByBlock[Dependency].insert(K);
  Memo.try_emplace(K, std::move(NewEntry));
  if (StatsEnabled)
    ++Counters.EntriesAdded;
  return true;
}

void JavaTypeFinalCacheV2::erase(Key K) {
  auto It = Memo.find(K);
  if (It == Memo.end())
    return;
  for (Value *V : It->second.Values) {
    auto Index = ByValue.find(V);
    assert(Index != ByValue.end() && "missing final value dependency");
    Index->second.erase(K);
    if (Index->second.empty())
      ByValue.erase(Index);
  }
  for (BasicBlock *BB : It->second.Blocks) {
    auto Index = ByBlock.find(BB);
    assert(Index != ByBlock.end() && "missing final block dependency");
    Index->second.erase(K);
    if (Index->second.empty())
      ByBlock.erase(Index);
  }
  Memo.erase(It);
  if (StatsEnabled)
    ++Counters.EntriesRemoved;
}

void JavaTypeFinalCacheV2::recordHit() {
  if (StatsEnabled)
    ++Counters.Hits;
}

void JavaTypeFinalCacheV2::recordMiss() {
  if (StatsEnabled)
    ++Counters.Misses;
}

void JavaTypeFinalCacheV2::recordValidation() {
  if (StatsEnabled)
    ++Counters.Validations;
}

void JavaTypeFinalCacheV2::recordValidationFailure() {
  if (StatsEnabled)
    ++Counters.ValidationFailures;
}

void JavaTypeFinalCacheV2::recordValidation(bool Success) {
  recordValidation();
  if (!Success)
    recordValidationFailure();
}

void JavaTypeFinalCacheV2::recordBypass() {
  if (StatsEnabled)
    ++Counters.Bypasses;
}

void JavaTypeFinalCacheV2::clear() {
  if (StatsEnabled) {
    ++Counters.Clears;
    Counters.EntriesRemoved += Memo.size();
  }
  Memo.clear();
  ByValue.clear();
  ByBlock.clear();
}

void JavaTypeFinalCacheV2::notifyOracleChanged() {
  for (auto &E : Memo)
    E.second.NeedsValidation |= !E.second.NullEdges.empty();
}

size_t JavaTypeFinalCacheV2::invalidate(
    ArrayRef<Value *> Affected, ArrayRef<BasicBlock *> AffectedBlocks,
    bool CFGChanged) {
  size_t OldSize = Memo.size();
  if (StatsEnabled)
    ++Counters.Invalidations;
  if (CFGChanged) {
    // A source-only pre-mutation notification cannot identify a newly added
    // destination that was outside every previously recorded region.
    clear();
    return OldSize;
  }
  DenseSet<Key> Removed;
  for (Value *V : Affected)
    if (auto It = ByValue.find(V); It != ByValue.end())
      Removed.insert(It->second.begin(), It->second.end());
  for (BasicBlock *BB : AffectedBlocks)
    if (auto It = ByBlock.find(BB); It != ByBlock.end())
      Removed.insert(It->second.begin(), It->second.end());
  for (Key K : Removed)
    erase(K);
  // The owner may maintain/clear LVI after this notification. Unaffected IR
  // dependencies survive, but their null decisions must be checked again.
  notifyOracleChanged();
  return OldSize - Memo.size();
}

void JavaTypeCacheV2::bind(Function &F) {
  assert((!Owner || Owner == &F) && "cache belongs to another function");
  Owner = &F;
  Facts.bind(F);
}

void JavaTypeCacheV2::setStatsEnabled(bool Enabled) {
  Base.setStatsEnabled(Enabled);
  Defined.setStatsEnabled(Enabled);
  Facts.setStatsEnabled(Enabled);
  Final.setStatsEnabled(Enabled);
}

void JavaTypeCacheV2::resetStats() {
  Base.resetStats();
  Defined.resetStats();
  Facts.resetStats();
  Final.resetStats();
}

void JavaTypeCacheV2::clear() {
  Base.clear();
  Defined.clear();
  Facts.clear();
  Final.clear();
}

void JavaTypeCacheV2::notifyOracleChanged() {
  Facts.invalidate();
  Final.notifyOracleChanged();
}

JavaTypeCacheV2InvalidationInfo
JavaTypeCacheV2::invalidate(ArrayRef<Value *> Changed, bool CFGChanged,
                            JavaTypeBaseInvalidationMode Mode) {
  JavaTypeCacheV2InvalidationInfo Info;
  size_t OldBase = Base.size();
  size_t OldDefined = Defined.size();
  size_t OldFacts = Facts.size();
  size_t OldFinal = Final.size();
  SmallPtrSet<Value *, 32> Seen;
  SmallPtrSet<BasicBlock *, 32> SeenBlocks;
  SmallVector<Value *, 32> Affected;
  SmallVector<BasicBlock *, 16> Blocks;
  SmallVector<Value *, 32> Pending(Changed.begin(), Changed.end());
  auto AddBlock = [&](BasicBlock *BB) {
    if (SeenBlocks.insert(BB).second)
      Blocks.push_back(BB);
  };
  while (!Pending.empty()) {
    Value *V = Pending.pop_back_val();
    if (!V || !Seen.insert(V).second)
      continue;
    if (++Info.ValuesVisited > MaxDependencyValues) {
      Info.FullClear = true;
      break;
    }
    if (auto *BB = dyn_cast<BasicBlock>(V)) {
      if (Owner && BB->getParent() != Owner) {
        Info.FullClear = true;
        break;
      }
      AddBlock(BB);
      // Block notifications also cover values about to be erased with a
      // block. For insertion/movement this is conservative but keeps one
      // notification contract for both block contents and their users.
      for (Instruction &I : *BB)
        Pending.push_back(&I);
      continue;
    }
    Function *ValueOwner = nullptr;
    if (auto *I = dyn_cast<Instruction>(V)) {
      ValueOwner = I->getFunction();
      AddBlock(I->getParent());
    } else if (auto *A = dyn_cast<Argument>(V)) {
      ValueOwner = A->getParent();
    } else {
      Info.FullClear = true;
      break;
    }
    if (Owner && ValueOwner != Owner) {
      Info.FullClear = true;
      break;
    }
    Affected.push_back(V);
    for (User *U : V->users()) {
      if (++Info.UsesVisited > MaxDependencyEdges) {
        Info.FullClear = true;
        break;
      }
      Pending.push_back(U);
    }
    if (Info.FullClear)
      break;
  }
  if (Info.FullClear) {
    if (Base.StatsEnabled)
      ++Base.Counters.Invalidations;
    if (Defined.StatsEnabled)
      ++Defined.Counters.Invalidations;
    if (Facts.StatsEnabled)
      ++Facts.Counters.Invalidations;
    if (Final.StatsEnabled)
      ++Final.Counters.Invalidations;
    clear();
    Info.BaseRemoved = OldBase;
    Info.DefinedRemoved = OldDefined;
    Info.FactsRemoved = OldFacts;
    Info.FinalRemoved = OldFinal;
    return Info;
  }
  Info.BaseRemoved =
      Base.invalidate(Affected, CFGChanged, Mode, Info.BaseSlotsScanned);
  Info.DefinedRemoved = Defined.invalidate(Affected, CFGChanged, Mode,
                                           Info.DefinedSlotsScanned);
  Info.FactsRemoved = Facts.invalidate();
  Info.FinalRemoved = Final.invalidate(Affected, Blocks, CFGChanged);
  // The remaining entries were visited to mark oracle observations stale.
  Info.FinalTypesSlotsScanned = CFGChanged ? 0 : Final.size();
  return Info;
}
