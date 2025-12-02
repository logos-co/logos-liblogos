CXX = clang++

# Qt paths (Homebrew on Apple Silicon) - needed for linking
QT_PREFIX = /opt/homebrew/opt/qt
QT_LIBS = -F$(QT_PREFIX)/lib -framework QtCore

CXXFLAGS = -std=c++17 -Wall -Wextra -I../src
LDFLAGS = -L../result/lib -llogos_core $(QT_LIBS)
RPATH = -Wl,-rpath,@loader_path/../result/lib -Wl,-rpath,@loader_path/../lib -Wl,-rpath,../result/lib

TARGET = cli
SRC = cli.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(RPATH)

clean:
	rm -f $(TARGET)

.PHONY: all clean
