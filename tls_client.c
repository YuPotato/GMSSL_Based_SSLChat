#include <gmssl/tls.h>
#include <gmssl/error.h>
#include <gmssl/sm3.h>
#include <gmssl/sm4.h>
#include <gmssl/hex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include "config.h"

void record_chathistory(const char *role, const char *msg);

// 线程参数：传递TLS_CONNECT上下文（包含socket和加密信息），而非原始socket
typedef struct {
    TLS_CONNECT *conn;
    int *running;  // 线程运行标志（优雅终止用）
} ThreadArgs;

// 接收线程：通过TLS_CONNECT读取加密数据（自动解密）
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
        record_chathistory("Server", (char *)buffer);
    }
    return NULL;
}

// 发送线程：通过TLS_CONNECT发送加密数据（自动加密、添加认证）
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
        record_chathistory("Client", message);
    }
    return NULL;
}

void run_tls_client() {
    TLS_CTX ctx;
    TLS_CONNECT conn;
    int sock;
    struct sockaddr_in server_addr;
    int running = 1;  // 线程运行标志（优雅终止）
    ThreadArgs args = {&conn, &running};  // 线程参数

    // 1. 初始化TLS上下文（添加错误详情）
    if (tls_ctx_init(&ctx, TLS_protocol_tls12, TLS_client_mode) != 1) {
        fprintf(stderr, "Failed to initialize TLS context\n");
        return;
    }

    // 2. 设置客户端证书和密钥（校验文件路径，添加错误详情）
    if (tls_ctx_set_certificate_and_key(&ctx, CLIENT_CERT_FILE, CLIENT_KEY_FILE, "123456") != 1) {
        fprintf(stderr, "Failed to set client cert/key\n");
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 3. 设置CA证书（校验文件，添加错误详情）
    if (tls_ctx_set_ca_certificates(&ctx, CA_CERT_FILE, 1) != 1) {
        fprintf(stderr, "Failed to set CA cert\n");
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 4. 创建TCP socket（标准流程，无问题）
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 5. 设置服务端地址（标准流程）
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid server address");
        close(sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 6. 连接服务端（标准流程）
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to server failed");
        close(sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 7. 初始化TLS连接（绑定上下文）
    if (tls_init(&conn, &ctx) != 1) {
        fprintf(stderr, "Failed to initialize TLS connection\n");
        close(sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 8. 绑定socket到TLS连接（关键：TLS会通过该socket传输加密数据）
    if (tls_set_socket(&conn, sock) != 1) {
        fprintf(stderr, "Failed to set socket for TLS\n");
        tls_cleanup(&conn);
        close(sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 9. 执行TLS1.2握手（添加错误详情）
    if (tls12_do_connect(&conn) != 1) {
        fprintf(stderr, "TLS handshake failed\n");
        tls_cleanup(&conn);
        close(sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    printf("TLS handshake successful\n");

    // 10. 创建线程（传递TLS_CONNECT上下文，而非原始socket）
    pthread_t recv_th, send_th;
    if (pthread_create(&recv_th, NULL, receive_thread, &args) != 0) {
        perror("Failed to create receive thread");
        running = 0;
        tls_shutdown(&conn);
        tls_cleanup(&conn);
        close(sock);
        tls_ctx_cleanup(&ctx);
        return;
    }
    if (pthread_create(&send_th, NULL, send_thread, &args) != 0) {
        perror("Failed to create send thread");
        running = 0;
        pthread_cancel(recv_th);
        pthread_join(recv_th, NULL);
        tls_shutdown(&conn);
        tls_cleanup(&conn);
        close(sock);
        tls_ctx_cleanup(&ctx);
        return;
    }

    // 11. 等待发送线程结束（优雅终止）
    pthread_join(send_th, NULL);
    printf("Waiting for receive thread to exit...\n");
    // 使用 shutdown 触发 recv 返回 0/错误，从而让线程自然退出，避免 pthread_cancel 导致的资源状态不一致
    shutdown(sock, SHUT_RD);
    pthread_join(recv_th, NULL);

    // 12. 关闭TLS连接（添加错误处理）
    int ret = tls_shutdown(&conn);
    if (ret != 1) {
        fprintf(stderr, "TLS shutdown result: %d\n", ret);
    }

    // 13. 资源清理（顺序：TLS连接 → socket → TLS上下文）
    tls_cleanup(&conn);
    close(sock);
    tls_ctx_cleanup(&ctx);
}
// Helper to derive key and IV from password
void derive_key_iv(const char *pass, uint8_t key[16], uint8_t iv[16]) {
    SM3_CTX sm3_ctx;
    uint8_t digest[32];
    sm3_init(&sm3_ctx);
    sm3_update(&sm3_ctx, (const uint8_t *)pass, strlen(pass));
    sm3_finish(&sm3_ctx, digest);
    memcpy(key, digest, 16);
    memcpy(iv, digest + 16, 16);
}

//使用SM4进行加密存储
void record_chathistory(const char *role, const char *msg) {
    FILE *fp = fopen("chat_history.txt", "a");
    if (fp == NULL) {
        perror("Failed to open chat history file");
        return;
    }
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
    
    char full_msg[2048];
    snprintf(full_msg, sizeof(full_msg), "[%s] %s: %s", time_str, role, msg);
    // Ensure message ends with newline if it doesn't
    size_t len = strlen(full_msg);
    if (len > 0 && full_msg[len - 1] != '\n') {
        strcat(full_msg, "\n");
        len++;
    }

    uint8_t key[16];
    uint8_t iv[16];
    derive_key_iv("123456", key, iv);

    SM4_KEY sm4_key;
    sm4_set_encrypt_key(&sm4_key, key);

    uint8_t ciphertext[4096];
    size_t ciphertext_len;

    if (sm4_cbc_padding_encrypt(&sm4_key, iv, (uint8_t *)full_msg, len, ciphertext, &ciphertext_len) != 1) {
        fprintf(stderr, "Failed to encrypt message\n");
        fclose(fp);
        return;
    }

    for (size_t i = 0; i < ciphertext_len; i++) {
        fprintf(fp, "%02X", ciphertext[i]);
    }
    fprintf(fp, "\n");
    
    fclose(fp);
}
//使用SM4解密
void load_chathistory() {
    FILE *fp = fopen("chat_history.txt", "r");
    if (fp == NULL) {
        printf("No chat history found.\n");
        return;
    }
    
    char line[8192];
    uint8_t ciphertext[4096];
    size_t ciphertext_len;
    uint8_t plaintext[4096];
    size_t plaintext_len;

    uint8_t key[16];
    uint8_t iv[16];

    char password[256];
    printf("Enter password: ");
    scanf("%255s", password);

    derive_key_iv(password, key, iv);

    SM4_KEY sm4_key;
    sm4_set_decrypt_key(&sm4_key, key);

    printf("\n--- Chat History ---\n");
    while (fgets(line, sizeof(line), fp) != NULL) {
        // Remove newline
        size_t line_len = strlen(line);
        while (line_len > 0 && (line[line_len-1] == '\n' || line[line_len-1] == '\r')) {
            line[line_len-1] = '\0';
            line_len--;
        }
        if (line_len == 0) continue;

        if (hex_to_bytes(line, line_len, ciphertext, &ciphertext_len) != 1) {
            // fprintf(stderr, "Invalid hex in chat history\n");
            continue; 
        }

        if (sm4_cbc_padding_decrypt(&sm4_key, iv, ciphertext, ciphertext_len, plaintext, &plaintext_len) != 1) {
            fprintf(stderr, "Failed to decrypt message\n");
            continue;
        }
        plaintext[plaintext_len] = '\0';
        printf("%s", (char *)plaintext);
    }
    printf("--------------------\n\n");
    
    fclose(fp);
}

int main() {
    int choice;
    printf("1.start chat\n");
    printf("2.load chat history\n");
    printf("Enter your choice: ");
    scanf("%d", &choice);
    if (choice == 1) {
        run_tls_client();
    } else if (choice == 2) {
        load_chathistory();
    } else {
        printf("Invalid choice\n");
    }
    return 0;
}