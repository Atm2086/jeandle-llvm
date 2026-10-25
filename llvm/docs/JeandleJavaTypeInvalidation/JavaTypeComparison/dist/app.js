"use strict";

// The walkthrough is explanatory data, not an execution trace or benchmark.
const steps = [
  {kicker:"显式遍历", title:"发现 p，求解外部输入 a", description:"从 p 开始，建立 BaseTypeNode。用 Frames 保存遍历进度；a 是叶子，先得到 Cat 并完成缓存。", state:"Frames    [ p ]\nOpenNodes [ p ]\nRoots     [ p ]\n已完成     a → Cat", takeaway:"叶子先完成，再继续遍历兄弟依赖。", active:["p"], done:["a"], edges:["pa"], cycle:false, types:["待求解","待求解"]},
  {kicker:"继续发现依赖", title:"进入 q，求解外部输入 b", description:"p 还依赖 q，所以继续 Discover(q)。q 的外部输入 b 求出 Dog；此时 p、q 都还没有完成。", state:"Frames    [ p, q ]\nOpenNodes [ p, q ]\nRoots     [ p, q ]\n已完成     a → Cat, b → Dog", takeaway:"Frames 记录遍历位置，OpenNodes 保留未完成分量中的节点。", active:["q"], done:["a","b"], edges:["pq","qb"], cycle:false, types:["待求解","待求解"]},
  {kicker:"识别强连通分量", title:"q 再遇到 p，合并候选根", description:"q 依赖仍在 OpenNodes 中的 p。沿 Roots 弹掉比 p 更新的候选根，识别 {p, q} 属于同一个依赖环。", state:"回边       q → p\nOpenNodes [ p, q ]\nRoots     [ p ]\n分量       { p, q }", takeaway:"遇到环不再仅靠递归截断；把互相依赖的成员归到一起。", active:["p","q"], done:["a","b"], edges:["pq","qp"], cycle:true, types:["未完成","未完成"]},
  {kicker:"建立分量初始界限", title:"汇合环外输入，结合独立 Seed", description:"分量完成发现后，仅汇合已完成的外部输入。此例 Cat 与 Dog 合并为 Animal；p、q 没有额外 Seed，所以初始界限都为 Animal。", state:"外部输入   a: Cat, b: Dog\n外部汇合   Animal\nBaseMemo  p: Animal（内部估计）\n          q: Animal（内部估计）", takeaway:"有 freeze 的环还必须检查其输入的 definedness；有独立 Seed 的成员可以得到更强界限。", active:["p","q"], done:["a","b"], edges:["pa","qb"], cycle:true, types:["Animal*","Animal*"]},
  {kicker:"工作队列迭代", title:"重算成员，变化则通知 Users", description:"按成员的输入计算 Derived，与 Seed、Previous 相交。只有结果变化，才把依赖它的成员重新放进 Pending；本例的 Animal 已经稳定。", state:"Derived = join(输入) ∩ Seed\nRefined = Previous ∩ Derived\n结果变化 → Users 入队\nPending  → 空", takeaway:"这是单调收紧，不是无限绕环递归；不同成员有 Seed 时，稳定结果未必相同。", active:["p","q"], done:["a","b"], edges:["pq","qp"], cycle:true, types:["Animal*","Animal*"]},
  {kicker:"完成并复用", title:"整个分量完成，继续上层求值", description:"工作队列为空，p、q 的结果完成。之后当前 memo 中对任一成员的查询都能直接复用；原版 Q 结束时，这些查询内结果随之销毁。", state:"BaseMemo  a → Cat\n          b → Dog\n          p → Animal（完成）\n          q → Animal（完成）", takeaway:"星号表示内部迭代估计。源码在收敛前已写入 memo，但不会把未完成分量交给上层继续使用。", active:[], done:["a","b","p","q"], edges:[], cycle:true, types:["Animal","Animal"]}
];
let stepIndex = 0;
const dots = document.getElementById("step-dots");
steps.forEach((step, index) => {
  const button = document.createElement("button");
  button.textContent = String(index + 1);
  button.setAttribute("aria-label", `第 ${index + 1} 步：${step.title}`);
  button.addEventListener("click", () => renderStep(index));
  dots.append(button);
});
function renderStep(index) {
  stepIndex = Math.max(0, Math.min(steps.length - 1, index));
  const step = steps[stepIndex];
  document.getElementById("step-count").textContent = `${String(stepIndex + 1).padStart(2,"0")} / 06`;
  ["kicker","title","description","state","takeaway"].forEach(key => {
    document.getElementById(`step-${key}`).textContent = step[key];
  });
  ["a","b","p","q"].forEach(node => {
    document.getElementById(`node-${node}`).classList.toggle("active", step.active.includes(node));
    document.getElementById(`node-${node}`).classList.toggle("done", step.done.includes(node));
  });
  ["pa","qb","pq","qp"].forEach(edge => {
    document.getElementById(`edge-${edge}`).classList.toggle("active", step.edges.includes(edge));
  });
  document.getElementById("scc-boundary").classList.toggle("active",step.cycle);
  document.getElementById("type-p").textContent = step.types[0];
  document.getElementById("type-q").textContent = step.types[1];
  document.getElementById("scc-prev").disabled = stepIndex === 0;
  document.getElementById("scc-next").disabled = stepIndex === steps.length - 1;
  [...dots.children].forEach((button, index) => {
    button.classList.toggle("active",index === stepIndex);
    button.setAttribute("aria-pressed",String(index === stepIndex));
  });
}
document.getElementById("scc-prev").addEventListener("click", () => renderStep(stepIndex - 1));
document.getElementById("scc-next").addEventListener("click", () => renderStep(stepIndex + 1));
document.getElementById("scc-reset").addEventListener("click", () => renderStep(0));
renderStep(0);

document.querySelectorAll("[data-filter]").forEach(button => {
  button.addEventListener("click", () => {
    document.querySelectorAll("[data-filter]").forEach(other => {
      other.classList.toggle("selected", other === button);
      other.setAttribute("aria-pressed",String(other === button));
    });
    let visible = 0;
    document.querySelectorAll("tr[data-kind]").forEach(row => {
      row.hidden = button.dataset.filter !== "all" && row.dataset.kind !== button.dataset.filter;
      if (!row.hidden) visible++;
    });
    document.getElementById("data-count").textContent = `${visible} 项`;
  });
});

document.querySelectorAll('input[name="defined-case"]').forEach(input => {
  input.addEventListener("change", () => {
    const proven = input.value === "defined";
    document.getElementById("freeze-code").textContent = proven ? "x : ptr noundef, JavaKlass = Cat\nf = freeze x" : "x : ptr, JavaKlass = Cat\nf = freeze x";
    const badge = document.getElementById("freeze-badge");
    badge.classList.toggle("warning",!proven);
    badge.textContent = proven ? "可以透传 · Memo[x] = true" : "不能安全透传";
    document.getElementById("freeze-result").textContent = proven ? "f 的 Base 类型：Cat" : "f 的 Base 类型：unknown";
    document.getElementById("freeze-explanation").textContent = proven ? "noundef 提供输入 x 已定义的证明。此时 freeze(x) 保持 x，Base 与身份匹配都可以继续沿 x 推导。" : "仅有 Cat 类型约束不足以证明 x 已定义。x 如果是 poison，freeze 可产生其他对象引用；不能沿用 Cat。";
  });
});

const dialog = document.getElementById("source-dialog");
document.querySelectorAll("[data-source]").forEach(button => {
  button.addEventListener("click", () => {
    const data = window.JAVA_TYPE_SOURCES?.[button.dataset.source];
    if (!data) return;
    document.getElementById("source-title").textContent = data.title;
    const body = document.getElementById("source-body");
    body.replaceChildren();
    data.excerpts.forEach(excerpt => {
      const section = document.createElement("section");
      section.className = "source-block";
      const heading = document.createElement("h3");
      heading.textContent = `${excerpt.path} : ${excerpt.start}–${excerpt.end}`;
      const pre = document.createElement("pre");
      excerpt.code.split("\n").forEach((text,index) => {
        const line = document.createElement("span");
        line.className = "source-line";
        const number = document.createElement("span");
        number.className = "line-no";
        number.textContent = String(excerpt.start + index);
        number.setAttribute("aria-hidden","true");
        line.append(number,document.createTextNode(text || " "));
        pre.append(line);
      });
      section.append(heading,pre);
      body.append(section);
    });
    dialog.showModal();
    dialog.scrollTop = 0;
  });
});
document.getElementById("source-close").addEventListener("click", () => dialog.close());
dialog.addEventListener("click", event => {
  if (event.target !== dialog) return;
  const rect = dialog.getBoundingClientRect();
  if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) dialog.close();
});

// Keep the overall diagrams above a separate, keyboard-accessible question area.
const answerTabs = [...document.querySelectorAll('[role="tab"][data-answer]')];
function showAnswer(tab, moveFocus = false) {
  answerTabs.forEach(other => {
    const selected = other === tab;
    other.setAttribute("aria-selected", String(selected));
    other.tabIndex = selected ? 0 : -1;
    document.getElementById(other.getAttribute("aria-controls")).hidden = !selected;
  });
  if (moveFocus) tab.focus();
}
answerTabs.forEach((tab, index) => {
  tab.addEventListener("click", () => showAnswer(tab));
  tab.addEventListener("keydown", event => {
    let next = index;
    if (event.key === "ArrowRight") next = (index + 1) % answerTabs.length;
    else if (event.key === "ArrowLeft") next = (index + answerTabs.length - 1) % answerTabs.length;
    else if (event.key === "Home") next = 0;
    else if (event.key === "End") next = answerTabs.length - 1;
    else return;
    event.preventDefault();
    showAnswer(answerTabs[next], true);
  });
});

document.querySelectorAll(".diagram-figure").forEach(figure => {
  const viewport = figure.querySelector(".diagram-viewport");
  const canvas = figure.querySelector(".diagram-canvas");
  const output = figure.querySelector("output");
  let scale = 1;
  const redraw = () => {
    // Preserve readable text on small screens; the figure scrolls horizontally.
    canvas.style.width = `${Math.max(viewport.clientWidth, 960) * scale}px`;
    output.textContent = `${Math.round(scale * 100)}%`;
    figure.querySelector('[data-zoom="out"]').disabled = scale <= .5;
    figure.querySelector('[data-zoom="in"]').disabled = scale >= 2;
  };
  figure.querySelectorAll("[data-zoom]").forEach(button => {
    if (button.dataset.zoom === "reset") button.textContent = "重置视图";
    button.addEventListener("click", () => {
      scale = button.dataset.zoom === "reset" ? 1 : Math.max(.5, Math.min(2, scale + (button.dataset.zoom === "in" ? .25 : -.25)));
      redraw();
      if (button.dataset.zoom === "reset") viewport.scrollTo({left:0, top:0, behavior:"instant"});
    });
  });
  new ResizeObserver(redraw).observe(viewport);
  redraw();
});

const navLinks = [...document.querySelectorAll(".top-nav a")];
let navScheduled = false;
function updateNav() {
  const active = [...navLinks].reverse().find(link => document.querySelector(link.hash).getBoundingClientRect().top <= 160) || navLinks[0];
  navLinks.forEach(link => {
    link.classList.toggle("active", link === active);
    if (link === active) link.setAttribute("aria-current","location");
    else link.removeAttribute("aria-current");
  });
  navScheduled = false;
}
window.addEventListener("scroll", () => {
  if (!navScheduled) {
    navScheduled = true;
    requestAnimationFrame(updateNav);
  }
}, {passive:true});
updateNav();
