CC = gcc
FLAGS=`pkg-config --cflags --libs libdrm`
FLAGS+=-O0 -g

# 获取所有的 .c 文件，并去除路径和扩展名作为目标文件名
SRCS := $(wildcard *.c)
TARGETS := $(SRCS:.c=)

all: $(TARGETS)

# 模式规则：%.o 依赖于 %.c
%: %.c
	$(CC)  -o $@ $< $(FLAGS)

clean:
	rm -f $(TARGETS)

