#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace polyx::manifest
{
// Request: what Unity exports for PolyX to merge. See docs/UNITY-PIPELINE.md.
struct MeshEntry
{
    std::string mesh;                   // mesh name (fallback identity)
    std::string nodePath;               // "/Name/Name/..." node path (primary identity)
    std::vector<std::string> textures;  // one per material slot / submesh (in slot order)
};

struct RequestItem
{
    std::string fbx;                  // path to the source FBX
    std::vector<MeshEntry> meshes;    // one or more meshes inside this FBX
};

struct Request
{
    int version = 1;
    std::string atlasSize = "auto"; // "auto" | "1024" | "1024x512"
    std::string assetsRoot;
    std::string outputRoot;
    std::string atlasOut;
    std::vector<RequestItem> items;
};

// Result: what PolyX writes back for Unity to re-wire.
struct ResultItem
{
    std::string fbx;
    std::string nodePath;
    std::string mesh;
    std::string outputFbx;
    std::string uvSet;
    std::string status; // "ok" | "warn" | "skipped" | "error"
    std::string detail; // e.g. "submesh:2", "kind:full", "mesh-not-found"
};

struct Result
{
    std::string atlasOut;
    int atlasWidth = 0;
    int atlasHeight = 0;
    std::vector<ResultItem> items;
    std::vector<std::string> warnings;
};

// JSON I/O (UTF-8). Both return false and set errorMessage on failure.
bool ReadRequest(const std::filesystem::path& file, Request& out, std::string* errorMessage = nullptr);
bool WriteResult(const std::filesystem::path& file, const Result& result, std::string* errorMessage = nullptr);
} // namespace polyx::manifest
