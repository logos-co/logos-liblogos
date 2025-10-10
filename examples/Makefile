CC = clang
CFLAGS = -std=c11 -Wall -Wextra -I../src
LDFLAGS = -L../result/lib -llogos_core
RPATH = -Wl,-rpath,@loader_path/../result/lib -Wl,-rpath,@loader_path/../lib -Wl,-rpath,../result/lib

TARGET = cli
SRC = cli.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(RPATH)

clean:
	rm -f $(TARGET)

.PHONY: all clean
