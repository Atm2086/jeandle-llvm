//===- JavaTypeAnalysis.cpp - Cached Java type queries --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Jeandle/JavaTypeAnalysis.h"
#include "llvm/Analysis/Jeandle/JavaTypeAnalysisStats.h"
#include "llvm/Analysis/Jeandle/LVINullEdgeOracle.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

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

cl::opt<bool> ForceBaseScanJavaTypeInvalidation(
    "jeandle-java-type-force-base-scan-invalidation",
    cl::desc("Scan all base cache entries instead of deleting affected keys"),
    cl::Hidden, cl::init(false));

using QueryClock = std::chrono::steady_clock;

/// Converts the time elapsed since Start into nanoseconds; only called when
/// statistics are enabled.
uint64_t elapsedNanoseconds(QueryClock::time_point Start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(QueryClock::now() - Start).count());
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

/// Public query entry: selects the cache mode and, when statistics are enabled,
/// reports one optional observation event.
/// With verification enabled it also runs a query that shares no cache and
/// compares the results when neither side was truncated or depends on a PHI
/// cycle. With statistics disabled nothing is timed or counted; the verification
/// recomputation never affects the returned result.
/// @brief query entry, optional statistics and verification
/// @param V
/// @param Context
/// @return
JavaType JavaTypeAnalysisResult::getJavaType(Value *V, Instruction *Context) {
  assert(V && "cannot query the Java type of a null Value");
  assert((!Context || Context->getFunction() == &F) &&
         "context must belong to the analyzed Function");

  QueryClock::time_point Start = Observer ? QueryClock::now()
                                          : QueryClock::time_point{};

  BasicBlock *ContextBB = Context ? Context->getParent() : nullptr;

  size_t OldBaseEntries = Cache.baseSize();
  CachedJavaTypeQueryInfo Info;
  LVINullEdgeOracle IsNullEdge{LVI};
  JavaType Result;
  bool Bypassed = false;
  if (DisableJavaTypeCache) {
    Bypassed = true;
    Result = jeandle::getJavaType(V, &DT, Context, IsNullEdge);
  } else {
    Result = getCachedJavaType(V, &DT, Context, IsNullEdge, Cache, Info);
  }

  // 可选重算校验，这里好像有点奇怪的问题
  bool Verified = false;
  bool VerificationSkipped = false;
  if (VerifyJavaTypeCache && !DisableJavaTypeCache) {
    Verified = true;
    // The reference result always comes from the untouched original engine.
    JavaType Fresh = jeandle::getJavaType(V, &DT, Context, IsNullEdge);
    // The original API does not expose truncation/cycle diagnostics. A separate
    // cold evaluation supplies only those flags, never the reference result.
    JavaTypeCache DiagnosticCache;
    CachedJavaTypeQueryInfo FreshInfo;
    getCachedJavaType(V, &DT, Context, IsNullEdge, DiagnosticCache, FreshInfo);
    // 缓存节省预算；PHI 环中完整缓存与局部贡献也可能精度不同。
    // 跳过严格相等不代表验证通过，单独记录，后续需补语义包含关系校验。
    if (Info.SkipVerification || FreshInfo.SkipVerification)
      VerificationSkipped = true;
    else if (Result != Fresh)
      reportVerificationFailure(F, V, ContextBB, Result, Fresh);
  }

  if (Observer) {
    JavaTypeQueryEvent Event;
    Event.Info = Info;
    Event.QueryValue = V;
    Event.Context = ContextBB;
    Event.BaseInsertions = Cache.baseSize() - OldBaseEntries;
    Event.ElapsedNanoseconds = elapsedNanoseconds(Start);
    Event.BaseEntries = Cache.baseSize();
    Event.PathFactsEntries = Cache.pathFactsSize();
    Event.FinalEntries = Cache.finalTypesSize();
    Event.Bypassed = Bypassed;
    Event.Verified = Verified;
    Event.VerificationSkipped = VerificationSkipped;
    Observer->onQuery(Event);
  }
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
  auto Start = Observer ? QueryClock::now() : QueryClock::time_point{};
  JavaTypeCacheInvalidationInfo Info;
  if (ForceFull) {
    Info.BaseRemoved = Cache.baseSize();
    Info.FactsRemoved = Cache.pathFactsSize();
    Info.FinalRemoved = Cache.finalTypesSize();
    Info.FullClear = true;
    Cache.clear();
  } else {
    Info = Cache.invalidate(
        Changed, CFGChanged,
        ForceBaseScanJavaTypeInvalidation
            ? JavaTypeBaseInvalidationMode::Scan
            : JavaTypeBaseInvalidationMode::SelectiveKeys);
  }
  if (!Observer)
    return;
  JavaTypeInvalidationEvent Event;
  Event.Info = Info;
  Event.BaseEntries = Cache.baseSize();
  Event.PathFactsEntries = Cache.pathFactsSize();
  Event.FinalEntries = Cache.finalTypesSize();
  Event.CFGChanged = CFGChanged;
  Event.ElapsedNanoseconds = elapsedNanoseconds(Start);
  Observer->onInvalidation(Event);
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

/// Obtains DT and LVI from the analysis manager and creates the Function-level
/// Result holding references to them. Statistics are off by default; when they
/// are enabled, the non-invalidating statistics analysis is fetched and injected
/// as the observer. The function is not traversed here: type computation and
/// cache population happen lazily on later queries.
JavaTypeAnalysis::Result JavaTypeAnalysis::run(Function &F,
                                               FunctionAnalysisManager &FAM) {
  DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  LazyValueInfo &LVI = FAM.getResult<LazyValueAnalysis>(F);
  JavaTypeQueryObserver *Observer = nullptr;
  if (javaTypeStatsEnabled())
    Observer = &FAM.getResult<JavaTypeStatsAnalysis>(F);
  return JavaTypeAnalysisResult(F, DT, LVI, Observer);
}
