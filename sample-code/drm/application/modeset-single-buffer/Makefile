# Variables
CC = gcc
CFLAGS = -g -I/home/amd/umd/drm/include/drm
LDFLAGS = -ldrm
TARGET = modeset-single-buffer
SRC = modeset-single-buffer.c

# Targets
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

