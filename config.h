#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 4437
#define CA_CERT_FILE "certs/ca-cert.pem"
#define SERVER_CERT_FILE "certs/server-cert.pem"
#define SERVER_KEY_FILE "certs/server-key.pem"
#define CLIENT_CERT_FILE "certs/client-cert.pem"
#define CLIENT_KEY_FILE "certs/client-key.pem"
#define CHAT_HISTORY_FILE "chat_history.txt"
static const uint8_t CHAT_KEY[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10
};