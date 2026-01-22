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
// jsonParamToQVariant Tests
// =============================================================================

// Verifies that jsonParamToQVariant() correctly converts string type parameters
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsStringType) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "Hello World";
    param["type"] = "string";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_TRUE(result.canConvert<QString>());
    EXPECT_EQ(result.toString().toStdString(), "Hello World");
}

// Verifies that jsonParamToQVariant() correctly converts QString type parameters
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsQStringType) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "Qt String";
    param["type"] = "QString";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_TRUE(result.canConvert<QString>());
    EXPECT_EQ(result.toString().toStdString(), "Qt String");
}

// Verifies that jsonParamToQVariant() correctly converts int type parameters
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsIntType) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "42";
    param["type"] = "int";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_TRUE(result.canConvert<int>());
    EXPECT_EQ(result.toInt(), 42);
}

// Verifies that jsonParamToQVariant() correctly converts integer type parameters
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsIntegerType) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "123";
    param["type"] = "integer";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_TRUE(result.canConvert<int>());
    EXPECT_EQ(result.toInt(), 123);
}

// Verifies that jsonParamToQVariant() correctly converts bool type to true
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsBoolTypeTrue) {
    QJsonObject param1;
    param1["name"] = "testParam";
    param1["value"] = "true";
    param1["type"] = "bool";
    
    QVariant result1 = ProxyAPI::jsonParamToQVariant(param1);
    EXPECT_TRUE(result1.isValid());
    EXPECT_TRUE(result1.canConvert<bool>());
    EXPECT_TRUE(result1.toBool());
    
    // Test with "1"
    QJsonObject param2;
    param2["name"] = "testParam";
    param2["value"] = "1";
    param2["type"] = "bool";
    
    QVariant result2 = ProxyAPI::jsonParamToQVariant(param2);
    EXPECT_TRUE(result2.isValid());
    EXPECT_TRUE(result2.canConvert<bool>());
    EXPECT_TRUE(result2.toBool());
}

// Verifies that jsonParamToQVariant() correctly converts bool type to false
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsBoolTypeFalse) {
    QJsonObject param1;
    param1["name"] = "testParam";
    param1["value"] = "false";
    param1["type"] = "bool";
    
    QVariant result1 = ProxyAPI::jsonParamToQVariant(param1);
    EXPECT_TRUE(result1.isValid());
    EXPECT_TRUE(result1.canConvert<bool>());
    EXPECT_FALSE(result1.toBool());
    
    // Test with "0"
    QJsonObject param2;
    param2["name"] = "testParam";
    param2["value"] = "0";
    param2["type"] = "bool";
    
    QVariant result2 = ProxyAPI::jsonParamToQVariant(param2);
    EXPECT_TRUE(result2.isValid());
    EXPECT_TRUE(result2.canConvert<bool>());
    EXPECT_FALSE(result2.toBool());
}

// Verifies that jsonParamToQVariant() correctly converts boolean type parameters
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsBooleanType) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "true";
    param["type"] = "boolean";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_TRUE(result.canConvert<bool>());
    EXPECT_TRUE(result.toBool());
}

// Verifies that jsonParamToQVariant() correctly converts double type parameters
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsDoubleType) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "3.14159";
    param["type"] = "double";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_TRUE(result.canConvert<double>());
    EXPECT_NEAR(result.toDouble(), 3.14159, 0.00001);
}

// Verifies that jsonParamToQVariant() correctly converts float type parameters
TEST_F(ProxyAPITest, JsonParamToQVariant_ConvertsFloatType) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "2.718";
    param["type"] = "float";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_TRUE(result.canConvert<double>());
    EXPECT_NEAR(result.toDouble(), 2.718, 0.001);
}

// Verifies that jsonParamToQVariant() returns invalid QVariant for bad int values
TEST_F(ProxyAPITest, JsonParamToQVariant_ReturnsInvalidForBadInt) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "not_a_number";
    param["type"] = "int";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_FALSE(result.isValid());
}

// Verifies that jsonParamToQVariant() returns invalid QVariant for bad bool values
TEST_F(ProxyAPITest, JsonParamToQVariant_ReturnsInvalidForBadBool) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "maybe";
    param["type"] = "bool";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_FALSE(result.isValid());
}

// Verifies that jsonParamToQVariant() returns invalid QVariant for bad double values
TEST_F(ProxyAPITest, JsonParamToQVariant_ReturnsInvalidForBadDouble) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "not_a_number";
    param["type"] = "double";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_FALSE(result.isValid());
}

// Verifies that jsonParamToQVariant() treats unknown types as strings
TEST_F(ProxyAPITest, JsonParamToQVariant_TreatsUnknownTypeAsString) {
    QJsonObject param;
    param["name"] = "testParam";
    param["value"] = "some value";
    param["type"] = "unknown_type";
    
    QVariant result = ProxyAPI::jsonParamToQVariant(param);
    
    EXPECT_TRUE(result.isValid());
    EXPECT_TRUE(result.canConvert<QString>());
    EXPECT_EQ(result.toString().toStdString(), "some value");
}

// =============================================================================
// asyncOperation Tests
// =============================================================================

// Verifies that asyncOperation() does not crash when called with a null callback
TEST_F(ProxyAPITest, AsyncOperation_DoesNotCrashWithNullCallback) {
    // Should not crash
    EXPECT_NO_THROW(ProxyAPI::asyncOperation("test data", nullptr, nullptr));
}

// Verifies that asyncOperation() handles null data gracefully
TEST_F(ProxyAPITest, AsyncOperation_HandlesNullData) {
    // Should not crash with null data
    EXPECT_NO_THROW(ProxyAPI::asyncOperation(nullptr, testCallback, nullptr));
    
    // Note: We can't easily test the callback execution without event loop processing
    // This test ensures the function doesn't crash with null data
}

// Verifies that asyncOperation() accepts valid data and callback
TEST_F(ProxyAPITest, AsyncOperation_AcceptsValidData) {
    // Should not crash with valid data
    EXPECT_NO_THROW(ProxyAPI::asyncOperation("test data", testCallback, nullptr));
}

// =============================================================================
// loadPluginAsync Tests
// =============================================================================

// Verifies that loadPluginAsync() does not crash when called with a null callback
TEST_F(ProxyAPITest, LoadPluginAsync_DoesNotCrashWithNullCallback) {
    // Should not crash
    EXPECT_NO_THROW(ProxyAPI::loadPluginAsync("test_plugin", nullptr, nullptr));
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
    
    // Should not crash and should start async operation
    EXPECT_NO_THROW(ProxyAPI::loadPluginAsync("test_plugin", testCallback, nullptr));
    
    // Callback should NOT be called immediately for known plugins
    EXPECT_FALSE(s_callback_called);
}

// =============================================================================
// callPluginMethodAsync Tests
// =============================================================================

// Verifies that callPluginMethodAsync() does not crash when called with a null callback
TEST_F(ProxyAPITest, CallPluginMethodAsync_DoesNotCrashWithNullCallback) {
    // Should not crash
    EXPECT_NO_THROW(ProxyAPI::callPluginMethodAsync("plugin", "method", "[]", nullptr, nullptr));
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
    
    // Should not crash and should start async operation
    EXPECT_NO_THROW(ProxyAPI::callPluginMethodAsync("test_plugin", "testMethod", "[]", testCallback, nullptr));
    
    // Callback should NOT be called immediately for loaded plugins
    EXPECT_FALSE(s_callback_called);
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

// Verifies that registerEventListener() does not crash with null parameters
TEST_F(ProxyAPITest, RegisterEventListener_DoesNotCrashWithNullParams) {
    // Test various null combinations - should not crash
    EXPECT_NO_THROW(ProxyAPI::registerEventListener(nullptr, "event", testCallback, nullptr));
    EXPECT_NO_THROW(ProxyAPI::registerEventListener("plugin", nullptr, testCallback, nullptr));
    EXPECT_NO_THROW(ProxyAPI::registerEventListener("plugin", "event", nullptr, nullptr));
    EXPECT_NO_THROW(ProxyAPI::registerEventListener(nullptr, nullptr, nullptr, nullptr));
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
