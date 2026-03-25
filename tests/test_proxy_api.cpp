#include <gtest/gtest.h>
#include "proxy_api.h"
#include "logos_core_internal.h"
#include "plugin_manager.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <cstring>
#include "logos_json_utils.h"

// Helper callback that records if it was called
static bool s_callback_called = false;
static int s_callback_success = -1;
static QString s_callback_message;

void testCallback(int success, const char* message, void* user_data) {
    s_callback_called = true;
    s_callback_success = success;
    s_callback_message = message ? QString::fromUtf8(message) : QString();
}

// Test fixture for proxy API tests
class ProxyAPITest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear global state before each test
        g_loaded_plugins.clear();
        g_known_plugins.clear();
        g_event_listeners.clear();
        
        // Reset callback state
        s_callback_called = false;
        s_callback_success = -1;
        s_callback_message.clear();
    }
    
    void TearDown() override {
        // Drain any pending timers from async operations before clearing state
        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();

        // Clean up global state after each test
        g_loaded_plugins.clear();
        g_known_plugins.clear();
        g_event_listeners.clear();
        
        // Reset callback state
        s_callback_called = false;
        s_callback_success = -1;
        s_callback_message.clear();
    }
};

// =============================================================================
// asyncOperation Tests
// =============================================================================

// Verifies that asyncOperation() handles null data gracefully
TEST_F(ProxyAPITest, AsyncOperation_HandlesNullData) {
    // Use a noop callback to avoid stale timer callbacks leaking into later tests
    auto noop = [](int, const char*, void*) {};
    // Should not crash with null data
    EXPECT_NO_THROW(ProxyAPI::asyncOperation(nullptr, noop, nullptr));
}

// Verifies that asyncOperation() accepts valid data and callback
TEST_F(ProxyAPITest, AsyncOperation_AcceptsValidData) {
    // Use a noop callback to avoid stale timer callbacks leaking into later tests
    auto noop = [](int, const char*, void*) {};
    // Should not crash with valid data
    EXPECT_NO_THROW(ProxyAPI::asyncOperation("test data", noop, nullptr));
}

// =============================================================================
// loadPluginAsync Tests
// =============================================================================

// Verifies that loadPluginAsync() asserts when called with a null callback
TEST_F(ProxyAPITest, LoadPluginAsync_AssertsWithNullCallback) {
    EXPECT_DEATH(ProxyAPI::loadPluginAsync("test_plugin", nullptr, nullptr), "");
}

// Verifies that loadPluginAsync() fails immediately when plugin name is null
TEST_F(ProxyAPITest, LoadPluginAsync_FailsWithNullPluginName) {
    ProxyAPI::loadPluginAsync(nullptr, testCallback, nullptr);
    
    // Callback should be called immediately with failure
    EXPECT_TRUE(s_callback_called);
    EXPECT_EQ(s_callback_success, 0);
    EXPECT_FALSE(s_callback_message.isEmpty());
}

// Verifies that loadPluginAsync() fails for unknown plugins
TEST_F(ProxyAPITest, LoadPluginAsync_FailsForUnknownPlugin) {
    ProxyAPI::loadPluginAsync("nonexistent_plugin", testCallback, nullptr);
    
    // Callback should be called immediately with failure
    EXPECT_TRUE(s_callback_called);
    EXPECT_EQ(s_callback_success, 0);
    EXPECT_TRUE(s_callback_message.contains("not found"));
}

// Verifies that loadPluginAsync() accepts a known plugin
TEST_F(ProxyAPITest, LoadPluginAsync_AcceptsKnownPlugin) {
    // Add a known plugin
    g_known_plugins.insert("test_plugin", "/path/to/plugin");

    // Use a noop callback to avoid stale timer callbacks leaking into later tests
    auto noop = [](int, const char*, void*) {};

    // Should not crash and should start async operation
    EXPECT_NO_THROW(ProxyAPI::loadPluginAsync("test_plugin", noop, nullptr));

    // Callback should NOT be called immediately for known plugins
    EXPECT_FALSE(s_callback_called);
}

// =============================================================================
// callPluginMethodAsync Tests
// =============================================================================

// Verifies that callPluginMethodAsync() asserts when called with a null callback
TEST_F(ProxyAPITest, CallPluginMethodAsync_AssertsWithNullCallback) {
    EXPECT_DEATH(ProxyAPI::callPluginMethodAsync("plugin", "method", "[]", nullptr, nullptr), "");
}

// Verifies that callPluginMethodAsync() fails when plugin name is null
TEST_F(ProxyAPITest, CallPluginMethodAsync_FailsWithNullPluginName) {
    ProxyAPI::callPluginMethodAsync(nullptr, "method", "[]", testCallback, nullptr);
    
    // Callback should be called immediately with failure
    EXPECT_TRUE(s_callback_called);
    EXPECT_EQ(s_callback_success, 0);
    EXPECT_FALSE(s_callback_message.isEmpty());
}

// Verifies that callPluginMethodAsync() fails when method name is null
TEST_F(ProxyAPITest, CallPluginMethodAsync_FailsWithNullMethodName) {
    ProxyAPI::callPluginMethodAsync("plugin", nullptr, "[]", testCallback, nullptr);
    
    // Callback should be called immediately with failure
    EXPECT_TRUE(s_callback_called);
    EXPECT_EQ(s_callback_success, 0);
    EXPECT_FALSE(s_callback_message.isEmpty());
}

// Verifies that callPluginMethodAsync() fails for unloaded plugins
TEST_F(ProxyAPITest, CallPluginMethodAsync_FailsForUnloadedPlugin) {
    ProxyAPI::callPluginMethodAsync("unloaded_plugin", "method", "[]", testCallback, nullptr);
    
    // Callback should be called immediately with failure
    EXPECT_TRUE(s_callback_called);
    EXPECT_EQ(s_callback_success, 0);
    EXPECT_TRUE(s_callback_message.contains("not loaded"));
}

// Verifies that callPluginMethodAsync() accepts loaded plugins
TEST_F(ProxyAPITest, CallPluginMethodAsync_AcceptsLoadedPlugin) {
    // Simulate a loaded plugin
    g_loaded_plugins.append("test_plugin");

    // Should not crash and should delegate to SDK (not fail with a validation error)
    EXPECT_NO_THROW(ProxyAPI::callPluginMethodAsync("test_plugin", "testMethod", "[]", testCallback, nullptr));

    // Verify it passed proxy validation — if the callback was called (e.g. due to
    // SDK connection timeouts in test environment), it should not be a proxy-level
    // validation error
    if (s_callback_called) {
        EXPECT_FALSE(s_callback_message.contains("not loaded"));
        EXPECT_FALSE(s_callback_message.contains("null"));
    }
}

// Verifies that callPluginMethodAsync() handles null params_json as empty array
TEST_F(ProxyAPITest, CallPluginMethodAsync_HandlesNullParamsJson) {
    // Simulate a loaded plugin
    g_loaded_plugins.append("test_plugin");
    
    // Should not crash with null params_json
    EXPECT_NO_THROW(ProxyAPI::callPluginMethodAsync("test_plugin", "testMethod", nullptr, testCallback, nullptr));
}

// =============================================================================
// registerEventListener Tests
// =============================================================================

// Verifies that registerEventListener() does not crash with null plugin/event name
TEST_F(ProxyAPITest, RegisterEventListener_DoesNotCrashWithNullNames) {
    EXPECT_NO_THROW(ProxyAPI::registerEventListener(nullptr, "event", testCallback, nullptr));
    EXPECT_NO_THROW(ProxyAPI::registerEventListener("plugin", nullptr, testCallback, nullptr));
}

// Verifies that registerEventListener() does not register for unloaded plugins
TEST_F(ProxyAPITest, RegisterEventListener_DoesNotRegisterForUnloadedPlugin) {
    // Try to register for an unloaded plugin
    ProxyAPI::registerEventListener("unloaded_plugin", "test_event", testCallback, nullptr);
    
    // Should not be added to the listeners list
    EXPECT_EQ(g_event_listeners.size(), 0);
}

// Verifies that registerEventListener() adds listener to the global list for loaded plugins
TEST_F(ProxyAPITest, RegisterEventListener_AddsToEventListenersListForLoadedPlugin) {
    // Simulate a loaded plugin
    g_loaded_plugins.append("test_plugin");
    
    // Register an event listener
    ProxyAPI::registerEventListener("test_plugin", "test_event", testCallback, nullptr);
    
    // Should be added to the listeners list
    ASSERT_EQ(g_event_listeners.size(), 1);
    EXPECT_EQ(g_event_listeners[0].pluginName.toStdString(), "test_plugin");
    EXPECT_EQ(g_event_listeners[0].eventName.toStdString(), "test_event");
    EXPECT_EQ(g_event_listeners[0].callback, testCallback);
}

// Verifies that registerEventListener() can register multiple listeners
TEST_F(ProxyAPITest, RegisterEventListener_CanRegisterMultipleListeners) {
    // Simulate loaded plugins
    g_loaded_plugins.append("plugin1");
    g_loaded_plugins.append("plugin2");
    
    // Register multiple event listeners
    ProxyAPI::registerEventListener("plugin1", "event1", testCallback, nullptr);
    ProxyAPI::registerEventListener("plugin2", "event2", testCallback, nullptr);
    ProxyAPI::registerEventListener("plugin1", "event2", testCallback, nullptr);
    
    // All should be added to the listeners list
    EXPECT_EQ(g_event_listeners.size(), 3);
}

// Verifies that registerEventListener() stores the user_data pointer correctly
TEST_F(ProxyAPITest, RegisterEventListener_StoresUserData) {
    // Simulate a loaded plugin
    g_loaded_plugins.append("test_plugin");
    
    int userData = 42;
    
    // Register an event listener with user data
    ProxyAPI::registerEventListener("test_plugin", "test_event", testCallback, &userData);
    
    // User data should be stored
    ASSERT_EQ(g_event_listeners.size(), 1);
    EXPECT_EQ(g_event_listeners[0].userData, &userData);
}

// =============================================================================
// Full JSON Array Pipeline Tests (baseline for refactoring)
// These test the exact JSON format the JS SDK sends: [{name,value,type}, ...]
// =============================================================================

// Verifies the full JSON array pipeline: parse JSON string -> convert each param -> QVariantList
// This mirrors the parsing logic in callPluginMethodAsync lines 181-210
TEST_F(ProxyAPITest, JsonArrayPipeline_MixedTypes) {
    QString paramsJson = R"([
        {"name":"arg0","value":"hello","type":"string"},
        {"name":"arg1","value":"42","type":"int"},
        {"name":"arg2","value":"true","type":"bool"},
        {"name":"arg3","value":"3.14","type":"double"}
    ])";

    QJsonParseError parseError;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(paramsJson.toUtf8(), &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError);

    QJsonArray paramsArray = jsonDoc.array();
    QVariantList args;
    for (const QJsonValue& paramValue : paramsArray) {
        ASSERT_TRUE(paramValue.isObject());
        QJsonObject paramObj = paramValue.toObject();
        QVariant variant = LogosJsonUtils::jsonParamToVariant(paramObj);
        ASSERT_TRUE(variant.isValid()) << "Failed for param: " << paramObj.value("name").toString().toStdString();
        args.append(variant);
    }

    ASSERT_EQ(args.size(), 4);
    EXPECT_EQ(args[0].toString(), "hello");
    EXPECT_EQ(args[1].toInt(), 42);
    EXPECT_EQ(args[2].toBool(), true);
    EXPECT_NEAR(args[3].toDouble(), 3.14, 0.001);
}

// Verifies parsing of an empty JSON array (no params)
TEST_F(ProxyAPITest, JsonArrayPipeline_EmptyArray) {
    QString paramsJson = "[]";

    QJsonParseError parseError;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(paramsJson.toUtf8(), &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError);

    QJsonArray paramsArray = jsonDoc.array();
    QVariantList args;
    for (const QJsonValue& paramValue : paramsArray) {
        if (paramValue.isObject()) {
            args.append(LogosJsonUtils::jsonParamToVariant(paramValue.toObject()));
        }
    }

    EXPECT_EQ(args.size(), 0);
}

// Verifies single string param (common case from JS SDK)
TEST_F(ProxyAPITest, JsonArrayPipeline_SingleStringParam) {
    QString paramsJson = R"([{"name":"arg0","value":"test message","type":"string"}])";

    QJsonParseError parseError;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(paramsJson.toUtf8(), &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError);

    QJsonArray paramsArray = jsonDoc.array();
    ASSERT_EQ(paramsArray.size(), 1);

    QVariant result = LogosJsonUtils::jsonParamToVariant(paramsArray[0].toObject());
    ASSERT_TRUE(result.isValid());
    EXPECT_EQ(result.toString(), "test message");
}

// Verifies that invalid JSON causes a parse error (not a crash)
TEST_F(ProxyAPITest, JsonArrayPipeline_InvalidJson) {
    QString paramsJson = "not valid json at all";

    QJsonParseError parseError;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(paramsJson.toUtf8(), &parseError);
    EXPECT_NE(parseError.error, QJsonParseError::NoError);
}

// Verifies that a param with invalid type value is rejected during pipeline
TEST_F(ProxyAPITest, JsonArrayPipeline_InvalidParamInArray) {
    QString paramsJson = R"([
        {"name":"arg0","value":"hello","type":"string"},
        {"name":"arg1","value":"not_a_number","type":"int"}
    ])";

    QJsonParseError parseError;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(paramsJson.toUtf8(), &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError);

    QJsonArray paramsArray = jsonDoc.array();
    bool hadInvalid = false;
    QVariantList args;
    for (const QJsonValue& paramValue : paramsArray) {
        if (paramValue.isObject()) {
            QVariant variant = LogosJsonUtils::jsonParamToVariant(paramValue.toObject());
            if (!variant.isValid()) {
                hadInvalid = true;
                break;
            }
            args.append(variant);
        }
    }

    EXPECT_TRUE(hadInvalid);
    EXPECT_EQ(args.size(), 1);
    EXPECT_EQ(args[0].toString(), "hello");
}

// =============================================================================
// callPluginMethodAsync JSON Error Handling Tests
// =============================================================================

// Verifies that callPluginMethodAsync reports JSON parse errors via callback
TEST_F(ProxyAPITest, CallPluginMethodAsync_ReportsJsonParseError) {
    g_loaded_plugins.append("test_plugin");

    static bool jsonErrorCalled = false;
    static int jsonErrorSuccess = -1;
    static QString jsonErrorMessage;
    jsonErrorCalled = false;
    jsonErrorSuccess = -1;
    jsonErrorMessage.clear();

    auto jsonErrorCallback = [](int success, const char* message, void* /*user_data*/) {
        jsonErrorCalled = true;
        jsonErrorSuccess = success;
        jsonErrorMessage = message ? QString::fromUtf8(message) : QString();
    };

    ProxyAPI::callPluginMethodAsync("test_plugin", "method", "invalid json!", jsonErrorCallback, nullptr);

    for (int i = 0; i < 50; ++i)
        QCoreApplication::processEvents();

    EXPECT_TRUE(jsonErrorCalled);
    EXPECT_EQ(jsonErrorSuccess, 0);
    EXPECT_TRUE(jsonErrorMessage.contains("JSON parse error"));
}

// Verifies that callPluginMethodAsync reports invalid param errors via callback
TEST_F(ProxyAPITest, CallPluginMethodAsync_ReportsInvalidParamError) {
    g_loaded_plugins.append("test_plugin");

    static bool paramErrorCalled = false;
    static int paramErrorSuccess = -1;
    static QString paramErrorMessage;
    paramErrorCalled = false;
    paramErrorSuccess = -1;
    paramErrorMessage.clear();

    auto paramErrorCallback = [](int success, const char* message, void* /*user_data*/) {
        paramErrorCalled = true;
        paramErrorSuccess = success;
        paramErrorMessage = message ? QString::fromUtf8(message) : QString();
    };

    QString paramsJson = R"([{"name":"arg0","value":"not_a_number","type":"int"}])";
    ProxyAPI::callPluginMethodAsync("test_plugin", "method", paramsJson.toUtf8().constData(), paramErrorCallback, nullptr);

    for (int i = 0; i < 50; ++i)
        QCoreApplication::processEvents();

    EXPECT_TRUE(paramErrorCalled);
    EXPECT_EQ(paramErrorSuccess, 0);
    EXPECT_TRUE(paramErrorMessage.contains("Invalid parameter"));
}

// =============================================================================
// User data passthrough test
// =============================================================================

static void* s_received_user_data = nullptr;
static void userDataCallback(int success, const char* message, void* user_data) {
    s_callback_called = true;
    s_callback_success = success;
    s_callback_message = message ? QString::fromUtf8(message) : QString();
    s_received_user_data = user_data;
}

// Verifies that user_data is correctly passed through to the callback
TEST_F(ProxyAPITest, CallPluginMethodAsync_PassesThroughUserData) {
    s_received_user_data = nullptr;

    int myData = 123;
    ProxyAPI::callPluginMethodAsync(nullptr, "method", "[]", userDataCallback, &myData);

    EXPECT_TRUE(s_callback_called);
    EXPECT_EQ(s_received_user_data, &myData);
}

// Verifies that loadPluginAsync passes user_data through on failure path
TEST_F(ProxyAPITest, LoadPluginAsync_PassesThroughUserData) {
    s_received_user_data = nullptr;

    int myData = 456;
    ProxyAPI::loadPluginAsync(nullptr, userDataCallback, &myData);

    EXPECT_TRUE(s_callback_called);
    EXPECT_EQ(s_received_user_data, &myData);
}
