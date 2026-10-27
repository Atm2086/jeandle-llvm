//===- CachedJavaTypeV2Test.cpp - Cached Java type migration tests ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/Jeandle/CachedJavaTypeV2.h"
#include "llvm/Analysis/Jeandle/LVINullEdgeOracle.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <iterator>
#include <memory>
#include <vector>

using namespace llvm;
using namespace llvm::jeandle;

namespace {

class CachedJavaTypeV2Test : public testing::Test {
protected:
  LLVMContext Context;
  VMCallbacks Saved{};

  void SetUp() override {
    if (const VMCallbacks *CB = getVMCallbacks())
      Saved = *CB;
    VMCallbacks CB{};
    CB.IsInterface = [](uintptr_t) { return false; };
    CB.IsUnverifiedInterface = [](uintptr_t) { return false; };
    CB.IsEffectivelyFinal = [](uintptr_t) { return false; };
    CB.GetSecondarySupers = [](uintptr_t) { return std::vector<uintptr_t>{}; };
    CB.IsSubtype = [](uintptr_t A, uintptr_t B) { return A == B || B == 5; };
    CB.GetCommonSuperKlass = [](uintptr_t A, uintptr_t B) {
      return A == B ? A : uintptr_t(5);
    };
    registerVMCallbacks(CB);
  }

  void TearDown() override { registerVMCallbacks(Saved); }

  std::unique_ptr<Module> parse(StringRef IR) {
    SMDiagnostic Error;
    auto M = parseAssemblyString(IR, Error, Context);
    if (!M) {
      ADD_FAILURE() << Error.getMessage().str();
      return nullptr;
    }
    if (verifyModule(*M, &errs())) {
      ADD_FAILURE() << "Invalid test module";
      return nullptr;
    }
    return M;
  }

  static bool noNullEdge(Value *, BasicBlock *, BasicBlock *) { return false; }

  static Value *returnedValue(Function &F) {
    return cast<ReturnInst>(F.back().getTerminator())->getReturnValue();
  }

  static JavaType query(Value *V, JavaTypeCacheV2 &Cache,
                        CachedJavaTypeQueryInfo &Info) {
    return getCachedJavaTypeV2(V, nullptr, nullptr, noNullEdge, Cache, Info);
  }

  static JavaType fresh(Value *V) {
    return getJavaType(V, nullptr, nullptr, noNullEdge);
  }
};

TEST_F(CachedJavaTypeV2Test, BaseMemoReusesCompletedResultAndResetsInfo) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a,
        i1 %pick) {
      %result = select i1 %pick, ptr addrspace(1) %a,
                                ptr addrspace(1) null
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Value *V = returnedValue(*M->getFunction("test"));
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));
  EXPECT_EQ(Info.BaseHits, 0U);
  EXPECT_EQ(Info.BaseMisses, 1U);
  EXPECT_GE(Cache.baseSize(), 2U);
  size_t Size = Cache.baseSize();

  Info.SkipVerification = true;
  Info.FactsHits = Info.FactsMisses = Info.FinalHits = Info.FinalMisses = 42;
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_EQ(Info.BaseMisses, 0U);
  EXPECT_FALSE(Info.SkipVerification);
  EXPECT_EQ(Info.FactsHits, 0U);
  EXPECT_EQ(Info.FactsMisses, 0U);
  EXPECT_EQ(Info.FinalHits, 0U);
  EXPECT_EQ(Info.FinalMisses, 0U);
  EXPECT_EQ(Cache.baseSize(), Size);
}

TEST_F(CachedJavaTypeV2Test, ClearRefreshesLoadMetadataAndDerivedResult) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(ptr %slot, i1 %pick) {
      %loaded = load ptr addrspace(1), ptr %slot,
                     !java-klass !0, !java-klass-exact !1
      %result = select i1 %pick, ptr addrspace(1) %loaded,
                                ptr addrspace(1) null
      ret ptr addrspace(1) %result
    }
    !0 = !{i64 9}
    !1 = !{}
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));
  EXPECT_GE(Cache.baseSize(), 2U);

  Cache.clear();
  EXPECT_EQ(Cache.baseSize(), 0U);
  auto *Load = cast<LoadInst>(&F.front().front());
  Load->setMetadata(
      "java-klass",
      MDNode::get(Context, ConstantAsMetadata::get(
                               ConstantInt::get(Type::getInt64Ty(Context), 22))));
  EXPECT_EQ(query(V, Cache, Info), JavaType(22, true));
  EXPECT_EQ(Info.BaseMisses, 1U);
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_EQ(Info.BaseHits, 1U);
}

TEST_F(CachedJavaTypeV2Test, CompletedSCCMembersAreReusableInEitherOrder) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a,
        ptr addrspace(1) "java-klass"="22" "java-klass-exact" %b,
        i1 %again) {
    entry:
      br label %loop
    loop:
      %left = phi ptr addrspace(1) [ %a, %entry ], [ %right, %loop ]
      %right = phi ptr addrspace(1) [ %b, %entry ], [ %left, %loop ]
      br i1 %again, label %loop, label %exit
    exit:
      ret ptr addrspace(1) %left
    }
  )IR");
  ASSERT_NE(M, nullptr);
  auto Loop = std::next(M->getFunction("test")->begin());
  Value *Left = &Loop->front();
  Value *Right = &*std::next(Loop->begin());
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    Value *First = Reverse ? Right : Left;
    Value *Second = Reverse ? Left : Right;
    JavaTypeCacheV2 Cache;
    CachedJavaTypeQueryInfo Info;
    EXPECT_EQ(query(First, Cache, Info), JavaType(5, false));
    EXPECT_EQ(query(First, Cache, Info), fresh(First));
    size_t Size = Cache.baseSize();
    EXPECT_EQ(query(Second, Cache, Info), fresh(Second));
    EXPECT_EQ(Info.BaseHits, 1U);
    EXPECT_EQ(Info.BaseMisses, 0U);
    EXPECT_EQ(Cache.baseSize(), Size);
  }
}

TEST_F(CachedJavaTypeV2Test, FreezeOfPossiblyPoisonOperandRemainsUnknown) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a) {
      %result = freeze ptr addrspace(1) %a
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(F.getArg(0), Cache, Info), JavaType(9, true));
  EXPECT_TRUE(query(V, Cache, Info).isUnknown());
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_EQ(Info.BaseHits, 1U);
  DominatorTree DT(F);
  EXPECT_EQ(getCachedJavaTypeV2(V, &DT, F.back().getTerminator(), noNullEdge,
                                Cache, Info),
            getJavaType(V, &DT, F.back().getTerminator(), noNullEdge));
}

TEST_F(CachedJavaTypeV2Test, FreezeOfDefinedOperandRetainsType) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) noundef "java-klass"="9" "java-klass-exact" %a) {
      %result = freeze ptr addrspace(1) %a
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  DominatorTree DT(F);
  EXPECT_EQ(getCachedJavaTypeV2(V, &DT, F.back().getTerminator(), noNullEdge,
                                Cache, Info),
            getJavaType(V, &DT, F.back().getTerminator(), noNullEdge));
}

TEST_F(CachedJavaTypeV2Test, ReturnedCallKeepsItsIndependentTypeSeed) {
  auto M = parse(R"IR(
    declare ptr addrspace(1) @identity(ptr addrspace(1) returned)
    define ptr addrspace(1) @test(ptr addrspace(1) %a) {
      %result = call "java-klass"="9" "java-klass-exact" ptr addrspace(1)
                     @identity(ptr addrspace(1) %a)
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_TRUE(query(F.getArg(0), Cache, Info).isUnknown());
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_EQ(Info.BaseHits, 1U);
  DominatorTree DT(F);
  EXPECT_EQ(getCachedJavaTypeV2(V, &DT, F.back().getTerminator(), noNullEdge,
                                Cache, Info),
            getJavaType(V, &DT, F.back().getTerminator(), noNullEdge));
}

TEST_F(CachedJavaTypeV2Test, ContextFactsDoNotLeakBetweenQueriesOrIntoBase) {
  auto M = parse(R"IR(
    declare i1 @jeandle.check_instanceof(ptr, ptr addrspace(1) nonnull)
    define ptr addrspace(1) @test(ptr addrspace(1) %a) {
    entry:
      %check = call i1 @jeandle.check_instanceof(
          ptr inttoptr (i64 9 to ptr), ptr addrspace(1) %a)
      br i1 %check, label %accepted, label %rejected
    accepted:
      ret ptr addrspace(1) %a
    rejected:
      ret ptr addrspace(1) %a
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *V = F.getArg(0);
  Instruction *Accepted = std::next(F.begin())->getTerminator();
  Instruction *Rejected = F.back().getTerminator();
  DominatorTree DT(F);
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    JavaTypeCacheV2 Cache;
    CachedJavaTypeQueryInfo Info;
    Instruction *First = Reverse ? Rejected : Accepted;
    Instruction *Second = Reverse ? Accepted : Rejected;
    bool Cold = true;
    for (Instruction *At : {First, Second, First}) {
      JavaType Result =
          getCachedJavaTypeV2(V, &DT, At, noNullEdge, Cache, Info);
      EXPECT_EQ(Result, getJavaType(V, &DT, At, noNullEdge));
      EXPECT_EQ(Result.Klass, At == Accepted ? 9U : 0U);
      EXPECT_EQ(Info.BaseHits, Cold ? 0U : 1U);
      EXPECT_EQ(Info.BaseMisses, Cold ? 1U : 0U);
      Cold = false;
      EXPECT_EQ(Info.FactsHits, 0U);
      EXPECT_EQ(Info.FinalHits, 0U);
    }
    EXPECT_TRUE(query(V, Cache, Info).isUnknown());
    EXPECT_EQ(Info.BaseHits, 1U);
    EXPECT_EQ(query(V, Cache, Info), fresh(V));
  }
}

TEST_F(CachedJavaTypeV2Test, SharpenKeepsEdgeFactsSeparateFromWarmBaseMemo) {
  auto M = parse(R"IR(
    declare i1 @jeandle.check_instanceof(ptr, ptr addrspace(1) nonnull)
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="5" %a) {
    entry:
      %first = call i1 @jeandle.check_instanceof(
          ptr inttoptr (i64 22 to ptr), ptr addrspace(1) %a)
      br i1 %first, label %bypass, label %checked
    checked:
      %second = call i1 @jeandle.check_instanceof(
          ptr inttoptr (i64 9 to ptr), ptr addrspace(1) %a)
      br i1 %second, label %accepted, label %rejected
    bypass:
      br label %accepted
    accepted:
      %yes = phi ptr addrspace(1) [ %a, %checked ], [ %a, %bypass ]
      ret ptr addrspace(1) %yes
    rejected:
      %no = phi ptr addrspace(1) [ %a, %checked ]
      ret ptr addrspace(1) %no
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *V = F.getArg(0);
  BasicBlock *Checked = &*std::next(F.begin());
  BasicBlock *Accepted = &*std::next(F.begin(), 3);
  BasicBlock *Rejected = &F.back();
  DominatorTree DT(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;

  EXPECT_EQ(query(V, Cache, Info), JavaType(5, false));
  EXPECT_EQ(getCachedJavaTypeV2(V, &DT, Rejected->getTerminator(), noNullEdge,
                               Cache, Info),
            getJavaType(V, &DT, Rejected->getTerminator(), noNullEdge));

  // The predecessor's failed check and the current edge's failed check both
  // contribute, but the cached attribute-derived base must not enter sharpen.
  JavaType Denied =
      sharpen(V, Checked->getTerminator(), DT, Rejected, noNullEdge);
  EXPECT_EQ(Denied.Klass, 0U);
  EXPECT_FALSE(Denied.Exact);
  EXPECT_TRUE(Denied.Interfaces.empty());
  EXPECT_EQ(Denied.ExcludedKlasses.size(), 2U);
  EXPECT_TRUE(Denied.ExcludedKlasses.contains(22));
  EXPECT_TRUE(Denied.ExcludedKlasses.contains(9));

  JavaType Allowed =
      sharpen(V, Checked->getTerminator(), DT, Accepted, noNullEdge);
  EXPECT_EQ(Allowed.Klass, 9U);
  EXPECT_FALSE(Allowed.Exact);
  EXPECT_EQ(query(V, Cache, Info), JavaType(5, false));
  EXPECT_EQ(Info.BaseHits, 1U);
}

TEST_F(CachedJavaTypeV2Test, SharedBudgetChangesRebuildQueryLocalFacts) {
  auto M = parse(R"IR(
    declare i1 @jeandle.check_instanceof(ptr, ptr addrspace(1) nonnull)
    define ptr addrspace(1) @test(ptr addrspace(1) %a) {
    entry:
      %check = call i1 @jeandle.check_instanceof(
          ptr inttoptr (i64 9 to ptr), ptr addrspace(1) %a)
      br i1 %check, label %accepted, label %rejected
    accepted:
      br label %query
    query:
      ret ptr addrspace(1) %a
    rejected:
      ret ptr addrspace(1) %a
    }
  )IR");
  ASSERT_NE(M, nullptr);
  auto *Option =
      cl::getRegisteredOptions().lookup("jeandle-max-facts-join-blocks");
  ASSERT_NE(Option, nullptr);
  auto &Budget = *static_cast<cl::opt<unsigned> *>(Option);
  unsigned SavedBudget = Budget;
  auto RestoreBudget = make_scope_exit([&] { Budget = SavedBudget; });

  Function &F = *M->getFunction("test");
  Value *V = F.getArg(0);
  Instruction *At = std::next(F.begin(), 2)->getTerminator();
  DominatorTree DT(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  for (unsigned Limit : {1U, 128U, 1U, 128U}) {
    SCOPED_TRACE(Limit);
    Budget = Limit;
    JavaType Result =
        getCachedJavaTypeV2(V, &DT, At, noNullEdge, Cache, Info);
    EXPECT_EQ(Result, getJavaType(V, &DT, At, noNullEdge));
    EXPECT_EQ(Result, Limit == 1 ? JavaType() : JavaType(9, false));
    EXPECT_GT(Cache.baseSize(), 0U);
  }
  // The same Base memo survives all four queries; neither a truncated result
  // nor the later complete path facts may become persistent base knowledge.
  EXPECT_TRUE(query(V, Cache, Info).isUnknown());
  EXPECT_EQ(Info.BaseHits, 1U);
}

static Instruction *findV2BaseTestInstruction(Function &F, StringRef Name) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (I.getName() == Name)
        return &I;
  return nullptr;
}

TEST_F(CachedJavaTypeV2Test, InvalidateTraversesUncachedIntegerProofDependencies) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) noundef "java-klass"="9" "java-klass-exact" %a,
        ptr addrspace(1) noundef "java-klass"="9" "java-klass-exact" %b,
        ptr addrspace(1) "java-klass"="22" "java-klass-exact" %unrelated,
        i32 noundef %n) {
      %sum = add i32 %n, 1
      %condition = icmp eq i32 %sum, 0
      %selected = select i1 %condition, ptr addrspace(1) %a,
                                       ptr addrspace(1) %b
      %result = freeze ptr addrspace(1) %selected
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  auto *Sum = cast<BinaryOperator>(findV2BaseTestInstruction(F, "sum"));
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));
  EXPECT_EQ(query(F.getArg(2), Cache, Info), JavaType(22, true));

  auto Invalidated = Cache.invalidate({Sum});
  EXPECT_FALSE(Invalidated.FullClear);
  EXPECT_GE(Invalidated.BaseRemoved, 2U);
  Sum->setHasNoSignedWrap(true);

  EXPECT_TRUE(query(V, Cache, Info).isUnknown());
  EXPECT_GT(Info.BaseMisses, 0U);
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_EQ(query(F.getArg(2), Cache, Info), JavaType(22, true));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(CachedJavaTypeV2Test, ReturnedCallSeedDoesNotHideItsOperandDependency) {
  auto M = parse(R"IR(
    declare ptr addrspace(1) @identity(ptr addrspace(1) returned)
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a,
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %unrelated) {
      %result = call "java-klass"="5" ptr addrspace(1)
                     @identity(ptr addrspace(1) %a)
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));
  EXPECT_EQ(query(F.getArg(1), Cache, Info), JavaType(9, true));

  auto Invalidated = Cache.invalidate({F.getArg(0)});
  EXPECT_FALSE(Invalidated.FullClear);
  EXPECT_GE(Invalidated.BaseRemoved, 2U);
  F.addParamAttr(0, Attribute::get(Context, "java-klass", "22"));

  EXPECT_EQ(query(V, Cache, Info), JavaType(22, true));
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_EQ(query(F.getArg(1), Cache, Info), JavaType(9, true));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(CachedJavaTypeV2Test, InvalidateSCCMemberRecomputesWholeDependentCycle) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a,
        ptr addrspace(1) "java-klass"="22" "java-klass-exact" %b,
        i1 %again) {
    entry:
      br label %loop
    loop:
      %left = phi ptr addrspace(1) [ %a, %entry ], [ %right, %loop ]
      %right = phi ptr addrspace(1) [ %b, %entry ], [ %left, %loop ]
      br i1 %again, label %loop, label %exit
    exit:
      ret ptr addrspace(1) %left
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  auto *Left = cast<PHINode>(findV2BaseTestInstruction(F, "left"));
  auto *Right = cast<PHINode>(findV2BaseTestInstruction(F, "right"));
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(Left, Cache, Info), JavaType(5, false));
  EXPECT_EQ(query(Right, Cache, Info), JavaType(5, false));

  auto Invalidated = Cache.invalidate({Right});
  EXPECT_FALSE(Invalidated.FullClear);
  EXPECT_EQ(Invalidated.BaseRemoved, 2U);
  Right->setIncomingValue(0, F.getArg(0));

  EXPECT_EQ(query(Left, Cache, Info), JavaType(9, true));
  EXPECT_EQ(query(Left, Cache, Info), fresh(Left));
  EXPECT_EQ(query(Right, Cache, Info), fresh(Right));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_EQ(query(F.getArg(1), Cache, Info), JavaType(22, true));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(CachedJavaTypeV2Test, CFGEdgeRemovalInvalidatesOnlyExplicitPhiClosure) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a,
        ptr addrspace(1) "java-klass"="22" "java-klass-exact" %b,
        i1 %pick) {
    entry:
      %stable = select i1 %pick, ptr addrspace(1) %a,
                                ptr addrspace(1) null
      br i1 %pick, label %left, label %right
    left:
      br label %merge
    right:
      br label %merge
    merge:
      %result = phi ptr addrspace(1) [ %a, %left ], [ %b, %right ]
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  auto *Phi = cast<PHINode>(returnedValue(F));
  Value *Stable = findV2BaseTestInstruction(F, "stable");
  BasicBlock *Right = &*std::next(F.begin(), 2);
  Instruction *RightBranch = Right->getTerminator();
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(Phi, Cache, Info), JavaType(5, false));
  EXPECT_EQ(query(Stable, Cache, Info), JavaType(9, true));

  // Redirect one predecessor to return directly. The PHI edit, rather than
  // the predecessor terminator's SSA users, identifies the affected base key.
  auto Invalidated = Cache.invalidate({RightBranch, Phi});
  EXPECT_FALSE(Invalidated.FullClear);
  EXPECT_EQ(Invalidated.BaseRemoved, 1U);
  ReturnInst::Create(Context, F.getArg(1), RightBranch->getIterator());
  RightBranch->eraseFromParent();
  Phi->removeIncomingValue(Right, false);

  EXPECT_EQ(query(Phi, Cache, Info), JavaType(9, true));
  EXPECT_EQ(query(Phi, Cache, Info), fresh(Phi));
  EXPECT_EQ(query(Stable, Cache, Info), JavaType(9, true));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(CachedJavaTypeV2Test, PureBlockSplitKeepsUnchangedBaseResult) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a,
        i1 %pick) {
      %result = select i1 %pick, ptr addrspace(1) %a,
                                ptr addrspace(1) null
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));
  size_t Size = Cache.baseSize();

  // The new edge does not change any type/proof operand or destroy a value.
  F.front().splitBasicBlock(F.front().getTerminator()->getIterator(), "split");

  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_EQ(Cache.baseSize(), Size);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(CachedJavaTypeV2Test, NotifyBeforeRAUWAndDeleteKeepsUnrelatedBaseEntries) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a,
        ptr addrspace(1) "java-klass"="22" "java-klass-exact" %b,
        i1 %pick) {
      %old = select i1 %pick, ptr addrspace(1) %a,
                             ptr addrspace(1) null
      %result = getelementptr i8, ptr addrspace(1) %old, i64 0
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Instruction *Old = findV2BaseTestInstruction(F, "old");
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));
  EXPECT_EQ(query(F.getArg(1), Cache, Info), JavaType(22, true));

  auto Invalidated = Cache.invalidate({Old});
  EXPECT_FALSE(Invalidated.FullClear);
  EXPECT_EQ(Invalidated.BaseRemoved, 2U);
  Old->replaceAllUsesWith(F.getArg(1));
  Old->eraseFromParent();

  EXPECT_EQ(query(V, Cache, Info), JavaType(22, true));
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_EQ(query(F.getArg(0), Cache, Info), JavaType(9, true));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(CachedJavaTypeV2Test, CalleeAttributeMutationUsesConservativeFallback) {
  auto M = parse(R"IR(
    declare ptr addrspace(1) @identity(ptr addrspace(1) returned)
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a) {
      %result = call ptr addrspace(1) @identity(ptr addrspace(1) %a)
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Function *Callee = M->getFunction("identity");
  Value *V = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(V, Cache, Info), JavaType(9, true));

  auto Invalidated = Cache.invalidate({Callee});
  EXPECT_TRUE(Invalidated.FullClear);
  EXPECT_EQ(Cache.baseSize(), 0U);
  Callee->removeParamAttr(0, Attribute::Returned);

  EXPECT_TRUE(query(V, Cache, Info).isUnknown());
  EXPECT_EQ(query(V, Cache, Info), fresh(V));
  EXPECT_FALSE(verifyModule(*M, &errs()));
}

TEST_F(CachedJavaTypeV2Test, DiagnosticsDistinguishSharedInputsFromRootHits) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" %a, i1 %pick) {
      %shared = select i1 %pick, ptr addrspace(1) %a,
                                ptr addrspace(1) null
      %first = getelementptr i8, ptr addrspace(1) %shared, i64 0
      %second = getelementptr i8, ptr addrspace(1) %shared, i64 0
      ret ptr addrspace(1) %second
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Value *First = findV2BaseTestInstruction(F, "first");
  Value *Second = returnedValue(F);
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  JavaTypeCacheV2Diagnostics D;
  EXPECT_EQ(getCachedJavaTypeV2(First, nullptr, nullptr, noNullEdge, Cache,
                                Info, &D),
            fresh(First));
  EXPECT_EQ(Info.BaseMisses, 1U);
  EXPECT_EQ(D.NodesEvaluated, 3U);
  EXPECT_EQ(D.InputMisses, 2U);
  EXPECT_EQ(D.PriorQueryHits, 0U);
  EXPECT_EQ(D.EntriesAdded, 3U);

  // A different root still reuses its completed shared dependency.
  EXPECT_EQ(getCachedJavaTypeV2(Second, nullptr, nullptr, noNullEdge, Cache,
                                Info, &D),
            fresh(Second));
  EXPECT_EQ(Info.BaseHits, 0U);
  EXPECT_EQ(Info.BaseMisses, 1U);
  EXPECT_EQ(D.InputHits, 1U);
  EXPECT_EQ(D.InputMisses, 0U);
  EXPECT_EQ(D.PriorQueryHits, 1U);
  EXPECT_EQ(D.CurrentQueryHits, 0U);
  EXPECT_EQ(D.NodesEvaluated, 1U);
  EXPECT_EQ(D.EntriesAdded, 1U);

  // Contextual queries must expose Base reuse too. Non-exact base forces the
  // value dimension to walk the forwarders, but none needs Base evaluation.
  DominatorTree DT(F);
  EXPECT_EQ(getCachedJavaTypeV2(Second, &DT, F.back().getTerminator(),
                                noNullEdge, Cache, Info, &D),
            getJavaType(Second, &DT, F.back().getTerminator(), noNullEdge));
  EXPECT_EQ(Info.BaseHits, 3U);
  EXPECT_EQ(Info.BaseMisses, 0U);
  EXPECT_EQ(D.PriorQueryHits, 3U);
  EXPECT_EQ(D.NodesEvaluated, 0U);
  EXPECT_EQ(D.EntriesAdded, 0U);

  JavaTypeCacheV2 Cold;
  EXPECT_EQ(getCachedJavaTypeV2(Second, &DT, F.back().getTerminator(),
                                noNullEdge, Cold, Info, &D),
            getJavaType(Second, &DT, F.back().getTerminator(), noNullEdge));
  EXPECT_EQ(Info.BaseMisses, 1U);
  EXPECT_EQ(Info.BaseHits, 2U);
  EXPECT_EQ(D.PriorQueryHits, 0U);
  EXPECT_EQ(D.CurrentQueryHits, 2U);
  EXPECT_EQ(D.NodesEvaluated, 3U);
}

TEST_F(CachedJavaTypeV2Test, DiagnosticCycleEdgesAreNotCountedAsCacheMisses) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" %a, i1 %again) {
    entry:
      br label %loop
    loop:
      %result = phi ptr addrspace(1) [ %a, %entry ], [ %result, %loop ]
      br i1 %again, label %loop, label %exit
    exit:
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Value *V = returnedValue(*M->getFunction("test"));
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  JavaTypeCacheV2Diagnostics D;
  EXPECT_EQ(getCachedJavaTypeV2(V, nullptr, nullptr, noNullEdge, Cache, Info, &D),
            fresh(V));
  EXPECT_EQ(D.InputEdgesVisited, D.InputHits + D.InputMisses + D.OpenNodeEdges);
  EXPECT_EQ(D.NodesEvaluated, 2U);
  EXPECT_EQ(D.InputMisses, 1U);
  EXPECT_EQ(D.OpenNodeEdges, 1U);
  EXPECT_EQ(D.CyclicSCCs, 1U);
  EXPECT_GE(D.RefinementSteps, 1U);
  EXPECT_EQ(D.PriorQueryHits, 0U);

  EXPECT_EQ(getCachedJavaTypeV2(V, nullptr, nullptr, noNullEdge, Cache, Info, &D),
            fresh(V));
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_EQ(D.PriorQueryHits, 1U);
  EXPECT_EQ(D.NodesEvaluated, 0U);
  EXPECT_EQ(D.CyclicSCCs, 0U);
  EXPECT_EQ(D.RefinementSteps, 0U);
}

TEST_F(CachedJavaTypeV2Test, UnknownIsAReusableCompletedResult) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(ptr addrspace(1) %a, i1 %pick) {
      %result = select i1 %pick, ptr addrspace(1) %a,
                                ptr addrspace(1) null
      ret ptr addrspace(1) %result
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Value *V = returnedValue(*M->getFunction("test"));
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  JavaTypeCacheV2Diagnostics D;
  EXPECT_TRUE(query(V, Cache, Info).isUnknown());
  EXPECT_TRUE(getCachedJavaTypeV2(V, nullptr, nullptr, noNullEdge, Cache, Info, &D)
                  .isUnknown());
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_EQ(D.PriorQueryHits, 1U);
  EXPECT_EQ(D.NodesEvaluated, 0U);
}

TEST_F(CachedJavaTypeV2Test, OversizedInvalidationFallsBackToClear) {
  auto M = parse(R"IR(
    define ptr addrspace(1) @test(
        ptr addrspace(1) "java-klass"="9" "java-klass-exact" %a,
        i32 %n) {
      ret ptr addrspace(1) %a
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  Instruction *Return = F.front().getTerminator();
  Value *Previous = F.getArg(1);
  for (unsigned I = 0; I < 4100; ++I)
    Previous = BinaryOperator::CreateAdd(
        Previous, ConstantInt::get(Type::getInt32Ty(Context), 1), "",
        Return->getIterator());
  JavaTypeCacheV2 Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(query(F.getArg(0), Cache, Info), JavaType(9, true));
  auto Invalidated = Cache.invalidate({F.getArg(1)});
  EXPECT_TRUE(Invalidated.FullClear);
  EXPECT_EQ(Invalidated.BaseRemoved, 1U);
  EXPECT_EQ(Cache.baseSize(), 0U);
  EXPECT_EQ(query(F.getArg(0), Cache, Info), fresh(F.getArg(0)));
}

static BasicBlock *findV2FactsTestBlock(Function &F, StringRef Name) {
  for (BasicBlock &BB : F)
    if (BB.getName() == Name)
      return &BB;
  llvm_unreachable("missing facts test block");
}

using FactsCacheMode = JavaTypeFactsCacheV2::Mode;

TEST_F(CachedJavaTypeV2Test, FactsEdgeModesReuseSharedEdgesAndSeparateValues) {
  auto M = parse(R"IR(
    declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
    define ptr @test(ptr %obj, ptr %other) {
    entry:
      %check = call i1 @jeandle.check_instanceof(
          ptr inttoptr(i64 9 to ptr), ptr %obj)
      br i1 %check, label %a, label %reject
    a:
      br label %b
    b:
      br label %c
    c:
      ret ptr %obj
    reject:
      ret ptr null
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  DominatorTree DT(F);
  Instruction *AtB = findV2FactsTestBlock(F, "b")->getTerminator();
  Instruction *AtC = findV2FactsTestBlock(F, "c")->getTerminator();
  for (FactsCacheMode Mode : {FactsCacheMode::GuardOnly,
                             FactsCacheMode::StableOracleEpoch}) {
    SCOPED_TRACE(Mode == FactsCacheMode::GuardOnly ? "guard" : "stable");
    JavaTypeCacheV2 Base;
    JavaTypeFactsCacheV2 Facts(F, Mode);
    AssumptionCache AC(F);
    LazyValueInfo LVI(&AC, &M->getDataLayout());
    unsigned ActualCalls = 0, ReferenceCalls = 0;
    auto Oracle = [&](Value *V, BasicBlock *From, BasicBlock *To) {
      ++ActualCalls;
      return LVINullEdgeOracle{LVI}(V, From, To);
    };
    auto Compare = [&](Value *V, Instruction *At) {
      // Isolate the persistent edge layer from independent Base reuse.
      Base.clear();
      CachedJavaTypeQueryInfo Info;
      JavaType Actual = getCachedJavaTypeV2(V, &DT, At, Oracle, Base, Info,
                                            nullptr, &Facts);
      AssumptionCache ReferenceAC(F);
      LazyValueInfo ReferenceLVI(&ReferenceAC, &M->getDataLayout());
      auto ReferenceOracle = [&](Value *X, BasicBlock *From, BasicBlock *To) {
        ++ReferenceCalls;
        return LVINullEdgeOracle{ReferenceLVI}(X, From, To);
      };
      EXPECT_EQ(Actual, getJavaType(V, &DT, At, ReferenceOracle));
      return Actual;
    };
    EXPECT_EQ(Compare(F.getArg(0), AtB), JavaType(9, false));
    EXPECT_EQ(ActualCalls, 2U);
    EXPECT_EQ(Compare(F.getArg(0), AtC), JavaType(9, false));
    EXPECT_EQ(ActualCalls, Mode == FactsCacheMode::GuardOnly ? 5U : 3U);
    EXPECT_EQ(Compare(F.getArg(0), AtC), JavaType(9, false));
    EXPECT_EQ(ActualCalls, Mode == FactsCacheMode::GuardOnly ? 8U : 3U);
    EXPECT_EQ(ReferenceCalls, 8U);
    EXPECT_EQ(Facts.getStats().GuardHits, 5U);

    // Same edges but a different raw Value must not inherit obj's guard.
    EXPECT_TRUE(Compare(F.getArg(1), AtC).isUnknown());
    EXPECT_EQ(ActualCalls, Mode == FactsCacheMode::GuardOnly ? 11U : 6U);
    EXPECT_EQ(ReferenceCalls, 11U);
    EXPECT_EQ(Facts.guardSize(), 6U);
    EXPECT_EQ(Facts.nullSize(), Mode == FactsCacheMode::GuardOnly ? 0U : 6U);
    EXPECT_EQ(Facts.getStats().NullRequests, 11U);
    EXPECT_EQ(Facts.getStats().NullHits,
              Mode == FactsCacheMode::GuardOnly ? 0U : 5U);
    EXPECT_EQ(Facts.getStats().NullComputed, ActualCalls);
    EXPECT_EQ(Facts.getStats().GuardRequests, 11U);
    EXPECT_EQ(Facts.getStats().GuardComputed, 6U);
    EXPECT_EQ(Facts.getStats().GuardBypasses, 0U);
    Facts.clear();
    EXPECT_EQ(Facts.guardSize(), 0U);
    EXPECT_EQ(Facts.nullSize(), 0U);
    EXPECT_EQ(Facts.getStats().GuardHits, 5U);
    Facts.resetStats();
    EXPECT_EQ(Facts.getStats().GuardRequests, 0U);
    EXPECT_EQ(Facts.getStats().NullRequests, 0U);
  }
}

TEST_F(CachedJavaTypeV2Test, FactsEdgesPreserveBudgetPhiCyclesAndSwitchResults) {
  auto M = parse(R"IR(
    declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
    define ptr @chain(ptr %obj) {
    entry:
      %check = call i1 @jeandle.check_instanceof(
          ptr inttoptr(i64 9 to ptr), ptr %obj)
      br i1 %check, label %a, label %reject
    a:
      br label %b
    b:
      br label %exit
    exit:
      ret ptr %obj
    reject:
      ret ptr null
    }
    define ptr @cycle(ptr "java-klass"="9" %left,
                      ptr "java-klass"="22" %right, i1 %again) {
    entry:
      br label %loop
    loop:
      %a = phi ptr [ %left, %entry ], [ %b, %loop ]
      %b = phi ptr [ %right, %entry ], [ %a, %loop ]
      br i1 %again, label %loop, label %exit
    exit:
      ret ptr %b
    }
    define ptr @irreducible(ptr %obj, i1 %pick, i1 %again) {
    entry:
      br i1 %pick, label %a, label %b
    a:
      %check = call i1 @jeandle.check_instanceof(
          ptr inttoptr(i64 9 to ptr), ptr %obj)
      br i1 %check, label %b, label %exit
    b:
      br i1 %again, label %a, label %exit
    exit:
      ret ptr %obj
    }
    define ptr @switchguard(ptr %obj) {
    entry:
      %check = call i1 @jeandle.check_instanceof(
          ptr inttoptr(i64 9 to ptr), ptr %obj)
      %wide = zext i1 %check to i32
      switch i32 %wide, label %reject [ i32 1, label %good
                                        i32 0, label %reject ]
    good:
      br label %exit
    reject:
      ret ptr null
    exit:
      ret ptr %obj
    }
  )IR");
  ASSERT_NE(M, nullptr);
  auto *Option =
      cl::getRegisteredOptions().lookup("jeandle-max-facts-join-blocks");
  ASSERT_NE(Option, nullptr);
  auto &Budget = *static_cast<cl::opt<unsigned> *>(Option);
  unsigned SavedBudget = Budget;
  auto RestoreBudget = make_scope_exit([&] { Budget = SavedBudget; });
  for (StringRef Name : {"chain", "cycle", "irreducible", "switchguard"}) {
    SCOPED_TRACE(Name.str());
    Function &F = *M->getFunction(Name);
    DominatorTree DT(F);
    if (Name == "irreducible") {
      EXPECT_FALSE(DT.dominates(findV2FactsTestBlock(F, "a"),
                                findV2FactsTestBlock(F, "b")));
      EXPECT_FALSE(DT.dominates(findV2FactsTestBlock(F, "b"),
                                findV2FactsTestBlock(F, "a")));
    }
    SmallVector<Value *, 3> Values{F.getArg(0)};
    if (Name == "cycle") {
      Values.push_back(findV2BaseTestInstruction(F, "a"));
      Values.push_back(findV2BaseTestInstruction(F, "b"));
    }
    for (FactsCacheMode Mode : {FactsCacheMode::GuardOnly,
                               FactsCacheMode::StableOracleEpoch}) {
      SCOPED_TRACE(Mode == FactsCacheMode::GuardOnly ? "guard" : "stable");
      JavaTypeCacheV2 Base;
      JavaTypeFactsCacheV2 Facts(F, Mode);
      // Complete warm entries must not evade a later query's budget or its
      // PHI/irreducible-cycle traversal decisions.
      for (unsigned Limit : {128U, 1U, 2U, 0U, 128U, 1U}) {
        SCOPED_TRACE(Limit);
        Budget = Limit;
        for (Value *V : Values) {
          Base.clear();
          CachedJavaTypeQueryInfo Info;
          Instruction *At = findV2FactsTestBlock(F, "exit")->getTerminator();
          JavaType Actual = getCachedJavaTypeV2(V, &DT, At, noNullEdge, Base,
                                                Info, nullptr, &Facts);
          EXPECT_EQ(Actual, getJavaType(V, &DT, At, noNullEdge));
          if (Name == "chain")
            EXPECT_EQ(Actual.Klass, Limit >= 3 ? 9U : 0U);
        }
      }
      EXPECT_GT(Facts.getStats().GuardHits, 0U);
      if (Mode == FactsCacheMode::StableOracleEpoch)
        EXPECT_GT(Facts.getStats().NullHits, 0U);
      else
        EXPECT_EQ(Facts.nullSize(), 0U);
    }
  }
}

TEST_F(CachedJavaTypeV2Test, FactsDefaultFollowsOracleChangesAndStableModeClears) {
  auto M = parse(R"IR(
    declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
    define ptr @test(ptr %arg, i1 %choose_null) {
    entry:
      %obj = select i1 %choose_null, ptr null, ptr %arg
      br i1 %choose_null, label %nullbb, label %checkbb
    nullbb:
      br label %join
    checkbb:
      %check = call i1 @jeandle.check_instanceof(
          ptr inttoptr(i64 9 to ptr), ptr %obj)
      br i1 %check, label %good, label %reject
    good:
      br label %join
    join:
      ret ptr %obj
    reject:
      ret ptr null
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  DominatorTree DT(F);
  Value *Obj = findV2BaseTestInstruction(F, "obj");
  BasicBlock *NullBB = findV2FactsTestBlock(F, "nullbb");
  BasicBlock *Join = findV2FactsTestBlock(F, "join");
  bool Enhanced = false;
  auto Oracle = [&](Value *V, BasicBlock *From, BasicBlock *To) {
    // Both versions are sound. Only the enhanced version proves the null arm.
    return Enhanced && V == Obj && From == NullBB && To == Join;
  };
  auto Compare = [&](JavaTypeFactsCacheV2 &Facts) {
    JavaTypeCacheV2 Base;
    CachedJavaTypeQueryInfo Info;
    JavaType Actual = getCachedJavaTypeV2(Obj, &DT, Join->getTerminator(),
                                          Oracle, Base, Info, nullptr, &Facts);
    EXPECT_EQ(Actual,
              getJavaType(Obj, &DT, Join->getTerminator(), Oracle));
    return Actual;
  };
  // The default must follow changes in a sound oracle without an epoch reset.
  JavaTypeFactsCacheV2 DefaultFacts(F);
  EXPECT_TRUE(Compare(DefaultFacts).isUnknown());
  Enhanced = true;
  EXPECT_EQ(Compare(DefaultFacts), JavaType(9, false));
  EXPECT_EQ(DefaultFacts.nullSize(), 0U);
  EXPECT_GT(DefaultFacts.getStats().GuardHits, 0U);

  // Explicit bool memoization instead requires a new epoch when knowledge
  // changes, even though every instruction and CFG edge is unchanged.
  Enhanced = false;
  JavaTypeFactsCacheV2 StableFacts(F, FactsCacheMode::StableOracleEpoch);
  EXPECT_TRUE(Compare(StableFacts).isUnknown());
  EXPECT_GT(StableFacts.nullSize(), 0U);
  StableFacts.clear();
  Enhanced = true;
  EXPECT_EQ(Compare(StableFacts), JavaType(9, false));
}

TEST_F(CachedJavaTypeV2Test, FactsClearRefreshesGuardsAndAddedCFGEdges) {
  for (FactsCacheMode Mode : {FactsCacheMode::GuardOnly,
                             FactsCacheMode::StableOracleEpoch}) {
    SCOPED_TRACE(Mode == FactsCacheMode::GuardOnly ? "guard" : "stable");
    auto M = parse(R"IR(
      declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
      define ptr @test(ptr %obj) {
      entry:
        %check = call i1 @jeandle.check_instanceof(
            ptr inttoptr(i64 9 to ptr), ptr %obj)
        br i1 %check, label %good, label %reject
      good:
        br label %exit
      exit:
        ret ptr %obj
      reject:
        ret ptr null
      }
    )IR");
    ASSERT_NE(M, nullptr);
    Function &F = *M->getFunction("test");
    DominatorTree DT(F);
    JavaTypeFactsCacheV2 Facts(F, Mode);
    auto Compare = [&] {
      JavaTypeCacheV2 Base;
      CachedJavaTypeQueryInfo Info;
      Instruction *At = findV2FactsTestBlock(F, "exit")->getTerminator();
      JavaType Actual = getCachedJavaTypeV2(F.getArg(0), &DT, At, noNullEdge,
                                            Base, Info, nullptr, &Facts);
      EXPECT_EQ(Actual, getJavaType(F.getArg(0), &DT, At, noNullEdge));
      return Actual;
    };
    EXPECT_EQ(Compare(), JavaType(9, false));
    Facts.clear();
    auto *Check = cast<CallInst>(findV2BaseTestInstruction(F, "check"));
    Check->setArgOperand(
        0, ConstantExpr::getIntToPtr(
               ConstantInt::get(Type::getInt64Ty(Context), 22),
               PointerType::get(Context, 0)));
    EXPECT_EQ(Compare(), JavaType(22, false));
    EXPECT_EQ(Compare(), JavaType(22, false));

    // A new incoming path bypasses the successful check. All edge entries,
    // including negative facts, must be rebuilt against the updated CFG/DT.
    Facts.clear();
    Instruction *Old = findV2FactsTestBlock(F, "reject")->getTerminator();
    BranchInst::Create(findV2FactsTestBlock(F, "exit"), Old->getIterator());
    Old->eraseFromParent();
    DT.recalculate(F);
    EXPECT_FALSE(verifyModule(*M, &errs()));
    EXPECT_TRUE(Compare().isUnknown());
    EXPECT_TRUE(Compare().isUnknown());
  }
}

TEST_F(CachedJavaTypeV2Test, FactsClearBeforeRAUWAndDeletionDropsRawKeys) {
  for (FactsCacheMode Mode : {FactsCacheMode::GuardOnly,
                             FactsCacheMode::StableOracleEpoch}) {
    SCOPED_TRACE(Mode == FactsCacheMode::GuardOnly ? "guard" : "stable");
    auto M = parse(R"IR(
      declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
      define ptr @test(ptr %obj, ptr %replacement) {
      entry:
        %old = select i1 true, ptr %obj, ptr %obj
        %check = call i1 @jeandle.check_instanceof(
            ptr inttoptr(i64 9 to ptr), ptr %old)
        br i1 %check, label %good, label %reject
      good:
        ret ptr %old
      reject:
        ret ptr null
      }
    )IR");
    ASSERT_NE(M, nullptr);
    Function &F = *M->getFunction("test");
    DominatorTree DT(F);
    JavaTypeCacheV2 Base;
    JavaTypeFactsCacheV2 Facts(F, Mode);
    Instruction *At = findV2FactsTestBlock(F, "good")->getTerminator();
    auto Compare = [&](Value *V) {
      CachedJavaTypeQueryInfo Info;
      JavaType Actual = getCachedJavaTypeV2(V, &DT, At, noNullEdge, Base, Info,
                                            nullptr, &Facts);
      EXPECT_EQ(Actual, getJavaType(V, &DT, At, noNullEdge));
      return Actual;
    };
    Instruction *Old = findV2BaseTestInstruction(F, "old");
    EXPECT_EQ(Compare(Old), JavaType(9, false));
    EXPECT_GT(Facts.guardSize(), 0U);
    Facts.clear();
    Base.invalidate({Old});
    EXPECT_EQ(Facts.guardSize(), 0U);
    EXPECT_EQ(Facts.nullSize(), 0U);
    Old->replaceAllUsesWith(F.getArg(1));
    Old->eraseFromParent();
    EXPECT_FALSE(verifyModule(*M, &errs()));
    EXPECT_TRUE(Compare(F.getArg(0)).isUnknown());
    EXPECT_EQ(Compare(F.getArg(1)), JavaType(9, false));
  }
}

TEST_F(CachedJavaTypeV2Test, FactsClearRefreshesMovedDefinitionContext) {
  for (FactsCacheMode Mode : {FactsCacheMode::GuardOnly,
                             FactsCacheMode::StableOracleEpoch}) {
    SCOPED_TRACE(Mode == FactsCacheMode::GuardOnly ? "guard" : "stable");
    auto M = parse(R"IR(
      declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
      define ptr @test(ptr %obj) {
      entry:
        %value = select i1 true, ptr %obj, ptr %obj
        %check = call i1 @jeandle.check_instanceof(
            ptr inttoptr(i64 9 to ptr), ptr %obj)
        br i1 %check, label %good, label %reject
      good:
        br label %exit
      exit:
        ret ptr %value
      reject:
        ret ptr null
      }
    )IR");
    ASSERT_NE(M, nullptr);
    Function &F = *M->getFunction("test");
    DominatorTree DT(F);
    JavaTypeCacheV2 Base;
    JavaTypeFactsCacheV2 Facts(F, Mode);
    Instruction *V = findV2BaseTestInstruction(F, "value");
    auto Compare = [&] {
      CachedJavaTypeQueryInfo Info;
      Instruction *At = findV2FactsTestBlock(F, "exit")->getTerminator();
      JavaType Actual = getCachedJavaTypeV2(V, &DT, At, noNullEdge, Base, Info,
                                            nullptr, &Facts);
      EXPECT_EQ(Actual, getJavaType(V, &DT, At, noNullEdge));
      return Actual;
    };
    Compare();
    Facts.clear();
    // Moving an instruction leaves its Base inputs intact, but changes the
    // definition-point path facts used by the value dimension.
    V->moveBefore(
        findV2FactsTestBlock(F, "good")->getTerminator()->getIterator());
    DT.recalculate(F);
    EXPECT_FALSE(verifyModule(*M, &errs()));
    EXPECT_EQ(Compare(), JavaType(9, false));
    EXPECT_EQ(Compare(), JavaType(9, false));
  }
}

TEST_F(CachedJavaTypeV2Test, FactsNullMemoRetainsRawPhiIncomingIdentity) {
  auto M = parse(R"IR(
    define ptr @test(ptr "java-klass"="9" %typed, i1 %pick) {
    entry:
      %first = getelementptr i8, ptr null, i64 0
      %second = getelementptr i8, ptr null, i64 0
      br i1 %pick, label %nullpath, label %typedpath
    nullpath:
      br label %join
    typedpath:
      br label %join
    join:
      %p = phi ptr [ %first, %nullpath ], [ %typed, %typedpath ]
      %q = phi ptr [ %second, %nullpath ], [ %typed, %typedpath ]
      ret ptr %q
    }
  )IR");
  ASSERT_NE(M, nullptr);
  Function &F = *M->getFunction("test");
  DominatorTree DT(F);
  Value *First = findV2BaseTestInstruction(F, "first");
  Value *Second = findV2BaseTestInstruction(F, "second");
  ASSERT_EQ(First->stripPointerCasts(), Second->stripPointerCasts());
  BasicBlock *From = findV2FactsTestBlock(F, "nullpath");
  BasicBlock *To = findV2FactsTestBlock(F, "join");
  unsigned FirstCalls = 0, SecondCalls = 0;
  auto StableOracle = [&](Value *V, BasicBlock *A, BasicBlock *B) {
    // The first wrapper has a positive proof; the second returns unknown.
    // Canonicalizing the memo key would incorrectly reuse one response for
    // the other, even though this oracle is stable and sound for both.
    return V == First && A == From && B == To;
  };
  auto CountedOracle = [&](Value *V, BasicBlock *A, BasicBlock *B) {
    if (A == From && B == To) {
      FirstCalls += V == First;
      SecondCalls += V == Second;
    }
    return StableOracle(V, A, B);
  };
  JavaTypeCacheV2 Base;
  JavaTypeFactsCacheV2 Facts(F, FactsCacheMode::StableOracleEpoch);
  for (StringRef Name : {"p", "q", "p", "q"}) {
    Value *V = findV2BaseTestInstruction(F, Name);
    Base.clear();
    CachedJavaTypeQueryInfo Info;
    EXPECT_EQ(getCachedJavaTypeV2(V, &DT, To->getTerminator(), CountedOracle,
                                  Base, Info, nullptr, &Facts),
              getJavaType(V, &DT, To->getTerminator(), StableOracle));
  }
  EXPECT_EQ(FirstCalls, 1U);
  EXPECT_EQ(SecondCalls, 1U);
}

} // namespace
