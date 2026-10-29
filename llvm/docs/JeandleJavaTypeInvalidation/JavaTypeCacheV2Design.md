# JavaType V2 四类缓存及失效契约

本文件描述当前实现。原版 `llvm/lib/IR/Jeandle/JavaType.cpp` 和
`llvm/include/llvm/IR/Jeandle/JavaType.h` 保持不变；V2 的求解副本仍在
`CachedJavaTypeV2.cpp`，缓存所有权、监控和失效实现在独立的
`JavaTypeCacheV2.h/.cpp`。

## 组织与查询入口

`JavaTypeCacheV2` 是单个 Function 的聚合所有者，分别持有四个独立类：

| 成员 / 类 | 保存内容 | 复用边界 |
| --- | --- | --- |
| `Base` / `JavaTypeBaseCacheV2` | `Value* → JavaType` 的基础类型结果 | 相同 IR、VM 类型环境；修改后可保留不受影响的值 |
| `Defined` / `JavaTypeDefinedCacheV2` | `Value* → bool` 的完成 definedness 证明 | 同上；`false` 表示未能证明 defined |
| `Facts` / `JavaTypeFactsCacheV2` | `(V, From, To)` 的根 guard 和完整 null-edge 判断 | 本次有效期内跨查询复用；IR 修改后清空 |
| `Final` / `JavaTypeFinalCacheV2` | `(V, ContextBB, InitialBudget)` 的完整顶层查询结果 | 依赖未变，且所需 oracle 判断仍有效 |

最外层只传入一个缓存引用：

```cpp
JavaType getCachedJavaTypeV2(Value *V, DominatorTree *DT,
                            Instruction *Context, IsNullEdgeOracle IsNullEdge,
                            JavaTypeCacheV2 &Cache);
```

预算、递归栈、SCC 工作图、`Processed` 和 `ValueTypes` 仍是单次查询状态。
只有完整顶层结果进入 Final；递归中的 `valueDimension` / `factsAt`
不读取 Final。截断结果也只按其初始预算复用，因此提高预算会使用另一个键。
Context 用所在块作键，对应当前算法按块求路径事实的语义。

每个缓存类拥有自己的累计 `Stats`，默认关闭。聚合类提供统一开关及
`resetStats()`；`clear()` 清结果但保留累计计数。Base 的跨查询命中细分
需另开详细统计，才建立查询开始时的键快照。Analysis 的 Observer
汇总这些计数的差分及查询时间；无 Observer 时不复制计数快照、不读时钟。
Facts 计数衡量 helper 请求/计算，不能直接解释为 LVI 内部求解次数或耗时。

## Base 与 Defined：共享修改前的 users 闭包

聚合类在 IR 修改前，从通知的值出发收集完整旧 SSA users 闭包，
再分别调用 Base、Defined 和 Final 的失效方法。遍历不因值不是指针、
或值没有缓存条目而停止；整数条件和 poison 相关 flag 修改也会传播。
Base 为空时仍处理 Defined。

普通修改只删除闭包中的 Base / Defined 键。兼容现有只提供修改源的
CFG 通知时，两者额外丢弃派生结果，保留不受影响的叶子结果。
Base 的 returned-argument call 是 identity 表达式，不作为独立叶子保留。
Defined 的冻结结果等是否为叶子，按其自身证明规则判断。

超过 4096 个值或 16384 条 use 边、遇到非局部修改或跨函数值，
聚合类回退全清。该预算仅限制失效工作，不修改查询求解预算。

## Facts：边粒度复用，修改时保守清空

两个表分别保存根 guard 和 null-edge 判断，键都保留实际传入的
`(V, From, To)`。它们表示沿哪条边成立的事实，单个 Context 不能区分
同一块的不同入边及其分支方向。

根 guard 仅在 `ConditionDepth == 0` 时读写缓存，递归条件追踪保留
原来的深度约束。默认 `GuardOnly` 不缓存 null 判断，也不启用 Final；
因此允许任意随调用变化的 oracle。

显式选择 `StableOracleEpoch` 后，调用方承诺通知之间 oracle 判断可复用，
才启用 null 表和 Final。IR 修改时 Facts 整体清空。只维护/重置 oracle、
IR 不变时，也必须调用聚合类 `notifyOracleChanged()`。

## Final：独立依赖区域、反向索引与重验证

Final 发布时单独收集依赖，不能依赖本次实际的 memo miss：
Base / Defined / Facts 已热时，miss 轨迹会漏掉依赖。

依赖包含查询值和本次所有 null 判断值的传递操作数、定义块、PHI 入边块、
Context 块、null 判断的 From / To，以及这些块的完整 CFG 前驱闭包。
块依赖覆盖块内所有指令，也覆盖当前不存在有效 guard、LVI 隐式扫描
同块指令等情况。修改点的旧 users 闭包把区域外的 SSA 修改传播到区域内。
不进入全局变量初始值或被调函数体；这类非局部变化要求全清。

`ByValue` 和 `ByBlock` 是依赖到 Final 键的反向索引。普通修改只删除
与受影响值/块相交的 Final 条目，无关分支可以保留。依赖超过
4096 个值/块或 16384 条边时，只放弃发布这个 Final 条目，查询照常返回。
CFG 结构变化可能引入旧区域外的新前驱，当前通知协议无法完整描述新拓扑，
因此 CFG 修改保守清空 Final。

每个 Final 条目还按调用顺序保存完整 `isNullEdge` 决策，包括从 Facts
命中的决策。IR 修改通知会将幸存条目标记为需要重验证；单独的 oracle
维护通知也如此。下次命中时绕过 Facts 表重新计算这些 null 判断：

- 全部相同：复用 Final，并清除重验证标记。
- 任一不同：删除 Final，按原查询算法重新计算。

这是为了处理 LVI 清理/维护后的精度变化。一个结果的 IR 依赖未变，
不等于 LVI 在不同缓存状态下必然给出相同判断。重验证失败统计与
普通 Final miss 分开记录。当前仍需遍历幸存 Final 标记重验证，
并非失效成本完全与幸存条目数量无关。

## Analysis、PassManager 与调用方职责

`JavaTypeAnalysisResult` 持有聚合缓存，使用 `StableOracleEpoch`，查询
走 V2。其主动修改通知转发给聚合类，并执行 `LVI.clear()`；
DT 仍由调用方在下一次查询前更新。完整 `clear()` 清四类缓存及 LVI。
`-jeandle-verify-java-type-cache` 严格比较原版查询的完整结果，
不再因为预算截断或 PHI 环跳过验证。

普通 IR API 不会自动调用 JavaType 的失效方法。这里有两种不同入口：

1. `Result.invalidate(Changed)`：pass 主动调用，在修改前读取旧依赖图，
   选择性处理缓存。修改后在同一 pass 内再次查询需要这条路径。
2. `Result.invalidate(F, PA, Inv)`：PassManager 在 pass 返回后，依据
   `PreservedAnalyses` 判断整个分析结果是否应丢弃；JavaType、DT 或 LVI
   未被有效保留时，Result 失效。该回调不会从 IR 自动推导修改集。

因此跨 pass 保留要求 pass 正确维护缓存及其依赖，再声明 preserve。
不维护的 pass 可以让分析在边界失效；不能在修改后继续使用旧 Result，
再指望 pass 返回时的失效修复此前的查询。`DenseMap<Value*, ...>` 不会
自动监听 `setOperand`、metadata/attribute 修改、RAUW 或删除。

```cpp
// 修改既有指令：通知必须在修改前，旧 users 图此时仍可遍历。
Result.invalidate(*I);
I->setOperand(Index, NewValue);

// 插入或移动还要通知目标块，覆盖新出现的块内事实。
Value *Changed[] = {MovedInstruction, DestinationBlock};
Result.invalidate(Changed);
// 执行移动；若同时改变 CFG，改用 CFGChanged=true 并维护 DT。
```

所有语义修改通知走聚合/Analysis 入口。单独清某个子缓存只适用于容量
管理等不改变 IR/oracle 的操作。VM 类型环境或未知范围变化使用全清。
不得在查询进行中修改 IR、失效或逐出 SCC 求解中的条目。

## 验证与限制

四类缓存的语义回归覆盖 Defined 正/负证明复用及失效、Base 为空时的
Defined 处理、Final 的预算键和顶层发布、热子缓存下的依赖、无关分支
保留、oracle 维护重验证、真实 LVI 的 assume 修改和 Analysis 生命周期。
原版 JavaType、原版结构 CSV、直接 V1 实现保持不变。

验证结果：

- `cmake --build build/build-ninja-release --target JavaTypeQueryTests opt -j 2`
  构建通过。
- `JavaTypeQueryTests` 共 83 项，82 项通过；其中 V2 的 36 项全部通过，
  包含本次新增 9 项。唯一失败是既有直接 V1
  `JavaTypeQueryTest.CachedEntryNormalizesFreeze`，其测试及 V1 实现未改动。
- 全 `llvm/test/Jeandle` 的 lit 回归：807 项，782 通过、25 unsupported、
  0 失败。统计断言更新为边 Facts 双表计数及 Final 选择性保留行为。
- 真实 LVI 测试在同一 IR 上观察到冷查询 unknown，清理并分段预热后
  为 klass 9，再次清理后回到 unknown；Final 两次重验证失败均触发重算。
- Final 的 4100 值依赖测试只旁路该条目的发布，原版与 V2 查询结果相同，
  已有无关 Final 仍可命中。
- 以任务开始时 `e2d4a5a43` 及文件 SHA256 为基线审计。原版两文件、
  原版 CSV、V1 两文件和直接 V1 freeze 测试均未改动。
  `factsAt`、`phiValueType`、`computeEdgeGuard`、`computeNullEdge` 等
  12 个核心辅助函数体与任务基线逐字相同；Base/Defined 求解改动为
  缓存借用及统计接入，保留原求解与传播算法。

这里不宣称性能提升：Final 依赖
收集、反向索引、重验证及 Analysis 的 LVI 清理都有成本，需要真实负载
分阶段测量；目前也没有增加全局容量上限或基于访问频次的逐出机制。
