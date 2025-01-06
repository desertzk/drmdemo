CC = gcc
CFLAGS = -g -I/home/amd/umd/drm/include/drm
LDFLAGS = -ldrm

# 获取所有的 .c 文件，并去除路径和扩展名作为目标文件名
SRCS := $(wildcard *.c)
TARGETS := $(SRCS:.c=)

all: $(TARGETS)

# 模式规则：%.o 依赖于 %.c
%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

