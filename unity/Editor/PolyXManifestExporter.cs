// PolyX manifest exporter (Unity Editor tool).
//
// Full mode: scans an FBX folder and writes polyx_manifest.json.
// Incremental: pick Atlas Texture + Include Materials, derive FBX/meshes that
// use those materials, compute first empty 8x8 hole, write polyx_incremental.json.
//
// Menu: Tools > PolyX > Manifest Exporter.
#if UNITY_EDITOR
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEngine;

namespace PolyX.EditorTools
{
    public class PolyXManifestExporter : EditorWindow
    {
        private const string DefaultFolder = "Assets/Game/3D/Mesh/Pet";
        private const string ManifestName = "polyx_manifest.json";
        private const string IncrementalManifestName = "polyx_incremental.json";
        private const int BlockSize = 8;

        [SerializeField] private string _folder = DefaultFolder;
        [SerializeField] private string _excludeFolders = "";
        [SerializeField] private Material[] _excludeMaterials = new Material[0];

        [SerializeField] private Texture2D _atlasTexture;
        [SerializeField] private Material _atlasMaterial; // optional output material remap
        [SerializeField] private Material[] _includeMaterials = new Material[0];

        // Cached first-empty-block result. Never scan pixels inside OnGUI — Blit/ReadPixels
        // while IMGUI is painting will leave RenderTexture.active wrong and black out the Editor.
        private string _cachedAtlasPath;
        private int _cachedStartX;
        private int _cachedStartY;
        private bool _cachedStartValid;
        private bool _startScanPending;

        [MenuItem("Tools/PolyX/Manifest Exporter")]
        public static void Open()
        {
            GetWindow<PolyXManifestExporter>("PolyX Manifest");
        }

        private void OnDisable()
        {
            _startScanPending = false;
        }

        private void OnGUI()
        {
            EditorGUILayout.LabelField("Scan an FBX folder and export a PolyX request JSON (relative paths).",
                EditorStyles.wordWrappedLabel);
            EditorGUILayout.Space();

            using (new EditorGUILayout.HorizontalScope())
            {
                _folder = EditorGUILayout.TextField("FBX Folder", _folder);
                if (GUILayout.Button("Pick", GUILayout.Width(50)))
                {
                    string start = Directory.Exists(AbsFromAsset(_folder)) ? AbsFromAsset(_folder) : Application.dataPath;
                    string abs = EditorUtility.OpenFolderPanel("Select FBX folder (must be inside Assets)", start, "");
                    if (!string.IsNullOrEmpty(abs))
                    {
                        string rel = AssetFromAbs(abs);
                        if (rel != null) _folder = rel;
                        else EditorUtility.DisplayDialog("PolyX", "Folder must be inside the project's Assets.", "OK");
                    }
                }
            }
            EditorGUILayout.LabelField("Exclude folders (name or sub-path; one per line or comma-separated):",
                EditorStyles.wordWrappedLabel);
            _excludeFolders = EditorGUILayout.TextArea(_excludeFolders, GUILayout.Height(48));

            var so = new SerializedObject(this);
            so.Update();
            EditorGUILayout.PropertyField(so.FindProperty("_excludeMaterials"), new GUIContent("Exclude Materials"), true);
            so.ApplyModifiedProperties();
            EditorGUILayout.Space();

            using (new EditorGUI.DisabledScope(string.IsNullOrEmpty(_folder)))
            {
                if (GUILayout.Button("Scan & Export JSON", GUILayout.Height(30)))
                {
                    Export();
                }
            }

            EditorGUILayout.Space(12);
            EditorGUILayout.LabelField("Incremental", EditorStyles.boldLabel);
            EditorGUILayout.LabelField(
                "Append new meshes into an existing atlas. Pick the target atlas and materials to include; FBX entries are derived from those materials.",
                EditorStyles.wordWrappedLabel);
            EditorGUILayout.Space(4);

            so.Update();
            EditorGUI.BeginChangeCheck();
            EditorGUILayout.PropertyField(so.FindProperty("_atlasTexture"), new GUIContent("Atlas Texture"), true);
            bool atlasChanged = EditorGUI.EndChangeCheck();
            EditorGUILayout.PropertyField(so.FindProperty("_atlasMaterial"),
                new GUIContent("Atlas Material (Optional)"), true);
            EditorGUILayout.PropertyField(so.FindProperty("_includeMaterials"), new GUIContent("Include Materials"), true);
            so.ApplyModifiedProperties();

            string currentAtlasPath =
                _atlasTexture != null ? AssetDatabase.GetAssetPath(_atlasTexture) : string.Empty;
            if (atlasChanged || _cachedAtlasPath != currentAtlasPath)
            {
                ScheduleStartScan();
            }

            if (_atlasTexture != null && _startScanPending)
            {
                EditorGUILayout.LabelField("Append start 8x8", "scanning…");
            }
            else if (_atlasTexture != null && _cachedStartValid)
            {
                EditorGUILayout.LabelField("Append start 8x8", "(" + _cachedStartX + ", " + _cachedStartY + ")");
            }
            else if (_atlasTexture != null)
            {
                EditorGUILayout.HelpBox("Atlas has no append space, or pixels could not be read.", MessageType.Warning);
            }

            EditorGUILayout.Space();
            bool canIncremental = !string.IsNullOrEmpty(_folder) && _atlasTexture != null &&
                                  _includeMaterials != null && _includeMaterials.Any(m => m != null);
            using (new EditorGUI.DisabledScope(!canIncremental))
            {
                if (GUILayout.Button("Scan & Build JSON", GUILayout.Height(30)))
                {
                    ExportIncremental();
                }
            }
        }

        private void Export()
        {
            string folder;
            List<string> excludes;
            if (!PrepareFolder(out folder, out excludes)) return;

            var excludeMats = new HashSet<Material>((_excludeMaterials ?? new Material[0]).Where(m => m != null));
            var items = ScanMeshes(folder, excludes, excludeMats: excludeMats, includeMats: null);

            var manifest = new Manifest { version = 1, mode = "full", atlasSize = "auto", items = items };
            WriteManifest(folder, ManifestName, manifest, items);
        }

        private void ExportIncremental()
        {
            string folder;
            List<string> excludes;
            if (!PrepareFolder(out folder, out excludes)) return;

            if (_atlasTexture == null)
            {
                EditorUtility.DisplayDialog("PolyX", "Assign Atlas Texture.", "OK");
                return;
            }

            var includeMats = new HashSet<Material>((_includeMaterials ?? new Material[0]).Where(m => m != null));
            if (includeMats.Count == 0)
            {
                EditorUtility.DisplayDialog("PolyX", "Assign at least one Include Material.", "OK");
                return;
            }

            int startX, startY;
            if (!TryFindAppendStart(_atlasTexture, out startX, out startY))
            {
                EditorUtility.DisplayDialog("PolyX", "Atlas Texture has no append space after its last occupied 8x8 block.", "OK");
                return;
            }

            string atlasAssetPath = AssetDatabase.GetAssetPath(_atlasTexture);
            if (string.IsNullOrEmpty(atlasAssetPath))
            {
                EditorUtility.DisplayDialog("PolyX", "Atlas Texture must be a project asset.", "OK");
                return;
            }

            var items = ScanMeshes(folder, excludes, excludeMats: null, includeMats: includeMats);
            if (items.Count == 0)
            {
                EditorUtility.DisplayDialog("PolyX",
                    "No FBX meshes found under the folder that use the Include Materials.", "OK");
                return;
            }

            var manifest = new Manifest
            {
                version = 1,
                mode = "incremental",
                atlasSize = "",
                targetAtlas = RelFromFolder(folder, atlasAssetPath),
                targetMaterial = _atlasMaterial != null ? _atlasMaterial.name : "",
                startX = startX,
                startY = startY,
                items = items
            };
            WriteManifest(folder, IncrementalManifestName, manifest, items,
                "start=(" + startX + "," + startY + ")\nAtlas: " + atlasAssetPath +
                "\nMaterial: " + (_atlasMaterial != null ? _atlasMaterial.name : "(unchanged)") + "\n");
        }

        private bool PrepareFolder(out string folder, out List<string> excludes)
        {
            folder = (_folder ?? string.Empty).Trim().Replace('\\', '/').TrimEnd('/');
            excludes = (_excludeFolders ?? string.Empty)
                .Split(new[] { ',', ';', '\n', '\r' }, StringSplitOptions.RemoveEmptyEntries)
                .Select(s => s.Trim().Replace('\\', '/').Trim('/'))
                .Where(s => s.Length > 0)
                .ToList();

            if (!AssetDatabase.IsValidFolder(folder))
            {
                EditorUtility.DisplayDialog("PolyX", "Not a valid asset folder:\n" + folder, "OK");
                return false;
            }
            return true;
        }

        // excludeMats: full-mode blacklist. includeMats: incremental whitelist (null = all).
        private List<MFbx> ScanMeshes(string folder, List<string> excludes,
            HashSet<Material> excludeMats, HashSet<Material> includeMats)
        {
            string[] fbxGuids = AssetDatabase.FindAssets("t:Model", new[] { folder });
            var items = new List<MFbx>();
            var warnings = new List<string>();
            var excluded = new List<string>();

            try
            {
                for (int i = 0; i < fbxGuids.Length; i++)
                {
                    string fbxPath = AssetDatabase.GUIDToAssetPath(fbxGuids[i]);
                    if (!fbxPath.EndsWith(".fbx", StringComparison.OrdinalIgnoreCase)) continue;
                    if (IsExcluded(fbxPath, excludes)) { excluded.Add(fbxPath + "  [excluded folder]"); continue; }
                    EditorUtility.DisplayProgressBar("PolyX", fbxPath, (float)i / Mathf.Max(1, fbxGuids.Length));

                    var root = AssetDatabase.LoadAssetAtPath<GameObject>(fbxPath);
                    if (root == null) { warnings.Add("Cannot load model: " + fbxPath); continue; }

                    var fbxEntry = new MFbx { fbx = RelFromFolder(folder, fbxPath) };
                    foreach (var r in root.GetComponentsInChildren<Renderer>(true))
                    {
                        Mesh mesh = MeshOf(r);
                        if (mesh == null) continue;

                        if (HasTilingUVs(mesh))
                        {
                            excluded.Add(fbxPath + " :: " + r.name + "  [tiling UVs outside 0..1]");
                            continue;
                        }

                        var mats = r.sharedMaterials;
                        if (mats == null || mats.Length == 0)
                        {
                            excluded.Add(fbxPath + " :: " + r.name + "  [no material]");
                            continue;
                        }

                        if (includeMats != null)
                        {
                            if (!mats.Any(m => m != null && includeMats.Contains(m)))
                            {
                                continue;
                            }
                        }
                        else if (excludeMats != null)
                        {
                            Material hitMat = mats.FirstOrDefault(m => m != null && excludeMats.Contains(m));
                            if (hitMat != null)
                            {
                                excluded.Add(fbxPath + " :: " + r.name + "  [excluded material: " + hitMat.name + "]");
                                continue;
                            }
                        }

                        var texPaths = new List<string>();
                        bool anyTexture = false;
                        foreach (var mat in mats)
                        {
                            Texture mt = MainTexture(mat);
                            string mp = mt != null ? AssetDatabase.GetAssetPath(mt) : null;
                            if (!string.IsNullOrEmpty(mp))
                            {
                                texPaths.Add(RelFromFolder(folder, mp));
                                anyTexture = true;
                            }
                            else
                            {
                                texPaths.Add("");
                            }
                        }
                        if (!anyTexture)
                        {
                            excluded.Add(fbxPath + " :: " + r.name + "  [no main texture]");
                            continue;
                        }

                        bool allTextured = texPaths.TrueForAll(t => !string.IsNullOrEmpty(t));
                        bool sameShader = true;
                        Shader shader0 = null;
                        foreach (var mat in mats)
                        {
                            if (mat == null) { sameShader = false; break; }
                            if (shader0 == null) shader0 = mat.shader;
                            else if (mat.shader != shader0) { sameShader = false; break; }
                        }

                        fbxEntry.meshes.Add(new MMesh
                        {
                            mesh = mesh.name,
                            nodePath = NodePath(r.transform, root.transform),
                            textures = texPaths,
                            mergeSubmeshes = mats.Length > 1 && allTextured && sameShader,
                        });
                    }

                    if (fbxEntry.meshes.Count > 0) items.Add(fbxEntry);
                }
            }
            finally
            {
                EditorUtility.ClearProgressBar();
            }

            foreach (string line in excluded) Debug.Log("[PolyX] excluded - " + line);
            foreach (string w in warnings) Debug.LogWarning("[PolyX] " + w);
            return items;
        }

        private void WriteManifest(string folder, string fileName, Manifest manifest, List<MFbx> items,
            string extraDialog = null)
        {
            string json = JsonUtility.ToJson(manifest, true);
            string outAssetPath = folder + "/" + fileName;
            File.WriteAllText(AbsFromAsset(outAssetPath), json, new UTF8Encoding(false));
            AssetDatabase.ImportAsset(outAssetPath);

            int meshCount = items.Sum(e => e.meshes.Count);
            Debug.Log("[PolyX] Exported " + meshCount + " meshes across " + items.Count + " FBX -> " + outAssetPath);

            var asset = AssetDatabase.LoadAssetAtPath<TextAsset>(outAssetPath);
            if (asset != null) EditorGUIUtility.PingObject(asset);
            EditorUtility.DisplayDialog("PolyX",
                "Exported " + meshCount + " meshes across " + items.Count + " FBX.\n" +
                (extraDialog ?? "") + "\n" + outAssetPath,
                "OK");
        }

        private void ScheduleStartScan()
        {
            _cachedAtlasPath =
                _atlasTexture != null ? AssetDatabase.GetAssetPath(_atlasTexture) : string.Empty;
            _cachedStartValid = false;
            if (_atlasTexture == null)
            {
                _startScanPending = false;
                return;
            }

            if (_startScanPending) return;
            _startScanPending = true;

            // Run after the current IMGUI event so we never touch RenderTexture.active mid-paint.
            EditorApplication.delayCall += () =>
            {
                if (this == null) return;
                _startScanPending = false;
                Texture2D atlas = _atlasTexture;
                _cachedAtlasPath =
                    atlas != null ? AssetDatabase.GetAssetPath(atlas) : string.Empty;
                int sx = 0;
                int sy = 0;
                _cachedStartValid = atlas != null && TryFindAppendStart(atlas, out sx, out sy);
                if (_cachedStartValid)
                {
                    _cachedStartX = sx;
                    _cachedStartY = sy;
                }
                Repaint();
            };
        }

        // Coordinates match PolyX C++ atlas buffers: (0,0) = top-left, Y increases downward.
        private static bool TryFindAppendStart(Texture2D atlas, out int startX, out int startY)
        {
            startX = 0;
            startY = 0;
            if (atlas == null) return false;

            Color32[] pixels = ReadPixelsTopLeft(atlas);
            if (pixels == null) return false;

            int w = atlas.width;
            int h = atlas.height;
            if (pixels.Length < w * h || w % BlockSize != 0 || h % BlockSize != 0) return false;

            // The atlas is packed left-to-right, top-to-bottom. Scan in the
            // exact reverse order to find the last occupied block, then return
            // its successor in forward row-major order.
            for (int y = h - BlockSize; y >= 0; y -= BlockSize)
            {
                for (int x = w - BlockSize; x >= 0; x -= BlockSize)
                {
                    if (!IsBlockEmpty(pixels, w, h, x, y, BlockSize))
                    {
                        startX = x + BlockSize;
                        startY = y;
                        if (startX >= w)
                        {
                            startX = 0;
                            startY += BlockSize;
                        }
                        if (startY >= h) return false;
                        return true;
                    }
                }
            }

            // Entire atlas is empty.
            return true;
        }

        private static bool IsBlockEmpty(Color32[] pixels, int width, int height, int blockX, int blockY, int blockSize)
        {
            int x1 = Mathf.Min(blockX + blockSize, width);
            int y1 = Mathf.Min(blockY + blockSize, height);
            for (int y = blockY; y < y1; y++)
            {
                int row = y * width;
                for (int x = blockX; x < x1; x++)
                {
                    Color32 c = pixels[row + x];
                    // Generated/source PNGs often encode unused black space with
                    // alpha 255. Empty means black RGB; alpha is intentionally ignored.
                    if (c.r != 0 || c.g != 0 || c.b != 0) return false;
                }
            }
            return true;
        }

        // Returns a top-left origin buffer (row 0 = top), matching C++ atlas layout.
        // Never use Graphics.Blit/RenderTexture here: this runs from editor UI code,
        // and touching RenderTexture.active can corrupt the Editor's own render target.
        private static Color32[] ReadPixelsTopLeft(Texture2D src)
        {
            if (src == null) return null;

            Color32[] raw = null;
            if (src.isReadable)
            {
                try
                {
                    raw = src.GetPixels32();
                }
                catch (Exception e)
                {
                    Debug.LogWarning("[PolyX] GetPixels32 failed, reading source PNG: " + e.Message);
                }
            }

            if (raw == null)
            {
                string assetPath = AssetDatabase.GetAssetPath(src);
                if (string.IsNullOrEmpty(assetPath))
                {
                    Debug.LogError("[PolyX] Atlas Texture must be a project asset.");
                    return null;
                }

                TextureImporter importer = AssetImporter.GetAtPath(assetPath) as TextureImporter;
                if (importer == null)
                {
                    Debug.LogError("[PolyX] Atlas Texture has no TextureImporter: " + assetPath);
                    return null;
                }

                bool restoreReadability = !importer.isReadable;
                try
                {
                    if (restoreReadability)
                    {
                        importer.isReadable = true;
                        importer.SaveAndReimport();
                    }

                    Texture2D readableAsset = AssetDatabase.LoadAssetAtPath<Texture2D>(assetPath);
                    if (readableAsset == null)
                    {
                        Debug.LogError("[PolyX] Failed to reload Atlas Texture: " + assetPath);
                        return null;
                    }
                    raw = readableAsset.GetPixels32();
                }
                catch (Exception e)
                {
                    Debug.LogError("[PolyX] Failed to read Atlas Texture pixels: " + e.Message);
                    return null;
                }
                finally
                {
                    if (restoreReadability)
                    {
                        TextureImporter currentImporter =
                            AssetImporter.GetAtPath(assetPath) as TextureImporter;
                        if (currentImporter != null && currentImporter.isReadable)
                        {
                            currentImporter.isReadable = false;
                            currentImporter.SaveAndReimport();
                        }
                    }
                }
            }

            return FlipBottomUpToTopLeft(raw, src.width, src.height);
        }

        private static Color32[] FlipBottomUpToTopLeft(Color32[] raw, int w, int h)
        {
            if (raw == null || raw.Length < w * h) return null;
            var flipped = new Color32[w * h];
            for (int y = 0; y < h; y++)
            {
                int srcRow = (h - 1 - y) * w;
                int dstRow = y * w;
                Array.Copy(raw, srcRow, flipped, dstRow, w);
            }
            return flipped;
        }

        private static Mesh MeshOf(Renderer r)
        {
            if (r is SkinnedMeshRenderer smr) return smr.sharedMesh;
            var mf = r.GetComponent<MeshFilter>();
            return mf != null ? mf.sharedMesh : null;
        }

        private static bool HasTilingUVs(Mesh mesh)
        {
            const float eps = 0.01f;
            var uv = mesh.uv;
            for (int i = 0; i < uv.Length; i++)
            {
                if (uv[i].x < -eps || uv[i].x > 1f + eps || uv[i].y < -eps || uv[i].y > 1f + eps)
                {
                    return true;
                }
            }
            return false;
        }

        private static bool IsExcluded(string assetPath, List<string> excludes)
        {
            if (excludes == null || excludes.Count == 0) return false;
            string p = assetPath.Replace('\\', '/');
            foreach (var e in excludes)
            {
                if (p.IndexOf("/" + e + "/", StringComparison.OrdinalIgnoreCase) >= 0) return true;
                if (p.StartsWith(e + "/", StringComparison.OrdinalIgnoreCase)) return true;
                if (p.EndsWith("/" + e, StringComparison.OrdinalIgnoreCase)) return true;
            }
            return false;
        }

        private static Texture MainTexture(Material m)
        {
            if (m == null) return null;
            foreach (string prop in new[] { "_BaseMap", "_MainTex" })
            {
                if (m.HasProperty(prop))
                {
                    var t = m.GetTexture(prop);
                    if (t != null) return t;
                }
            }
            return m.mainTexture;
        }

        private static string NodePath(Transform t, Transform root)
        {
            var names = new List<string>();
            for (var cur = t; cur != null; cur = cur.parent)
            {
                names.Add(cur.name);
                if (cur == root) break;
            }
            names.Reverse();
            return "/" + string.Join("/", names);
        }

        private static string RelFromFolder(string folder, string target)
        {
            var from = folder.Split('/').Where(s => s.Length > 0).ToList();
            var to = target.Split('/').Where(s => s.Length > 0).ToList();
            int common = 0;
            while (common < from.Count && common < to.Count && from[common] == to[common]) common++;

            var sb = new StringBuilder();
            for (int i = common; i < from.Count; i++) sb.Append("../");
            for (int i = common; i < to.Count; i++)
            {
                sb.Append(to[i]);
                if (i < to.Count - 1) sb.Append('/');
            }
            return sb.ToString();
        }

        private static string ProjectRoot => Directory.GetParent(Application.dataPath).FullName.Replace('\\', '/');
        private static string AbsFromAsset(string assetPath) => ProjectRoot + "/" + assetPath;

        private static string AssetFromAbs(string abs)
        {
            abs = abs.Replace('\\', '/');
            string root = ProjectRoot + "/";
            return abs.StartsWith(root, StringComparison.OrdinalIgnoreCase) ? abs.Substring(root.Length) : null;
        }

        [Serializable]
        private class MMesh
        {
            public string mesh;
            public string nodePath;
            public List<string> textures = new List<string>();
            public bool mergeSubmeshes;
        }

        [Serializable]
        private class MFbx
        {
            public string fbx;
            public List<MMesh> meshes = new List<MMesh>();
        }

        [Serializable]
        private class Manifest
        {
            public int version = 1;
            public string mode = "full";
            public string atlasSize = "auto";
            public string targetAtlas = "";
            public string targetMaterial = "";
            public int startX;
            public int startY;
            public List<MFbx> items = new List<MFbx>();
        }
    }
}
#endif
