#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#endif

namespace fs = std::filesystem;

// Helper function to get the directory of the current executable
static fs::path getExecutableDir() {
#ifdef __APPLE__
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        return fs::path(path).parent_path();
    }
#elif defined(__linux__)
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        return fs::path(path).parent_path();
    }
#endif
    return fs::path();
}

class CLITest : public ::testing::Test {
protected:
    fs::path logoscoreBinary;
    
    void SetUp() override {
        // Check for LOGOSCORE_BINARY environment variable first
        const char* envBinary = std::getenv("LOGOSCORE_BINARY");
        if (envBinary && fs::exists(envBinary)) {
            logoscoreBinary = envBinary;
            return;
        }
        
        // Get the directory where the test executable is located
        fs::path execDir = getExecutableDir();
        
        // Find the logoscore binary - try multiple locations
        std::vector<fs::path> searchPaths;
        
        // First, check in the same directory as the test executable (Nix builds)
        if (!execDir.empty()) {
            searchPaths.push_back(execDir / "logoscore");
        }
        
        // Then try paths relative to current working directory
        searchPaths.push_back(fs::current_path() / ".." / "bin" / "logoscore");        // Running from build/tests
        searchPaths.push_back(fs::current_path() / "bin" / "logoscore");               // Running from build root
        searchPaths.push_back(fs::current_path() / ".." / ".." / "bin" / "logoscore"); // Other build configurations
        searchPaths.push_back(fs::current_path().parent_path() / "logoscore");         // Direct parent
        
        for (const auto& path : searchPaths) {
            if (fs::exists(path)) {
                logoscoreBinary = fs::canonical(path);
                return;
            }
        }
        
        // Binary not found, skip tests
        std::string triedPaths;
        for (size_t i = 0; i < searchPaths.size(); ++i) {
            if (i > 0) triedPaths += ", ";
            triedPaths += "\"" + searchPaths[i].string() + "\"";
        }
        GTEST_SKIP() << "logoscore binary not found. Set LOGOSCORE_BINARY env var or build the binary first. Tried: " 
                     << triedPaths;
    }
    
    // Helper to run logoscore command
    int runLogoscore(const std::string& args, std::string* output = nullptr) {
        std::string cmd = logoscoreBinary.string() + " " + args;
        if (output) {
            cmd += " 2>&1";
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) return -1;
            
            char buffer[128];
            while (fgets(buffer, sizeof(buffer), pipe)) {
                *output += buffer;
            }
            int status = pclose(pipe);
            return WEXITSTATUS(status);
        } else {
            int status = system(cmd.c_str());
            return WEXITSTATUS(status);
        }
    }
    
    // Helper to run logoscore with timeout (for commands that run event loop)
    // Uses timeout command to kill process after specified seconds
    int runLogoscoreWithTimeout(const std::string& args, std::string* output, int timeoutSecs = 2) {
        std::string cmd = "timeout " + std::to_string(timeoutSecs) + " " + logoscoreBinary.string() + " " + args + " 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return -1;
        
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            *output += buffer;
        }
        int status = pclose(pipe);
        // timeout command returns 124 when it times out (which is expected for event loop)
        // We'll treat timeout as success for our purposes since we got the output
        return WEXITSTATUS(status);
    }
};

// Test: logoscore --help
// Verifies that help text is displayed correctly and includes all expected options
TEST_F(CLITest, HelpCommand) {
    std::string output;
    int exitCode = runLogoscore("--help", &output);
    
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("Logos Core"), std::string::npos) << "Help should contain application description";
    EXPECT_NE(output.find("--help"), std::string::npos) << "Help should contain --help option";
    EXPECT_NE(output.find("--version"), std::string::npos) << "Help should contain --version option";
    EXPECT_NE(output.find("--modules-dir"), std::string::npos) << "Help should contain --modules-dir option";
}

// Test: logoscore --version
// Verifies that version information is displayed correctly
TEST_F(CLITest, VersionCommand) {
    std::string output;
    int exitCode = runLogoscore("--version", &output);
    
    EXPECT_EQ(exitCode, 0);
    // Should contain some version string (we set "1.0" in main.cpp)
    EXPECT_FALSE(output.empty()) << "Version output should not be empty";
}

// Test: --modules-dir option actually sets the custom modules directory
// Verifies that the --modules-dir option works by checking the output
TEST_F(CLITest, ModulesDirOption_SetDirectory) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("--modules-dir /tmp/test_modules", &output);
    
    // Check that the directory was actually set (qDebug output from app_lifecycle.cpp)
    EXPECT_NE(output.find("Custom plugins directory set to:"), std::string::npos) 
        << "Should see debug message that custom directory was set";
    EXPECT_NE(output.find("/tmp/test_modules"), std::string::npos) 
        << "Should see the custom directory path in output";
}

// Test: --load-modules option actually attempts to load specified modules
// Verifies that the --load-modules option works by trying to load a non-existent module
TEST_F(CLITest, LoadModulesOption_LoadsModules) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("--load-modules fake_module_xyz", &output);
    
    // Check that it actually tried to load the module (and failed since it doesn't exist)
    // The warning comes from main.cpp when logos_core_load_plugin fails
    EXPECT_NE(output.find("Failed to load module:"), std::string::npos)
        << "Should see warning that module failed to load";
    EXPECT_NE(output.find("fake_module_xyz"), std::string::npos)
        << "Should see the module name in output";
}

// Test: -l alias works as short form for --load-modules
// Verifies that the -l option produces the same behavior as --load-modules
TEST_F(CLITest, LoadModulesShortAlias_Works) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("-l fake_module_alias", &output);
    
    // Check that -l alias actually attempts to load the module (same behavior as --load-modules)
    EXPECT_NE(output.find("Failed to load module:"), std::string::npos)
        << "Should see warning that module failed to load";
    EXPECT_NE(output.find("fake_module_alias"), std::string::npos)
        << "Should see the module name in output";
}

// Test: -m alias works as short form for --modules-dir
// Verifies that the -m option produces the same behavior as --modules-dir
TEST_F(CLITest, ModulesDirShortAlias_Works) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("-m /tmp/test_modules_alias", &output);
    
    // Check that -m alias actually sets the directory (same behavior as --modules-dir)
    EXPECT_NE(output.find("Custom plugins directory set to:"), std::string::npos)
        << "Should see debug message that custom directory was set";
    EXPECT_NE(output.find("/tmp/test_modules_alias"), std::string::npos)
        << "Should see the custom directory path in output";
}

// Test: invalid option
// Verifies error handling when an unknown option is provided
TEST_F(CLITest, InvalidOption) {
    std::string output;
    int exitCode = runLogoscore("--invalid-option-xyz", &output);
    
    // Qt's QCommandLineParser exits with code 1 for unknown options
    EXPECT_NE(exitCode, 0) << "Invalid option should cause non-zero exit code";
    EXPECT_FALSE(output.empty()) << "Error output should not be empty";
}
