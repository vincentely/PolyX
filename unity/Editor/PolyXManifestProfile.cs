// Saved configuration asset for the PolyX manifest exporter.
#if UNITY_EDITOR
using UnityEditor;
using UnityEditor.Callbacks;
using UnityEngine;

namespace PolyX.EditorTools
{
    [CreateAssetMenu(
        fileName = "PolyX Manifest Configuration",
        menuName = "PolyX/Manifest Configuration")]
    public sealed class PolyXManifestProfile : ScriptableObject
    {
        [Tooltip("Root folder scanned for FBX model assets.")]
        public string folder = PolyXManifestExporter.DefaultFolder;

        [TextArea(2, 5)]
        [Tooltip("Folder names or sub-paths to exclude, separated by commas or new lines.")]
        public string excludeFolders = "";

        public Material[] excludeMaterials = new Material[0];

        [Header("Incremental")]
        public Texture2D atlasTexture;
        public Material atlasMaterial;
        public Material[] includeMaterials = new Material[0];
    }

    internal static class PolyXManifestProfileOpener
    {
        // A configuration asset is a work entry point: double-clicking it opens
        // the exporter and immediately switches the working copy to that asset.
        [OnOpenAsset]
        private static bool OpenProfile(int instanceId, int line)
        {
            var profile = EditorUtility.InstanceIDToObject(instanceId) as PolyXManifestProfile;
            if (profile == null) return false;

            PolyXManifestExporter.Open(profile);
            return true;
        }
    }
}
#endif
