#ifndef INSTALLED_MODULE_H
#define INSTALLED_MODULE_H

// A module directory laid out the way lgpm installs one: the variant's files,
// plus the package's manifest.json and a `variant` file beside them.

#include <lgx.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace installed_module {

namespace fs = std::filesystem;

inline constexpr const char* kVariant = "linux-amd64";

inline void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// Builds a one-variant package named `name` from scratch in `work` and
// installs it as `modulesDir/<name>`. Returns that directory, or empty on
// failure.
inline fs::path install(const fs::path& work, const fs::path& modulesDir, const std::string& name) {
    const fs::path content = work / (name + "_content");
    writeText(content / (name + "_plugin.so"), "fake library content");
    writeText(content / "qml" / "Main.qml", "import QtQuick\nItem {}\n");

    const fs::path lgxPath = work / (name + ".lgx");
    if (!lgx_create(lgxPath.string().c_str(), name.c_str()).success)
        return {};
    lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
    if (!pkg)
        return {};
    const std::string main = name + "_plugin.so";
    bool ok = lgx_add_variant(pkg, kVariant, content.string().c_str(), main.c_str()).success &&
              lgx_save(pkg, lgxPath.string().c_str()).success;

    const fs::path staging = work / (name + "_staging");
    ok = ok && lgx_extract(pkg, kVariant, staging.string().c_str()).success;
    const char* manifest = ok ? lgx_get_manifest_json(pkg) : nullptr;
    const fs::path dir = modulesDir / name;
    if (manifest) {
        fs::create_directories(modulesDir);
        fs::rename(staging / kVariant, dir);
        writeText(dir / "manifest.json", manifest);
        writeText(dir / "variant", kVariant);
    }
    lgx_free_package(pkg);
    return manifest ? dir : fs::path();
}

} // namespace installed_module

#endif // INSTALLED_MODULE_H
