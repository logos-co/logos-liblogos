#include "logos_api.h"
#include "logos_consumer.h"
#include "logos_core.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>

namespace {

// core_service's answer over the shell binding; null when the call failed.
nlohmann::json call(logos_consumer* shell, const char* method, const char* args)
{
    char* out = nullptr;
    char* err = nullptr;
    const int status = logos_consumer_call(shell, "core_service", method, args, 20000, &out, &err);
    nlohmann::json value = status == 0 && out ? nlohmann::json::parse(out, nullptr, false)
                                              : nlohmann::json();
    logos_consumer_string_free(out);
    logos_consumer_string_free(err);
    return value.is_discarded() ? nlohmann::json() : value;
}

} // namespace

// A Basecamp-shaped process, with the Qt host runtime linked ahead of core: the
// runtime comes up with capability_module as its token authority, the shell
// admits a UI plugin through core_service, and the Qt side adopts the credential
// capability_module minted. The check runs this once more with the same modules
// as a plain modules directory (LOGOS_TEST_MODULES_NOT_BUNDLED=1) and requires
// that run to fail: capability_module then cannot run in-process, there is no
// authority, and so no shell binding.
TEST(QtEmbedderAdmission, TheShellAdmitsAUiPluginThroughCoreService)
{
    const char* modules = std::getenv("TEST_MODULES_DIR");
    ASSERT_TRUE(modules && *modules) << "TEST_MODULES_DIR must name the bundled modules";
    QTemporaryDir state;
    ASSERT_TRUE(state.isValid());
    logos_core_set_persistence_base_path(state.path().toUtf8().constData());
    if (std::getenv("LOGOS_TEST_MODULES_NOT_BUNDLED")) {
        logos_core_add_modules_dir(modules);
    } else {
        const char* dirs[] = {modules, nullptr};
        ASSERT_EQ(logos_core_set_bundled_modules_dirs(dirs), 0);
    }
    ASSERT_EQ(logos_core_set_shell_identity("basecamp"), 0);
    logos_core_start();

    logos_consumer* shell = logos_core_take_shell_binding();
    ASSERT_NE(shell, nullptr) << "no shell binding: the runtime has no token authority";

    const nlohmann::json loaded = call(shell, "listModules", R"(["loaded"])");
    EXPECT_NE(loaded.dump().find("\"capability_module\""), std::string::npos) << loaded.dump();

    const nlohmann::json admitted =
        call(shell, "admitConsumer", R"(["embedder_probe","presentation"])");
    ASSERT_EQ(admitted.value("status", std::string{}), "ok") << admitted.dump();
    const logos::ConsumerIdentity consumer = logos::adoptAdmittedConsumer(
        QStringLiteral("embedder_probe"),
        QString::fromStdString(admitted.value("credential", std::string{})));
    EXPECT_TRUE(consumer) << "the Qt side refused the credential capability_module minted";
    delete consumer.api;

    logos_consumer_release(shell);
    logos_core_cleanup();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    logos_core_init(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
