#include "logos_api.h"
#include "logos_consumer.h"
#include "logos_core.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>

namespace {

void mirrorCoreToken(const char* key, const char* token, void*)
{
    TokenManager::instance().saveToken(key, token);
}

bool isLoaded(const char* name)
{
    char** loaded = logos_core_get_loaded_modules();
    bool found = false;
    for (char** it = loaded; it && *it; ++it) {
        found = found || std::strcmp(*it, name) == 0;
        delete[] *it;
    }
    delete[] loaded;
    return found;
}

} // namespace

// Detector: the Qt-free core saves module tokens in the plain protocol's store,
// so the Qt store an embedder's LogosAPI reads held no capability_module token
// and admitConsumer refused every UI plugin. The check runs this once more with
// LOGOS_TEST_NO_TOKEN_LISTENER=1 and requires that run to fail.
TEST(QtEmbedderTokens, AdmitConsumerSeesCoreTokensThroughTheListener)
{
    const char* modules = std::getenv("TEST_MODULES_DIR");
    ASSERT_TRUE(modules && *modules) << "TEST_MODULES_DIR must name the bundled modules";
    QTemporaryDir state;
    ASSERT_TRUE(state.isValid());
    logos_core_set_persistence_base_path(state.path().toUtf8().constData());
    logos_core_add_modules_dir(modules);
    if (!std::getenv("LOGOS_TEST_NO_TOKEN_LISTENER"))
        logos_core_set_token_listener(mirrorCoreToken, nullptr);
    logos_core_start();
    ASSERT_TRUE(isLoaded("capability_module"));

    LogosAPI host(QStringLiteral("core"));
    EXPECT_FALSE(TokenManager::instance().getToken(QStringLiteral("capability_module")).isEmpty())
        << "core's capability_module token never reached the Qt store";
    const logos::ConsumerIdentity admitted =
        logos::admitConsumer(QStringLiteral("embedder_probe"), &host);
    EXPECT_TRUE(admitted) << "the Qt host refused a UI consumer";
    delete admitted.api;

    logos_core_set_token_listener(nullptr, nullptr);
    logos_core_cleanup();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    logos_core_init(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
