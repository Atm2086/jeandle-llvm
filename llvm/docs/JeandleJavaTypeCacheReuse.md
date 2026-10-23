# JavaType context-cache reuse tests

These are direct query-API mechanism tests, not a JVM or production pass replay.
The fixture has no java-klass base attribute. A check_instanceof guard supplies
klass 22, followed by 32 CFG blocks and 16 distinct query contexts. The mutation
case changes the guard to klass 33. Queries use the real LVINullEdgeOracle.

## Build and run

Using an existing LLVM build directory:

```sh
ninja -C build/build-ninja-profiling JavaTypeQueryTests JavaTypeCacheReuseReplay
build/build-ninja-profiling/unittests/IR/JavaTypeQueryTests --gtest_filter='JavaTypeCacheReuseTest.*'
python3 llvm/utils/jeandle/replay-java-type-context-cache.py \
  --binary build/build-ninja-profiling/bin/JavaTypeCacheReuseReplay \
  --out /tmp/java-type-context-reuse --rounds 3 --batches 4096 --cpu 16
```

Use a new output directory for each run. The runner executes one separate
correctness diagnostic per variant, followed by exactly the requested number
of paired timing rounds. There are no adaptive rounds or additional timing
warmups.

## Cases and comparisons

- **final**: 16 queries of the same value and context block. Cold clears only
  JavaTypeCache before each query; cached retains it until the next batch.
  Expected cached final hits/misses: 15/1.
- **prefix**: the same value at 16 successive blocks. Each batch starts with an
  empty JavaTypeCache so later batches cannot turn this into a Final-hit test.
  Expected cached final hits: 0; PathFacts hits: 15.
- **mutation**: 16 queries, a real unused arithmetic-operand edit, 16 queries,
  a guard-klass edit, then 16 queries. Both modes cache between queries.
  Full clears JavaTypeCache at each edit; cached uses selective invalidation.
  Both notify before mutation and clear LVI after the guard edit.
  Expected full final hits/misses: 45/3; selective: 46/2.

The unit tests additionally check same-block instruction contexts, retained
entries after a benign edit, removal after the guard edit, and the new type.
Diagnostics compare every query with a completely fresh DT, assumption cache,
LVI and JavaTypeCache. They count calls into the null-edge oracle and cache
lookups; these are not measurements of internal DT nodes.

Fixture construction is outside the native timer. Timed runs disable oracle
counting and fresh-reference checks. LVI can stay warm between cold JavaType
queries: this comparison isolates JavaType cross-query reuse, not rebuilding
all supporting analyses. Timings include each batch's cache reset, and the
mutation case includes edits and invalidation. Clearing containers in the cold
baseline is included in its cost. VM callbacks use a small synthetic hierarchy.
Three rounds provide diagnostic evidence, not a production speed guarantee.
