CC = gcc
CFLAGS = -pthread
LDFLAGS = -pthread
TARGET = port_scanner
SRC = port_scanner.c

all: $(TARGET)


$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)


test: $(TARGET)
	@echo "--- Running test scan on localhost (ports 1-1024) ---"
	./$(TARGET) -j 8 -w 500 -t localhost -p 1-1024
	@echo "--- Test scan complete ---"

clean:
	rm -f $(TARGET)

.PHONY: all test clean

