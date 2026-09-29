//===- LVINullEdgeOracle.cpp - LVI-backed Java null-edge queries ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/LVINullEdgeOracle.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

/// LazyValueInfo-backed null-edge oracle. This fork's getPredicateOnEdge
/// returns a Constant*: the constant result of the comparison, or nullptr when
/// unknown. Unknown answers false (the edge is kept) — only positive proof may
/// skip an edge. V is stripped of pointer casts, aliases and freeze wrappers
/// (the same canonicalization the engine uses) so a null test on the
/// underlying oop is still recognized when the query value carries a wrapper
/// (freeze(x) equals x unless x is poison, and branching on poison is
/// undefined, so null proofs transfer).
bool LVINullEdgeOracle::operator()(Value *V, BasicBlock *FromBB,
                                   BasicBlock *ToBB) const {
  while (true) {
    V = V->stripPointerCastsAndAliases();
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
      V = FI->getOperand(0);
      continue;
    }
    break;
  }
  auto *PT = dyn_cast<PointerType>(V->getType());
  if (!PT)
    return false;
  Constant *C = LVI.getPredicateOnEdge(
      ICmpInst::ICMP_EQ, V, ConstantPointerNull::get(PT), FromBB, ToBB);
  return C && C->isOneValue();
}

