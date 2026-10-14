//===- JavaTypeAnalysis.cpp - Cached Java type queries --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/JavaTypeAnalysis.h"
#include "llvm/Analysis/Jeandle/LVINullEdgeOracle.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>

#define DEBUG_TYPE "java-type-analysis"

using namespace llvm;
using namespace llvm::jeandle;

namespace {

cl::opt<bool> DisableJavaTypeCache(
    "jeandle-disable-java-type-cache",
    cl::desc("Bypass the cross-query Java type caches"),
    cl::Hidden,
    cl::init(false));

cl::opt<bool> VerifyJavaTypeCache(
    "jeandle-verify-java-type-cache",
    cl::desc("Recompute Java type queries without cross-query caches"),
    cl::Hidden,
    cl::init(false));

cl::opt<bool> ForceFullJavaTypeInvalidation(
    "jeandle-java-type-force-full-invalidation",
    cl::desc("Use full clears for Java type mutation notifications (A/B baseline)"),
    cl::Hidden, cl::init(false));

ALWAYS_ENABLED_STATISTIC(TotalInvalidations, "Java type invalidation requests");
ALWAYS_ENABLED_STATISTIC(TotalSelectiveInvalidations, "Java type selective invalidations");
ALWAYS_ENABLED_STATISTIC(TotalCFGInvalidations, "Java type CFG invalidations");
ALWAYS_ENABLED_STATISTIC(TotalInvalidationVisits, "Java type invalidation values visited");
ALWAYS_ENABLED_STATISTIC(TotalBaseRemoved, "Java type base entries invalidated");
ALWAYS_ENABLED_STATISTIC(TotalFactsRemoved, "Java type path facts entries invalidated");
ALWAYS_ENABLED_STATISTIC(TotalFinalRemoved, "Java type final type entries invalidated");
ALWAYS_ENABLED_STATISTIC(TotalBaseRetained, "Java type base entries retained");
ALWAYS_ENABLED_STATISTIC(TotalFactsRetained, "Java type path facts entries retained");
ALWAYS_ENABLED_STATISTIC(TotalFinalRetained, "Java type final type entries retained");
ALWAYS_ENABLED_STATISTIC(TotalInvalidationTimeNanoseconds, "Java type total invalidation time (ns)");
ALWAYS_ENABLED_STATISTIC(TotalQueryTimeNanoseconds, "Java type total query time (ns)");

ALWAYS_ENABLED_STATISTIC(TotalQueries,
                        "Java type analysis queries");
ALWAYS_ENABLED_STATISTIC(TotalFactsHits,
                        "Java type path-facts cache hits");
ALWAYS_ENABLED_STATISTIC(TotalFactsMisses,
                        "Java type path-facts cache misses");
ALWAYS_ENABLED_STATISTIC(TotalFinalHits,
                        "Java type final-type cache hits");
ALWAYS_ENABLED_STATISTIC(TotalFinalMisses,
                        "Java type final-type cache misses");
ALWAYS_ENABLED_STATISTIC(TotalBaseHits,
                        "Java type base-only query cache hits");
ALWAYS_ENABLED_STATISTIC(TotalBaseMisses,
                        "Java type base-only query cache misses");
ALWAYS_ENABLED_STATISTIC(TotalBaseInsertions,
                        "Java type base-cache insertions");
ALWAYS_ENABLED_STATISTIC(TotalBypasses,
                        "Java type cache bypasses");
ALWAYS_ENABLED_STATISTIC(TotalClears,
                        "Java type cache clears");
ALWAYS_ENABLED_STATISTIC(TotalVerifications,
                        "Java type cache verification queries");
ALWAYS_ENABLED_STATISTIC(TotalVerificationSkips,
                        "Java type truncated or PHI-cycle comparisons skipped");

using QueryClock = std::chrono::steady_clock;

/// 计算从 Start 到现在的耗时，统一转换为纳秒，用于累计查询时间。
uint64_t elapsedNanoseconds(QueryClock::time_point Start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(QueryClock::now() - Start).count());
}

/// 输出缓存命中率；尚无查询样本时输出 n/a，避免除以零。
void printRate(raw_ostream &OS, uint64_t Hits, uint64_t Misses) {
  if (Hits + Misses == 0) {
    OS << "n/a";
    return;
  }
  OS << format("%.2f%%", 100.0 * static_cast<double>(Hits) / static_cast<double>(Hits + Misses));
}

/// 输出 JavaType 的类、精确性、接口和排除类型，供验证失败时诊断使用。
void printJavaType(raw_ostream &OS, const JavaType &Type) {
  OS << "{Klass=" << Type.Klass
     << ", Exact=" << (Type.Exact ? "true" : "false")
     << ", Interfaces=[";
  bool First = true;
  for (uintptr_t Klass : Type.Interfaces) {
    if (!First)
      OS << ", ";
    OS << Klass;
    First = false;
  }
  OS << "], ExcludedKlasses=[";
  First = true;
  for (uintptr_t Klass : Type.ExcludedKlasses) {
    if (!First)
      OS << ", ";
    OS << Klass;
    First = false;
  }
  OS << "]}";
}

/// 报告缓存结果与重新计算结果的不一致，打印函数、查询值及上下文后终止。
[[noreturn]] void reportVerificationFailure(
    Function &F, Value *V, BasicBlock *ContextBB, const JavaType &Cached,
    const JavaType &Fresh) {
  std::string Message;
  raw_string_ostream OS(Message);
  OS << "JavaTypeAnalysis verification failed in function '" << F.getName()
     << "' for ";
  V->printAsOperand(OS, false);
  OS << " at ";
  if (ContextBB)
    ContextBB->printAsOperand(OS, false);
  else
    OS << "<base>";
  OS << ": cached=";
  printJavaType(OS, Cached);
  OS << ", fresh=";
  printJavaType(OS, Fresh);
  reportFatalInternalError(StringRef(Message));
}

} // namespace

AnalysisKey JavaTypeAnalysis::Key;

/// 对外查询入口：选择缓存模式，并累计查询次数、缓存规模与耗时。
/// 启用验证时额外执行一次不共享缓存的查询；双方均无截断或 PHI 环依赖时检查相等。
/// 计时覆盖整个入口，包含可选的验证重算，但重算不计入对外查询次数。
/// @brief 查询入口、统计和校验
/// @param V
/// @param Context
/// @return
JavaType JavaTypeAnalysisResult::getJavaType(Value *V, Instruction *Context) {
  assert(V && "cannot query the Java type of a null Value");
  assert((!Context || Context->getFunction() == &F) &&
         "context must belong to the analyzed Function");

  // 测试
  QueryClock::time_point Start = QueryClock::now();
  ++Stats.Queries;
  ++TotalQueries;

  BasicBlock *ContextBB = Context ? Context->getParent() : nullptr;

  size_t OldBaseEntries = Cache.baseSize();
  CachedJavaTypeQueryInfo Info;
  LVINullEdgeOracle IsNullEdge{LVI};
  JavaType Result;
  if (DisableJavaTypeCache) {
    ++Stats.Bypasses;
    ++TotalBypasses;
    Result = jeandle::getJavaType(V, &DT, Context, IsNullEdge);
  } else {
    Result = getCachedJavaType(V, &DT, Context, IsNullEdge, Cache, Info);
  }
  Stats.BaseHits += Info.BaseHits;
  Stats.BaseMisses += Info.BaseMisses;
  TotalBaseHits += Info.BaseHits;
  TotalBaseMisses += Info.BaseMisses;
  Stats.FactsHits += Info.FactsHits;
  Stats.FactsMisses += Info.FactsMisses;
  TotalFactsHits += Info.FactsHits;
  TotalFactsMisses += Info.FactsMisses;
  Stats.FinalHits += Info.FinalHits;
  Stats.FinalMisses += Info.FinalMisses;
  TotalFinalHits += Info.FinalHits;
  TotalFinalMisses += Info.FinalMisses;

  // 更新统计
  Stats.BaseInsertions += Cache.baseSize() - OldBaseEntries;
  TotalBaseInsertions += Cache.baseSize() - OldBaseEntries;
  Stats.PeakBaseEntries = std::max<size_t>(Stats.PeakBaseEntries, Cache.baseSize());
  Stats.PeakFactsEntries =
      std::max<size_t>(Stats.PeakFactsEntries, Cache.pathFactsSize());
  Stats.PeakFinalEntries =
      std::max<size_t>(Stats.PeakFinalEntries, Cache.finalTypesSize());

  // 可选重算校验，这里好像有点奇怪的问题
  if (VerifyJavaTypeCache && !DisableJavaTypeCache) {
    ++Stats.Verifications;
    ++TotalVerifications;
    // The reference result always comes from the untouched original engine.
    JavaType Fresh = jeandle::getJavaType(V, &DT, Context, IsNullEdge);
    // The original API does not expose truncation/cycle diagnostics. A separate
    // cold evaluation supplies only those flags, never the reference result.
    JavaTypeCache DiagnosticCache;
    CachedJavaTypeQueryInfo FreshInfo;
    getCachedJavaType(V, &DT, Context, IsNullEdge, DiagnosticCache, FreshInfo);
    // 缓存节省预算；PHI 环中完整缓存与局部贡献也可能精度不同。
    // 跳过严格相等不代表验证通过，单独记录，后续需补语义包含关系校验。
    if (Info.SkipVerification || FreshInfo.SkipVerification) {
      ++Stats.VerificationSkips;
      ++TotalVerificationSkips;
    } else if (Result != Fresh)
      reportVerificationFailure(F, V, ContextBB, Result, Fresh);
  }

  uint64_t Elapsed = elapsedNanoseconds(Start);
  Stats.TotalQueryTimeNanoseconds += Elapsed;
  TotalQueryTimeNanoseconds += Elapsed;
  return Result;
}

/// 粗粒度失效：清空基础类型、路径事实及最终类型缓存，保留累计统计。
/// pass 内发生相关 IR 修改后，必须在下一次查询前调用；DT/LVI 由调用方维护。
void JavaTypeAnalysisResult::clear() {
  invalidateImpl({}, false, true);
}

/// Invalidate the old use closure before RAUW/deletion, including non-key
/// conditions. Caller maintains DT/LVI before the next query.
void JavaTypeAnalysisResult::invalidate(Value &Changed) {
  Value *V = &Changed;
  invalidate({V});
}

void JavaTypeAnalysisResult::invalidate(ArrayRef<Value *> Changed,
                                        bool CFGChanged) {
  invalidateImpl(Changed, CFGChanged, ForceFullJavaTypeInvalidation);
}

void JavaTypeAnalysisResult::invalidateForCFGChange(Value &Changed) {
  Value *V = &Changed;
  invalidate({V}, true);
}

void JavaTypeAnalysisResult::invalidateImpl(ArrayRef<Value *> Changed,
                                            bool CFGChanged, bool ForceFull) {
  auto Start = QueryClock::now();
  ++Stats.Invalidations;
  ++TotalInvalidations;
  if (CFGChanged) {
    ++Stats.CFGInvalidations;
    ++TotalCFGInvalidations;
  }
  JavaTypeCacheInvalidationInfo Info;
  if (ForceFull) {
    Info.BaseRemoved = Cache.baseSize();
    Info.FactsRemoved = Cache.pathFactsSize();
    Info.FinalRemoved = Cache.finalTypesSize();
    Info.FullClear = true;
    Cache.clear();
  } else {
    Info = Cache.invalidate(Changed, CFGChanged);
  }
  if (Info.FullClear) {
    ++Stats.Clears;
    ++TotalClears;
  } else {
    ++Stats.SelectiveInvalidations;
    ++TotalSelectiveInvalidations;
  }
  Stats.InvalidationVisits += Info.ValuesVisited;
  TotalInvalidationVisits += Info.ValuesVisited;
  Stats.BaseRemoved += Info.BaseRemoved;
  TotalBaseRemoved += Info.BaseRemoved;
  Stats.FactsRemoved += Info.FactsRemoved;
  TotalFactsRemoved += Info.FactsRemoved;
  Stats.FinalRemoved += Info.FinalRemoved;
  TotalFinalRemoved += Info.FinalRemoved;
  Stats.BaseRetained += Cache.baseSize();
  TotalBaseRetained += Cache.baseSize();
  Stats.FactsRetained += Cache.pathFactsSize();
  TotalFactsRetained += Cache.pathFactsSize();
  Stats.FinalRetained += Cache.finalTypesSize();
  TotalFinalRetained += Cache.finalTypesSize();
  uint64_t Elapsed = elapsedNanoseconds(Start);
  Stats.InvalidationTimeNanoseconds += Elapsed;
  TotalInvalidationTimeNanoseconds += Elapsed;
}

/// 供 FunctionAnalysisManager 判断本分析是否失效，返回 true 表示应丢弃结果。
/// 未显式保留本分析，或其依赖的 DominatorTree/LVI 失效时，整个 Result 失效。
bool JavaTypeAnalysisResult::invalidate( Function &F,
                                         const PreservedAnalyses &PA,
                                         FunctionAnalysisManager::Invalidator &Inv)
{
  auto PAC = PA.getChecker<JavaTypeAnalysis>();
  return !(PAC.preserved() ||
           PAC.preservedSet<AllAnalysesOn<Function>>()) ||
         Inv.invalidate<DominatorTreeAnalysis>(F, PA) ||
         Inv.invalidate<LazyValueAnalysis>(F, PA);
}

/// 打印当前 Function 的累计查询统计与三个缓存的当前及峰值大小。
/// facts/final 统计对应共享缓存查找，base 命中率仅对应显式 base-only 查询。
void JavaTypeAnalysisResult::print(raw_ostream &OS) const {
  OS << "JavaTypeAnalysis for function: " << F.getName() << '\n';
  OS << "  queries: " << Stats.Queries << '\n';
  OS << "  path facts hits: " << Stats.FactsHits << '\n';
  OS << "  path facts misses: " << Stats.FactsMisses << '\n';
  OS << "  path facts hit rate: ";
  printRate(OS, Stats.FactsHits, Stats.FactsMisses);
  OS << '\n';
  OS << "  final type hits: " << Stats.FinalHits << '\n';
  OS << "  final type misses: " << Stats.FinalMisses << '\n';
  OS << "  final type hit rate: ";
  printRate(OS, Stats.FinalHits, Stats.FinalMisses);
  OS << '\n';
  OS << "  base-only query hits: " << Stats.BaseHits << '\n';
  OS << "  base-only query misses: " << Stats.BaseMisses << '\n';
  OS << "  base-only query hit rate: ";
  printRate(OS, Stats.BaseHits, Stats.BaseMisses);
  OS << '\n';
  OS << "  base insertions: " << Stats.BaseInsertions << '\n';
  OS << "  current base entries: " << Cache.baseSize() << '\n';
  OS << "  peak base entries: " << Stats.PeakBaseEntries << '\n';
  OS << "  current path facts entries: " << Cache.pathFactsSize() << '\n';
  OS << "  peak path facts entries: " << Stats.PeakFactsEntries << '\n';
  OS << "  current final type entries: " << Cache.finalTypesSize() << '\n';
  OS << "  peak final type entries: " << Stats.PeakFinalEntries << '\n';
  OS << "  clears: " << Stats.Clears << '\n';
  OS << "  invalidation requests: " << Stats.Invalidations << '\n';
  OS << "  selective invalidations: " << Stats.SelectiveInvalidations << '\n';
  OS << "  CFG invalidations: " << Stats.CFGInvalidations << '\n';
  OS << "  invalidation values visited: " << Stats.InvalidationVisits << '\n';
  OS << "  base entries invalidated: " << Stats.BaseRemoved << '\n';
  OS << "  path facts entries invalidated: " << Stats.FactsRemoved << '\n';
  OS << "  final type entries invalidated: " << Stats.FinalRemoved << '\n';
  OS << "  base entries retained: " << Stats.BaseRetained << '\n';
  OS << "  path facts entries retained: " << Stats.FactsRetained << '\n';
  OS << "  final type entries retained: " << Stats.FinalRetained << '\n';
  OS << "  total invalidation time (ns): " << Stats.InvalidationTimeNanoseconds << '\n';
  OS << "  bypasses: " << Stats.Bypasses << '\n';
  OS << "  verifications: " << Stats.Verifications << '\n';
  OS << "  verification skips: " << Stats.VerificationSkips << '\n';
  OS << "  total query time (ns): " << Stats.TotalQueryTimeNanoseconds << '\n';
}

/// 从分析管理器取得 DT 和 LVI，创建持有这些依赖引用的 Function 级 Result。
/// 此处不预先遍历函数，类型计算及缓存填充在后续查询时按需执行。
JavaTypeAnalysis::Result JavaTypeAnalysis::run(Function &F, FunctionAnalysisManager &FAM)
{
  DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  LazyValueInfo &LVI = FAM.getResult<LazyValueAnalysis>(F);
  return JavaTypeAnalysisResult(F, DT, LVI);
}

/// 获取并打印指定 Function 的分析统计；仅观察已有统计，不主动发起类型查询。
/// 不修改 IR，因此保留全部分析。
PreservedAnalyses JavaTypeAnalysisPrinterPass::run( Function &F,
                                                    FunctionAnalysisManager &FAM)
{
  FAM.getResult<JavaTypeAnalysis>(F).print(OS);
  return PreservedAnalyses::all();
}

/// 在模块处理位置打印进程级累计统计；数据包含此前处理的函数及模块。
/// 不重置计数、不修改 IR，也不主动触发各函数的 JavaType 查询。
PreservedAnalyses JavaTypeAnalysisSummaryPrinterPass::run( Module &M,
                                                           ModuleAnalysisManager &)
{
  OS << "JavaTypeAnalysis process summary after module: " << M.getName() << '\n';
  OS << "  queries: " << TotalQueries.getValue() << '\n';
  OS << "  path facts hits: " << TotalFactsHits.getValue() << '\n';
  OS << "  path facts misses: " << TotalFactsMisses.getValue() << '\n';
  OS << "  final type hits: " << TotalFinalHits.getValue() << '\n';
  OS << "  final type misses: " << TotalFinalMisses.getValue() << '\n';
  OS << "  base-only query hits: " << TotalBaseHits.getValue() << '\n';
  OS << "  base-only query misses: " << TotalBaseMisses.getValue() << '\n';
  OS << "  base insertions: " << TotalBaseInsertions.getValue() << '\n';
  OS << "  clears: " << TotalClears.getValue() << '\n';
  OS << "  invalidation requests: " << TotalInvalidations.getValue() << '\n';
  OS << "  selective invalidations: " << TotalSelectiveInvalidations.getValue() << '\n';
  OS << "  CFG invalidations: " << TotalCFGInvalidations.getValue() << '\n';
  OS << "  invalidation values visited: " << TotalInvalidationVisits.getValue() << '\n';
  OS << "  base entries invalidated: " << TotalBaseRemoved.getValue() << '\n';
  OS << "  path facts entries invalidated: " << TotalFactsRemoved.getValue() << '\n';
  OS << "  final type entries invalidated: " << TotalFinalRemoved.getValue() << '\n';
  OS << "  base entries retained: " << TotalBaseRetained.getValue() << '\n';
  OS << "  path facts entries retained: " << TotalFactsRetained.getValue() << '\n';
  OS << "  final type entries retained: " << TotalFinalRetained.getValue() << '\n';
  OS << "  total invalidation time (ns): " << TotalInvalidationTimeNanoseconds.getValue() << '\n';
  OS << "  total query time (ns): " << TotalQueryTimeNanoseconds.getValue() << '\n';
  OS << "  bypasses: " << TotalBypasses.getValue() << '\n';
  OS << "  verifications: " << TotalVerifications.getValue() << '\n';
  OS << "  verification skips: " << TotalVerificationSkips.getValue() << '\n';
  return PreservedAnalyses::all();
}
