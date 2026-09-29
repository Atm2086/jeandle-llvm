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


} // namespace llvm

#endif
