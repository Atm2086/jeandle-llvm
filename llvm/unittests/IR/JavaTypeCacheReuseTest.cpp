//===- JavaTypeCacheReuseTest.cpp - Expensive context query reuse tests
//-----===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "JavaTypeCacheReuseTestSupport.h"
#include "llvm/IR/Verifier.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::jeandle::cache_reuse_test;

TEST(JavaTypeCacheReuseTest, DeepSameBlockFinalHitsAvoidOracle) {
  VMEnvironment Env;
  Fixture X;
  ASSERT_FALSE(verifyFunction(*X.F, &errs()));
  auto *Ctx = X.Contexts.front();
  auto Cold = X.query(Ctx);
  ASSERT_EQ(Cold.Klass, 22U);
  ASSERT_EQ(Cold, X.fresh(Ctx));
  ASSERT_GT(X.Last.FactsMisses, 32U);
  ASSERT_GT(X.OracleCalls, 32U);
  uint64_t Calls = X.OracleCalls;
  // A different instruction in the same block still has the same context key.
  for (auto *I : {static_cast<Instruction *>(X.Noise), Ctx}) {
    EXPECT_EQ(X.query(I), Cold);
    EXPECT_EQ(X.Last.FinalHits, 1U);
    EXPECT_EQ(X.Last.FactsMisses, 0U);
    EXPECT_FALSE(X.Last.SkipVerification);
    EXPECT_EQ(X.OracleCalls, Calls);
  }
}

TEST(JavaTypeCacheReuseTest, DifferentBlocksReuseExpensiveFactsPrefix) {
  VMEnvironment Env;
  Fixture X;
  ASSERT_EQ(X.query(X.Contexts.front()).Klass, 22U);
  uint64_t ColdMisses = X.Last.FactsMisses;
  for (unsigned I = 1; I != X.Contexts.size(); ++I) {
    uint64_t Before = X.OracleCalls;
    auto T = X.query(X.Contexts[I]);
    EXPECT_EQ(T, X.fresh(X.Contexts[I]));
    EXPECT_EQ(T.Klass, 22U);
    EXPECT_EQ(X.Last.FinalHits, 0U);
    EXPECT_GT(X.Last.FactsHits, 0U);
    EXPECT_LT(X.Last.FactsMisses, ColdMisses);
    EXPECT_EQ(X.OracleCalls - Before, 1U);
    EXPECT_FALSE(X.Last.SkipVerification);
  }
}

TEST(JavaTypeCacheReuseTest, BatchMutationRetainsOrDropsContextCorrectly) {
  VMEnvironment Env;
  Fixture X;
  auto *Ctx = X.Contexts.front();
  for (unsigned I = 0; I != 16; ++I)
    ASSERT_EQ(X.query(Ctx).Klass, 22U);
  ASSERT_GT(X.Cache.finalTypesSize(), 0U);
  auto Benign = X.editNoise();
  EXPECT_EQ(Benign.FinalRemoved, 0U);
  EXPECT_EQ(Benign.FactsRemoved, 0U);
  uint64_t Calls = X.OracleCalls;
  for (unsigned I = 0; I != 16; ++I) {
    EXPECT_EQ(X.query(Ctx), X.fresh(Ctx));
    EXPECT_EQ(X.Last.FinalHits, 1U);
    EXPECT_EQ(X.OracleCalls, Calls);
  }
  auto Relevant = X.editGuard();
  EXPECT_GT(Relevant.FinalRemoved, 0U);
  EXPECT_GT(Relevant.FactsRemoved, 0U);
  auto After = X.query(Ctx);
  EXPECT_EQ(After.Klass, 33U);
  EXPECT_EQ(After, X.fresh(Ctx));
  EXPECT_EQ(X.Last.FinalHits, 0U);
  EXPECT_GT(X.Last.FactsMisses, 32U);
  EXPECT_GT(X.OracleCalls, Calls);
  EXPECT_EQ(X.query(Ctx), After);
  EXPECT_EQ(X.Last.FinalHits, 1U);
  EXPECT_FALSE(verifyFunction(*X.F, &errs()));
}
