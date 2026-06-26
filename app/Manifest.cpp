#include "app/Manifest.h"

#include <fstream>

#include <json.hpp>

namespace polyx::manifest
{
namespace
{
using nlohmann::json;

std::string GetString(const json& obj, const char* key, const std::string& fallback = std::string())
{
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_string())
    {
        return it->get<std::string>();
    }
    return fallback;
}
} // namespace

bool ReadRequest(const std::filesystem::path& file, Request& out, std::string* errorMessage)
{
    out = Request{};

    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to open request file: " + file.string();
        }
        return false;
    }

    json root;
    try
    {
        stream >> root;
    }
    catch (const std::exception& e)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("Request JSON parse error: ") + e.what();
        }
        return false;
    }

    if (!root.is_object())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Request root must be a JSON object.";
        }
        return false;
    }

    if (const auto it = root.find("version"); it != root.end() && it->is_number_integer())
    {
        out.version = it->get<int>();
    }
    out.atlasSize = GetString(root, "atlasSize", "auto");
    out.assetsRoot = GetString(root, "assetsRoot");
    out.outputRoot = GetString(root, "outputRoot");
    out.atlasOut = GetString(root, "atlasOut");

    if (const auto it = root.find("items"); it != root.end() && it->is_array())
    {
        out.items.reserve(it->size());
        for (const json& entry : *it)
        {
            if (!entry.is_object())
            {
                continue;
            }
            RequestItem item;
            item.fbx = GetString(entry, "fbx");
            if (const auto meshIt = entry.find("meshes"); meshIt != entry.end() && meshIt->is_array())
            {
                for (const json& meshNode : *meshIt)
                {
                    if (!meshNode.is_object())
                    {
                        continue;
                    }
                    MeshEntry meshEntry;
                    meshEntry.mesh = GetString(meshNode, "mesh");
                    meshEntry.nodePath = GetString(meshNode, "nodePath");
                    if (const auto texIt = meshNode.find("textures"); texIt != meshNode.end() && texIt->is_array())
                    {
                        for (const json& tex : *texIt)
                        {
                            if (tex.is_string())
                            {
                                meshEntry.textures.push_back(tex.get<std::string>());
                            }
                        }
                    }
                    else
                    {
                        const std::string single = GetString(meshNode, "texture"); // back-compat
                        if (!single.empty())
                        {
                            meshEntry.textures.push_back(single);
                        }
                    }
                    if (const auto mergeIt = meshNode.find("mergeSubmeshes");
                        mergeIt != meshNode.end() && mergeIt->is_boolean())
                    {
                        meshEntry.mergeSubmeshes = mergeIt->get<bool>();
                    }
                    item.meshes.push_back(std::move(meshEntry));
                }
            }
            out.items.push_back(std::move(item));
        }
    }

    return true;
}

bool WriteResult(const std::filesystem::path& file, const Result& result, std::string* errorMessage)
{
    try
    {
        json root = json::object();
        root["atlasOut"] = result.atlasOut;
        root["atlasWidth"] = result.atlasWidth;
        root["atlasHeight"] = result.atlasHeight;

        json items = json::array();
        for (const ResultItem& item : result.items)
        {
            json entry = json::object();
            entry["fbx"] = item.fbx;
            entry["nodePath"] = item.nodePath;
            entry["mesh"] = item.mesh;
            entry["outputFbx"] = item.outputFbx;
            entry["uvSet"] = item.uvSet;
            entry["status"] = item.status;
            entry["detail"] = item.detail;
            items.push_back(std::move(entry));
        }
        root["items"] = std::move(items);

        json warnings = json::array();
        for (const std::string& warning : result.warnings)
        {
            warnings.push_back(warning);
        }
        root["warnings"] = std::move(warnings);

        std::ofstream stream(file, std::ios::binary);
        if (!stream)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Failed to open result file for writing: " + file.string();
            }
            return false;
        }

        stream << root.dump(2) << '\n';
        if (!stream.good())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Failed while writing result file: " + file.string();
            }
            return false;
        }

        return true;
    }
    catch (const std::exception& e)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("Result JSON error: ") + e.what();
        }
        return false;
    }
}
} // namespace polyx::manifest
