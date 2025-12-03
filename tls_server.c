#include <gmssl/tls.h>
#include <gmssl/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include "config.h"
#include <signal.h>
// 线程参数结构体：传递TLS连接上下文和运行标志（优雅终止用）
typedef struct {
    TLS_CONNECT *conn;    // TLS连接上下文（包含加密密钥和socket）
    int *running;         // 线程运行状态标志
} ThreadArgs;

// 接收线程：通过TLS加密通道读取客户端数据（自动解密、校验）
void *receive_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    TLS_CONNECT *conn = args->conn;
    int *running = args->running;
    uint8_t buffer[1024];
    size_t len;
    int ret;

    while (*running) {
        ret = tls_recv(conn, buffer, sizeof(buffer) - 1, &len);
        if (ret != 1) {
            if (*running) {
                if (ret == 0) printf("\nConnection closed by peer.\n");
                else fprintf(stderr, "\ntls_recv error\n");
            }
            *running = 0;
            break;
        }
        buffer[len] = '\0';
        printf("\033[0;31m%s\033[0m", (char *)buffer);
        fflush(stdout);
    }
    return NULL;
}

// 发送线程：通过TLS加密通道向客户端发送数据（自动加密、打包）
void *send_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    TLS_CONNECT *conn = args->conn;
    int *running = args->running;
    char message[1024];
    size_t sent;
    int ret;

    while (*running) {
        if (fgets(message, sizeof(message), stdin) == NULL) {
            *running = 0;
            break;
        }

        if (strncmp(message, "exit", 4) == 0 && (message[4] == '\n' || message[4] == '\0')) {
            *running = 0;
            break;
        }
        
        ret = tls_send(conn, (uint8_t *)message, strlen(message), &sent);
        if (ret != 1) {
            fprintf(stderr, "tls_send failed\n");
            *running = 0;
            break;
        }
    }
    return NULL;
}

void run_tls_server() {
    TLS_CTX ctx;
    TLS_CONNECT conn;
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int running = 1;  // 线程运行状态标志（1=运行，0=退出）
    ThreadArgs args = {&conn, &running};  // 线程参数

    // 1. 初始化TLS服务端上下文
    if (tls_ctx_init(&ctx, TLS_protocol_tls12, TLS_server_mode) != 1) {
        fprintf(stderr, "Failed to init TLS context\n");
        return;
    }

    // 2. 设置服务端证书和密钥（国密证书需符合SM2格式）
    if (tls_ctx_set_certificate_and_key(&ctx, SERVER_CERT_FILE, SERVER_KEY_FILE, "123456") != 1) {
        fprintf(stderr, "Failed to set server cert/key\n");
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 3. 设置CA证书（用于验证客户端证书，无需手动修改conn的成员）
    if (tls_ctx_set_ca_certificates(&ctx, CA_CERT_FILE, 1) != 1) {
        fprintf(stderr, "Failed to set CA cert\n");
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 4. 设置证书验证深度（可选，默认已合理，显式设置更清晰）
    ctx.verify_depth = 1;  // 验证客户端证书的1层链（直接由CA签发）

    // 5. 创建TCP服务端socket
    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 6. 绑定端口（允许端口复用，避免服务重启时端口占用）
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt SO_REUSEADDR failed");
        close(server_sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡
    server_addr.sin_port = htons(SERVER_PORT);
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 7. 开始监听客户端连接
    if (listen(server_sock, 5) < 0) {
        perror("Listen failed");
        close(server_sock);
        tls_ctx_cleanup(&ctx);
        return;
    }
    printf("Server listening on port %d (TLS1.2 国密)...\n", SERVER_PORT);

    // 8. 接受客户端TCP连接
    if ((client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len)) < 0) {
        perror("Accept failed");
        close(server_sock);
        tls_ctx_cleanup(&ctx);
        return;
    }
    printf("Client connected: %s:%d\n", 
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // 9. 初始化TLS连接（绑定上下文）
    if (tls_init(&conn, &ctx) != 1) {
        fprintf(stderr, "Failed to init TLS connection\n");
        close(client_sock);
        close(server_sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 10. 绑定客户端socket到TLS连接（TLS会通过该socket传输加密数据）
    if (tls_set_socket(&conn, client_sock) != 1) {
        fprintf(stderr, "Failed to set TLS socket\n");
        tls_cleanup(&conn);
        close(client_sock);
        close(server_sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 11. 执行TLS1.2握手（服务端用tls12_do_accept）
    if (tls12_do_accept(&conn) != 1) {
        fprintf(stderr, "TLS handshake failed\n");
        tls_cleanup(&conn);
        close(client_sock);
        close(server_sock);
        tls_ctx_cleanup(&ctx);
        return;
    }


    // 12. 创建收发线程（传递TLS连接上下文，而非原始socket）
    pthread_t recv_th, send_th;
    if (pthread_create(&recv_th, NULL, receive_thread, &args) != 0) {
        perror("Create receive thread failed");
        running = 0;
        goto cleanup;  // 跳转到资源清理逻辑
    }
    if (pthread_create(&send_th, NULL, send_thread, &args) != 0) {
        perror("Create send thread failed");
        running = 0;
        pthread_cancel(recv_th);
        pthread_join(recv_th, NULL);
        goto cleanup;
    }

    // 13. 等待发送线程结束
    pthread_join(send_th, NULL);
    printf("Waiting for receive thread to exit...\n");
    // 使用 shutdown 触发 recv 返回 0/错误，从而让线程自然退出，避免 pthread_cancel 导致的资源状态不一致
    shutdown(client_sock, SHUT_RD);
    pthread_join(recv_th, NULL);

cleanup:
    // 14. 关闭TLS连接（优雅关闭，发送close_notify）
    {
        int ret = tls_shutdown(&conn);
        if (ret != 1) {
            fprintf(stderr, "TLS shutdown result: %d\n", ret);
        }
    }

    // 15. 资源清理（顺序：TLS连接 → 客户端socket → 服务端socket → TLS上下文）
    tls_cleanup(&conn);
    close(client_sock);
    close(server_sock);
    tls_ctx_cleanup(&ctx);
    printf("Server closed all resources\n");
}

int main() {
    run_tls_server();
    return 0;
}