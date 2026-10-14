//===- JavaTypeAnalysis.h - Cached Java type queries ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_JEANDLE_JAVATYPEANALYSIS_H
#define LLVM_ANALYSIS_JEANDLE_JAVATYPEANALYSIS_H

#include "llvm/Analysis/Jeandle/CachedJavaType.h"
#include "llvm/IR/PassManager.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace llvm {

class BasicBlock;
class DominatorTree;
class Function;
class Instruction;
class LazyValueInfo;
class Module;
class raw_ostream;
class Value;

namespace jeandle {

/// 单个 Function 的分析结果，供优化 Pass 查询 JavaType 并管理缓存失效。
/// 持有 BaseTypes、PathFacts 和 FinalTypes，跨查询复用基础类型、路径事实及最终类型；
/// 递归栈和预算仍属于单次查询，DT/LVI 由分析管理器提供，本类只持有引用。
/// IR 修改前通知受影响的值；下一次查询前必须更新 DT/LVI。
class JavaTypeAnalysisResult {
public:

  LLVM_ABI JavaType getJavaType(Value *V,
                                Instruction *Context = nullptr);
  LLVM_ABI void clear();
  // Notify before RAUW/deletion so the old use graph can still be traversed.
  // Unknown changes and changes to VM type information require clear().
  LLVM_ABI void invalidate(Value &Changed);
  LLVM_ABI void invalidate(ArrayRef<Value *> Changed, bool CFGChanged = false);
  LLVM_ABI void invalidateForCFGChange(Value &Changed);
  LLVM_ABI void print(raw_ostream &OS) const;

  // llvm 分析管理器的失效回调
  LLVM_ABI bool invalidate(Function &F, const PreservedAnalyses &PA,
                           FunctionAnalysisManager::Invalidator &Inv);

private:

  friend class JavaTypeAnalysis;

  JavaTypeAnalysisResult(Function &F, DominatorTree &DT, LazyValueInfo &LVI)
      : F(F), DT(DT), LVI(LVI) {}

  Function &F;
  DominatorTree &DT;
  LazyValueInfo &LVI;

  JavaTypeCache Cache;

  void invalidateImpl(ArrayRef<Value *> Changed, bool CFGChanged,
                      bool ForceFull);

  /// 当前 Result 的累计查询、缓存和校验统计；clear 只清缓存，不重置这些计数。
  /// BaseHits/BaseMisses 仅统计无上下文查询，FactsHits/FactsMisses 统计共享路径事实查找。
  /// FinalHits/FinalMisses 同时统计顶层与 PHI incoming 的最终缓存查找。
  struct Statistics {
    uint64_t Queries = 0;
    uint64_t FactsHits = 0;
    uint64_t FactsMisses = 0;
    uint64_t FinalHits = 0;
    uint64_t FinalMisses = 0;
    uint64_t BaseHits = 0;
    uint64_t BaseMisses = 0;
    uint64_t BaseInsertions = 0;
    uint64_t Bypasses = 0;
    uint64_t Clears = 0;
    uint64_t Invalidations = 0;
    uint64_t SelectiveInvalidations = 0;
    uint64_t CFGInvalidations = 0;
    uint64_t InvalidationVisits = 0;
    uint64_t BaseRemoved = 0;
    uint64_t FactsRemoved = 0;
    uint64_t FinalRemoved = 0;
    // Sums over invalidation events, not counts of distinct retained entries.
    uint64_t BaseRetained = 0;
    uint64_t FactsRetained = 0;
    uint64_t FinalRetained = 0;
    uint64_t InvalidationTimeNanoseconds = 0;
    uint64_t Verifications = 0;
    uint64_t VerificationSkips = 0;
    uint64_t TotalQueryTimeNanoseconds = 0;
    size_t PeakFactsEntries = 0;
    size_t PeakFinalEntries = 0;
    size_t PeakBaseEntries = 0;
  } Stats;
};

/// FunctionAnalysisManager 的分析入口：工厂方法
class JavaTypeAnalysis : public AnalysisInfoMixin<JavaTypeAnalysis> {
  friend AnalysisInfoMixin<JavaTypeAnalysis>;
  LLVM_ABI static AnalysisKey Key;

public:
  using Result = JavaTypeAnalysisResult;
  LLVM_ABI Result run(Function &F, FunctionAnalysisManager &FAM);
};

/// Function 级统计打印 Pass：获取该函数的 Result，输出查询统计和缓存大小。
/// 不主动发起类型查询，不修改 IR，保留全部分析。
class JavaTypeAnalysisPrinterPass : public PassInfoMixin<JavaTypeAnalysisPrinterPass> {
  raw_ostream &OS;

public:
  explicit JavaTypeAnalysisPrinterPass(raw_ostream &OS) : OS(OS) {}
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }
};

/// Module 级统计打印 Pass：在当前模块处理位置输出进程级累计查询统计。
/// 统计可包含此前处理的函数及模块；不遍历查询类型、不重置计数，也不修改 IR。
class JavaTypeAnalysisSummaryPrinterPass : public PassInfoMixin<JavaTypeAnalysisSummaryPrinterPass> {
  raw_ostream &OS;

public:
  explicit JavaTypeAnalysisSummaryPrinterPass(raw_ostream &OS) : OS(OS) {}
  LLVM_ABI PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  static bool isRequired() { return true; }
};

} // namespace jeandle
} // namespace llvm

#endif // LLVM_ANALYSIS_JEANDLE_JAVATYPEANALYSIS_H
