# PolyX × Unity 流水线契约

PolyX 下一版面向 Unity 工程中的美术使用：**不再从目录约定猜关系**，而是由 Unity 侧导出资产关系，C++ 侧据此整体合并图集并重映射 UV。两端通过**两份 JSON 文件**握手。

```
Unity 工具  --(request.json)-->  PolyX (C++)
                                    │ 读 FBX + 贴图 → 区域分析/去重/装箱/重映射 UV
Unity 回写  <--(result.json)----  ┘ 写 atlas.png + 重映射后的 FBX
```

格式用 **JSON**：Unity 端 `JsonUtility`（内置，零包依赖）即可读写；C++ 端用 vendored 单头库 [`third_party/json.hpp`](../third_party/json.hpp)（nlohmann/json）。Schema 刻意设计为 **JsonUtility 兼容**（顶层对象、数组包在字段里、全字符串/整数字段、无注释）。

---

## 请求文件 request.json（Unity → C++）

```json
{
  "version": 1,
  "atlasSize": "auto",
  "assetsRoot": "C:/proj/Assets",
  "outputRoot": "C:/proj/PolyXOut",
  "atlasOut":   "C:/proj/PolyXOut/Atlas/atlas.png",
  "items": [
    {
      "fbx":         "C:/proj/Assets/Models/Tenant_A.fbx",
      "mesh":        "Body",
      "nodePath":    "/Tenant_A/Body",
      "texture":     "C:/proj/Assets/Tex/T_Actor.tga",
      "textureKind": "palette"
    }
  ]
}
```

| 字段 | 说明 |
|------|------|
| `version` | 契约版本，当前 `1` |
| `atlasSize` | `"auto"` \| `"1024"` \| `"1024x512"` |
| `assetsRoot` | Unity Assets 根；用于计算输出 FBX 的相对镜像路径 |
| `outputRoot` | 输出根；`outputFbx = outputRoot / relative(fbx, assetsRoot)` |
| `atlasOut` | 图集 PNG 的写出绝对路径 |
| `items[].fbx` | 源 FBX 绝对路径（同一 FBX 的多网格重复此列） |
| `items[].mesh` | 网格名（匹配兜底） |
| `items[].nodePath` | `/名/名/…` 节点路径（主匹配键） |
| `items[].texture` | 源贴图绝对路径 |
| `items[].textureKind` | `palette`（处理）\| `full`（跳过，仅标记） |

## 结果文件 result.json（C++ → Unity）

```json
{
  "atlasOut":    "C:/proj/PolyXOut/Atlas/atlas.png",
  "atlasWidth":  1024,
  "atlasHeight": 1024,
  "items": [
    { "fbx":"...", "nodePath":"/Tenant_A/Body", "mesh":"Body",
      "outputFbx":"C:/proj/PolyXOut/Models/Tenant_A.fbx",
      "uvSet":"map1", "status":"ok", "detail":"" }
  ],
  "warnings": [ "Mesh '/X/Y' 未匹配到节点" ]
}
```

`status ∈ { ok, warn, skipped, error }`，细节进 `detail`（如 `submesh:2`、`kind:full`、`mesh-not-found`）。

---

## 语义（已确认的设计决策）

| 决策 | 落点 |
|------|------|
| **nodePath 主、mesh 名兜底** | C++ 按 `nodePath` 在 FBX 节点树定位；不中回退 `mesh` 名；仍不中 → `error/mesh-not-found` |
| **粒度到 mesh；submesh 警告** | 检到网格多材质组 → `warn/submesh:N`，仍按给定贴图整体重映射 |
| **简化、两端易解析** | JSON + JsonUtility（Unity）/ nlohmann（C++） |
| **镜像目录，便于覆盖回工程** | `outputFbx = outputRoot / relative(fbx, assetsRoot)` |
| **先只做色块** | `textureKind=full` → `skipped/kind:full`，不进图集、不改 UV |

## 三个必须注意的坑

1. **UTF-8 路径**：Unity 工程路径常含中文。C++ 侧按 UTF-8 读 JSON，并用 `std::filesystem::u8path` 构造路径，避免窄字节乱码。
2. **nodePath ↔ FBX 节点**：Unity 导入时层级一般镜像 FBX 节点名，但有根节点/重名/改名边角差异 → 主键 + 兜底 + 报错。
3. **覆盖 FBX ≠ Unity 就用图集**：覆盖回工程只换了 **UV**；材质指向 atlas 由 Unity 用自己的 remap 处理 → **必须由 Unity 工具按 `result.json` 再做一步材质重指**。

---

## 实现状态

- **Phase A（已完成）**：JSON 读写层 [`app/Manifest.h`](../app/Manifest.h) / `app/Manifest.cpp`（结构体 + `ReadRequest` / `WriteResult`），单元测试覆盖。`json.hpp` 已 vendored 并接入 CMake。现有目录模式不受影响。
- **Phase B（待做）**：`MergeProcessor`（读请求 → 按 fbx 分组 → **逐 mesh 用各自贴图**分析 → 单图集 → 镜像输出 FBX → 写结果），CLI `--request <f> --result <f>`，以及 `UVAnalyzer`/`ApplyScenePlan` 改为 per-mesh 贴图。
- **Phase C（Unity 侧，另一仓）**：导出 request、跑 PolyX、按 result 导入 atlas + 重映射 FBX + 重指材质 + 回滚。
