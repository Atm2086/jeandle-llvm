//===- JeandleJavaTypeCacheInvalidation.cpp - Cache invalidation benchmark ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/CachedJavaType.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Jeandle/VMCallback.h"

#include "benchmark/benchmark.h"

#include <algorithm>
#include <memory>
#include <string>

using namespace llvm;
using namespace llvm::jeandle;

namespace {

struct VMEnvironment {
  VMEnvironment() {
    VMCallbacks Callbacks;
    Callbacks.IsSubtype = [](uintptr_t A, uintptr_t B) {
      return A == B || B == 1;
    };
    Callbacks.IsInterface = [](uintptr_t) { return false; };
    Callbacks.IsEffectivelyFinal = [](uintptr_t) { return false; };
    Callbacks.GetSecondarySupers = [](uintptr_t) {
      return std::vector<uintptr_t>{};
    };
    Callbacks.GetCommonSuperKlass = [](uintptr_t A, uintptr_t B) {
      return A == B ? A : uintptr_t(1);
    };
    registerVMCallbacks(Callbacks);
  }
};

VMEnvironment Environment;

Function *createFunction(Module &M, unsigned PtrArgs, bool NeedsCondition) {
  LLVMContext &C = M.getContext();
  SmallVector<Type *, 32> Params(PtrArgs, PointerType::get(C, 0));
  if (NeedsCondition)
    Params.push_back(Type::getInt1Ty(C));
  Function *F = Function::Create(
      FunctionType::get(Type::getVoidTy(C), Params, false),
      Function::ExternalLinkage, "benchmark", M);
  for (unsigned I = 0; I != PtrArgs; ++I)
    F->addParamAttr(
        I, Attribute::get(C, "java-klass",
                          std::to_string(22 + (I % 8) * 11)));
  return F;
}

JavaType query(Value *V, DominatorTree *DT, Instruction *Context,
               JavaTypeCache &Cache, CachedJavaTypeQueryInfo &Info) {
  return getCachedJavaType(V, DT, Context, nullptr, Cache, Info);
}

struct LinearFixture {
  LLVMContext C;
  std::unique_ptr<Module> M;
  Function *F = nullptr;
  Value *Root = nullptr;
  JavaTypeCache Prepared;
  CachedJavaTypeQueryInfo Info;

  LinearFixture(unsigned Entries, unsigned Affected) {
    M = std::make_unique<Module>("p1", C);
    F = createFunction(*M, Entries, true);
    BasicBlock *BB = BasicBlock::Create(C, "entry", F);
    IRBuilder<> Builder(BB);
    Value *Condition = F->getArg(Entries);
    Root = F->getArg(0);
    Value *Tail = Root;
    for (unsigned I = 1; I != Affected; ++I)
      Tail = Builder.CreateSelect(Condition, Tail, Tail, "affected");
    Builder.CreateRetVoid();

    query(Tail, nullptr, nullptr, Prepared, Info);
    for (unsigned I = Affected; I != Entries; ++I)
      query(F->getArg(I), nullptr, nullptr, Prepared, Info);
  }
};

struct ContextFixture {
  LLVMContext C;
  std::unique_ptr<Module> M;
  Function *F = nullptr;
  Value *Obj = nullptr;
  SmallVector<Instruction *, 16> Contexts;
  DominatorTree DT;
  JavaTypeCache Prepared;
  CachedJavaTypeQueryInfo Info;

  ContextFixture(unsigned ContextCount) : DT() {
    M = std::make_unique<Module>("p2-p4", C);
    F = createFunction(*M, 1, false);
    Obj = F->getArg(0);

    SmallVector<BasicBlock *, 16> Blocks;
    for (unsigned I = 0; I != ContextCount; ++I)
      Blocks.push_back(BasicBlock::Create(C, "context", F));
    for (unsigned I = 0; I != ContextCount; ++I) {
      if (I + 1 != ContextCount)
        BranchInst::Create(Blocks[I + 1], Blocks[I]);
      else
        ReturnInst::Create(C, Blocks[I]);
      Contexts.push_back(Blocks[I]->getTerminator());
    }
    DT.recalculate(*F);

    for (Instruction *Context : Contexts)
      query(Obj, &DT, Context, Prepared, Info);
  }
};

struct BatchFixture {
  LLVMContext C;
  std::unique_ptr<Module> M;
  Function *F = nullptr;
  SmallVector<Value *, 32> Roots;
  Value *Shared = nullptr;
  JavaTypeCache Prepared;
  CachedJavaTypeQueryInfo Info;

  BatchFixture(unsigned BranchCount, unsigned OverlapPercent) {
    M = std::make_unique<Module>("p3", C);
    F = createFunction(*M, BranchCount, true);
    BasicBlock *BB = BasicBlock::Create(C, "entry", F);
    IRBuilder<> Builder(BB);
    Value *Condition = F->getArg(BranchCount);

    SmallVector<Value *, 32> Leaves;
    for (unsigned I = 0; I != BranchCount; ++I) {
      Value *Root = F->getArg(I);
      Roots.push_back(Root);
      Leaves.push_back(
          Builder.CreateSelect(Condition, Root, Root, "branch"));
    }
    Shared = Leaves.front();
    for (unsigned I = 1; I != BranchCount; ++I)
      Shared = Builder.CreateSelect(Condition, Shared, Leaves[I], "shared");

    unsigned SharedDepth = 0;
    if (OverlapPercent != 0) {
      unsigned Denominator = 100 - OverlapPercent;
      SharedDepth = std::max(1U, BranchCount * OverlapPercent / Denominator);
    }
    for (unsigned I = 0; I != SharedDepth; ++I)
      Shared = Builder.CreateSelect(Condition, Shared, Shared, "overlap");
    Builder.CreateRetVoid();

    query(Shared, nullptr, nullptr, Prepared, Info);
  }
};

struct ChurnFixture {
  LLVMContext C;
  std::unique_ptr<Module> M;
  Function *F = nullptr;
  SmallVector<Value *, 32> Values;
  CachedJavaTypeQueryInfo Info;

  ChurnFixture(unsigned MaxEntries) {
    M = std::make_unique<Module>("p5", C);
    F = createFunction(*M, MaxEntries, false);
    BasicBlock *BB = BasicBlock::Create(C, "entry", F);
    ReturnInst::Create(C, BB);
    for (Argument &Arg : F->args())
      Values.push_back(&Arg);
  }

  void populate(JavaTypeCache &Cache, unsigned Count) {
    for (unsigned I = 0; I != Count; ++I)
      query(Values[I], nullptr, nullptr, Cache, Info);
  }
};

void setInvalidationCounters(benchmark::State &State,
                             const JavaTypeCacheInvalidationInfo &Info) {
  State.counters["values_visited"] = Info.ValuesVisited;
  State.counters["uses_visited"] = Info.UsesVisited;
  State.counters["base_slots_scanned"] = Info.BaseSlotsScanned;
  State.counters["path_slots_scanned"] = Info.PathFactsSlotsScanned;
  State.counters["final_slots_scanned"] = Info.FinalTypesSlotsScanned;
  State.counters["base_removed"] = Info.BaseRemoved;
  State.counters["path_removed"] = Info.FactsRemoved;
  State.counters["final_removed"] = Info.FinalRemoved;
}

template <JavaTypeBaseInvalidationMode Mode>
void BM_P1_SparseBaseEdit(benchmark::State &State) {
  auto Fixture =
      std::make_unique<LinearFixture>(State.range(0), State.range(1));
  if (Fixture->Prepared.baseSize() != static_cast<size_t>(State.range(0))) {
    State.SkipWithError("P1 fixture did not populate the requested base size");
    return;
  }

  JavaTypeCacheInvalidationInfo Last;
  size_t FinalBaseEntries = 0;
  for (auto _ : State) {
    State.PauseTiming();
    JavaTypeCache Cache = Fixture->Prepared;
    const size_t Before = Cache.baseSize();
    State.ResumeTiming();
    Last = Cache.invalidate({Fixture->Root}, false, Mode);
    const size_t After = Cache.baseSize();
    FinalBaseEntries = After;
    if (Before != static_cast<size_t>(State.range(0)) || After > Before ||
        Last.BaseRemoved != Before - After) {
      State.SkipWithError("P1 invalidation counters do not match cache sizes");
    }
    benchmark::DoNotOptimize(Last.BaseRemoved);
  }
  setInvalidationCounters(State, Last);
  State.counters["base_entries"] = Fixture->Prepared.baseSize();
  State.counters["base_after"] = FinalBaseEntries;
}

void BM_P1_SelectiveKeys(benchmark::State &State) {
  BM_P1_SparseBaseEdit<JavaTypeBaseInvalidationMode::SelectiveKeys>(State);
}

void BM_P1_Scan(benchmark::State &State) {
  BM_P1_SparseBaseEdit<JavaTypeBaseInvalidationMode::Scan>(State);
}

void BM_P2_ContextFanout(benchmark::State &State) {
  auto Fixture = std::make_unique<ContextFixture>(State.range(0));
  if (Fixture->Prepared.finalTypesSize() == 0 ||
      (State.range(0) > 1 && Fixture->Prepared.pathFactsSize() == 0)) {
    State.SkipWithError("P2 fixture did not populate contextual caches");
    return;
  }

  JavaTypeCacheInvalidationInfo Last;
  size_t FinalPathEntries = 0;
  size_t FinalTypeEntries = 0;
  for (auto _ : State) {
    State.PauseTiming();
    JavaTypeCache Cache = Fixture->Prepared;
    const size_t BeforePath = Cache.pathFactsSize();
    const size_t BeforeFinal = Cache.finalTypesSize();
    State.ResumeTiming();
    Last = Cache.invalidate({Fixture->Obj});
    FinalPathEntries = Cache.pathFactsSize();
    FinalTypeEntries = Cache.finalTypesSize();
    if (Last.FactsRemoved != BeforePath - FinalPathEntries ||
        Last.FinalRemoved != BeforeFinal - FinalTypeEntries) {
      State.SkipWithError("P2 invalidation counters do not match cache sizes");
    }
    benchmark::DoNotOptimize(Last.FactsRemoved);
  }
  setInvalidationCounters(State, Last);
  State.counters["contexts"] = State.range(0);
  State.counters["path_entries"] = Fixture->Prepared.pathFactsSize();
  State.counters["final_entries"] = Fixture->Prepared.finalTypesSize();
  State.counters["path_after"] = FinalPathEntries;
  State.counters["final_after"] = FinalTypeEntries;
}

void BM_P3_BatchOverlap(benchmark::State &State) {
  auto Fixture =
      std::make_unique<BatchFixture>(State.range(0), State.range(1));
  const bool Batched = State.range(2);

  JavaTypeCacheInvalidationInfo Last;
  size_t FinalBaseEntries = 0;
  size_t FinalPathEntries = 0;
  size_t FinalTypeEntries = 0;
  for (auto _ : State) {
    State.PauseTiming();
    JavaTypeCache Cache = Fixture->Prepared;
    const size_t BeforeBase = Cache.baseSize();
    const size_t BeforePath = Cache.pathFactsSize();
    const size_t BeforeFinal = Cache.finalTypesSize();
    State.ResumeTiming();
    if (Batched) {
      Last = Cache.invalidate(Fixture->Roots);
    } else {
      Last = {};
      for (Value *Root : Fixture->Roots) {
        JavaTypeCacheInvalidationInfo Current = Cache.invalidate({Root});
        Last.ValuesVisited += Current.ValuesVisited;
        Last.BaseSlotsScanned += Current.BaseSlotsScanned;
        Last.PathFactsSlotsScanned += Current.PathFactsSlotsScanned;
        Last.FinalTypesSlotsScanned += Current.FinalTypesSlotsScanned;
        Last.UsesVisited += Current.UsesVisited;
        Last.BaseRemoved += Current.BaseRemoved;
        Last.FactsRemoved += Current.FactsRemoved;
        Last.FinalRemoved += Current.FinalRemoved;
        Last.FullClear |= Current.FullClear;
      }
    }
    FinalBaseEntries = Cache.baseSize();
    FinalPathEntries = Cache.pathFactsSize();
    FinalTypeEntries = Cache.finalTypesSize();
    if (BeforeBase < FinalBaseEntries || BeforePath < FinalPathEntries ||
        BeforeFinal < FinalTypeEntries ||
        Last.BaseRemoved != BeforeBase - FinalBaseEntries ||
        Last.FactsRemoved != BeforePath - FinalPathEntries ||
        Last.FinalRemoved != BeforeFinal - FinalTypeEntries) {
      State.SkipWithError("P3 invalidation counters do not match cache sizes");
    }
    benchmark::DoNotOptimize(Last.ValuesVisited);
  }
  setInvalidationCounters(State, Last);
  State.counters["branches"] = State.range(0);
  State.counters["overlap_pct"] = State.range(1);
  State.counters["batched"] = Batched;
  State.counters["base_after"] = FinalBaseEntries;
  State.counters["path_after"] = FinalPathEntries;
  State.counters["final_after"] = FinalTypeEntries;
}

void BM_P4_ContextReset(benchmark::State &State) {
  auto Fixture = std::make_unique<ContextFixture>(State.range(0));
  unsigned PostQueries = static_cast<unsigned>(State.range(1));

  for (auto _ : State) {
    State.PauseTiming();
    JavaTypeCache Cache = Fixture->Prepared;
    CachedJavaTypeQueryInfo Info;
    State.ResumeTiming();
    Cache.invalidate({Fixture->Obj});
    for (unsigned I = 0; I != PostQueries; ++I) {
      Instruction *Context =
          Fixture->Contexts[I % Fixture->Contexts.size()];
      JavaType Type = query(Fixture->Obj, &Fixture->DT, Context, Cache, Info);
      benchmark::DoNotOptimize(Type.Klass);
    }
  }
  State.counters["contexts"] = State.range(0);
  State.counters["post_queries"] = PostQueries;
}

void BM_P5_HistoryAndChurn(benchmark::State &State) {
  unsigned Active = static_cast<unsigned>(State.range(0));
  unsigned HighWater = static_cast<unsigned>(State.range(1));
  unsigned Cycles = static_cast<unsigned>(State.range(2));
  auto Fixture =
      std::make_unique<ChurnFixture>(std::max(Active, HighWater));

  for (auto _ : State) {
    State.PauseTiming();
    JavaTypeCache Cache;
    Fixture->populate(Cache, HighWater);
    Cache.clear();
    State.ResumeTiming();
    for (unsigned Cycle = 0; Cycle != Cycles; ++Cycle) {
      Fixture->populate(Cache, Active);
      Cache.invalidate(
          ArrayRef<Value *>(Fixture->Values.data(), Active));
    }
    benchmark::DoNotOptimize(Cache.baseSize());
  }
  State.counters["active"] = Active;
  State.counters["high_water"] = HighWater;
  State.counters["cycles"] = Cycles;
}

void registerP1() {
  for (int Entries : {64, 1024, 16384}) {
    for (int Affected : {1, 8, 64}) {
      benchmark::RegisterBenchmark("BM_P1_SelectiveKeys",
                                   BM_P1_SelectiveKeys)
          ->Args({Entries, Affected});
      benchmark::RegisterBenchmark("BM_P1_Scan", BM_P1_Scan)
          ->Args({Entries, Affected});
    }
  }
}

void registerP2() {
  for (int Contexts : {1, 4, 16, 64})
    benchmark::RegisterBenchmark("BM_P2_ContextFanout",
                                 BM_P2_ContextFanout)
        ->Arg(Contexts);
}

void registerP3() {
  for (int Branches : {1, 8, 32, 128})
    for (int Overlap : {0, 50, 90})
      for (int Batched : {0, 1})
        benchmark::RegisterBenchmark("BM_P3_BatchOverlap",
                                     BM_P3_BatchOverlap)
            ->Args({Branches, Overlap, Batched});
}

void registerP4() {
  for (int PostQueries : {0, 1, 8, 64})
    benchmark::RegisterBenchmark("BM_P4_ContextReset",
                                 BM_P4_ContextReset)
        ->Args({64, PostQueries});
}

void registerP5() {
  for (int HighWater : {0, 4096})
    for (int Cycles : {100, 1000, 10000})
      benchmark::RegisterBenchmark("BM_P5_HistoryAndChurn",
                                   BM_P5_HistoryAndChurn)
          ->Args({64, HighWater, Cycles})
          ->Iterations(1);
}

void registerBenchmarks() {
  registerP1();
  registerP2();
  registerP3();
  registerP4();
  registerP5();
}

} // namespace

int main(int Argc, char **Argv) {
  registerBenchmarks();
  benchmark::Initialize(&Argc, Argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
