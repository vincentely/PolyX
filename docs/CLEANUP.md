# 代码清理施工图：移除 0.2.0 重构遗留物

> ✅ **已执行（2026-06-25）**：A/B/C 三部分均已完成（含移除随之失效的 `<array>` / `<sstream>` / `<cstring>` include 与重复的 `#include "uv/UVAnalyzer.h"`）。x64 Debug 构建通过，样例数据回归无新增告警，`PolyXTests` 66 项断言通过。下文保留为变更记录。

> 目标：删除 0.1.0→0.2.0 重构后被新的"并查集分区"路径取代、但仍残留的死代码，缩小后续迭代的改动面。
>
> 行号基于当前 `master`（提交 `429b6c3`），改动前请对照实际文件。

## 背景

`UVAnalyzer::AnalyzeScene` 现在走的是 **Union-Find 区域分组**（见 [ARCHITECTURE.md §3.1](ARCHITECTURE.md)）。而旧版"逐瓦片打分选择"（实心块 / 渐变条评分）整条链路**已无任何调用者**，连同一批未使用的结构体一起残留至今。

验证方式（确认无调用者）：

```bash
# 这些符号除定义/互相调用外，无其它引用
grep -rn "ChooseTileRect\|BuildSampleRect\|ScoreSolidBlock" --include=*.cpp --include=*.h .
```

---

## A. `uv/UVAnalyzer.cpp` — 删除死函数

匿名命名空间内，以下函数**仅被彼此调用、无外部调用者**，可整体删除：

| 函数 | 行号区间 | 说明 |
|------|----------|------|
| `MakeContext` | 37–44 | 旧的日志上下文串，仅旧路径用 |
| `MakeTileRect` | 58–66 | 仅被下面几个死函数调用 |
| `BuildSampleRect` | 68–165 | 仅被 `ChooseTileRect` 调用 |
| `ScoreSolidBlock` | 167–184 | 实心块评分，仅被 `ChooseTileRect` 调用 |
| `ScoreVerticalGradientStrip` | 186–215 | 纵向渐变条评分，同上 |
| `ScoreHorizontalGradientStrip` | 217–251 | 横向渐变条评分，同上 |
| `ChooseTileRect` | 253–314 | 旧瓦片选择入口，**无任何调用者** |

> ✅ **保留**：`CollectMeshes`(19)、`QuantizeToBlockOrigin`(46–56)、`BuildTileKey`(316–326)、`UnionFind`(328–364) —— 这些是现行路径在用的。
>
> ⚠️ 注意 `QuantizeToBlockOrigin`(46–56) 夹在 `MakeContext` 与 `MakeTileRect` 之间，**删除时不要误删**。即：删 37–44，保留 46–56，再删 58–314。

## B. `uv/UVAnalyzer.cpp` — 移除 `LayerPlan` 填充（保留 primaryUvSetName）

`AnalyzeScene` 中遍历 UV 集的循环（约 427–444 行）目前同时做两件事：设置 `primaryUvSetName`（**要保留**）、构造并 push `LayerPlan`（**要删除**）。

仅删除该循环末尾这几行：

```cpp
LayerPlan layerPlan;
layerPlan.uvSetName = uvSetName;
meshPlan.layers.push_back(std::move(layerPlan));   // ← 删除这 3 行
```

`primaryUvSetName` 的赋值逻辑原样保留。

## C. `uv/UVAnalyzer.h` — 删除未使用结构体并精简 `MeshPlan`

| 结构体 | 行号 | 处置 |
|--------|------|------|
| `TrianglePlan` | 27–34 | 删除（仅被 `LayerPlan` 引用） |
| `LayerPlan` | 36–40 | 删除 |
| `CellCoord` | 65–70 | 删除 |
| `CellCoordHash` | 72–78 | 删除 |
| `CellMapping` | 80–87 | 删除 |
| `PolyKey` | 89–94 | 删除 |
| `PolyKeyHash` | 96–102 | 删除 |
| `PolyStripMapping` | 104–111 | 删除 |

并从 `MeshPlan`(51–56) 删除 `layers` 成员：

```cpp
struct MeshPlan
{
    std::vector<LayerPlan> layers;        // ← 删除此行
    std::vector<RegionMapping> regions;
    std::vector<int> polyToRegion;
};
```

> ✅ **保留**：`UvPoint`(21–25)、`RegionMapping`(42–49)、`TileCandidate`(58–63)、`ScenePlan`(113–119)、`UVAnalyzer`(121–127)。

---

## 待确认（低置信度，单独评估）

- **`ScenePlan::triangleCount`**：似乎从未被赋有效值/读取，可考虑一并移除（确认无引用后）。
- **`TileCandidate::sourceRect` / `AtlasEntry::sourceRect`**：被一路透传存储，但下游逻辑未见消费。先保留，待确认是否用于将来功能或可删。

---

## 验收

1. 删除后 `uv/UVAnalyzer.cpp` 中可顺带移除不再需要的 include（如 `<array>`、`<sstream>` 若仅旧路径使用——删后由编译器/告警确认）。
2. 重新构建：`cmake --build out/build/x64-release`，应无 error/warning 回归。
3. 跑示例数据冒烟：`$env:POLYX_NO_PAUSE=1; PolyX Bin`，确认 `Bin/output/atlas.png` 与各 FBX 正常生成、校验阶段无新增 `** COLOR MISMATCH **` / `** GEOMETRY CHANGED **` 告警。
4. 建议此清理**独立成一个提交**（纯删除、行为不变），便于 review 与回滚。
