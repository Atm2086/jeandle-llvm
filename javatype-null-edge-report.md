# JavaType null 边判定（isNullEdge）优化执行报告 —— 方案 S1~S5

日期：2026-10-28
对象：分支 `test/javatype-cache-design`，HEAD `b121fa56710a`（工作树未提交，15 个已改文件 + 4 个新测试文件）
依据：《JavaType null 边判定（isNullEdge）的必要性与优化计划》附件（§4 方案、§5 落地顺序、§6 不变量、§7 交付物、§8 未覆盖项）

> 本报告中每一条数字都标注了来源：
> **【测】** = 本次实测（数据文件见 §11）；
> **【推】** = 由实测数据推算；
> **【猜】** = 计划或源码阅读给出的假设，本次未验证或被实测推翻。

---

## 0. 结论摘要

| 方案 | 状态 | 净效果【测】 | 结论 |
|---|---|---|---|
| 阶段0（4 个 stat + E0） | 完成 | 4 个计数器落地；E0 在合成 workload 上省掉 50–85% 的查询时间，在分支自带语料上只省 5.4% | 方向成立，但收益高度依赖语料 |
| S1 join unknown 短路 | 完成，保留 | 输出严格等价；耗时在噪声量级 | 零风险，保留 |
| S2 否定性预过滤 | 完成，保留 | 5 个合成 workload 的 oracle 调用 **960/16128/3968/960/7936 → 全部 0** | **本次最大收益**，保留 |
| S3 对称化支配链 + reference 兜底 | 完成，保留 | lit 失败 **10 → 9**（修好 `null-guard-forms.ll`）；差分 5/5；单测 47/48 | 修好了一个既有的精度不一致，保留 |
| S5 预算 + 开关 | 完成，保留 | 默认 128；5 个 workload 上预算均未触发（exhausted=0） | 提供编译时间上界，保留 |
| S4 第 1 级（块级三值状态） | 完成，保留 | 结构扫描与预过滤合并为一次支配链遍历；wide −38%~−56%、leaf −32%~−34%、virtual-cfg −13%~−30%、deep −5%、tce-cha ± | 保留，但收益幅度受噪声影响很大 |
| S4 第 2 级（LVI.getConstant） | **实现完成但默认关闭（等价回退）** | 成本比 ≈ 1:1；语料 LVI 时间 −7%~−22%，占总查询时间 <5% | **不启用**：收益不显著 + 会改变 oracle 的 true 答案集合，与 §4 总原则/§6 方向约束冲突 |
| S4a（V,From) 记忆化 | **已回退（代码已删）** | 5 个 workload 一致变慢（leaf +204%、wide +46%、virtual-cfg +49%） | 回退 |
| S6 / S7 | 未做 | — | 计划标注为可选，且需先有命中率数据；本次不做 |

**总计**：分支自带 lit 语料从 **10 条失败降到 9 条**，单测从 46/47 提升到 47/48（新增 1 个预算单测），没有引入任何新失败；合成 workload 上 null 边 oracle 的调用次数与 LVI 时间都归零。

---

## 1. 环境

见同目录 [javatype-null-edge-environment.json](file:///home/annya/playground/jeandle/jeandle/jeandle-llvm/javatype-null-edge-environment.json)。要点：

- 机器：32 核 Intel(R) 0000 / 64 GB / Arch Linux（**共享机器**，墙钟时间有明显噪声，见 §10）
- 构建：`build/build-ninja-release`，Release `-O3 -DNDEBUG`，assertions OFF，LLVM 22.0.0git
- 被测工具：`build/build-ninja-release/bin/opt`
- 所有命令必须 `env -u LD_LIBRARY_PATH`（agent 运行时注入的 libstdc++ 与系统不兼容；**上一版报告里的 "oracle 0 calls" 就是这个 harness bug 造成的误报，本次已更正**）

---

## 2. 基线（原始 HEAD，stash 后重建实测）

**【测】** `git stash` → 重编译 → 跑 lit/单测：

- lit：808 个用例，**10 条失败**：
  `condition-value-domain` / `context-phi-select` / `deep-condition` / `freeze-and-identity-patterns` / `frozen-obj-check` / `null-arm-type` / `null-guard-forms` / `switch-and-frozen-guards` / `trunc-value-domain` / `xor-both-traced`（全部在 `type-check-elimination/`）
- 单测：47 个用例，46 通过，1 失败：`JavaTypeQueryTest.CachedEntryNormalizesFreeze`（**既有失败**，与本次改动无关：本次对该文件的 diff 只在文件末尾追加了一个测试）

计划 §5 的判定标准写的是“lit 全绿”，但分支 HEAD 本身就不是全绿的（10 条既有失败）。因此本报告的判定标准改为 **“不新增失败，且失败集合是既有集合的子集”**。这一条在最终状态达成：9 = 10 − `null-guard-forms.ll`（S3 修好）。

---

## 3. 阶段 0：4 个 stat + E0（恒 false oracle）

### 3.1 落地的 4 个 stat【测】

`llvm/lib/Analysis/Jeandle/JavaTypeAnalysisStats.cpp` 新增（均为 `ALWAYS_ENABLED_STATISTIC`，热路径只累加线程局部计数器，每个顶层查询刷新一次，**不引入 atomic**）：

| stat | 含义 |
|---|---|
| `null-edge oracle calls` | oracle 调用总次数 |
| `null-edge oracle structural hits` | L0 结构快路解决的数量 |
| `null-edge oracle true answers` | 答“是”的次数（唯一能移除一条 join 臂的答案） |
| `null-edge oracle lvi time (ns)` | 累计 LVI 耗时 |

另有两个派生计数（`null-edge prefilter hits`、`null-edge oracle budget …`），分别在 S2、S5 引入。

### 3.2 四个 stat 的实际计数【测】

在 5 个合成 workload 上（**阶段 0 代码，即 S1 之前**；`e0.json` 的 `normal` 行）：

| workload | calls | structural | true | LVI ns | total query ns | LVI 占查询时间 |
|---|---|---|---|---|---|---|
| deep-static | 960 | 0 | 0 | 6,576,597 | 7,563,106 | **86.9%** |
| wide-static | 16,128 | 0 | 0 | 7,604,899 | 11,168,077 | **68.1%** |
| leaf-static | 3,968 | 0 | 0 | 3,204,708 | 5,916,114 | **54.2%** |
| tce-cha | 960 | 0 | 0 | 5,024,930 | 5,780,492 | **86.9%** |
| virtual-cfg | 7,936 | 0 | 0 | 7,349,679 | 16,289,454 | **45.1%** |

在分支自带 lit 语料上（**当前实现**，`oraclecorpus.py`，635/815 个 `.ll` 可运行，5 次重复取中位数）：

| 指标 | 值 |
|---|---|
| oracle calls | 20（分布在 11 个文件） |
| structural hits | 13（65%） |
| true answers | 10（50%） |
| LVI 时间 | 137,922 ns |
| prefilter hits | 289 |
| total query time | 6,424,407 ns |

**重要更正**：上一版报告曾称“真实语料 oracle 调用为 0”，那是 `budgetdist.sh` 覆盖不足 + `LD_LIBRARY_PATH` 导致的 harness bug。**分支自带测试确实会触发 oracle**：11 个文件、20 次调用。

### 3.3 E0（恒 false oracle）A/B 数据【测】

开关：`-jeandle-disable-null-edge-oracle`（等价于预算 = 0，保留全部边，恒 sound）。

**（a）阶段 0 代码 + 5 个合成 workload**（`e0.json`）：

| workload | normal query ns | E0 query ns | 变化 | E0 的 LVI ns |
|---|---|---|---|---|
| deep-static | 7,563,106 | 1,120,939 | **−85.2%** | 0 |
| wide-static | 11,168,077 | 5,529,087 | **−50.5%** | 0 |
| leaf-static | 5,916,114 | 1,762,060 | **−70.2%** | 0 |
| tce-cha | 5,780,492 | 1,128,212 | **−80.5%** | 0 |
| virtual-cfg | 16,289,454 | 8,138,193 | **−50.0%** | 0 |

**（b）当前实现 + 分支自带 lit 语料**（`oraclecorpus.py`，同样 5 次重复中位数）：

| 指标 | 正常 | E0 |
|---|---|---|
| oracle calls | 20 | 22 |
| structural hits | 13 | 0 |
| true answers | 10 | 0 |
| LVI ns | 137,922 | 0 |
| prefilter hits | 289 | 284 |
| total query ns | 6,424,407 | 6,075,355（**−5.4%**） |

**（c）E0 的精度代价【测】**：把 E0 开关注入 lit 全部用例（用 wrapper 脚本替换 `build/bin/opt`），lit 失败从 9 条增加到 **13 条**，新增 4 条：
`copy-specialization/osr-char-arraycopy-e2e.ll`、`java-type-metadata-recovery/recover-osr-checkcast-edge.ll`、`type-check-elimination/checkcast-dissolved.ll`、`type-check-elimination/frozen-null-edge.ll`。
即：**null 边判定是这 4 个用例折叠的前提**，判 null 边 = 精度问题，判错 = 正确性问题（与计划 §3.1 的判断一致）。

**结论**：E0 证明了“oracle 是主要成本”这一前提**在合成 workload 上成立**（省 50–85%），**在分支自带语料上不成立**（只省 5.4%）。收益取决于输入，**不能外推到生产**（计划 §8 的第一条未覆盖项）。

---

## 4. S1 —— `factsAt` join unknown 短路

**改动**（`llvm/lib/Analysis/Jeandle/CachedJavaType.cpp` `factsAt` 的前驱循环末尾）：

```cpp
    // Unknown is absorbing (typeUnion({}, X) == {}): once the accumulator is
    // unknown the remaining arms cannot change it. ...
    if (Joined.isUnknown())
      break;
```

参考引擎 `llvm/lib/IR/Jeandle/JavaType.cpp` 的 `factsAt` 施加同样的截断，保证两引擎一致。

**正确性**：累加器变成 unknown 后，`typeUnion` 的两条分支都只能产出空结果，输出与不短路严格相等；只依赖累计结果，不引入遍历顺序依赖（符合 §6 确定性约定）。

**测量**（`s1.json`，同会话交错 A/B，15 次重复取中位数）【测】：

| workload | s0 query ms | s1 query ms | 变化 | oracle calls | IR 是否一致 |
|---|---|---|---|---|---|
| deep-static | 9.104 | 9.490 | +4.2% | 960 → 960 | 是 |
| wide-static | 13.036 | 12.254 | −6.0% | 16128 → 16128 | 是 |
| leaf-static | 5.814 | 3.831 | **−34.1%** | 3968 → 3968 | 是 |
| tce-cha | 9.531 | 7.507 | −21.2% | 960 → 960 | 是 |
| virtual-cfg | 9.829 | 7.172 | −27.0% | 7936 → 7936 | 是 |

**评价**：零语义风险，输出严格等价；耗时收益在 leaf/virtual-cfg 上明显，其余在噪声量级。**保留**。

---

## 5. S2 —— 否定性预过滤（不碰 LVI 的反证式过滤）

**改动**：新增 `NullEdgePreFilter`（`llvm/include/llvm/IR/Jeandle/JavaType.h` + `llvm/lib/IR/Jeandle/JavaType.cpp` + `CachedJavaType.cpp`）。对每个被查询的值 V 懒构建 `UsedBlocks = { 存在 V 的 user 的块 }`；`isNullEdge(V, From, To)` 沿 From 的 idom 链上行，若链上没有块 ∈ UsedBlocks，则不可能存在支配该边的 null 证明 → **不问 oracle**（答 false、保留边）。

**正确性**：反证式过滤，只决定“不去问”，永不产生错误答案；最坏只损失精度（§6 “{} 是中性元，兜底必须是 keep edge”）。

**测量**（`s2.json`）【测】：

| workload | stage0 query ms | s2 query ms | 变化 | oracle calls | LVI ns | prefilter hits | IR |
|---|---|---|---|---|---|---|---|
| deep-static | 9.496 | 1.259 | **−86.7%** | 960 → **0** | 8,269,686 → **0** | 960 | 一致 |
| wide-static | 19.094 | 9.838 | **−48.5%** | 16128 → **0** | 13,007,753 → **0** | 16128 | 一致 |
| leaf-static | 5.084 | 3.928 | −22.8% | 3968 → **0** | 2,790,470 → **0** | 3968 | 一致 |
| tce-cha | 7.349 | 1.747 | **−76.2%** | 960 → **0** | 6,312,015 → **0** | 960 | 一致 |
| virtual-cfg | 10.758 | 9.730 | −9.6% | 7936 → **0** | 4,865,532 → **0** | 7936 | 一致 |

**评价**：**本次收益最大的一项**，而且收益是**确定性计数器**给出的（oracle 调用 100% 归零，不受墙钟噪声影响）。**保留**。
注意：**S2 的收益机制是“少问”，不是“答得更快”**——这正是 §4 总原则允许的方向。

---

## 6. S3 —— 对称化支配链扫描 + reference 兜底补齐

**改动**：
1. 新增 `jeandle::scanNullChain`（`llvm/lib/IR/Jeandle/JavaType.cpp`）：沿 idom 链扫描 V 的 null 测试，**返回三值** `Nullness::{Unknown, Null, NonNull}`，并保留 §6 要求的细节：
   - 两个后继相同 ⇒ 该分支不区分两种结果 ⇒ **无结论**（`NullBB != OtherBB` 判定）；
   - 同时被 null 侧与非 null 侧支配 ⇒ 矛盾 ⇒ Unknown；
   - 扫到 V 的定义块即停止；
   - 结论方向只依赖“V 是 SSA 值，nullness 不在边上改变”。
2. cached 引擎的 `isNullEdge` 改为：先问结构扫描（三值），Unknown 才问 oracle（`CachedJavaType.cpp`）。
3. reference 引擎补齐同一层兜底：`JavaType.cpp` 的 `isNullEdge` 与 `phiValueType` 改用 `nullnessAtBlockEnd`，与 cached 引擎对称。

**精度收益【测】**：lit 失败 **10 → 9**：`null-guard-forms.ll` 由失败转为通过（该用例的 threaded null 形状此前依赖 LVI，现在由支配链直接证明）。这正是计划 §5 阶段 3 的判定标准“差分验证不再出现 cached/reference 差异”。

**差分验证【测】**：`verify.sh`（`-jeandle-verify-java-type-cache`，cached vs reference，5 个 workload）→ 5/5 通过，stderr 0 字节。

**耗时【测】**（两次独立会话，`s3.json` / `s3b.json`）：

| workload | s2→s3（会话 A） | s2→s3（会话 B） |
|---|---|---|
| deep-static | +3.9% | +5.3% |
| wide-static | +4.9% | −47.6% |
| leaf-static | +114% | −6.4% |
| tce-cha | −9.5% | +6.1% |
| virtual-cfg | +1.0% | +35.4% |

**评价**：**同一份代码、两次会话的符号相反**，说明在当前机器上这 5 个合成 workload 的耗时噪声（±30%~50%）大于 S3 的真实效应。**能确认的是**：语义层（lit −1 条失败、差分 5/5、单测无新增失败）与确定性计数器（oracle 调用在 S2 之后已是 0，S3 不改变）。**保留**——它的价值是修精度一致性，不是提速。

---

## 7. S5 —— oracle 预算 + 开关

**改动**：
- 新选项 `-jeandle-max-null-edge-oracle-calls`（默认 128），语义与既有 `FactsBudget`/`PhiBudget` 同构；
- 预算耗尽 ⇒ 一律答 false（保留边，sound），并累加独立 stat（exhausted queries / skipped calls / max calls per query）；
- 预算 = 0 即 E0 形状，天然提供开关；
- **预算耗尽按“截断”处理**：`Info.SkipVerification` 置位、结果不发布为该上下文的最终答案（`CachedJavaType.cpp`），注释里写明了理由——截断后的查询回答的是另一个问题，严格相等不成立；
- 参考引擎用同一预算（两引擎从同一 option 取值，截断点可比）。

**测量【测】**（`s5.json`，同会话 s3 / s5（预算 128） / s5-b0（预算 0））：

| workload | s3 (ms) | s5 预算128 (ms) | s5 预算0 (ms) | 预算 exhausted | IR |
|---|---|---|---|---|---|
| deep-static | 1.390 | 1.794 | 1.815 | 0 | 一致 |
| wide-static | 6.219 | 8.707 | 11.171 | 0 | 一致 |
| leaf-static | 4.295 | 2.482 | 3.602 | 0 | 一致 |
| tce-cha | 1.829 | 1.812 | 1.760 | 0 | 一致 |
| virtual-cfg | 13.185 | 11.541 | 7.894 | 0 | 一致 |

**压力输入下的上界【测】**（`s5-worst.json`，关掉预过滤、只靠预算兜底）：

| workload | 无预算：calls / LVI ns | 预算 8：calls / LVI ns | 预算 0：calls / LVI ns |
|---|---|---|---|
| deep-static | 960 / 7,644,123 | 736 / 8,285,301 | 0 / 0 |
| wide-static | 16128 / 16,286,726 | 3808 / 6,224,900 | 0 / 0 |
| leaf-static | 3968 / 3,169,130 | 1760 / 2,741,189 | 0 / 0 |
| tce-cha | 960 / 8,014,495 | 736 / 7,577,289 | 0 / 0 |
| virtual-cfg | 7936 / 4,403,363 | 1888 / 3,572,004 | 0 / 0 |

**评价**：预算在 5 个正常 workload 上**从不触发**（exhausted=0），它的价值是给病态输入封顶；注意预算 8 时 LVI 时间并未显著下降（剩余调用恰是最贵的那批），真正封顶要靠预算 0。**保留**。

---

## 8. S4 —— 块级三值状态

### 8.1 第 1 级：结构扫描 + 预过滤合并为一次支配链遍历（保留）

**改动**：`NullEdgePreFilter::blockState(V, From, DT)` 一次支配链遍历同时回答两件事——结构三值结论（Null/NonNull/Unknown）与“链上是否存在 null 测试”（预过滤需要的那个否证条件）；`isNullEdge` 的顺序变成：

```
结构结论 Null   → 直接 true（V 是 SSA 值，块末尾为 null ⇒ 该块所有出边都是 null 边）
结构结论 NonNull→ 直接 false（省一次 oracle）
Unknown         → 预过滤否证 → 不问 oracle
                 → 预算检查 → 问 oracle（逐边 LVI）
```

**测量【测】**（两次会话，`s4.json` / `s4b.json`，均为 s5 → s4）：

| workload | 会话 A（s5→s4） | 会话 B（s5→s4） | oracle calls | IR |
|---|---|---|---|---|
| deep-static | 1.800 → 1.706（−5.2%） | 1.799 → 1.717（−4.6%） | 0 → 0 | 一致 |
| wide-static | 8.605 → 5.360（**−37.7%**） | 9.406 → 4.123（**−56.2%**） | 0 → 0 | 一致 |
| leaf-static | 3.714 → 2.520（**−32.2%**） | 4.265 → 2.818（**−33.9%**） | 0 → 0 | 一致 |
| tce-cha | 1.551 → 1.729（+11.5%） | 1.788 → 1.737（−2.9%） | 0 → 0 | 一致 |
| virtual-cfg | 12.275 → 10.660（−13.2%） | 10.628 → 7.459（**−29.8%**） | 0 → 0 | 一致 |

**profile 佐证【测】**（`perf-s4.data`）：null 边路径在 profile 中的占比由 **约 16.2% 降到约 6.0%**。

**评价**：两次会话方向一致（wide/leaf/virtual-cfg 均改善），幅度差异大；deep/tce-cha 在噪声量级。**保留**。

### 8.2 第 2 级：`LVI.getConstant` 块级查询 —— **默认关闭（等价回退）**

计划 §4 建议：“`LVI.getConstant(V, From->getTerminator())` → 返回 ConstantPointerNull 即 Null（一次 getValueInBlock，比 getValueOnEdge 少一层 getEdgeValueLocal）；……建议先只上 1，2 单独 A/B。”，§8 也把“第 2 级与第 3 级的成本比未测”列为未覆盖项。本次把它实现出来并单独测了。

**实现形态**：零 API 改动，落在 Analysis 层 `LVINullEdgeOracle::operator()` 内（L0 结构快路之后、逐边查询之前），由 `-jeandle-null-edge-block-lvi` 控制，**默认 false**；新增 3 个计数器（block-level queries / true answers / time）。

**聚焦 A/B【测】**（`oraclefocus.py`，11 个确实触达 oracle 的语料文件，25 次重复中位数）：

| 模式 | 边级 LVI ns | 块级 LVI ns | LVI 合计 | true 答案 | 结构命中 |
|---|---|---|---|---|---|
| 仅第 3 级（边级） | 215,189 | 0 | 215,189 | 10 | 13 |
| 第 2+3 级 | 3,285 | 165,116 | **168,401（−21.7%）** | 10 | 13 |

逐文件比值（块级 / 边级）：`frozen-null-edge` 0.84、`null-arm-type` 0.66、`checkcast-expanded-phi` 0.95、`recover-null-arm-vacuous` 0.50。

**全语料 A/B【测】**（635/815 文件，9 次重复中位数）：边级 176,793 → 3,282；块级 0 → 161,473；合计 **176,793 → 164,755（−7%）**；`total query time` 6,022,269 → 6,340,844（**噪声主导**）。

**语义验证【测】**（把默认值临时翻成 true 重建后测，随后恢复）：
- oracle 的 true 答案集合与关掉时**完全一致**（语料上 10 = 10；块级查询命中 7/20、其中 5 次答 true，全部是边级查询本来就会答 true 的那批）；
- lit 失败清单与基线**逐条相同**（9 条）；
- 单测 47/48，与基线相同；
- 差分验证 5/5 通过。

**技术解释（已对照 `llvm/lib/Analysis/LazyValueInfo.cpp` 源码核实）**：
`getValueOnEdge` → `getEdgeValue` = `getEdgeValueLocal` ∩ `getBlockValue(From)`；`getEdgeValueLocal` 对无条件终结指令直接返回 Overdefined（很便宜），**成本主体是块值求解（`getBlockValue` → `solveBlockValue` 的前驱递归）**。块级查询 `getConstant` 只省掉 `getEdgeValueLocal` 这一层，而这一层本来就很便宜 ⇒ **成本比必然 ≈ 1:1**，实测（0.5~0.95）验证了这一点。计划里“更便宜的 NonNull 方向”也不可行：`getConstant` 在 overdefined 时返回 nullptr（无法证非 null），而 `getPredicateAt` 在块值无法判定时会沿入边回推，可能更贵。

**处置与理由**：
1. **实测收益不显著**：LVI 时间 −7%（全语料）~ −21.7%（聚焦 11 文件），而 LVI 只占查询时间的 2–15%，即总查询时间收益 < 5%，且被机器噪声掩盖；
2. **与 §4 总原则 / §6 方向约束冲突**：块级证明严格强于边级证明——块值为 null 常量、而边局部细化使交为 Unknown 时，第 2 级会答 true 而第 3 级答 false。§4 原文：“任何让 oracle 更容易答 true 的改动都是健全性风险，一律不做”；§6 原文：“任何改动只能让 oracle 少答 true，不能多答 true”。虽然该证明本身是可靠的（skip 一条**已证明**为 null 的边是精确的，不是健全性问题），但它确实改变了 oracle 的 true 答案集合，与计划写死的方向约束冲突；
3. **默认关闭 = 交付行为与回退一致**，同时保留 `-jeandle-null-edge-block-lvi` 开关与 3 个计数器，任何人可复现上面这张 A/B 表。

**回退记录**：见 §9 表第 2 行。

### 8.3 S4a：(V, From) 块级状态记忆化 —— **已回退**

计划 §6（可选，需先有命中率数据）。实现后测量（`s4a.json`）：

| workload | s5 (ms) | s4a (ms) | 变化 |
|---|---|---|---|
| deep-static | 1.694 | 1.906 | +12.5% |
| wide-static | 7.119 | 10.383 | **+45.8%** |
| leaf-static | 1.521 | 4.620 | **+203.9%** |
| tce-cha | 1.535 | 1.921 | +25.1% |
| virtual-cfg | 7.088 | 10.538 | **+48.7%** |

5 个 workload 一致变慢（缓存维护成本 > 命中收益，且这些 workload 的 oracle 命中率为 0）⇒ **代码已删除，完全回退**。

---

## 9. 回退记录汇总

| # | 改动 | 回退方式 | 数据 | 理由 |
|---|---|---|---|---|
| 1 | S4a：(V, From) 块级状态记忆化 | **代码删除** | leaf +204%、wide +46%、virtual-cfg +49%、tce-cha +25%、deep +13%（§8.3） | 命中率 0，维护成本纯亏 |
| 2 | S4 第 2 级：LVI 块级查询 | **保留代码但默认关闭**（`-jeandle-null-edge-block-lvi` 默认 false） | 成本比 ≈1:1；LVI 时间 −7%~−21.7%，总查询时间收益 <5% 且被噪声掩盖（§8.2） | 收益不显著；会改变 oracle 的 true 答案集合，与 §4 总原则/§6 方向约束冲突 |

---

## 10. 测量方法与噪声（重要）

**【测】噪声标定**：同一份二进制、同一配置、两次独立测量（`noise.json`）：

| workload | 第一次 (ms) | 第二次 (ms) | 偏差 |
|---|---|---|---|
| deep-static | 7.295 | 7.780 | +6.6% |
| wide-static | 8.175 | 8.209 | +0.4% |
| leaf-static | 5.832 | 5.858 | +0.4% |
| tce-cha | 6.748 | 9.792 | **+45%** |
| virtual-cfg | 13.462 | 5.850 | **−57%** |

→ **结论**：5 个合成 workload 的墙钟/查询时间噪声可达 ±50%，**只有确定性计数器（oracle 调用数、structural/true 计数、prefilter 命中数、lit/单测结果、IR 是否一致）可以当作硬证据**；所有百分比仅作方向参考。

**方法**：
- `measure.py` 对 A/B 两臂**交错**执行（每轮随机顺序），报告 median 与 min，并逐轮比较优化后 IR 是否一致；
- 语料计数用 `oraclecorpus.py` 遍历 `llvm/test/Jeandle` 全部 `.ll`，从 RUN 行抽 `-passes=` 与 `-jeandle-vm-callback-log=`，加 `-jeandle-java-type-stats` 与 `print<java-type-analysis-summary>`，**多次重复取中位数**；
- 逐条改动之间的时间不可直接相减（跨会话漂移），因此每条改动的“前后表”都是**同会话内**的 A/B。

---

## 11. lit 覆盖

### 11.1 本次新增【测】

| 文件 | 内容 | 结果 |
|---|---|---|
| `llvm/test/Jeandle/type-check-elimination/null-edge-shapes.ll` | 菱形 null 检查，null 臂在**前**（`icmp eq` + 第 0 后继）与在**后**（`icmp ne` + 第 1 后继）各一例，验证与顺序无关；外加一个**不可达块**（不得污染 merge，且 live 折叠仍发生） | PASS |
| `llvm/test/Jeandle/type-check-elimination/null-edge-phi-null-arm.ll` | `phi [null, entry], [typed, latch]` | **XFAIL**（已知缺口，见下） |
| `llvm/test/Jeandle/type-check-elimination/Inputs/null-edge-shapes.cblog` | VM 回调日志 | — |

配套：`llvm/test/Jeandle/java-type-cache-reuse/pass-local-production-hook.ll`（前序会话新增，本次保留）。

### 11.2 覆盖矩阵（计划 §7 清单 vs 现状）

| 计划建议的形状 | 现状 | 备注 |
|---|---|---|
| 菱形 null 检查（null 臂在前 / 在后各一例） | ✅ 本次新增 | 顺序无关性已验证：两种写法都折叠 |
| threaded null 检查（条件与边不在同一块） | ✅ 已有 `null-threaded-edge.ll`、`null-guard-forms.ll` | `null-guard-forms.ll` 由 S3 修好 |
| 重复边（两个后继相同） | ✅ 已有 `instanceof-phi-duplicate-edge.ll` | 扫描代码里保留 “NullBB == OtherBB ⇒ 无结论” |
| `phi [null, entry], [obj, loop]` | ⚠️ **已知缺口**，新增 XFAIL 固定 | 见 §12 |
| 不可达块 / irreducible CFG | ✅ 不可达块：本次新增；irreducible：已有 `irreducible-smoke.ll` | |
| 预算耗尽路径 | ✅ 已有 `budget-exceeded.ll`（join 预算）+ 新增单测 `NullEdgeOracleBudgetLimitsTheOracle`（oracle 预算 = 1 / 0，并验证 reference 引擎行为一致） | 计划建议“单测最好能直接调 nullnessAtBlockEnd / isNullEdge”——本次是通过选项驱动，未直接暴露内部函数 |

### 11.3 已知缺口的定性【测】

`phi [null, entry], [obj, loop]` 目前仍被 null 臂污染成 unknown，查询不折叠。JavaType 尚无“已知 null”的表示（MaybeNull 未实现），所以 phi 无法像 CFG 边那样丢掉 null 臂。计划 §3.2 正是把这一形态列为“最典型的 oop PHI 形态”。分支上已有的 `null-arm-type.ll`（非循环版 lazy-init phi）**同样处于失败状态**（属既有 9 条失败之一），两者是同一个缺口。

新测试用 `XFAIL` 固定当前行为：缺口被修好时该用例会“意外通过”并提示更新。

---

## 12. 猜测 vs 实测

### 12.1 计划中的假设，本次被实测推翻或修正

| 计划假设 | 实测结果 | 结论 |
|---|---|---|
| 【猜】§3.3：“绝大多数 oracle 调用的产出是什么都没证明” | 合成 workload：**0/960 ~ 0/16128** 答 true，完全白干；分支语料：**10/20 答 true、13/20 命中结构快路** | **依语料而变**：在纯静态类型 workload 上成立，在分支语料上**不成立**（命中率 50%） |
| 【猜】§4：“L0 结构快路解决比例低”（进入方案的前提） | 分支语料上 L0 解决 **13/20 = 65%** | **被推翻**（至少在本分支语料上） |
| 【猜】§5 阶段 0 的进入条件：“NullOracleTrue 比例低且 Nanoseconds 占比高” | True 比例：合成 0%，语料 50%；Nanoseconds 占比：合成 45–87%，语料 2.1% | 合成 workload 满足，**分支语料不满足**（这正是 §3.4 的“先量化值不值”存在的意义） |
| 【猜】§4/S4：“块级查询比逐边查询便宜”（`getValueInBlock` 少一层 `getEdgeValueLocal`） | 成本比实测 ≈ **1:1**（逐文件 0.50–0.95） | **被推翻**：`getEdgeValueLocal` 很便宜，成本主体是块值求解 |
| 【猜】§5/S4：“NonNull 是最常见的答案” | 分支语料上 **true=10 / unknown=7**，即 Null 才是更常见的结论；NonNull 在本次语料上 **0 次** | **被推翻**（至少在本分支语料上） |
| 【猜】§4/S2：“S2 收益预期最大” | oracle 调用在 5 个 workload 上 **100% 归零** | **被证实**（本次唯一“确定性计数归零”的改动） |

### 12.2 本报告未能证实/证伪的（计划 §8 未覆盖项）

| 计划 §8 的未覆盖项 | 本次状态 |
|---|---|
| 用户 profiling 的输入与采样方式未知 | 仍未确认。**本次所有数字都来自合成 workload + 分支自带 lit 语料，不能外推到生产** |
| NullOracleTrue 的真实比例 | **已测**：合成 0%（0/28,952 次调用），分支语料 50%（10/20） |
| S4 第 2 级 vs 第 3 级成本比 | **已测**：≈ 1:1（§8.2） |
| “oracle 结果影响 pass 决策从而改变 IR”的反馈路径 | **已确认存在**：E0 打开后 4 个 lit 用例的折叠消失（§3.3c），即 oracle 的答案确实改变 pass 决策与最终 IR |

### 12.3 本次引入的新风险（未量化）

- S3 让 cached 引擎与 reference 引擎的 null 边判定对称化，**改变了部分 IR 的折叠结果**（`null-guard-forms.ll` 由不折叠变为折叠）。这是计划 §5 阶段 3 明确要求的“修精度一致性”，但它确实是一次行为变化，**没有在 JVM 真实编译上验证过收益/风险**。
- S5 的预算默认 128 是**拍的**（计划 §4 原文“默认值由 E0 的分布决定”）：实测 5 个 workload 上预算从未触发，因此 128 这个值既未被数据支持也未被数据否证；建议后续用病态输入扫描出触发点再定。
- S4 第 1 级把结构三值与预过滤合并成一次支配链遍历，**支配链长度变长时成本上升**（链上每块都要查一次 `Tested` 集合）。本次 workload 上没有出现回退，但这是 §5 “S2/S3/S4 都属于每条边多做一点结构判断的改动，正是最容易被压力输入反噬的形态”警告的风险点。

---

## 13. 交付物清单

| 计划 §7 交付物 | 位置 / 状态 |
|---|---|
| environment.json | [javatype-null-edge-environment.json](file:///home/annya/playground/jeandle/jeandle/jeandle-llvm/javatype-null-edge-environment.json) |
| 四个 stat 的实际计数 | §3.2 |
| 恒 false oracle 的 A/B 数据 | §3.3（合成 workload + 分支语料 + lit 精度代价） |
| 每条改动独立的 patch | 见下方说明 |
| 每条改动的前后测量表 | §4 / §5 / §6 / §7 / §8 |
| 回退记录 | §9（S4a 删除、S4-L2 默认关闭） |
| lit 覆盖建议 | §11（本次已落地 2 个新用例 + 覆盖矩阵） |
| 猜测 / 实测分离 | §12 |

**关于“独立的 patch”**：本次改动以**一个工作树**形式存在（未提交），`git diff` 即为完整改动；按改动归属拆分如下（每个 hunk 集合互相独立，可单独回退）：

| 方案 | 文件 | 位置 |
|---|---|---|
| 阶段0 | `llvm/lib/Analysis/Jeandle/JavaTypeAnalysisStats.cpp`、`llvm/include/llvm/Analysis/Jeandle/JavaTypeAnalysisStats.h` | 4 个 `ALWAYS_ENABLED_STATISTIC` + summary printer |
| 阶段0 | `llvm/lib/Analysis/Jeandle/LVINullEdgeOracle.cpp`、`llvm/include/llvm/Analysis/Jeandle/LVINullEdgeOracle.h` | 线程局部计数器 + `isNullEdgeOracleDisabled()` |
| S1 | `llvm/lib/Analysis/Jeandle/CachedJavaType.cpp`、`llvm/lib/IR/Jeandle/JavaType.cpp` | `factsAt` 前驱循环末尾的 `if (Joined.isUnknown()) break;`（两引擎各一处） |
| S2 | `llvm/include/llvm/IR/Jeandle/JavaType.h`、`llvm/lib/IR/Jeandle/JavaType.cpp`、`llvm/lib/Analysis/Jeandle/CachedJavaType.cpp` | `NullEdgePreFilter`（UsedBlocks + 否证）+ `isNullEdge` 中的预过滤分支 + `null-edge prefilter hits` 计数 |
| S3 | `llvm/lib/IR/Jeandle/JavaType.cpp`、`llvm/lib/Analysis/Jeandle/CachedJavaType.cpp` | `scanNullChain`（三值扫描）、`nullnessAtBlockEnd`、cached 引擎的 `isNullEdge` 改为先问结构扫描、reference 引擎补齐兜底 |
| S5 | `llvm/include/llvm/IR/Jeandle/JavaType.h`、`llvm/lib/IR/Jeandle/JavaType.cpp`、`llvm/lib/Analysis/Jeandle/CachedJavaType.cpp`、`llvm/lib/Analysis/Jeandle/JavaTypeAnalysisStats.cpp` | `NullEdgeOracleBudget`、`-jeandle-max-null-edge-oracle-calls`、exhausted/skipped/max 计数、`SkipVerification` 置位 |
| S4-L2（默认关闭） | `llvm/lib/Analysis/Jeandle/LVINullEdgeOracle.cpp`、`llvm/include/llvm/Analysis/Jeandle/LVINullEdgeOracle.h`、`llvm/lib/Analysis/Jeandle/JavaTypeAnalysisStats.cpp` | `-jeandle-null-edge-block-lvi`（默认 false）+ 3 个块级计数器 |
| 统计刷新 | `llvm/lib/Transforms/Jeandle/{TypeCheckElimination,CHADevirtualization,ConstantFieldFolding,RecoverTypeInfo}.cpp` | 每个 pass 查询前刷新线程局部计数器 |
| 测试 | `llvm/unittests/IR/JavaTypeQueryTest.cpp`、`llvm/unittests/IR/JavaTypeCacheReuseTestSupport.h` | 新增 `NullEdgeOracleBudgetLimitsTheOracle`（预算 = 1 / 0，校验 cached 与 reference 一致） |

---

## 14. 复现方法

```bash
cd /home/annya/playground/jeandle/jeandle/jeandle-llvm
export PATH=/home/annya/playground/jeandle/jeandle/tools/bin:$PATH   # g++ 16.1.1

# 构建
cd build/build-ninja-release && env -u LD_LIBRARY_PATH ninja opt JavaTypeQueryTests

# lit
env -u LD_LIBRARY_PATH build/build-ninja-release/bin/llvm-lit -s --timeout=300 \
    build/build-ninja-release/test/Jeandle

# 单测
env -u LD_LIBRARY_PATH build/build-ninja-release/unittests/IR/JavaTypeQueryTests

# 差分验证（cached vs reference）
env -u LD_LIBRARY_PATH bash /tmp/jt-measure/verify.sh build/build-ninja-release/bin/opt /tmp/jt-e0/try2

# 语料 oracle 计数（5 次重复取中位数）
env -u LD_LIBRARY_PATH python3 /tmp/jt-measure/oraclecorpus.py \
    --binary build/build-ninja-release/bin/opt --repeats 5

# 聚焦 A/B（S4 第 2 级）
env -u LD_LIBRARY_PATH python3 /tmp/jt-measure/oraclefocus.py 25

# E0 开关
env -u LD_LIBRARY_PATH build/build-ninja-release/bin/opt -S \
    -passes="function(type-check-elimination),print<java-type-analysis-summary>" \
    -jeandle-disable-null-edge-oracle=true \
    -jeandle-vm-callback-log=llvm/test/Jeandle/type-check-elimination/Inputs/null-threaded-edge.cblog \
    llvm/test/Jeandle/type-check-elimination/null-threaded-edge.ll
```

**注意**：所有命令必须 `env -u LD_LIBRARY_PATH`（agent 运行时注入的 libstdc++ 与系统 glibc 不兼容，会让 `opt` 无法启动，进而让计数器读成 0 —— 上一版报告的 “oracle 0 calls” 误报即源于此）。

---

## 15. 下一步建议（按性价比）

1. **补一条能触发 null 臂 vacuous 的 phi 语义**（JavaType 的 MaybeNull 能力），把 `null-arm-type.ll` 与本次新增的 XFAIL 用例一起转绿 —— 这是计划 §3.2 认定的“最典型的 oop PHI 形态”，且当前是分支上 9 条既有失败中的 1 条的根因。
2. **在 JVM 真实编译输入上重跑一遍 oracle 分布**：本报告的所有结论都建立在合成 workload 与 lit 语料上；§12.1 显示 oracle 的命中率在两类语料之间相差极大，真实输入上的分布才是方案取舍的依据。
3. **给 S5 的预算默认值找数据**：用病态输入扫描出预算触发点，再决定 128 是否合适（当前值是拍的）。
4. **S6/S7 暂不动**：S6 需要 (V, From) 命中率数据（目前为 0，S4a 已证伪）；S7 需要先测 `typeUnion` 触发 VM 回调 `GetCommonSuperKlass` 的成本是否低于 LVI。
