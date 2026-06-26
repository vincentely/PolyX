// PolyX manifest exporter (Unity Editor tool).
//
// Scans an FBX folder (recursively), resolves each mesh's main texture via the
// asset graph, and writes a PolyX request JSON (relative paths) into that folder.
// The C++ PolyX tool consumes that JSON. See docs/UNITY-PIPELINE.md.
//
// Deploy: copy this file under an Editor folder in your Unity project
// (e.g. Assets/Editor/PolyX/). Menu: Tools > PolyX > Manifest Exporter.
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

        [SerializeField] private string _folder = DefaultFolder;

        [MenuItem("Tools/PolyX/Manifest Exporter")]
        public static void Open()
        {
            GetWindow<PolyXManifestExporter>("PolyX Manifest");
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
            EditorGUILayout.Space();

            using (new EditorGUI.DisabledScope(string.IsNullOrEmpty(_folder)))
            {
                if (GUILayout.Button("Scan & Export JSON", GUILayout.Height(30)))
                {
                    Export();
                }
            }
        }

        private void Export()
        {
            string folder = (_folder ?? string.Empty).Trim().Replace('\\', '/').TrimEnd('/');
            if (!AssetDatabase.IsValidFolder(folder))
            {
                EditorUtility.DisplayDialog("PolyX", "Not a valid asset folder:\n" + folder, "OK");
                return;
            }

            string[] fbxGuids = AssetDatabase.FindAssets("t:Model", new[] { folder });
            var items = new List<MFbx>();
            var warnings = new List<string>();
            int fbxCount = 0;

            try
            {
                for (int i = 0; i < fbxGuids.Length; i++)
                {
                    string fbxPath = AssetDatabase.GUIDToAssetPath(fbxGuids[i]);
                    if (!fbxPath.EndsWith(".fbx", StringComparison.OrdinalIgnoreCase)) continue;
                    fbxCount++;
                    EditorUtility.DisplayProgressBar("PolyX", fbxPath, (float)i / Mathf.Max(1, fbxGuids.Length));

                    var root = AssetDatabase.LoadAssetAtPath<GameObject>(fbxPath);
                    if (root == null) { warnings.Add("Cannot load model: " + fbxPath); continue; }

                    var fbxEntry = new MFbx { fbx = RelFromFolder(folder, fbxPath) };
                    foreach (var r in root.GetComponentsInChildren<Renderer>(true))
                    {
                        Mesh mesh = MeshOf(r);
                        if (mesh == null) continue;

                        var mats = r.sharedMaterials;
                        if (mats == null || mats.Length == 0)
                        {
                            warnings.Add("No material: " + fbxPath + " :: " + r.name);
                            continue;
                        }

                        // Collect the distinct main textures across all material slots.
                        // Multi-submesh meshes whose slots share ONE texture are fine (the whole
                        // mesh is atlased against it). Slots using DIFFERENT textures would need
                        // per-submesh atlasing (not yet supported), so skip them to avoid wrong UVs.
                        var texPaths = new List<string>();
                        foreach (var mat in mats)
                        {
                            Texture mt = MainTexture(mat);
                            if (mt == null) continue;
                            string mp = AssetDatabase.GetAssetPath(mt);
                            if (!string.IsNullOrEmpty(mp) && !texPaths.Contains(mp)) texPaths.Add(mp);
                        }
                        if (texPaths.Count == 0)
                        {
                            warnings.Add("No main texture: " + fbxPath + " :: " + r.name);
                            continue;
                        }
                        if (texPaths.Count > 1)
                        {
                            warnings.Add("Skipped (submeshes use " + texPaths.Count + " different textures; per-submesh atlas not supported): " + fbxPath + " :: " + r.name);
                            continue;
                        }

                        fbxEntry.meshes.Add(new MMesh
                        {
                            mesh = mesh.name,
                            nodePath = NodePath(r.transform, root.transform),
                            texture = RelFromFolder(folder, texPaths[0]),
                        });
                    }

                    if (fbxEntry.meshes.Count > 0) items.Add(fbxEntry);
                }
            }
            finally
            {
                EditorUtility.ClearProgressBar();
            }

            var manifest = new Manifest { version = 1, atlasSize = "auto", items = items };
            string json = JsonUtility.ToJson(manifest, true);
            string outAssetPath = folder + "/" + ManifestName;
            File.WriteAllText(AbsFromAsset(outAssetPath), json, new UTF8Encoding(false));
            AssetDatabase.ImportAsset(outAssetPath);

            int meshCount = items.Sum(e => e.meshes.Count);
            foreach (string w in warnings) Debug.LogWarning("[PolyX] " + w);
            Debug.Log("[PolyX] Exported " + meshCount + " meshes across " + items.Count + " FBX -> " + outAssetPath + " (warnings: " + warnings.Count + ")");

            var asset = AssetDatabase.LoadAssetAtPath<TextAsset>(outAssetPath);
            if (asset != null) EditorGUIUtility.PingObject(asset);
            EditorUtility.DisplayDialog("PolyX",
                "Exported " + meshCount + " meshes across " + items.Count + " FBX.\nWarnings: " + warnings.Count + "\n\n" + outAssetPath,
                "OK");
        }

        private static Mesh MeshOf(Renderer r)
        {
            if (r is SkinnedMeshRenderer smr) return smr.sharedMesh;
            var mf = r.GetComponent<MeshFilter>();
            return mf != null ? mf.sharedMesh : null;
        }

        // URP main map is _BaseMap; Built-in is _MainTex. Fall back to mainTexture.
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

        // "/Root/Child/Mesh" path from the model root to the renderer's transform.
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

        // Forward-slash path from `folder` (asset path) to `target` (asset path), with ../ as needed.
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
            public string texture;
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
            public string atlasSize = "auto";
            public List<MFbx> items = new List<MFbx>();
        }
    }
}
#endif
