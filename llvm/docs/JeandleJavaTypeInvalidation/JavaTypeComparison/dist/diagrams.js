/* Local, dependency-free overview diagrams. Both paths are from this checkout. */
(() => {
  'use strict';

  const escape = value => String(value).replace(/[&<>"']/g, character => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  })[character]);

  const text = (x, y, content, className = 'diagram-label', anchor = 'middle') =>
    `<text x="${x}" y="${y}" class="${className}" text-anchor="${anchor}">${escape(content)}</text>`;

  const lines = (x, y, contents, className = 'diagram-label', gap = 23, anchor = 'middle') =>
    contents.map((content, index) => text(x, y + index * gap, content, className, anchor)).join('');

  const node = (x, y, width, height, contents, options = {}) => {
    const gap = options.gap || 23;
    const baseline = y + height / 2 - (contents.length - 1) * gap / 2 + 5;
    return `<g class="diagram-node"><rect x="${x}" y="${y}" width="${width}" height="${height}" rx="${options.round || 5}"/>${lines(x + width / 2, baseline, contents, options.className || 'diagram-label', gap)}</g>`;
  };

  const diamond = (x, y, width, height, contents) =>
    `<g class="diagram-node"><path d="M ${x} ${y - height / 2} L ${x + width / 2} ${y} L ${x} ${y + height / 2} L ${x - width / 2} ${y} Z"/>${lines(x, y - (contents.length - 1) * 11 + 5, contents, 'diagram-label', 22)}</g>`;

  const edge = (path, marker, dashed = false) =>
    `<path class="diagram-edge${dashed ? ' diagram-dashed' : ''}" d="${path}" marker-end="url(#${marker})"/>`;

  const line = (x1, y1, x2, y2) =>
    `<line class="diagram-divider" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"/>`;

  const frame = (x, y, width, height, title, subtitle) =>
    `<rect class="diagram-frame" x="${x}" y="${y}" width="${width}" height="${height}" rx="7"/>${text(x + 20, y + 31, title, 'diagram-heading', 'start')}${subtitle ? text(x + 20, y + 56, subtitle, 'diagram-small', 'start') : ''}`;

  const style = `
    .diagram-svg { display:block; width:100%; height:auto; background:#fff; }
    .diagram-svg text { font-family:Inter,"Noto Sans SC","Microsoft YaHei",system-ui,sans-serif; }
    .diagram-svg .diagram-label { font-size:16px; fill:#222; }
    .diagram-svg .diagram-small { font-size:14px; fill:#555; }
    .diagram-svg .diagram-code { font-family:"SFMono-Regular",Consolas,"Liberation Mono",monospace; font-size:14px; fill:#333; }
    .diagram-svg .diagram-heading { font-size:18px; font-weight:650; fill:#151515; }
    .diagram-svg .diagram-title { font-size:20px; font-weight:700; fill:#111; }
    .diagram-svg .diagram-node rect, .diagram-svg .diagram-node path { fill:#fff; stroke:#454545; stroke-width:1.5; }
    .diagram-svg .diagram-frame { fill:#fff; stroke:#7a7a7a; stroke-width:1.3; }
    .diagram-svg .diagram-group { fill:none; stroke:#999; stroke-width:1; stroke-dasharray:4 4; }
    .diagram-svg .diagram-edge { fill:none; stroke:#555; stroke-width:1.5; stroke-linejoin:round; }
    .diagram-svg .diagram-dashed { stroke-dasharray:6 4; }
    .diagram-svg .diagram-divider { stroke:#d6d6d6; stroke-width:1; }
  `;

  const svg = (name, height, title, description, body) =>
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 ${height}" class="diagram-svg" role="img" aria-labelledby="${name}-title ${name}-description"><title id="${name}-title">${escape(title)}</title><desc id="${name}-description">${escape(description)}</desc><style>${style}</style><defs><marker id="${name}-arrow" markerWidth="7" markerHeight="7" refX="6" refY="3.5" orient="auto"><path d="M0,0 L7,3.5 L0,7 Z" fill="#555"/></marker></defs>${body}</svg>`;

  function queryDiagram() {
    const arrow = 'query-overview-arrow';
    const parts = [
      text(330, 26, '旧 Cached · getCachedJavaType', 'diagram-title'),
      text(970, 26, '新 JavaType · getJavaType', 'diagram-title'),
      line(640, 42, 640, 1120),

      // Control-flow edges are drawn first so labels and nodes remain legible.
      edge('M330 90 V100', arrow),
      edge('M330 144 V160', arrow),
      edge('M970 90 V160', arrow),
      edge('M165 205 H115 V300', arrow),
      edge('M805 205 H755 V300', arrow),
      edge('M115 400 V440', arrow),
      edge('M755 400 V440', arrow),
      edge('M330 250 V275 H425 V300', arrow),
      edge('M970 250 V275 H1065 V300', arrow),
      edge('M425 380 V420', arrow),
      edge('M567.5 340 H625 V974 H610', arrow),
      edge('M425 474 V510', arrow),
      edge('M425 600 V645', arrow),
      edge('M425 720 V760', arrow),
      edge('M425 810 V850', arrow),
      edge('M425 905 V950', arrow),
      edge('M1065 370 V430', arrow),
      edge('M1065 484 V525', arrow),
      edge('M1065 595 V645', arrow),
      edge('M1065 720 V760', arrow),
      edge('M1065 810 V850', arrow),

      text(136, 192, '否', 'diagram-small'),
      text(776, 192, '否', 'diagram-small'),
      text(357, 270, '是', 'diagram-small'),
      text(997, 270, '是', 'diagram-small'),
      text(597, 328, '命中', 'diagram-small'),
      text(465, 406, '未命中', 'diagram-small'),

      node(105, 40, 450, 50, ['getCachedJavaType(V, DT, Context, …)']),
      node(175, 100, 310, 44, ['stripCastsAndFreeze(V)']),
      node(745, 40, 450, 50, ['getJavaType(V, DT, Context, …)']),
      diamond(330, 205, 330, 90, ['DT 与 Context', '都存在？']),
      diamond(970, 205, 330, 90, ['DT 与 Context', '都存在？']),

      node(25, 300, 180, 100, ['getBaseJavaType', '递归求 Base', 'Cache.BaseTypes'], { gap: 23, className: 'diagram-small' }),
      node(665, 300, 180, 100, ['getBaseJavaType', 'SCC 求 Base', '局部 Base / Defined'], { gap: 23, className: 'diagram-small' }),
      node(25, 440, 180, 46, ['返回 Base'], { round: 18 }),
      node(665, 440, 180, 46, ['返回 Base'], { round: 18 }),
      text(115, 513, '不走上下文路径', 'diagram-small'),
      text(755, 513, '本次调用结束即释放', 'diagram-small'),

      diamond(425, 340, 285, 80, ['FinalTypes', '有 (V, ContextBB)？']),
      node(240, 420, 370, 54, ['构造 CachedJavaTypeQueryContext Q', '引用外部三张缓存'], { className: 'diagram-small', gap: 21 }),
      node(240, 510, 370, 90, ['valueDimension(V, Q)', 'PHI → phiValueType；其余 → Base', '复用外部 BaseTypes'], { gap: 24 }),
      node(240, 645, 370, 75, ['factsAt(V, ContextBB, Q)', 'PathFacts + 本次 Processed', '递归前驱、边事实与空边判断'], { className: 'diagram-small', gap: 21 }),
      node(240, 760, 370, 50, ['Result = typeIntersect(值维度, 路径事实)'], { className: 'diagram-small' }),
      node(240, 850, 370, 55, ['未截断 → 写回 FinalTypes', '发生截断 → 跳过发布'], { gap: 23 }),
      node(240, 950, 370, 48, ['返回 JavaType'], { round: 18 }),

      node(880, 300, 370, 70, ['构造 JavaTypeQueryContext Q', '本次查询持有 memo 与遍历状态'], { gap: 24 }),
      `<rect class="diagram-group" x="870" y="396" width="390" height="217" rx="7"/>`,
      text(886, 417, 'valueDimension', 'diagram-small', 'start'),
      node(885, 430, 360, 54, ['getBaseJavaType → 完整 Base', 'Q.BaseTypes · Q.DefinedValues'], { className: 'diagram-small', gap: 22 }),
      node(885, 525, 360, 70, ['需要时传播值维度；再与 Base 相交', 'PHI / select / identity / 合法 freeze', '完成结果按条件存入 Q.ValueTypes'], { className: 'diagram-small', gap: 21 }),
      node(880, 645, 370, 75, ['factsAt(V, ContextBB, Q)', '本次 Q.Processed', '递归前驱、边事实与空边判断'], { className: 'diagram-small', gap: 21 }),
      node(880, 760, 370, 50, ['Result = typeIntersect(值维度, 路径事实)'], { className: 'diagram-small' }),
      node(880, 850, 370, 55, ['返回 JavaType'], { round: 18 }),
      lines(1065, 951, ['Q 的所有 memo 在本次查询结束后释放。', '原版入口没有跨查询的 FinalTypes。'], 'diagram-small', 24),

      text(640, 1025, '省略部分提前返回：新值维度无可传播输入、Base 已精确、遇环或预算耗尽时，可直接返回 Base。', 'diagram-small'),
      node(25, 1045, 590, 75, ['旧 Base 求解：递归 Raw / Case + PHI 访问栈', 'CycleHit 限制中间结果缓存；旧入口直接剥离 freeze。'], { className: 'diagram-label', gap: 26 }),
      node(665, 1045, 590, 75, ['新 Base 求解：显式 DFS → SCC → 分量内工作队列', 'freeze 透传需要 definedness 证明；DefinedMemo 复用证明。'], { className: 'diagram-label', gap: 26 })
    ];
    return svg('query-overview', 1140, '新旧 JavaType 查询控制流程对比',
      '左侧为当前 Cached 引擎的旧查询骨架，右侧为当前 getJavaType。菱形表示条件，箭头表示执行方向。两版均有独立的无上下文 Base 返回路径。旧版可直接命中外部 FinalTypes，新版创建查询级 Q，通过完整 SCC Base 和值维度传播，再与上下文路径事实相交。', parts.join(''));
  }

  function storageDiagram() {
    const arrow = 'storage-overview-arrow';
    const row = (x, y, name, representation, note) =>
      text(x, y, name, 'diagram-label', 'start') +
      text(x + 170, y, representation, 'diagram-code', 'start') +
      (note ? text(x + 170, y + 20, note, 'diagram-small', 'start') : '');

    const parts = [
      text(320, 28, '旧 Cached · 外部缓存 + 单次查询状态', 'diagram-title'),
      text(960, 28, '新 JavaType · 单次 Q + 求解临时状态', 'diagram-title'),
      line(640, 48, 640, 675),

      frame(25, 65, 590, 240, 'JavaTypeCache · 跨多次查询保留', '由外部持有；IR 改写通过 invalidate / clear 管理'),
      line(45, 134, 595, 134),
      row(45, 164, 'BaseTypes', 'Value* → JavaType', '无上下文 Base 结果'),
      row(45, 214, 'PathFacts', '(Value*, BasicBlock*) → JavaType', '路径 / 基本块事实'),
      row(45, 264, 'FinalTypes', '(Value*, BasicBlock*) → JavaType', '值维度与路径事实相交后的结果'),
      edge('M140 355 V307', arrow, true),
      edge('M320 355 V307', arrow, true),
      edge('M500 355 V307', arrow, true),
      `<rect x="174" y="322" width="292" height="23" fill="#fff"/>`,
      text(320, 339, 'Q 引用 / 读写外部三张 DenseMap', 'diagram-small'),

      frame(25, 355, 590, 310, 'CachedJavaTypeQueryContext · 单次查询', 'BaseTypes & · PathFacts * · FinalTypes const *'),
      line(45, 424, 595, 424),
      row(45, 453, 'Processed', '(Value*, BasicBlock*) → optional<JavaType>'),
      text(215, 475, 'nullopt = 求值中；有值 = 已完成', 'diagram-small', 'start'),
      row(45, 515, 'PhiVisited', 'SmallPtrSet<const PHINode*>'),
      row(45, 555, 'DT / NullOracle', 'DominatorTree& / 回调'),
      text(45, 595, '预算、截断标记、CycleHit 标记、命中 / 未命中统计', 'diagram-small', 'start'),
      text(45, 636, '查询结束：销毁 Q 本地状态；外层三张表可继续复用。', 'diagram-small', 'start'),

      frame(665, 65, 590, 390, 'JavaTypeQueryContext · 单次查询内持有', '以下 memo 随 Q 创建，随 Q 销毁；DT 仍是引用'),
      line(685, 134, 1235, 134),
      row(685, 162, 'BaseTypes', 'Value* → JavaType', '已完成的 Base 结果'),
      row(685, 210, 'ValueTypes', 'Value* → JavaType', '值维度结果；不是 (V, ContextBB) 的 FinalTypes'),
      row(685, 258, 'DefinedValues', 'Value* → bool', 'DefinedMemo；false 表示未证明 defined'),
      row(685, 306, 'Processed', '(Value*, BasicBlock*) → optional<JavaType>'),
      row(685, 344, 'DT / NullOracle', 'DominatorTree& / 回调'),
      text(685, 386, 'PhiVisited / ValueVisited / ValueCycles / 预算 / ConditionDepth', 'diagram-small', 'start'),
      text(685, 430, '这四张表都是 DenseMap；没有外部 PathFacts / FinalTypes。', 'diagram-small', 'start'),
      edge('M960 455 V505', arrow),
      text(985, 486, '求解时临时创建', 'diagram-small', 'start'),
      frame(665, 505, 590, 160, 'Base SCC 求解器 · 单次求解临时状态', ''),
      text(685, 563, 'Nodes → BaseTypeNode：Inputs、Seed、Result、遍历索引', 'diagram-small', 'start'),
      text(685, 594, 'Frames / OpenNodes / Roots：显式 DFS 与 SCC 识别', 'diagram-small', 'start'),
      text(685, 625, 'Users / Pending / Queued：分量内的工作队列传播', 'diagram-small', 'start'),

      edge('M320 665 V715', arrow),
      edge('M960 665 V715', arrow),
      text(640, 699, '两条查询路径使用同一种返回结构', 'diagram-small'),
      frame(25, 715, 1230, 90, 'JavaType', ''),
      text(225, 748, 'Klass · Exact · Interfaces · ExcludedKlasses', 'diagram-code', 'start'),
      text(45, 780, '不编码 nullability；本图比较当前仓库中的两条查询路径，它们共用 JavaType 和基础类型运算。', 'diagram-small', 'start'),
      line(27, 828, 69, 828),
      text(78, 833, '实线框：持有的数据 / 状态；实线箭头：调用或产生结果', 'diagram-small', 'start'),
      `<path class="diagram-edge diagram-dashed" d="M755 828 H797"/>`,
      text(807, 833, '虚线箭头：引用外部缓存', 'diagram-small', 'start')
    ];

    return svg('storage-overview', 850, '新旧 JavaType 数据结构及生命周期对比',
      '左侧 JavaTypeCache 持有跨查询复用的 BaseTypes、PathFacts、FinalTypes；Cached 查询上下文引用它们，并单独持有 Processed 和递归状态。右侧 JavaTypeQueryContext 在一次查询内持有 BaseTypes、ValueTypes、DefinedValues、Processed，SCC 容器只存在于一次 Base 求解中。ValueTypes 与旧 FinalTypes 的键和语义均不同。两版返回同一种 JavaType。', parts.join(''));
  }

  function mount() {
    const queryHost = document.getElementById('query-diagram');
    const storageHost = document.getElementById('storage-diagram');
    if (queryHost) queryHost.innerHTML = queryDiagram();
    if (storageHost) storageHost.innerHTML = storageDiagram();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', mount, { once: true });
  } else {
    mount();
  }
})();
