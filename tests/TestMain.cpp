// Dependency-free unit tests for PolyX.
//
// Covers the pure-logic layers that do not require the FBX SDK:
//   - AtlasBuilder packing / dedup / auto-size (regression for the PackTiles
//     param-vs-member bug)
//   - TgaLoader 16-bit attribute-bit alpha (regression for the always-opaque bug)
//   - Config command-line parsing
//
// Tiny assert-based harness (no third-party framework) so it builds offline.

#include "app/Manifest.h"
#include "atlas/AtlasBuilder.h"
#include "atlas/TgaLoader.h"
#include "core/Config.h"
#include "uv/UVRegion.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        ++g_checks;                                                            \
        if (!(cond))                                                           \
        {                                                                      \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                      \
    } while (0)

namespace atlas = polyx::atlas;
namespace core = polyx::core;
namespace uvd = polyx::uv::detail;
namespace mf = polyx::manifest;
namespace fs = std::filesystem;

atlas::Image MakeSolid(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    atlas::Image image;
    image.width = w;
    image.height = h;
    image.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0);
    for (std::size_t i = 0; i + 3U < image.pixels.size(); i += 4U)
    {
        image.pixels[i + 0] = r;
        image.pixels[i + 1] = g;
        image.pixels[i + 2] = b;
        image.pixels[i + 3] = a;
    }
    return image;
}

bool Overlaps(const atlas::Rect& a, const atlas::Rect& b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

bool ParseArgs(std::vector<std::string> args, core::AppConfig& config, std::string* error)
{
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args)
    {
        argv.push_back(arg.data());
    }
    return core::ParseCommandLine(static_cast<int>(argv.size()), argv.data(), config, error);
}

void WriteTga(const fs::path& path,
              int width,
              int height,
              std::uint8_t pixelDepth,
              std::uint8_t imageDescriptor,
              std::uint8_t imageType,
              const std::vector<std::uint8_t>& pixels)
{
    std::uint8_t header[18] = { 0 };
    header[2] = imageType;
    header[12] = static_cast<std::uint8_t>(width & 0xFF);
    header[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);
    header[14] = static_cast<std::uint8_t>(height & 0xFF);
    header[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);
    header[16] = pixelDepth;
    header[17] = imageDescriptor;

    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

// --- AtlasBuilder ----------------------------------------------------------

// Regression for the PackTiles param-vs-member bug: with small default member
// dimensions (64x64) and auto-size on, a correct packer must grow past the
// members to fit the tiles. The buggy version trial-packed against the stale
// members and never converged, so Build() failed.
void TestAtlasAutoSizeGrowsPastMembers()
{
    atlas::AtlasBuilder builder(64, 64);
    builder.SetAutoSize(true);

    for (int i = 0; i < 5; ++i)
    {
        const atlas::Image tile = MakeSolid(32, 32, static_cast<std::uint8_t>(10 + i * 30), 0, 0, 255);
        std::string error;
        CHECK(builder.AddTile("tile" + std::to_string(i), tile, atlas::Rect{ 0, 0, 32, 32 }, &error));
    }

    std::string error;
    const bool ok = builder.Build(&error);
    CHECK(ok);
    if (!ok)
    {
        return;
    }

    const atlas::Image& atlasImage = builder.GetAtlasImage();
    CHECK(atlasImage.width == atlasImage.height);
    CHECK((atlasImage.width & (atlasImage.width - 1)) == 0); // power of two
    CHECK(atlasImage.width >= 32);

    const std::vector<atlas::AtlasEntry>& entries = builder.Entries();
    CHECK(entries.size() == 5U);
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const atlas::Rect& r = entries[i].atlasRect;
        CHECK(r.x >= 0 && r.y >= 0);
        CHECK(r.x + r.width <= atlasImage.width);
        CHECK(r.y + r.height <= atlasImage.height);
        for (std::size_t j = i + 1; j < entries.size(); ++j)
        {
            CHECK(!Overlaps(r, entries[j].atlasRect));
        }
    }
}

void TestAtlasDedupByKey()
{
    atlas::AtlasBuilder builder(256, 256);
    const atlas::Image tile = MakeSolid(16, 16, 5, 5, 5, 255);
    std::string error;
    CHECK(builder.AddTile("k", tile, atlas::Rect{ 0, 0, 16, 16 }, &error));
    CHECK(builder.AddTile("k", tile, atlas::Rect{ 0, 0, 16, 16 }, &error)); // same key + content -> no dup
    CHECK(builder.Build(&error));
    CHECK(builder.Entries().size() == 1U);
    CHECK(builder.FindEntry("k") != nullptr);
    CHECK(builder.FindEntry("missing") == nullptr);
}

void TestAtlasFixedSizeTooSmallFails()
{
    atlas::AtlasBuilder builder(16, 16); // fixed size, auto-size off by default
    const atlas::Image tile = MakeSolid(32, 32, 1, 2, 3, 255);
    std::string error;
    CHECK(builder.AddTile("big", tile, atlas::Rect{ 0, 0, 32, 32 }, &error));
    CHECK(!builder.Build(&error)); // tile wider than the atlas -> must fail
}

// Stress the skyline packer: many varied tiles must all land in-bounds and never
// overlap (the core correctness guarantee of the packing change).
void TestAtlasPackingDenseNoOverlap()
{
    atlas::AtlasBuilder builder;
    builder.SetAutoSize(true);
    const int sizes[][2] = {
        { 16, 8 }, { 8, 32 }, { 24, 16 }, { 32, 32 }, { 8, 8 },
        { 16, 16 }, { 40, 8 }, { 8, 24 }, { 24, 24 }, { 12, 20 }
    };

    int count = 0;
    for (int rep = 0; rep < 2; ++rep)
    {
        for (const auto& s : sizes)
        {
            const atlas::Image tile = MakeSolid(s[0], s[1],
                                                static_cast<std::uint8_t>(count * 9 + 1),
                                                static_cast<std::uint8_t>(count * 5 + 2),
                                                static_cast<std::uint8_t>(count * 3 + 3), 255);
            std::string error;
            CHECK(builder.AddTile("dense" + std::to_string(count), tile, atlas::Rect{ 0, 0, s[0], s[1] }, &error));
            ++count;
        }
    }

    std::string error;
    CHECK(builder.Build(&error));
    const atlas::Image& atlasImage = builder.GetAtlasImage();
    const std::vector<atlas::AtlasEntry>& entries = builder.Entries();
    CHECK(entries.size() == static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const atlas::Rect& r = entries[i].atlasRect;
        CHECK(r.x >= 0 && r.y >= 0);
        CHECK(r.x + r.width <= atlasImage.width);
        CHECK(r.y + r.height <= atlasImage.height);
        for (std::size_t j = i + 1; j < entries.size(); ++j)
        {
            CHECK(!Overlaps(r, entries[j].atlasRect));
        }
    }
}

// --- TgaLoader -------------------------------------------------------------

// Regression for the 16-bit alpha bug. With 1 attribute bit declared, the high
// bit must map to alpha (255 / 0); previously both branches yielded 255.
void TestTga16BitHonorsAlphaBit()
{
    const fs::path path = fs::temp_directory_path() / "polyx_test_16_alpha.tga";
    const std::uint16_t opaque = 0xFFFF;      // A=1, R=G=B=31
    const std::uint16_t transparent = 0x7C00; // A=0, R=31, G=B=0
    const std::vector<std::uint8_t> pixels = {
        static_cast<std::uint8_t>(opaque & 0xFF), static_cast<std::uint8_t>(opaque >> 8),
        static_cast<std::uint8_t>(transparent & 0xFF), static_cast<std::uint8_t>(transparent >> 8)
    };
    WriteTga(path, 2, 1, 16, 0x21, 2, pixels); // 0x20 top-origin | 0x01 alpha bit

    atlas::Image image;
    std::string error;
    CHECK(atlas::LoadTgaImageFile(path, image, &error));
    CHECK(image.width == 2 && image.height == 1);
    if (image.pixels.size() >= 8U)
    {
        CHECK(image.pixels[3] == 255); // opaque pixel
        CHECK(image.pixels[7] == 0);   // transparent pixel
        CHECK(image.pixels[0] == 255); // red of pixel 0
        CHECK(image.pixels[4] == 255); // red of pixel 1
    }
    fs::remove(path);
}

void TestTga16BitNoAlphaBitsIsOpaque()
{
    const fs::path path = fs::temp_directory_path() / "polyx_test_16_noalpha.tga";
    const std::uint16_t transparentBit = 0x7C00; // high bit clear
    const std::vector<std::uint8_t> pixels = {
        static_cast<std::uint8_t>(transparentBit & 0xFF), static_cast<std::uint8_t>(transparentBit >> 8)
    };
    WriteTga(path, 1, 1, 16, 0x20, 2, pixels); // top-origin, 0 attribute bits

    atlas::Image image;
    std::string error;
    CHECK(atlas::LoadTgaImageFile(path, image, &error));
    if (image.pixels.size() >= 4U)
    {
        CHECK(image.pixels[3] == 255); // no attribute bits -> forced opaque
    }
    fs::remove(path);
}

void TestTga24BitDecode()
{
    const fs::path path = fs::temp_directory_path() / "polyx_test_24.tga";
    const std::vector<std::uint8_t> pixels = { 10, 20, 30 }; // stored B, G, R
    WriteTga(path, 1, 1, 24, 0x20, 2, pixels);

    atlas::Image image;
    std::string error;
    CHECK(atlas::LoadTgaImageFile(path, image, &error));
    if (image.pixels.size() >= 4U)
    {
        CHECK(image.pixels[0] == 30);  // R
        CHECK(image.pixels[1] == 20);  // G
        CHECK(image.pixels[2] == 10);  // B
        CHECK(image.pixels[3] == 255); // A
    }
    fs::remove(path);
}

// --- Config ----------------------------------------------------------------

void TestConfigParsing()
{
    {
        core::AppConfig config;
        std::string error;
        CHECK(ParseArgs({ "PolyX", "-s", "512" }, config, &error));
        CHECK(!config.autoAtlasSize);
        CHECK(config.requestedAtlasWidth == 512U);
        CHECK(config.requestedAtlasHeight == 512U);
    }
    {
        core::AppConfig config;
        std::string error;
        CHECK(!ParseArgs({ "PolyX", "-s", "7" }, config, &error)); // not a power of two
    }
    {
        core::AppConfig config;
        std::string error;
        CHECK(ParseArgs({ "PolyX", "--auto-size" }, config, &error));
        CHECK(config.autoAtlasSize);
    }
    {
        core::AppConfig config;
        std::string error;
        CHECK(ParseArgs({ "PolyX", "--help" }, config, &error));
        CHECK(config.showHelp);
    }
    {
        core::AppConfig config;
        std::string error;
        CHECK(ParseArgs({ "PolyX", "-h" }, config, &error)); // -h is now help, not height
        CHECK(config.showHelp);
    }
    {
        core::AppConfig config;
        std::string error;
        CHECK(ParseArgs({ "PolyX", "--wid", "512", "--hei", "256" }, config, &error));
        CHECK(!config.autoAtlasSize);
        CHECK(config.requestedAtlasWidth == 512U);
        CHECK(config.requestedAtlasHeight == 256U);
    }
    {
        core::AppConfig config;
        std::string error;
        CHECK(!ParseArgs({ "PolyX", "--bogus" }, config, &error)); // unknown option
    }
}

// --- UV region helpers -----------------------------------------------------

void TestQuantizeToBlockOrigin()
{
    {
        const auto [x, y] = uvd::QuantizeToBlockOrigin(0.0, 1.0, 64, 64, 8);
        CHECK(x == 0 && y == 0); // u=0, v=1 -> top-left (V is flipped)
    }
    {
        const auto [x, y] = uvd::QuantizeToBlockOrigin(0.5, 0.5, 64, 64, 8);
        CHECK(x == 32 && y == 32);
    }
    {
        const auto [x, y] = uvd::QuantizeToBlockOrigin(1.0, 0.0, 64, 64, 8);
        CHECK(x == 56 && y == 56); // clamps to texel 63, block-aligned to 56
    }
    {
        const auto [x, y] = uvd::QuantizeToBlockOrigin(0.5, 0.9, 64, 64, 8);
        CHECK(x == 32 && y == 0); // v near 1 maps near the top edge
    }
}

void TestUnionFind()
{
    uvd::UnionFind uf(5);
    for (int i = 0; i < 5; ++i)
    {
        CHECK(uf.Find(i) == i);
    }
    uf.Unite(0, 1);
    uf.Unite(3, 4);
    CHECK(uf.Find(0) == uf.Find(1));
    CHECK(uf.Find(3) == uf.Find(4));
    CHECK(uf.Find(0) != uf.Find(2));
    CHECK(uf.Find(2) != uf.Find(3));
    uf.Unite(1, 4); // merge the two groups
    CHECK(uf.Find(0) == uf.Find(3));
}

// --- Manifest JSON I/O -----------------------------------------------------

void TestManifestRoundTrip()
{
    const fs::path reqPath = fs::temp_directory_path() / "polyx_req.json";
    {
        std::ofstream out(reqPath, std::ios::binary);
        out << R"({
  "version": 1,
  "atlasSize": "auto",
  "assetsRoot": "C:/proj/Assets",
  "outputRoot": "C:/proj/Out",
  "atlasOut": "C:/proj/Out/atlas.png",
  "items": [
    { "fbx": "C:/a/M.fbx", "mesh": "Body", "nodePath": "/M/Body", "texture": "C:/a/T.tga" },
    { "fbx": "C:/a/M.fbx", "mesh": "Hat", "nodePath": "/M/Hat", "texture": "C:/a/T2.tga" }
  ]
})";
    }

    mf::Request req;
    std::string error;
    const bool readOk = mf::ReadRequest(reqPath, req, &error);
    if (!readOk) std::printf("ReadRequest failed: %s\n", error.c_str());
    CHECK(readOk);
    CHECK(req.version == 1);
    CHECK(req.atlasSize == "auto");
    CHECK(req.outputRoot == "C:/proj/Out");
    CHECK(req.items.size() == 2U);
    if (req.items.size() == 2U)
    {
        CHECK(req.items[0].mesh == "Body");
        CHECK(req.items[0].nodePath == "/M/Body");
        CHECK(req.items[0].texture == "C:/a/T.tga");
        CHECK(req.items[1].mesh == "Hat");
        CHECK(req.items[1].texture == "C:/a/T2.tga");
    }
    fs::remove(reqPath);

    mf::Result res;
    res.atlasOut = "C:/proj/Out/atlas.png";
    res.atlasWidth = 1024;
    res.atlasHeight = 512;
    mf::ResultItem item;
    item.fbx = "C:/a/M.fbx";
    item.nodePath = "/M/Body";
    item.mesh = "Body";
    item.outputFbx = "C:/proj/Out/M.fbx";
    item.uvSet = "map1";
    item.status = "ok";
    res.items.push_back(item);
    res.warnings.push_back("test warning");

    const fs::path resPath = fs::temp_directory_path() / "polyx_res.json";
    const bool writeOk = mf::WriteResult(resPath, res, &error);
    if (!writeOk) std::printf("WriteResult failed: %s\n", error.c_str());
    CHECK(writeOk);

    std::string content;
    {
        std::ifstream in(resPath, std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(content.find("\"status\": \"ok\"") != std::string::npos);
    CHECK(content.find("\"atlasWidth\": 1024") != std::string::npos);
    CHECK(content.find("test warning") != std::string::npos);
    fs::remove(resPath);
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: do not lose output on abort

    try
    {
        TestAtlasAutoSizeGrowsPastMembers();
        TestAtlasDedupByKey();
        TestAtlasFixedSizeTooSmallFails();
        TestAtlasPackingDenseNoOverlap();
        TestTga16BitHonorsAlphaBit();
        TestTga16BitNoAlphaBitsIsOpaque();
        TestTga24BitDecode();
        TestConfigParsing();
        TestQuantizeToBlockOrigin();
        TestUnionFind();
        TestManifestRoundTrip();
    }
    catch (const std::exception& e)
    {
        std::printf("UNCAUGHT EXCEPTION: %s\n", e.what());
        return 2;
    }
    catch (...)
    {
        std::printf("UNCAUGHT unknown exception\n");
        return 2;
    }

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
