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
  "items": [
    {
      "fbx": "Pet_Grass02.fbx",
      "meshes": [
        { "mesh": "Pet_Grass02", "nodePath": "/Pet_Grass02",
          "texture": "../../../Texture/PetScene/petpark_guanmu_02.png" }
      ]
    }
  ]
}
```

结构是 **一个 FBX → 多个 mesh**，每个 mesh 带自己的贴图（可以各不相同）。

| 字段 | 说明 |
|------|------|
| `version` | 契约版本，当前 `1` |
| `atlasSize` | `"auto"` \| `"1024"` \| `"1024x512"` |
| `items[].fbx` | 源 FBX 路径，**相对于本 JSON 所在目录** |
| `items[].meshes[]` | 该 FBX 下的一个或多个网格 |
| `…meshes[].mesh` | 网格名（匹配兜底） |
| `…meshes[].nodePath` | `/名/名/…` 节点路径（主匹配键） |
| `…meshes[].texture` | 该网格的源贴图路径，**相对于本 JSON 所在目录**（多为 `../`） |

> C++ 按 `nodePath`（兜底 `mesh` 名）在 FBX 内**逐网格**匹配，各自用自己的贴图重映射 UV、合进同一张共享图集。
> 不区分 palette/full：manifest 里有什么就合，要不要收进来由人在 Unity 里决定（选哪些导出）。

**路径与输出约定**：所有路径相对于 request.json 所在目录。C++ 端 `PolyX <request.json>` 在 **可执行文件所在目录下创建 `output/`**，写出 `atlas.png` + 与 manifest 相对布局镜像的所有 FBX + `result.json`。

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

`status ∈ { ok, warn, error }`，细节进 `detail`（如 `submesh:2`、`mesh-not-found`、`fbx-load`、`texture-load`）。

---

## 语义（已确认的设计决策）

| 决策 | 落点 |
|------|------|
| **nodePath 主、mesh 名兜底** | C++ 按 `nodePath` 在 FBX 节点树定位；不中回退 `mesh` 名；仍不中 → `error/mesh-not-found` |
| **粒度到 mesh；submesh 警告** | 检到网格多材质组 → `warn/submesh:N`，仍按给定贴图整体重映射 |
| **简化、两端易解析** | JSON + JsonUtility（Unity）/ nlohmann（C++） |
| **镜像目录，便于覆盖回工程** | `outputFbx = <exe>/output / (fbx 相对 manifest 的路径)` |
| **处理所有条目** | 不区分 palette/full；manifest 里有什么就合进同一张图集 |

## 三个必须注意的坑

1. **UTF-8 路径**：Unity 工程路径常含中文。C++ 侧按 UTF-8 读 JSON，并用 `std::filesystem::u8path` 构造路径，避免窄字节乱码。
2. **nodePath ↔ FBX 节点**：Unity 导入时层级一般镜像 FBX 节点名，但有根节点/重名/改名边角差异 → 主键 + 兜底 + 报错。
3. **覆盖 FBX ≠ Unity 就用图集**：覆盖回工程只换了 **UV**；材质指向 atlas 需**手动在 Unity 里新建材质（贴 atlas）并指给这些 mesh**（当前不做自动回写）。

---

## 实现状态

- **Phase A（已完成）**：JSON 读写层 [`app/Manifest.h`](../app/Manifest.h) / `app/Manifest.cpp`（结构体 + `ReadRequest` / `WriteResult`），单元测试覆盖。`json.hpp` 已 vendored 并接入 CMake。现有目录模式不受影响。
- **Phase B（已完成）**：`BatchProcessor::RunManifest`（[app/BatchProcessor.cpp](../app/BatchProcessor.cpp)）——读 request → 逐 FBX 加载 → **按 nodePath/名 把场景网格匹配到 manifest 条目、各用自己的贴图分析**（`UVAnalyzer::AnalyzeScene` 现接收 per-mesh 贴图数组）→ 单张共享图集 → 逐网格重映射 UV、只对已合图的网格重指材质 → 镜像输出 FBX → 写 `result.json`。CLI：`PolyX <request.json>`，输出到 `<exe目录>/output/`。验证：PetScene 33 FBX → 2048×2048，33 ok，0 mismatch；折叠（folder）模式与单测仍绿。
  - 多材质网格（submesh）→ `warn/submesh:N`，仍按该网格贴图整体重映射。匹配不到的网格保持原样（不改 UV、不重指材质）。
- **Phase A（Unity 侧）**：[unity/Editor/PolyXManifestExporter.cs](../unity/Editor/PolyXManifestExporter.cs)（菜单 Tools › PolyX › Manifest Exporter）扫描 FBX 目录、解析主贴图、导出相对路径 request.json。
- **回写（手动）**：把 `output/` 覆盖回工程对应目录，新建材质指向 `atlas.png` 并指给这些 mesh。当前不做自动回写（`result.json` 仅供参考，可忽略）。
