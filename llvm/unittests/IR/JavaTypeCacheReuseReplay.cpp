//===- JavaTypeCacheReuseReplay.cpp - Bounded context query replay
//----------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "JavaTypeCacheReuseTestSupport.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include <chrono>
#include <cstdlib>
#include <sys/resource.h>

using namespace llvm;
using namespace llvm::jeandle::cache_reuse_test;
int main(int Argc, char **Argv) {
  if (Argc != 5) {
    errs() << "usage: JavaTypeCacheReuseReplay final|prefix|mutation "
              "cold|cached|full batches diagnostic|timing\n";
    return 2;
  }
  std::string Scenario = Argv[1], Mode = Argv[2], Kind = Argv[4];
  char *End = nullptr;
  uint64_t Batches = std::strtoull(Argv[3], &End, 10);
  if (*End || !Batches || Batches > 1000000 ||
      (Scenario != "final" && Scenario != "prefix" && Scenario != "mutation") ||
      (Mode != "cold" && Mode != "cached" && Mode != "full") ||
      (Kind != "diagnostic" && Kind != "timing"))
    return 2;
  bool Diagnostic = Kind == "diagnostic";
  VMEnvironment Env;
  Fixture X(Diagnostic);
  uint64_t Queries = 0, Hits = 0, Misses = 0, FactsHits = 0, FactsMisses = 0;
  uint64_t Checksum = 0, OracleCalls = 0;
  auto Query = [&](Instruction *Ctx, unsigned Expected) {
    if (Mode == "cold")
      X.Cache.clear(); // JavaType-only cold baseline; LVI may remain warm.
    auto T = X.query(Ctx);
    if (T.Klass != Expected || X.Last.SkipVerification)
      report_fatal_error("incorrect or truncated query");
    Checksum += T.Klass;
    if (Diagnostic) {
      if (T != X.fresh(Ctx))
        report_fatal_error("fresh DT/LVI/cache reference disagrees");
      ++Queries;
      Hits += X.Last.FinalHits;
      Misses += X.Last.FinalMisses;
      FactsHits += X.Last.FactsHits;
      FactsMisses += X.Last.FactsMisses;
    }
  };
  // Fixture construction and parsing are outside the measured interval.
  rusage Before{}, After{};
  getrusage(RUSAGE_SELF, &Before);
  auto Start = std::chrono::steady_clock::now();
  for (uint64_t B = 0; B != Batches; ++B) {
    X.reset();
    auto Batch = [&](unsigned Expected) {
      for (auto *Ctx : X.Contexts)
        Query(Scenario == "prefix" ? Ctx : X.Contexts.front(), Expected);
    };
    Batch(22);
    if (Scenario == "mutation") {
      X.editNoise(Mode == "full");
      Batch(22);
      X.editGuard(Mode == "full");
      Batch(33);
    }
    OracleCalls += X.OracleCalls;
  }
  double Wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - Start)
          .count();
  getrusage(RUSAGE_SELF, &After);
  auto Seconds = [](timeval T) { return T.tv_sec + T.tv_usec / 1e6; };
  json::Object O{{"scenario", Scenario},
                 {"mode", Mode},
                 {"kind", Kind},
                 {"batches", int64_t(Batches)},
                 {"wall_s", Wall},
                 {"user_s", Seconds(After.ru_utime) - Seconds(Before.ru_utime)},
                 {"checksum", int64_t(Checksum)}};
  if (Diagnostic) {
    O["queries"] = int64_t(Queries);
    O["final_hits"] = int64_t(Hits);
    O["final_misses"] = int64_t(Misses);
    O["facts_hits"] = int64_t(FactsHits);
    O["facts_misses"] = int64_t(FactsMisses);
    O["oracle_calls"] = int64_t(OracleCalls);
    O["fresh_reference_checked"] = true;
  }
  outs() << formatv("{0:2}", json::Value(std::move(O))) << "\n";
}
