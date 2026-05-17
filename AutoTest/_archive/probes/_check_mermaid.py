"""扫描所有论文章节的 mermaid 块，找出可能导致解析错误的行。
问题模式：
1. 未加引号的 node label 里含 [] () & | 等特殊字符
2. edge label 里含未转义的特殊字符
"""
import re, glob, os

base = r"docs\plan\overnight_render_paper_2026_05_15"
files = glob.glob(os.path.join(base, "0*.md"))

# 匹配 nodeId[label] 但 label 不是用 " 包裹的
# 有效: A["text"] 或 A[text without special]
# 无效: A[text with (parens)] 或 A[text[nested]]
node_pattern = re.compile(r'(\w+)\[([^\]"]+)\]')
# 匹配 edge label -- "text" --> 里的 text
edge_pattern = re.compile(r'--\s*"([^"]*)"')

problems = []

for fpath in sorted(files):
    fname = os.path.basename(fpath)
    with open(fpath, encoding="utf-8") as f:
        lines = f.readlines()
    in_mermaid = False
    for i, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith("```mermaid"):
            in_mermaid = True
            continue
        if stripped.startswith("```") and in_mermaid:
            in_mermaid = False
            continue
        if not in_mermaid:
            continue
        # Skip comments and subgraph/end
        if stripped.startswith("%%") or stripped.startswith("subgraph") or stripped == "end":
            continue
        # Check node labels
        for m in node_pattern.finditer(stripped):
            label = m.group(2)
            # Check for problematic chars in unquoted label
            if any(c in label for c in "[]()&|"):
                problems.append(f"{fname}:{i}: unquoted node with special char: {stripped[:120]}")
                break
        # Check edge labels (unquoted)
        # Pattern: -- text --> (without quotes)
        if re.search(r'--\s+[^">\s][^>]*-->', stripped):
            problems.append(f"{fname}:{i}: unquoted edge label: {stripped[:120]}")

if problems:
    print(f"Found {len(problems)} potential mermaid issues:")
    for p in problems:
        print(f"  {p}")
else:
    print("No mermaid issues found!")
