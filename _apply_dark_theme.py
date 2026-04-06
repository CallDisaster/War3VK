#!/usr/bin/env python3
"""
将 war3_perf_monitor.cpp 中嵌入的 HTML 前端改为暗色主题 + Spark 风格树形 Section Breakdown。
运行后可删除此脚本。
"""
import re, shutil, os

SRC = os.path.join(os.path.dirname(__file__),
                   'src', 'd3d9', 'war3', 'tools', 'war3_perf_monitor.cpp')

# ── 读取 ──
with open(SRC, 'r', encoding='utf-8') as f:
    content = f.read()

# ── 备份 ──
shutil.copy2(SRC, SRC + '.bak')

# ============================================================
# 1. 替换 CSS（<style> ... </style>）
# ============================================================
NEW_CSS = r"""    <style>
        :root {
            --bg-1: #1a1a2e;
            --bg-2: #16213e;
            --ink: #e0e0e0;
            --ink-soft: #8a8a9a;
            --accent: #e2b93b;
            --accent-2: #2a9d8f;
            --panel: rgba(25, 25, 45, 0.88);
            --line: rgba(255, 255, 255, 0.08);
            --glow: rgba(226, 185, 59, 0.18);
        }

        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: "Space Grotesk", "Noto Sans SC", sans-serif;
            background:
                radial-gradient(1200px 800px at 10% 0%, rgba(226,185,59,0.06), transparent 60%),
                radial-gradient(900px 700px at 90% 10%, rgba(42,157,143,0.08), transparent 55%),
                linear-gradient(180deg, var(--bg-1), var(--bg-2));
            color: var(--ink);
            min-height: 100vh;
            padding: 24px;
        }
        .app { max-width: 1400px; margin: 0 auto; display: grid; gap: 24px; }
        header {
            display: flex; align-items: center; justify-content: space-between;
            padding: 16px 20px;
            background: var(--panel); border: 1px solid var(--line);
            border-radius: 16px;
            box-shadow: 0 8px 32px rgba(0,0,0,0.3);
            backdrop-filter: blur(12px);
        }
        header h1 {
            font-size: 22px; margin: 0;
            font-family: "Noto Serif SC","Source Serif 4",serif;
            letter-spacing: 0.02em; color: var(--accent);
        }
        header .meta { display: flex; gap: 12px; align-items: center; color: var(--ink-soft); font-size: 13px; }
        .badge {
            padding: 4px 10px; border-radius: 999px;
            border: 1px solid var(--line);
            background: rgba(255,255,255,0.06); color: var(--ink-soft);
        }
        .panel {
            background: var(--panel); border: 1px solid var(--line);
            border-radius: 16px; padding: 20px;
            box-shadow: 0 8px 32px rgba(0,0,0,0.25);
            backdrop-filter: blur(12px);
        }
        .panel h2 { font-size: 16px; margin-bottom: 12px; color: var(--accent); letter-spacing: 0.02em; }
        .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 16px; }
        .split { display: grid; grid-template-columns: minmax(0,2fr) minmax(0,1fr); gap: 16px; }
        .split-item {
            background: rgba(255,255,255,0.03); border: 1px solid var(--line);
            border-radius: 12px; padding: 14px;
        }
        .split-item h2 { margin-bottom: 10px; }
        .card {
            background: rgba(255,255,255,0.04); border-radius: 14px; padding: 18px;
            text-align: center; border: 1px solid var(--line);
            box-shadow: 0 4px 16px rgba(0,0,0,0.2);
            transition: border-color 0.2s, box-shadow 0.2s;
        }
        .card:hover { border-color: rgba(226,185,59,0.3); box-shadow: 0 4px 20px rgba(226,185,59,0.1); }
        .card-value { font-size: 26px; font-weight: 700; color: var(--accent); }
        .card-label { font-size: 12px; color: var(--ink-soft); margin-top: 6px; }
        table {
            width: 100%; border-collapse: collapse; overflow: hidden;
            border-radius: 12px; border: 1px solid var(--line); table-layout: fixed;
        }
        th, td { padding: 10px 12px; text-align: left; border-bottom: 1px solid var(--line); }
        th { background: rgba(226,185,59,0.08); color: var(--accent); font-weight: 600; }
        thead th { position: sticky; top: 0; z-index: 2; }
        tr:hover { background: rgba(255,255,255,0.03); }
        .section-tools {
            display: flex; gap: 8px; margin-bottom: 12px; flex-wrap: wrap; align-items: center;
        }
        .section-tools button {
            border: 1px solid var(--line); background: rgba(255,255,255,0.06);
            color: var(--ink); border-radius: 999px; padding: 6px 12px;
            font-size: 12px; cursor: pointer; transition: all 0.2s;
        }
        .section-tools button:hover { border-color: var(--accent); color: var(--accent); background: rgba(226,185,59,0.1); }
        .section-tools input, .section-tools select {
            border: 1px solid var(--line); background: rgba(255,255,255,0.06);
            color: var(--ink); border-radius: 999px; padding: 6px 12px;
            font-size: 12px; outline: none; min-width: 180px;
        }
        .section-tools select option { background: #1a1a2e; color: var(--ink); }
        .section-tools input:focus, .section-tools select:focus {
            border-color: var(--accent); box-shadow: 0 0 0 3px rgba(226,185,59,0.15);
        }
        .hint { margin-top: 10px; color: var(--ink-soft); font-size: 12px; line-height: 1.5; }
        .relation-tools {
            display: flex; gap: 8px; margin-bottom: 12px; flex-wrap: wrap; align-items: center;
        }
        .relation-tools input, .relation-tools select {
            border: 1px solid var(--line); background: rgba(255,255,255,0.06);
            color: var(--ink); border-radius: 999px; padding: 6px 12px;
            font-size: 12px; outline: none; min-width: 180px;
        }
        .relation-tools select option { background: #1a1a2e; color: var(--ink); }
        .relation-tools input:focus, .relation-tools select:focus {
            border-color: var(--accent); box-shadow: 0 0 0 3px rgba(226,185,59,0.15);
        }
        .relation-parent, .relation-child {
            display: inline-block;
            font-family: "Consolas","JetBrains Mono",monospace;
            font-size: 12px; line-height: 1.3; max-width: 100%;
            white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
            vertical-align: bottom;
        }
        .relation-parent { color: #e2b93b; }
        .relation-child  { color: #2a9d8f; }
        .relation-share { display: grid; gap: 4px; }
        .relation-share .bar { background: linear-gradient(90deg, #2a9d8f, #55c070); }

        /* ── Spark-style Section Tree ── */
        .tree-container {
            font-family: "Consolas","JetBrains Mono",monospace;
            font-size: 13px; line-height: 1.1;
        }
        .tree-node-header {
            display: flex; align-items: center; padding: 3px 8px;
            border-radius: 4px; cursor: default; gap: 2px;
            transition: background 0.15s;
        }
        .tree-node-header:hover { background: rgba(255,255,255,0.04); }
        .tree-toggle {
            width: 16px; height: 16px;
            display: inline-flex; align-items: center; justify-content: center;
            cursor: pointer; user-select: none; color: var(--ink-soft);
            flex-shrink: 0; font-size: 10px; border-radius: 3px;
            transition: all 0.15s;
        }
        .tree-toggle:hover { background: rgba(255,255,255,0.08); color: var(--accent); }
        .tree-spacer { width: 16px; flex-shrink: 0; }
        .tree-children {
            margin-left: 8px; padding-left: 12px;
            border-left: 1px solid rgba(255,255,255,0.06);
        }
        .tree-children:hover { border-left-color: rgba(255,255,255,0.12); }
        .tree-name      { color: var(--ink); font-weight: 600; white-space: nowrap; }
        .tree-name-idle { color: #2a9d8f;   font-weight: 600; white-space: nowrap; }
        .tree-pct  { margin-left: 8px; font-weight: 700; font-size: 12px; white-space: nowrap; }
        .tree-ms   { margin-left: 4px; color: var(--ink-soft); font-size: 12px; white-space: nowrap; }
        .tree-gpu  { margin-left: 6px; color: #6a7fdb; font-size: 11px; white-space: nowrap; }
        .tree-calls{ margin-left: 6px; color: var(--ink-soft); font-size: 11px; white-space: nowrap; opacity: 0.7; }
        .tree-idle-tag {
            margin-left: 6px; padding: 0 5px; border-radius: 3px; font-size: 10px;
            color: #2a9d8f; border: 1px solid rgba(42,157,143,0.3);
            background: rgba(42,157,143,0.08);
        }
        .tree-self-bar {
            margin-left: auto; width: 120px; height: 4px;
            background: rgba(255,255,255,0.04); border-radius: 2px;
            flex-shrink: 0; overflow: hidden;
        }
        .tree-self-bar-fill { height: 100%; border-radius: 2px; background: linear-gradient(90deg, #e2b93b, #d62828); }

        .idle-tag {
            display: inline-block; margin-left: 8px; padding: 1px 6px;
            border-radius: 999px; font-size: 10px; color: #2a9d8f;
            border: 1px solid rgba(42,157,143,0.3); background: rgba(42,157,143,0.08);
            vertical-align: middle;
        }
        .dist { display: grid; gap: 4px; }
        .bar { height: 8px; border-radius: 4px; }
        .bar.incl { background: linear-gradient(90deg, var(--accent), var(--accent-2)); }
        .bar.self { background: linear-gradient(90deg, #d62828, #f77f00); }
        .bar-label { font-size: 11px; color: var(--ink-soft); }
        @media (max-width: 1200px) { .split { grid-template-columns: 1fr; } }
        footer { text-align: center; color: var(--ink-soft); font-size: 12px; }
    </style>"""

css_pat = re.compile(r'    <style>.*?    </style>', re.DOTALL)
content, n = css_pat.subn(NEW_CSS.strip(), content, count=1)
assert n == 1, f"CSS replacement failed, matched {n} times"

# ============================================================
# 2. 替换 Section Breakdown 的 HTML 部分（table → tree div）
# ============================================================
OLD_SECTION_HTML = """        <section class="panel">
            <h2>Section Breakdown</h2>
            <div class="section-tools">
                <button id="expandAll">全部展开</button>
                <button id="collapseAll">全部折叠</button>
                <select id="sortMode">
                    <option value="self">按 Self CPU 排序</option>
                    <option value="inclusive">按 Inclusive CPU 排序</option>
                    <option value="calls">按 Calls/Frame 排序</option>
                </select>
                <input id="sectionFilter" type="text" placeholder="搜索函数（支持路径关键字）" />
            </div>
            <table id="sectionTable">
                <thead>
                    <tr>
                        <th>Section</th>
                        <th>Avg CPU Incl (ms)</th>
                        <th>Avg CPU Self (ms)</th>
                        <th>Avg GPU (ms)</th>
                        <th>Total CPU Incl (ms)</th>
                        <th>Total CPU Self (ms)</th>
                        <th>Calls</th>
                        <th>Calls/Frame</th>
                        <th>Distribution</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
            <div class="hint">
                说明：`Inclusive` 含子函数，`Self` 为去除子函数后的自身耗时。排查瓶颈时优先看 `Self`；观察模块总负担时看 `Inclusive`。
            </div>
        </section>"""

NEW_SECTION_HTML = """        <section class="panel">
            <h2>Section Breakdown</h2>
            <div class="section-tools">
                <button id="expandAll">全部展开</button>
                <button id="collapseAll">全部折叠</button>
                <select id="sortMode">
                    <option value="self">按 Self CPU 排序</option>
                    <option value="inclusive">按 Inclusive CPU 排序</option>
                    <option value="calls">按 Calls/Frame 排序</option>
                </select>
                <input id="sectionFilter" type="text" placeholder="搜索函数（支持路径关键字）" />
            </div>
            <div class="tree-container" id="sectionTree"></div>
            <div class="hint">
                百分比 = Inclusive CPU / 平均帧时间。颜色编码：<span style="color:#55c070">■</span>&lt;1% <span style="color:#a3c23b">■</span>1-3% <span style="color:#e2b93b">■</span>3-10% <span style="color:#e28c3b">■</span>10-30% <span style="color:#d62828">■</span>&gt;30%。右侧小条 = Self CPU 占比。
            </div>
        </section>"""

content = content.replace(OLD_SECTION_HTML, NEW_SECTION_HTML, 1)

# ============================================================
# 3. 替换 Chart.js 配色为暗色系
# ============================================================
# Frame Time Chart gradient
content = content.replace(
    "gradient.addColorStop(0, 'rgba(194, 89, 42, 0.35)');",
    "gradient.addColorStop(0, 'rgba(226, 185, 59, 0.30)');")
content = content.replace(
    "gradient.addColorStop(1, 'rgba(17, 100, 102, 0.08)');",
    "gradient.addColorStop(1, 'rgba(42, 157, 143, 0.05)');")
content = content.replace(
    "borderColor: '#c2592a',",
    "borderColor: '#e2b93b',", 1)
content = content.replace(
    "borderColor: '#116466',",
    "borderColor: '#2a9d8f',", 1)
content = content.replace(
    "borderColor: 'rgba(17,100,102,0.45)',",
    "borderColor: 'rgba(42,157,143,0.45)',")
content = content.replace(
    "borderColor: 'rgba(214,40,40,0.45)',",
    "borderColor: 'rgba(214,40,40,0.55)',")
# Chart axis colors
content = content.replace(
    "color: 'rgba(31,27,22,0.12)'",
    "color: 'rgba(255,255,255,0.08)'")
content = content.replace(
    "ticks: { color: '#4a4037' }",
    "ticks: { color: '#8a8a9a' }")
# Doughnut colors
content = content.replace(
    "const colors = ['#c2592a', '#116466', '#d62828', '#6a4c93', '#2a9d8f', '#f4a261', '#8d99ae', '#457b9d', '#adb5bd'];",
    "const colors = ['#e2b93b', '#2a9d8f', '#d62828', '#6a7fdb', '#55c070', '#f4a261', '#e28c3b', '#457b9d', '#8a8a9a'];")

# ============================================================
# 4. 替换 Section Tree JS（从 "// Section Tree" 到 "rerenderTable();"）
# ============================================================
OLD_TREE_JS_START = "        // Section Tree"
OLD_TREE_JS_END   = "        rerenderTable();\n        renderRelationTable();"
NEW_TREE_JS = r"""        // Section Tree (Spark-style)
        const sections = Array.isArray(data.sections) ? data.sections : [];
        const treeContainer = document.getElementById('sectionTree');
        const collapsed = new Set();
        const treeState = { sortMode: 'self', filter: '' };

        const nodes = new Map();
        sections.forEach((s) => {
            const path = s.path || s.name;
            nodes.set(path, { ...s, path, children: [] });
        });
        const treeRoots = [];
        nodes.forEach((node) => {
            const pp = node.parentPath || '';
            if (pp && nodes.has(pp)) nodes.get(pp).children.push(node);
            else treeRoots.push(node);
        });
        const getTreeSortVal = (n) => {
            if (treeState.sortMode === 'inclusive') return n.avgCpuMs || 0;
            if (treeState.sortMode === 'calls') return n.callsPerFrame || 0;
            return n.avgSelfCpuMs || 0;
        };
        const sortTree = (arr) => {
            arr.sort((a, b) => getTreeSortVal(b) - getTreeSortVal(a));
            arr.forEach((n) => sortTree(n.children));
        };
        const getPctColor = (p) => {
            if (p < 1)  return '#55c070';
            if (p < 3)  return '#a3c23b';
            if (p < 10) return '#e2b93b';
            if (p < 30) return '#e28c3b';
            return '#d62828';
        };
        const maxSelfMs = Math.max(0.001, ...sections.map(s => s.avgSelfCpuMs || 0));

        const renderTreeNode = (node, container) => {
            const hasKids = node.children.length > 0;
            const framePct = (data.avgFrameTimeMs > 1e-6) ? ((node.avgCpuMs||0)/data.avgFrameTimeMs*100) : 0;
            const selfPct  = ((node.avgSelfCpuMs||0)/maxSelfMs*100);
            const pctColor = getPctColor(framePct);

            const div = document.createElement('div');
            div.className = 'tree-node';
            div.dataset.path = node.path;

            const hdr = document.createElement('div');
            hdr.className = 'tree-node-header';

            // toggle / spacer
            let childrenDiv = null;
            if (hasKids) {
                const tog = document.createElement('span');
                tog.className = 'tree-toggle';
                tog.textContent = '\u25BE';
                tog.addEventListener('click', () => {
                    const p = node.path;
                    if (collapsed.has(p)) {
                        collapsed.delete(p);
                        tog.textContent = '\u25BE';
                        childrenDiv.style.display = '';
                    } else {
                        collapsed.add(p);
                        tog.textContent = '\u25B8';
                        childrenDiv.style.display = 'none';
                    }
                });
                hdr.appendChild(tog);
            } else {
                const sp = document.createElement('span');
                sp.className = 'tree-spacer';
                hdr.appendChild(sp);
            }

            // name
            const nm = document.createElement('span');
            nm.className = node.isIdleWait ? 'tree-name-idle' : 'tree-name';
            nm.textContent = node.name;
            nm.title = node.path;
            hdr.appendChild(nm);

            if (node.isIdleWait) {
                const it = document.createElement('span');
                it.className = 'tree-idle-tag';
                it.textContent = 'Idle';
                hdr.appendChild(it);
            }

            // pct
            const pc = document.createElement('span');
            pc.className = 'tree-pct';
            pc.style.color = pctColor;
            pc.textContent = f2(framePct) + '%';
            hdr.appendChild(pc);

            // ms
            const ms = document.createElement('span');
            ms.className = 'tree-ms';
            ms.textContent = f3(node.avgCpuMs) + 'ms';
            hdr.appendChild(ms);

            // gpu
            if ((node.avgGpuMs||0) > 0.001) {
                const gp = document.createElement('span');
                gp.className = 'tree-gpu';
                gp.textContent = 'GPU:' + f3(node.avgGpuMs) + 'ms';
                hdr.appendChild(gp);
            }

            // calls
            if ((node.callsPerFrame||0) > 1.5) {
                const cl = document.createElement('span');
                cl.className = 'tree-calls';
                cl.textContent = '\u00d7' + f2(node.callsPerFrame);
                hdr.appendChild(cl);
            }

            // self bar
            const sb = document.createElement('div');
            sb.className = 'tree-self-bar';
            const sf = document.createElement('div');
            sf.className = 'tree-self-bar-fill';
            sf.style.width = Math.min(100, selfPct) + '%';
            sb.appendChild(sf);
            hdr.appendChild(sb);

            div.appendChild(hdr);

            // children container
            childrenDiv = document.createElement('div');
            childrenDiv.className = 'tree-children';
            if (hasKids) node.children.forEach((c) => renderTreeNode(c, childrenDiv));
            div.appendChild(childrenDiv);

            container.appendChild(div);
        };

        const buildTreeFilter = () => {
            const q = treeState.filter.trim().toLowerCase();
            if (!q) return null;
            const vis = new Set();
            nodes.forEach((nd) => {
                const hay = (nd.name+' '+nd.path).toLowerCase();
                if (!hay.includes(q)) return;
                let cur = nd;
                while (cur) {
                    if (vis.has(cur.path)) break;
                    vis.add(cur.path);
                    cur = cur.parentPath && nodes.has(cur.parentPath) ? nodes.get(cur.parentPath) : null;
                }
            });
            return vis;
        };
        const applyTreeFilter = () => {
            const wl = buildTreeFilter();
            treeContainer.querySelectorAll('.tree-node').forEach((el) => {
                const p = el.dataset.path || '';
                el.style.display = (wl && !wl.has(p)) ? 'none' : '';
            });
        };
        const rerenderTree = () => {
            treeContainer.innerHTML = '';
            sortTree(treeRoots);
            treeRoots.forEach((r) => renderTreeNode(r, treeContainer));
            applyTreeFilter();
        };

        document.getElementById('expandAll')?.addEventListener('click', () => {
            collapsed.clear();
            treeContainer.querySelectorAll('.tree-toggle').forEach(t => { t.textContent = '\u25BE'; });
            treeContainer.querySelectorAll('.tree-children').forEach(c => { c.style.display = ''; });
        });
        document.getElementById('collapseAll')?.addEventListener('click', () => {
            nodes.forEach((n, p) => { if (n.children.length > 0) collapsed.add(p); });
            treeContainer.querySelectorAll('.tree-toggle').forEach(t => { t.textContent = '\u25B8'; });
            treeContainer.querySelectorAll('.tree-children').forEach(c => {
                const nd = c.closest('.tree-node');
                if (nd && collapsed.has(nd.dataset.path)) c.style.display = 'none';
            });
        });
        document.getElementById('sortMode')?.addEventListener('change', function() {
            treeState.sortMode = this.value || 'self';
            rerenderTree();
        });
        document.getElementById('sectionFilter')?.addEventListener('input', function() {
            treeState.filter = this.value || '';
            applyTreeFilter();
        });

        rerenderTree();
        renderRelationTable();"""

i0 = content.find(OLD_TREE_JS_START)
i1 = content.find(OLD_TREE_JS_END)
assert i0 != -1 and i1 != -1, "Cannot locate Section Tree JS block"
i1_end = i1 + len(OLD_TREE_JS_END)
content = content[:i0] + NEW_TREE_JS + content[i1_end:]

# ── 写回 ──
with open(SRC, 'w', encoding='utf-8') as f:
    f.write(content)

print("✅ 暗色主题 + Spark 风格 Section Tree 已应用到", SRC)
print("   备份文件:", SRC + '.bak')
