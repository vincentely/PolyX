# PolyX 迭代待办（Backlog）

按优先级整理的技术债与改进项，作为后续迭代的输入。优先级：**P0 = 正确性/阻塞**，**P1 = 质量基建**，**P2 = 增强/优化**。

> 死代码的具体清理步骤单独见 [CLEANUP.md](CLEANUP.md)。

---

## 已确立的设计规则（已固化，不作为待办）

以下项经确认为**有意的设计决策**，已固化到 [core/Constants.h](../core/Constants.h)，不再视为技术债：

- **零图集间距（gutter = 0）**：本项目为 polygon（平涂 / 低面）画面风格、不使用 mipmap，瓦片紧贴排列不会渗色。常量 `kAtlasGutter = 0`，并在装箱处显式引用，明确这是规则而非遗漏。
- **8 像素块大小**：美术团队统一按 8px 网格作图。原散落的魔法数 8 已抽为全局常量 `kTextureBlockSize = 8`，作为量化与瓦片切分的基本单元，几乎不改动。

---

## P0 · 正确性

### 1. `AtlasBuilder::PackTiles` 形参与成员不一致（疑似 bug）
- **位置**：[atlas/AtlasBuilder.cpp](../atlas/AtlasBuilder.cpp)，`PackTiles` 内换行判断 `rowX + tile.width > targetWidth_`、高度判断 `rowY + tile.height > targetHeight_` 用的是**成员** `targetWidth_/targetHeight_`，而函数**入参** `targetWidth/targetHeight` 只用于"单个 tile 是否超宽"一处。
- **影响**：`CalculateAutoTargetSize` 用 `PackTiles(order, candidateSize, candidateSize, ...)` 试装箱时，实际是按成员尺寸（而非 `candidateSize`）判断换行/超高，导致**自动尺寸估算测的不是目标尺寸**。极端情况下会高估"能装下"，随后真实装箱失败 → `Build` 返回 false。
- ✅ **已修复（2026-06-25）**：换行 / 高度判断改用入参 `targetWidth/targetHeight`；新增回归测试 `TestAtlasAutoSizeGrowsPastMembers`（构建 + 运行通过）。

### 2. TGA 16-bit alpha 恒为不透明
- **位置**：[atlas/TgaLoader.cpp](../atlas/TgaLoader.cpp) `DecodePixel`：`rgba[3] = (value & 0x8000U) != 0U ? 255U : 255U;` —— 两个分支都是 255，alpha 位被忽略。
- **影响**：16bpp 带 1-bit alpha 的 TGA 透明信息丢失（当前示例数据是 TGA，需确认位深）。
- ✅ **已修复（2026-06-25）**：按图像描述符的 attribute-bit 计数决定是否采纳 16-bit 高位为 alpha（声明 0 位则视为不透明，避免给不带 alpha 的 16bpp 图误判透明，无回归风险）；新增 TGA 测试（16bpp alpha on/off + 24bpp）。

---

## P1 · 质量基建

### 3. 清理 0.2.0 重构遗留死代码
- `uv/UVAnalyzer` 中旧的瓦片选择路径（`ChooseTileRect` / `BuildSampleRect` / `ScoreSolidBlock` / `ScoreVerticalGradientStrip` / `ScoreHorizontalGradientStrip`）与一批未使用结构体均无人调用。
- ✅ **已清理（2026-06-25）**：删除 `ChooseTileRect` / `Score*` / `BuildSampleRect` / `MakeContext` / `MakeTileRect` 及 6 个未用结构体、`MeshPlan.layers`，并移除随之失效的 include；构建 + 样例回归通过。清单见 [CLEANUP.md](CLEANUP.md)。

### 4. 补自动化测试（`tests/` 当前为空）
- 适合优先覆盖的纯逻辑：
  - `UVAnalyzer` 并查集分区（构造已知共享/不共享 UV 的小网格）。
  - `AtlasBuilder` 去重、排序、装箱、自动尺寸（验证 #1 修复）。
  - `TgaLoader` 各位深 / RLE / 原点方向。
  - `Config::ParseCommandLine` 各选项与错误分支。
- 🟡 **部分完成（2026-06-25）**：已建**无第三方依赖**的测试目标 `PolyXTests`（`tests/TestMain.cpp`，80 项断言通过），覆盖 AtlasBuilder（打包/去重/自动尺寸）、TgaLoader（16/24bpp）、Config 解析，以及抽到 [uv/UVRegion.h](../uv/UVRegion.h) 的 `QuantizeToBlockOrigin`（含 V 翻转/钳制）与 `UnionFind`。运行：`ctest --test-dir out/build/x64-debug`。
- **待补**：UVAnalyzer 完整 `AnalyzeScene` 区域分组（需构造 FBX fixture）。

### 5. CI / 可复现构建
- ✅ **FBX SDK 路径已可覆盖（2026-06-25）**：去掉 `FORCE`，支持 `-DPOLYX_FBXSDK_ROOT=...` 或同名环境变量，缺省回退默认安装路径（已验证 `-D` 覆盖生效）。
- **待办**：搭建 CI（构建 + `ctest`），便于他机可复现。

---

## P2 · 增强 / 优化

> #6、#8 已固化为设计规则 / 已完成（见上）。**#7、#9、#10、#11 推迟到「具体功能迭代」阶段按需评估**——它们属优化 / 新功能范畴（性能、跨平台、CLI 语义、装箱算法），非基础打磨，且 #7/#9/#11 改动面大、有回归风险，宜结合具体需求再做。

### 6. 图集瓦片间距（gutter / padding）
- ✅ **已确立为设计规则**（零间距），固化到 `core::kAtlasGutter`，见上方「已确立的设计规则」。
- 仅当将来引入 mipmap 时才需重新评估非 0 间距 + 边缘扩边（extrude）。

### 7. 减少 FBX 重复加载
- 单个 FBX 在分析阶段加载一次后丢弃，导出阶段又加载 2 次（原始引用 + 工作副本）+ 校验再加载 1 次。
- **建议**：评估复用已加载场景或缓存分析结果，降低 I/O。

### 8. 参数化"8×8 块大小"
- ✅ **已完成**：魔法数 8 已抽为全局常量 `core::kTextureBlockSize`（见上方「已确立的设计规则」），`uv/UVAnalyzer` 的量化与区域矩形已改为引用该常量。

### 9. 跨平台
- 替换 GDI+（→ stb_image / stb_image_write）、`conio.h` 暂停、`Windows.h` 依赖。
- **建议**：把图片 I/O 抽象为接口，Windows 实现保留，新增可移植实现。

### 10. CLI 体验
- `-h` 表示 `--height` 而非 help，易误触；help 仅 `--help`。
- **建议**：评估是否保留 `-h` 语义，或为 help 增加更直觉的别名并在文档显著提示（README 已注明）。

### 11. 装箱算法升级
- 当前 shelf 行式装箱对小 tile 够用，但空间利用率一般。
- **建议**：如未来 tile 尺寸差异变大，可换 MaxRects / Guillotine 等。

### 12. 源码编码：启用 `/utf-8`
- ✅ **已完成（2026-06-25）**：CMake 对 MSVC 加 `add_compile_options(/utf-8)`；修复 [app/BatchProcessor.cpp](../app/BatchProcessor.cpp) 651/657 行损坏字节（两个 U+FFFD → `")->px("`）。全源码现为纯 ASCII，重建无 C4819 告警。

---

## 附：当前规模参考
- 示例数据：`Bin/input/Landlord/` 下 23 个角色 FBX + 1 张 `T_Actor.tga`，输出单张 `atlas.png`。
- 代码量：约 9 个源文件，分 5 层，无第三方依赖（除 FBX SDK 与系统 GDI+）。
