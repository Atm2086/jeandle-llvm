//===- JavaTypeQueryTest.cpp - Java 类型共享缓存测试 ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/JavaTypeAnalysis.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::jeandle;

namespace {

// 单个测试临时调整查询选项，结束后恢复，不影响其他测试。
template <typename T> class ScopedQueryOption {
  cl::opt<T> &Option;
  T Saved;

public:
  ScopedQueryOption(StringRef Name, T Value)
      : Option(*static_cast<cl::opt<T> *>(cl::getRegisteredOptions().lookup(Name))),
        Saved(Option.getValue()) {
    Option = Value;
  }
  ~ScopedQueryOption() { Option = Saved; }
};

class JavaTypeQueryTest : public testing::Test {
protected:
  LLVMContext C;
  std::unique_ptr<Module> M;
  VMCallbacks Saved{};

  void SetUp() override {
    if (const auto *CB = getVMCallbacks())
      Saved = *CB;
    VMCallbacks CB{};
    CB.IsSubtype = [](uintptr_t A, uintptr_t B) { return A == B || B == 1; };
    CB.IsInterface = [](uintptr_t) { return false; };
    CB.IsEffectivelyFinal = [](uintptr_t) { return false; };
    CB.GetSecondarySupers = [](uintptr_t) { return std::vector<uintptr_t>{}; };
    CB.GetCommonSuperKlass = [](uintptr_t A, uintptr_t B) {
      return A == B ? A : uintptr_t(1);
    };
    registerVMCallbacks(CB);
  }

  void TearDown() override { registerVMCallbacks(Saved); }

  Function *parse(StringRef IR) {
    SMDiagnostic Error;
    M = parseAssemblyString(IR, Error, C);
    return M ? M->getFunction("test") : nullptr;
  }

  BasicBlock *block(Function &F, StringRef Name) {
    for (BasicBlock &BB : F)
      if (BB.getName() == Name)
        return &BB;
    return nullptr;
  }

  std::string statistics(const JavaTypeAnalysisResult &Result) {
    std::string Text;
    raw_string_ostream OS(Text);
    Result.print(OS);
    return Text;
  }

  // 两个查询位置共享 guard -> hit 的类型事实。
  static constexpr const char *IR = R"(
    declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
    define void @test(ptr %obj, i1 %unrelated) {
    entry:
      br label %guard
    guard:
      %check = call i1 @jeandle.check_instanceof(ptr inttoptr (i64 22 to ptr), ptr %obj)
      br i1 %check, label %hit, label %miss
    hit:
      br label %tail
    tail:
      ret void
    miss:
      ret void
    }
  )";
};

TEST_F(JavaTypeQueryTest, SharedBaseTypes) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %obj) {
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  Value *V = F->getArg(0);
  EXPECT_EQ(jeandle::getCachedJavaType(V, &DT, nullptr, nullptr, Cache, Info).Klass,
            22U);
  EXPECT_EQ(Info.BaseMisses, 1U);
  EXPECT_EQ(Cache.baseSize(), 1U);
  EXPECT_EQ(jeandle::getCachedJavaType(V, &DT, nullptr, nullptr, Cache, Info).Klass,
            22U);
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_EQ(Info.BaseMisses, 0U); // Observations reset on each call.
  EXPECT_EQ(Cache.baseSize(), 1U);
}

TEST_F(JavaTypeQueryTest, CachedEntryNormalizesFreeze) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %obj) {
      %wrapped = freeze ptr %obj
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  Value *Wrapped = &F->getEntryBlock().front();
  Instruction *Context = F->getEntryBlock().getTerminator();
  JavaType Expected = jeandle::getJavaType(Wrapped, &DT, Context, nullptr);
  EXPECT_EQ(Expected.Klass, 22U);
  EXPECT_EQ(jeandle::getCachedJavaType(Wrapped, &DT, Context, nullptr, Cache, Info),
            Expected);
  EXPECT_EQ(jeandle::getCachedJavaType(F->getArg(0), &DT, Context, nullptr, Cache, Info),
            Expected);
  EXPECT_EQ(Info.FinalHits, 1U);
  EXPECT_EQ(Cache.finalTypesSize(), 1U);
  EXPECT_EQ(Cache.baseSize(), 1U);
}

TEST_F(JavaTypeQueryTest, SharedFactsAcrossContexts) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  Value *V = F->getArg(0);
  EXPECT_EQ(jeandle::getCachedJavaType(V, &DT, block(*F, "hit")->getTerminator(),
      nullptr, Cache, Info).Klass, 22U);
  EXPECT_GT(Cache.pathFactsSize(), 0U);

  Instruction *Tail = block(*F, "tail")->getTerminator();
  JavaType Shared = jeandle::getCachedJavaType(V, &DT, Tail, nullptr, Cache, Info);
  EXPECT_EQ(Shared.Klass, 22U);
  EXPECT_EQ(Shared, jeandle::getJavaType(V, &DT, Tail, nullptr));
  EXPECT_GT(Info.FactsHits, 0U);
  EXPECT_FALSE(Info.SkipVerification);
}

TEST_F(JavaTypeQueryTest, CompleteUnknownIsShared) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  Value *V = F->getArg(0);
  EXPECT_TRUE(jeandle::getCachedJavaType(V, &DT, block(*F, "guard")->getTerminator(),
      nullptr, Cache, Info).isUnknown());
  EXPECT_EQ(Cache.pathFactsSize(), 1U);
  // Expanding hit spends the sole block budget; guard's unknown must be reused.
  ScopedQueryOption<unsigned> Budget("jeandle-max-facts-join-blocks", 1);
  EXPECT_EQ(jeandle::getCachedJavaType(V, &DT, block(*F, "hit")->getTerminator(),
      nullptr, Cache, Info).Klass, 22U);
  EXPECT_EQ(Info.FactsHits, 1U);
  EXPECT_FALSE(Info.SkipVerification);
}

TEST_F(JavaTypeQueryTest, BudgetCutDoesNotPublishParent) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  Value *V = F->getArg(0);
  Instruction *Tail = block(*F, "tail")->getTerminator();
  {
    ScopedQueryOption<unsigned> Budget("jeandle-max-facts-join-blocks", 1);
    EXPECT_TRUE(jeandle::getCachedJavaType(V, &DT, Tail,
        nullptr, Cache, Info).isUnknown());
    EXPECT_TRUE(Info.SkipVerification);
    EXPECT_EQ(Cache.pathFactsSize(), 0U);
    EXPECT_EQ(Cache.finalTypesSize(), 0U);
  }
  EXPECT_EQ(jeandle::getCachedJavaType(V, &DT, Tail, nullptr, Cache, Info).Klass, 22U);
  EXPECT_FALSE(Info.SkipVerification);
}

TEST_F(JavaTypeQueryTest, IrreducibleCycleStaysLocal) {
  Function *F = parse(R"(
    define void @test(ptr %obj, i1 %c) {
    entry:
      br i1 %c, label %a, label %b
    a:
      br label %b
    b:
      br label %a
    }
  )");
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  Value *V = F->getArg(0);
  EXPECT_TRUE(jeandle::getCachedJavaType(V, &DT, block(*F, "a")->getTerminator(),
      nullptr, Cache, Info).isUnknown());
  EXPECT_TRUE(Info.SkipVerification);
  EXPECT_EQ(Cache.pathFactsSize(), 0U);
  EXPECT_EQ(Cache.finalTypesSize(), 0U);

  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  EXPECT_TRUE(Result.getJavaType(V, block(*F, "a")->getTerminator()).isUnknown());
  EXPECT_NE(statistics(Result).find("current final type entries: 0"),
            std::string::npos);
}

TEST_F(JavaTypeQueryTest, ResultInvalidationAfterConditionAndAttributeChange) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  Value *V = F->getArg(0);
  Instruction *Context = block(*F, "tail")->getTerminator();
  EXPECT_EQ(Result.getJavaType(V, Context).Klass, 22U);
  EXPECT_EQ(Result.getJavaType(V, Context).Klass, 22U);
  EXPECT_TRUE(Result.getJavaType(V).isUnknown());

  // 缓存键 V 未变，只改分支条件；细粒度入口当前必须回退为全量清空。
  auto *Branch = cast<BranchInst>(block(*F, "guard")->getTerminator());
  Branch->setCondition(F->getArg(1));
  Result.invalidate(*Branch);
  EXPECT_NE(statistics(Result).find("current final type entries: 0"),
            std::string::npos);
  FAM.getResult<LazyValueAnalysis>(*F).clear();
  EXPECT_TRUE(Result.getJavaType(V, Context).isUnknown());

  F->addParamAttr(0, Attribute::get(C, "java-klass", "22"));
  Result.clear();
  EXPECT_EQ(Result.getJavaType(V).Klass, 22U);
}

TEST_F(JavaTypeQueryTest, FinalTypesAreKeyedByContextBlock) {
  ScopedQueryOption<bool> Verify("jeandle-verify-java-type-cache", true);
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  Value *V = F->getArg(0);
  BasicBlock *Guard = block(*F, "guard");
  // 同一 BB 内不同指令共用最终缓存；完整的 unknown 也可缓存。
  EXPECT_TRUE(Result.getJavaType(V, &Guard->front()).isUnknown());
  EXPECT_TRUE(Result.getJavaType(V, Guard->getTerminator()).isUnknown());
  EXPECT_EQ(Result.getJavaType(V, block(*F, "hit")->getTerminator()).Klass, 22U);
  EXPECT_NE(statistics(Result).find("final type hits: 1"), std::string::npos);
  EXPECT_NE(statistics(Result).find("current final type entries: 2"),
            std::string::npos);
  Result.clear();
  EXPECT_NE(statistics(Result).find("current final type entries: 0"),
            std::string::npos);
  EXPECT_NE(statistics(Result).find("current path facts entries: 0"),
            std::string::npos);
  EXPECT_NE(statistics(Result).find("current base entries: 0"),
            std::string::npos);
}

TEST_F(JavaTypeQueryTest, RecursiveFinalHitStillAppliesIncomingGuard) {
  ScopedQueryOption<bool> Verify("jeandle-verify-java-type-cache", true);
  Function *F = parse(R"(
    declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
    define void @test(ptr "java-klass"="1" %obj) {
    entry:
      %check = call i1 @jeandle.check_instanceof(ptr inttoptr (i64 22 to ptr), ptr %obj)
      br i1 %check, label %hit, label %miss
    hit:
      %p = phi ptr [ %obj, %entry ]
      ret void
    miss:
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  EXPECT_EQ(Result.getJavaType(F->getArg(0), F->getEntryBlock().getTerminator()).Klass,
            1U);
  BasicBlock *Hit = block(*F, "hit");
  EXPECT_EQ(Result.getJavaType(&Hit->front(), Hit->getTerminator()).Klass, 22U);
  EXPECT_NE(statistics(Result).find("final type hits: 1"), std::string::npos);
  EXPECT_NE(statistics(Result).find("current final type entries: 2"),
            std::string::npos);
  EXPECT_NE(statistics(Result).find("verification skips: 0"), std::string::npos);
}

TEST_F(JavaTypeQueryTest, RecursiveFinalHitSavesPhiBudgetAndIsReadOnly) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %obj) {
    entry:
      br label %first
    first:
      %x = phi ptr [ %obj, %entry ]
      br label %second
    second:
      %y = phi ptr [ %x, %first ]
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  BasicBlock *First = block(*F, "first");
  BasicBlock *Second = block(*F, "second");
  Value *X = &First->front();
  Value *Y = &Second->front();
  EXPECT_EQ(jeandle::getCachedJavaType(X, &DT, First->getTerminator(),
      nullptr, Cache, Info).Klass, 22U);
  EXPECT_EQ(Cache.finalTypesSize(), 1U);
  ScopedQueryOption<unsigned> Budget("jeandle-max-facts-join-blocks", 1);
  EXPECT_EQ(jeandle::getCachedJavaType(Y, &DT, Second->getTerminator(),
      nullptr, Cache, Info).Klass, 22U);
  EXPECT_EQ(Info.FinalHits, 1U);
  EXPECT_FALSE(Info.SkipVerification);
  // Only the two top-level queries publish; incoming obj is never published.
  EXPECT_EQ(Cache.finalTypesSize(), 2U);

  JavaTypeCache ColdCache;
  EXPECT_TRUE(jeandle::getCachedJavaType(Y, &DT, Second->getTerminator(),
      nullptr, ColdCache, Info).isUnknown());
  EXPECT_TRUE(Info.SkipVerification);
}

TEST_F(JavaTypeQueryTest, PhiCycleIntermediateIsNotPublished) {
  ScopedQueryOption<bool> Verify("jeandle-verify-java-type-cache", true);
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %cat, ptr "java-klass"="33" %dog) {
    entry:
      br label %loop
    loop:
      %a = phi ptr [ %cat, %entry ], [ %b, %latch ]
      %b = phi ptr [ %dog, %entry ], [ %a, %latch ]
      br label %latch
    latch:
      br label %loop
    }
  )");
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  BasicBlock *Loop = block(*F, "loop");
  Value *A = &Loop->front();
  Value *B = Loop->front().getNextNode();
  Instruction *Context = block(*F, "latch")->getTerminator();
  // 查询 a 时内部 b 的局部贡献是 Dog，但只能发布 a 的完整 Animal 结果。
  EXPECT_EQ(Result.getJavaType(A, Context).Klass, 1U);
  EXPECT_NE(statistics(Result).find("current final type entries: 1"),
            std::string::npos);
  // 独立查询 b 时可读取 a 的完整缓存，不能返回之前递归中的局部 Dog。
  EXPECT_EQ(Result.getJavaType(B, Context).Klass, 1U);
  EXPECT_NE(statistics(Result).find("final type hits: 1"), std::string::npos);
  EXPECT_NE(statistics(Result).find("current final type entries: 2"),
            std::string::npos);
  EXPECT_EQ(Result.getJavaType(B, Context).Klass, 1U);
  EXPECT_NE(statistics(Result).find("verification skips: 3"), std::string::npos);
}

TEST_F(JavaTypeQueryTest, CompletePhiCacheCanImproveCyclePrecision) {
  ScopedQueryOption<bool> Verify("jeandle-verify-java-type-cache", true);
  Function *F = parse(R"(
    declare i1 @jeandle.check_instanceof(ptr, ptr nonnull)
    define void @test(ptr "java-klass"="22" %cat, ptr "java-klass"="33" %dog) {
    entry:
      br label %loop
    loop:
      %a = phi ptr [ %cat, %entry ], [ %b, %latch ]
      %b = phi ptr [ %dog, %entry ], [ %a, %latch ]
      br label %latch
    latch:
      %check = call i1 @jeandle.check_instanceof(ptr inttoptr (i64 22 to ptr), ptr %b)
      br i1 %check, label %loop, label %exit
    exit:
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  BasicBlock *Loop = block(*F, "loop");
  Value *A = &Loop->front();
  Value *B = Loop->front().getNextNode();
  Instruction *Context = block(*F, "latch")->getTerminator();
  // 冷查询中 b 的局部 Dog 贡献不能被不相关的 Cat guard 替换，a 得到 Animal。
  EXPECT_EQ(Result.getJavaType(A, Context).Klass, 1U);
  Result.clear();
  EXPECT_EQ(Result.getJavaType(B, Context).Klass, 1U);
  // 完整 b 缓存是 Animal，可被回边的 Cat guard 收窄，a 得到更精确的 Cat。
  EXPECT_EQ(Result.getJavaType(A, Context).Klass, 22U);
  EXPECT_EQ(Result.getJavaType(A, Context).Klass, 22U);
  EXPECT_NE(statistics(Result).find("verification skips: 4"), std::string::npos);
}

TEST_F(JavaTypeQueryTest, FactsBudgetCutDoesNotPublishFinalType) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  Value *V = F->getArg(0);
  Instruction *Context = block(*F, "tail")->getTerminator();
  {
    ScopedQueryOption<unsigned> Budget("jeandle-max-facts-join-blocks", 1);
    EXPECT_TRUE(Result.getJavaType(V, Context).isUnknown());
    EXPECT_NE(statistics(Result).find("current final type entries: 0"),
              std::string::npos);
  }
  EXPECT_EQ(Result.getJavaType(V, Context).Klass, 22U);
  EXPECT_NE(statistics(Result).find("current final type entries: 1"),
            std::string::npos);
}

TEST_F(JavaTypeQueryTest, PhiBudgetCutDoesNotPublishFinalType) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %obj) {
    entry:
      br label %merge
    merge:
      %p = phi ptr [ %obj, %entry ]
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  BasicBlock *Merge = block(*F, "merge");
  {
    ScopedQueryOption<unsigned> Budget("jeandle-max-facts-join-blocks", 0);
    EXPECT_TRUE(Result.getJavaType(&Merge->front(), Merge->getTerminator()).isUnknown());
    EXPECT_NE(statistics(Result).find("current final type entries: 0"),
              std::string::npos);
  }
  EXPECT_EQ(Result.getJavaType(&Merge->front(), Merge->getTerminator()).Klass, 22U);
  EXPECT_NE(statistics(Result).find("current final type entries: 1"),
            std::string::npos);
}

TEST_F(JavaTypeQueryTest, DisabledCacheBypassesFinalTypes) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  Value *V = F->getArg(0);
  Instruction *Context = block(*F, "hit")->getTerminator();
  EXPECT_EQ(Result.getJavaType(V, Context).Klass, 22U);
  {
    ScopedQueryOption<bool> Disable("jeandle-disable-java-type-cache", true);
    EXPECT_EQ(Result.getJavaType(V, Context).Klass, 22U);
    EXPECT_EQ(Result.getJavaType(V, block(*F, "tail")->getTerminator()).Klass, 22U);
  }
  EXPECT_NE(statistics(Result).find("final type hits: 0"), std::string::npos);
  EXPECT_NE(statistics(Result).find("current final type entries: 1"),
            std::string::npos);
}

TEST_F(JavaTypeQueryTest, DisabledCacheUsesOriginalAfterAttributeChange) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %obj) {
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  Value *V = F->getArg(0);
  Instruction *Context = F->getEntryBlock().getTerminator();
  EXPECT_EQ(Result.getJavaType(V, Context).Klass, 22U);

  // Deliberately leave the cache stale to prove bypass cannot consult it.
  // No cached query is made until clear() below.
  F->removeParamAttr(0, "java-klass");
  F->addParamAttr(0, Attribute::get(C, "java-klass", "33"));
  {
    ScopedQueryOption<bool> Disable("jeandle-disable-java-type-cache", true);
    EXPECT_EQ(Result.getJavaType(V, Context),
              jeandle::getJavaType(V, nullptr, Context, nullptr));
    EXPECT_EQ(Result.getJavaType(V).Klass, 33U);
  }
  EXPECT_NE(statistics(Result).find("final type hits: 0"), std::string::npos);
  EXPECT_NE(statistics(Result).find("current final type entries: 1"),
            std::string::npos);
  Result.clear();
  EXPECT_EQ(Result.getJavaType(V, Context).Klass, 33U);
}

TEST_F(JavaTypeQueryTest, CachedAndOriginalQueriesAgreeAcrossBudgets) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  Value *V = F->getArg(0);
  for (unsigned Limit : {0U, 1U, 128U}) {
    ScopedQueryOption<unsigned> Budget("jeandle-max-facts-join-blocks", Limit);
    for (BasicBlock &BB : *F) {
      SCOPED_TRACE(BB.getName().str() + " budget=" + std::to_string(Limit));
      JavaTypeCache Cache;
      CachedJavaTypeQueryInfo Info;
      Instruction *Context = BB.getTerminator();
      EXPECT_EQ(getCachedJavaType(V, &DT, Context, nullptr, Cache, Info),
                jeandle::getJavaType(V, &DT, Context, nullptr));
    }
  }
}

TEST_F(JavaTypeQueryTest, ValueInvalidationRetainsIndependentQueries) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %a,
                      ptr "java-klass"="33" %b, i1 %c) {
    entry:
      br label %body
    body:
      %derived = select i1 %c, ptr %a, ptr %a
      br label %tail
    tail:
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  Value *Derived = &block(*F, "body")->front();
  Instruction *Context = block(*F, "tail")->getTerminator();
  EXPECT_EQ(getCachedJavaType(Derived, &DT, Context, nullptr, Cache, Info).Klass,
            22U);
  EXPECT_EQ(getCachedJavaType(F->getArg(1), &DT, Context, nullptr, Cache, Info).Klass,
            33U);
  auto Removed = Cache.invalidate({F->getArg(0)});
  EXPECT_FALSE(Removed.FullClear);
  EXPECT_GT(Cache.baseSize(), 0U);
  EXPECT_GT(Cache.pathFactsSize(), 0U);
  EXPECT_GT(Cache.finalTypesSize(), 0U);
  F->removeParamAttr(0, "java-klass");
  F->addParamAttr(0, Attribute::get(C, "java-klass", "44"));
  EXPECT_EQ(getCachedJavaType(F->getArg(1), &DT, Context, nullptr, Cache, Info).Klass,
            33U);
  EXPECT_EQ(Info.FinalHits, 1U);
  EXPECT_EQ(getCachedJavaType(Derived, &DT, Context, nullptr, Cache, Info).Klass,
            44U);
  EXPECT_EQ(getCachedJavaType(Derived, &DT, Context, nullptr, Cache, Info),
            jeandle::getJavaType(Derived, &DT, Context, nullptr));
}

TEST_F(JavaTypeQueryTest, InvalidateBeforeRAUWAndDeletion) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %a,
                      ptr "java-klass"="33" %b, i1 %c) {
      %old = select i1 %c, ptr %a, ptr %a
      %derived = select i1 %c, ptr %old, ptr %old
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  Instruction *Old = &F->getEntryBlock().front();
  Instruction *Derived = Old->getNextNode();
  Instruction *Context = F->getEntryBlock().getTerminator();
  EXPECT_EQ(getCachedJavaType(Old, &DT, Context, nullptr, Cache, Info).Klass, 22U);
  EXPECT_EQ(getCachedJavaType(Derived, &DT, Context, nullptr, Cache, Info).Klass,
            22U);
  Cache.invalidate({Old});
  Old->replaceAllUsesWith(F->getArg(1));
  Old->eraseFromParent();
  EXPECT_FALSE(verifyFunction(*F, &errs()));
  EXPECT_EQ(getCachedJavaType(Derived, &DT, Context, nullptr, Cache, Info).Klass,
            33U);
  EXPECT_EQ(getCachedJavaType(F->getArg(0), &DT, nullptr, nullptr, Cache, Info).Klass,
            22U);
  EXPECT_EQ(Info.BaseHits, 1U);
}

TEST_F(JavaTypeQueryTest, CFGInvalidationRecomputesPhiAndRetainsLeafTypes) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %a,
                      ptr "java-klass"="33" %b, i1 %c) {
    entry:
      br i1 %c, label %left, label %right
    left:
      br label %merge
    right:
      br label %merge
    merge:
      %p = phi ptr [ %a, %left ], [ %b, %right ]
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  auto *PN = cast<PHINode>(&block(*F, "merge")->front());
  EXPECT_EQ(getCachedJavaType(PN, &DT, nullptr, nullptr, Cache, Info).Klass, 1U);
  BasicBlock *Right = block(*F, "right");
  Instruction *OldBranch = F->getEntryBlock().getTerminator();
  Cache.invalidate({OldBranch, Right->getTerminator()}, true);
  BranchInst::Create(block(*F, "left"), OldBranch->getIterator());
  OldBranch->eraseFromParent();
  PN->removeIncomingValue(Right, false);
  Right->eraseFromParent();
  DT.recalculate(*F);
  EXPECT_FALSE(verifyFunction(*F, &errs()));
  EXPECT_EQ(getCachedJavaType(F->getArg(1), &DT, nullptr, nullptr, Cache, Info).Klass,
            33U);
  EXPECT_EQ(Info.BaseHits, 1U);
  EXPECT_EQ(getCachedJavaType(PN, &DT, nullptr, nullptr, Cache, Info).Klass, 22U);
  EXPECT_EQ(getCachedJavaType(PN, &DT, nullptr, nullptr, Cache, Info),
            jeandle::getJavaType(PN, &DT, nullptr, nullptr));
}

TEST_F(JavaTypeQueryTest, GuardOperandChangeInvalidatesNonKeyDependencies) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  Value *V = F->getArg(0);
  Instruction *Context = block(*F, "tail")->getTerminator();
  EXPECT_EQ(Result.getJavaType(V, Context).Klass, 22U);
  auto *Check = cast<CallInst>(&block(*F, "guard")->front());
  Result.invalidate(*Check);
  Check->setArgOperand(0, ConstantExpr::getIntToPtr(
                             ConstantInt::get(Type::getInt64Ty(C), 33),
                             PointerType::get(C, 0)));
  FAM.getResult<LazyValueAnalysis>(*F).clear();
  EXPECT_EQ(Result.getJavaType(V, Context).Klass, 33U);
  EXPECT_NE(statistics(Result).find("base entries retained: 1"),
            std::string::npos);
}

TEST_F(JavaTypeQueryTest, FullInvalidationSwitchDropsAllEntries) {
  ScopedQueryOption<bool> Full("jeandle-java-type-force-full-invalidation", true);
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  EXPECT_EQ(Result.getJavaType(F->getArg(0),
                              block(*F, "tail")->getTerminator()).Klass, 22U);
  Result.invalidate(*block(*F, "guard")->getTerminator());
  EXPECT_NE(statistics(Result).find("current base entries: 0"), std::string::npos);
  EXPECT_NE(statistics(Result).find("current path facts entries: 0"),
            std::string::npos);
  EXPECT_NE(statistics(Result).find("current final type entries: 0"),
            std::string::npos);
  EXPECT_NE(statistics(Result).find("clears: 1"), std::string::npos);
}

TEST_F(JavaTypeQueryTest, UnsupportedInvalidationFallsBackToClear) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  DominatorTree DT(*F);
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(getCachedJavaType(F->getArg(0), &DT,
                             block(*F, "tail")->getTerminator(), nullptr,
                             Cache, Info).Klass, 22U);
  auto Removed = Cache.invalidate({F});
  EXPECT_TRUE(Removed.FullClear);
  EXPECT_EQ(Cache.baseSize(), 0U);
  EXPECT_EQ(Cache.pathFactsSize(), 0U);
  EXPECT_EQ(Cache.finalTypesSize(), 0U);
}

TEST_F(JavaTypeQueryTest, SelectiveResultStillDependsOnLVIInvalidation) {
  Function *F = parse(IR);
  ASSERT_NE(F, nullptr);
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  auto &Result = FAM.getResult<JavaTypeAnalysis>(*F);
  EXPECT_EQ(Result.getJavaType(F->getArg(0),
                              block(*F, "tail")->getTerminator()).Klass, 22U);
  PreservedAnalyses PA;
  PA.preserve<JavaTypeAnalysis>();
  PA.preserve<DominatorTreeAnalysis>();
  FAM.invalidate(*F, PA);
  EXPECT_EQ(FAM.getCachedResult<JavaTypeAnalysis>(*F), nullptr);
}

TEST_F(JavaTypeQueryTest, InvalidationWorkLimitFallsBackToClear) {
  Function *F = parse(R"(
    define void @test(ptr "java-klass"="22" %obj, i1 %c) {
      ret void
    }
  )");
  ASSERT_NE(F, nullptr);
  Instruction *Ret = F->getEntryBlock().getTerminator();
  Value *Previous = F->getArg(0);
  for (unsigned I = 0; I != 4100; ++I)
    Previous = SelectInst::Create(F->getArg(1), Previous, F->getArg(0),
                                  "derived", Ret->getIterator());
  JavaTypeCache Cache;
  CachedJavaTypeQueryInfo Info;
  EXPECT_EQ(getCachedJavaType(F->getArg(0), nullptr, nullptr, nullptr, Cache, Info)
                .Klass,
            22U);
  auto Invalidated = Cache.invalidate({F->getArg(1)});
  EXPECT_TRUE(Invalidated.FullClear);
  EXPECT_EQ(Invalidated.ValuesVisited, 4097U);
  EXPECT_EQ(Cache.baseSize(), 0U);
}

} // namespace
