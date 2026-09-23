// Which instance directory a module's host is given.

#include "fake_module_host.h"

namespace {

class InstancePersistenceTest : public FakeHostFixture {
protected:
    void TearDown() override {
        logos_core_set_persistence_base_path("");
        FakeHostFixture::TearDown();
    }

    // A host that writes down the instance directory it was given, then loads.
    void useRecordingHost() {
        const fs::path host = tmp.path / "recording_host";
        std::ofstream(host) << "#!/bin/sh\n"
            "while [ $# -gt 0 ]; do\n"
            "  case \"$1\" in\n"
            "    --instance-persistence-path) echo \"$2\" > \"" << (tmp.path / "given").string()
                                                              << "\" ; shift 2 ;;\n"
            "    *) shift ;;\n"
            "  esac\n"
            "done\n"
            "printf '%s\\n' '@logos-load-status ok'\n"
            "exec sleep 300\n";
        fs::permissions(host, fs::perms::owner_all);
        setenv("LOGOS_HOST_PATH", host.c_str(), 1);
    }

    std::string givenInstance() const {
        std::ifstream given(tmp.path / "given");
        std::string path;
        std::getline(given, path);
        return fs::path(path).filename().string();
    }
};

// Detector: the instance was the first directory entry in byte order, hidden
// ones included, so a `.snapshots` beside the real instance won and the module
// started from an empty directory. QDir skipped hidden entries.
TEST_F(InstancePersistenceTest, AHiddenDirectoryIsNotAnInstance) {
    useRecordingHost();
    const fs::path base = tmp.path / "persistence";
    fs::create_directories(base / "probe_module" / ".snapshots");
    fs::create_directories(base / "probe_module" / "abc123def456");
    logos_core_set_persistence_base_path(base.c_str());
    plantModule("probe_module", "anything");

    ASSERT_EQ(logos_core_load_module("probe_module", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_EQ(givenInstance(), "abc123def456");
}

TEST_F(InstancePersistenceTest, TheFirstInstanceByNameIsReused) {
    useRecordingHost();
    const fs::path base = tmp.path / "persistence";
    fs::create_directories(base / "probe_module" / "bbb");
    fs::create_directories(base / "probe_module" / "aaa");
    logos_core_set_persistence_base_path(base.c_str());
    plantModule("probe_module", "anything");

    ASSERT_EQ(logos_core_load_module("probe_module", LOGOS_LOAD_MODULE_ONLY), 1);
    EXPECT_EQ(givenInstance(), "aaa");
}

} // namespace
