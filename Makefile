# 定义编译器
CC = gcc

# 定义编译选项
CFLAGS = -g
LDFLAGS = -lpthread -lgmssl

# 定义目标文件名
TLS_SERVER_TARGET = tls_server
TLS_CLIENT_TARGET = tls_client
# 定义源文件
TLS_SERVER_SRCS = tls_server.c
TLS_CLIENT_SRCS = tls_client.c

# 默认目标
all: $(TLS_SERVER_TARGET) $(TLS_CLIENT_TARGET)


# 编译 tls_server
$(TLS_SERVER_TARGET): $(TLS_SERVER_SRCS)
	$(CC) $(CFLAGS) $(TLS_SERVER_SRCS) -o $(TLS_SERVER_TARGET) $(LDFLAGS)

# 编译 tls_client
$(TLS_CLIENT_TARGET): $(TLS_CLIENT_SRCS)
	$(CC) $(CFLAGS) $(TLS_CLIENT_SRCS) -o $(TLS_CLIENT_TARGET) $(LDFLAGS)

# 清理生成的文件
clean:
	rm -f $(TLS_SERVER_TARGET) $(TLS_CLIENT_TARGET)