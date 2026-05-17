"""提取 Phase 7.55 v4 关键指标。"""
import json
import re

with open("_phase755_v4_run.log", encoding="utf-8") as f:
    text = f.read()

# 提取 JSON object（log 末尾包含 result dict）
# 找到第一个 '{' 开始的位置
start = text.find('{\n  "ok"')
if start < 0:
    start = text.find('{ "ok"')
if start < 0:
    print("未找到 JSON 起点")
    exit(1)

# 简单暴力：找匹配的 '}' 用栈
depth = 0
end = -1
for i in range(start, len(text)):
    c = text[i]
    if c == '{':
        depth += 1
    elif c == '}':
        depth -= 1
        if depth == 0:
            end = i + 1
            break

if end < 0:
    print("未找到 JSON 终点")
    exit(1)

raw = text[start:end]
# 去掉异常的换行/空格污染
try:
    obj = json.loads(raw)
except Exception as e:
    print(f"JSON 解析失败：{e}")
    # 尝试 strip 看看
    print(raw[:200])
    exit(1)

# 关键字段
def deep_find(d, key, results):
    if isinstance(d, dict):
        for k, v in d.items():
            if k == key:
                results.append(v)
            deep_find(v, key, results)
    elif isinstance(d, list):
        for v in d:
            deep_find(v, key, results)

for key in [
    "ok", "stage", "avgFps",
    "drawTimeVBCacheCaptureCount",
    "drawTimeVBCacheConsumeHitCount",
    "drawTimeVBCacheConsumeMissCount",
    "semanticSceneSubmittedSkinned",
    "semanticSceneShadowCastersCount",
    "semanticSceneShadowMapDrawnCasters",
    "semanticSceneShadowMapExecutedThisFrame",
    "submittedObjectJaccardMilli",
    "semanticSceneSubmittedSkinnedPaletteCombinedHash",
    "semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount",
]:
    res = []
    deep_find(obj, key, res)
    if res:
        # 去重 + 取最近
        unique = list(dict.fromkeys(repr(v) for v in res))
        print(f"{key}: {unique[:5]}")
