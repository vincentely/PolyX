# PolyX

> 批量处理 FBX 模型、合并共享贴图为图集（Texture Atlas）的 Windows 命令行工具。

PolyX 把一批**共用同一张源贴图**的 FBX 模型，重新映射到一张紧凑的图集贴图上：抽取每个模型实际用到的贴图区域、按内容去重、打包成单张 `atlas.png`，并改写每个模型的 UV 坐标与材质引用，最终输出新的 FBX。

典型场景是卡通 / 低面数角色的美术资产管线——这类角色常用一张"色块 / 渐变条"调色板贴图上色，PolyX 负责把实际用到的色块压缩进一张小图集，从而**减少贴图数量、便于合批**。

---

## 功能概览

- **批量发现**：扫描 `input/` 下的每个"包"（子目录），每个包需含**唯一一张**源贴图 + 若干 FBX。
- **UV 区域分析**：用并查集把共享 UV 顶点的多边形聚成区域，定位它们在源贴图上采样的 8×8 对齐块。
- **内容去重**：像素完全相同的区域复用同一个图集瓦片（tile）。
- **图集打包**：行式（shelf）装箱，自动求最小 2 的幂正方形，或指定固定尺寸。
- **UV 重映射 + 材质重连**：改写每个模型 UV 指向图集，diffuse 贴图统一连到 `atlas.png`。
- **导出后校验**：重新加载输出 FBX，比对几何与采样颜色，发现偏差则告警。

> 支持的源贴图格式：`png / jpg / jpeg / bmp / tga / gif / tif / tiff`（TGA 由内置解码器处理，其余走 GDI+）。

---

## 处理流程

```
input/<包>/  ──→  [1] 发现包      子目录 = 1 个包：唯一贴图 + 递归收集的 FBX
                  [2] UV 分析     逐 FBX 找出网格采样了源贴图的哪些区域
                  [3] 抽取瓦片    按像素内容去重，生成唯一 tile 列表
                  [4] 打包图集    行式装箱 → atlas.png（2 的幂 / 固定尺寸）
                  [5] 导出 FBX    改写 UV 指向 tile + diffuse 重连 atlas
                  [6] 校验        重载输出，比对几何 & 采样颜色，告警
output/      ←──  atlas.png + 与 input 镜像的 FBX 目录树
```

详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

---

## 构建

### 依赖

| 依赖 | 说明 |
|------|------|
| **Autodesk FBX SDK 2020.3.9** | 路径在 [CMakeLists.txt](CMakeLists.txt) 中硬编码为 `C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9`，如安装在别处需修改。 |
| **MSVC（cl.exe）** | Windows 平台编译器。 |
| **CMake ≥ 3.20 + Ninja** | 见 [CMakePresets.json](CMakePresets.json)。 |
| **GDI+** | Windows 系统自带，用于 PNG/常见图片读写。 |

### 命令

```powershell
# 配置 + 构建（x64 Release，静态链接 FBX SDK，产物不依赖 libfbxsdk.dll）
cmake --preset x64-release
cmake --build out/build/x64-release
```

可用 preset：`x64-debug` / `x64-release` / `x86-debug` / `x86-release`。
Release preset 设 `POLYX_FBXSDK_SHARED=OFF`（静态链接）；Debug 默认动态链接，会在构建后把 `libfbxsdk.dll` 拷到产物目录旁。

---

## 用法

```
PolyX [options] [root-dir]
```

| 选项 | 说明 |
|------|------|
| `-h, --help` | 显示帮助并退出 |
| `--root <dir>` | 指定包含 `input/` 与 `output/` 的根目录 |
| `--auto-size` | 用能容纳所有瓦片的最小 2 的幂尺寸；按需**矩形渐进**（如 1024×1024 → 1024×2048 → 2048×2048），更省空间（**默认**） |
| `-s, --size <n>` | 固定图集边长（宽=高），**必须是 2 的幂**（256/512/1024/2048/4096…） |
| `-w, --width, --wid <n>` | 固定图集宽度（2 的幂） |
| `--height, --hei <n>` | 固定图集高度（2 的幂；注意 `-h` 现在是帮助，不再是高度） |
| `-q, --quiet` | 减少控制台输出 |

- **不带参数运行**会交互式提示输入根目录路径。
- **目录约定**：根目录须含 `input/` 与 `output/`，默认根目录为 `Bin`。
- **包结构**：`input/` 下每个子目录为一个"包"，须含**恰好一张**源贴图 + 若干 FBX（递归收集）；若 `input/` 下没有子目录，则 `input/` 本身视为一个包。
- **环境变量**：设 `POLYX_NO_PAUSE=1`（或 `true/yes/on`）可跳过结束时的"按任意键退出"，便于脚本化调用。

### 示例

```powershell
# 处理 Bin/input 下所有包，自动图集尺寸
PolyX Bin

# 指定根目录 + 固定 1024×1024 图集
PolyX --root D:\Assets\Chars -s 1024

# 固定非正方形尺寸（宽 1024、高 512）
PolyX --wid 1024 --hei 512 Bin

# 查看帮助
PolyX -h

# 脚本化（不暂停）
$env:POLYX_NO_PAUSE = 1; PolyX Bin
```

### 输出

- `<root>/output/atlas.png`：合并后的图集。
- `<root>/output/...`：与 `input/` 镜像的目录结构，内含改写后的 FBX（UV 指向图集、材质连到 `atlas.png`）。

---

## 打包发布

```powershell
# 默认从 out/release-vs/Release 取 PolyX.exe，生成 dist/PolyX-0.2.0-win64.zip
pwsh scripts/package-release.ps1
```

静态 Release 产物无需 `libfbxsdk.dll`，GDI+ 由系统提供。详见 [scripts/package-release.ps1](scripts/package-release.ps1)。

---

## 项目结构

```
app/      入口与批处理编排（main.cpp, BatchProcessor）
uv/       UV 区域分析（核心算法，UVAnalyzer）
atlas/    图集打包 + 图片 I/O（AtlasBuilder, TgaLoader）
fbx/      FBX SDK 封装（FbxLoader）
core/     命令行解析与日志（Config, Logger）
scripts/  发布打包脚本
docs/     架构、待办、清理清单
Bin/      示例数据（Landlord 角色），已被 .gitignore 忽略
```

---

## 平台说明

当前**仅支持 Windows**：依赖 GDI+、`conio.h`、`Windows.h`。跨平台移植需替换图片 I/O 与控制台暂停逻辑（见 [docs/BACKLOG.md](docs/BACKLOG.md)）。

## 版本

当前 `0.3.0`。后续迭代方向见 [docs/BACKLOG.md](docs/BACKLOG.md)，代码清理施工图见 [docs/CLEANUP.md](docs/CLEANUP.md)。
