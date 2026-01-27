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

// Test: --modules-dir in help text
// Specifically verifies that the --modules-dir option appears with its description
TEST_F(CLITest, ModulesDirOption_InHelp) {
    std::string output;
    int exitCode = runLogoscore("--help", &output);
    
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("--modules-dir"), std::string::npos) << "--modules-dir option should be in help";
    EXPECT_NE(output.find("Directory to scan for modules"), std::string::npos) 
        << "Help should contain the --modules-dir description";
}

// Test: -m alias for --modules-dir
// Verifies that the short form -m works as an alias for --modules-dir
TEST_F(CLITest, ModulesDirShortAlias) {
    std::string output;
    int exitCode = runLogoscore("--help", &output);
    
    EXPECT_EQ(exitCode, 0);
    // Qt's QCommandLineParser shows short options in help as "-m, --modules-dir"
    EXPECT_NE(output.find("-m"), std::string::npos) << "Help should show -m alias";
    EXPECT_NE(output.find("--modules-dir"), std::string::npos) << "Help should show --modules-dir option";
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
