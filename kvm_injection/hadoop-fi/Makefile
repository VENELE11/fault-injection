CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lpthread

TARGET = hadoop_injector
SRC = hadoop_injector.c

all: $(TARGET)
    @echo "=== hadoop_injector built ==="

$(TARGET): $(SRC)
    $(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
    rm -f $(TARGET)

.PHONY: all clean