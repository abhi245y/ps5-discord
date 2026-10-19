// ps5-discord core: background payload holding the Discord gateway session.
// PoC: logs in by QR code (printed to klog), keeps the token in a file,
// logs session state to klog.
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gateway.h"
#include "log.h"
#include "remote_auth.h"
#include "tls.h"

#ifdef __PROSPERO__
#define DATA_DIR "/data/ps5-discord"
#else
#define DATA_DIR "."
#endif
// ponytail: plaintext token on the console's data partition; encrypt at rest before release.
#define TOKEN_PATH DATA_DIR "/token"
// Control port: keeps one instance alive and takes "quit" (make stop) so a new build
// can replace the running one. Future UI app IPC goes here too.
#define CONTROL_PORT 9041

static int read_token(char *out, size_t cap)
{
    FILE *f = fopen(TOKEN_PATH, "r");
    if (!f)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    while (n > 0 && isspace((unsigned char)out[n - 1]))
        n--;
    out[n] = 0;
    return n > 0 ? 0 : -1;
}

static void save_token(const char *token)
{
    mkdir(DATA_DIR, 0700);
    FILE *f = fopen(TOKEN_PATH, "w");
    if (!f) {
        LOG("could not save token to %s", TOKEN_PATH);
        return;
    }
    fputs(token, f);
    fclose(f);
    chmod(TOKEN_PATH, 0600);
}

static void login(char *token, size_t cap)
{
    for (int backoff = 2; remote_auth_login(token, cap) != 0; backoff = backoff < 30 ? backoff * 2 : 30) {
        LOG("login failed, new QR in %d s", backoff);
        sleep((unsigned)backoff);
    }
    save_token(token);
}

static int claim_instance(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CONTROL_PORT)};
    // ponytail: open to the LAN for dev "make stop"; loopback-only once it carries app IPC.
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd; // kept open for the process lifetime
}

static void *control_loop(void *arg)
{
    int fd = (int)(intptr_t)arg;
    for (;;) {
        int c = accept(fd, NULL, NULL);
        if (c < 0)
            continue;
        char cmd[16] = {0};
        if (read(c, cmd, sizeof cmd - 1) > 0 && strncmp(cmd, "quit", 4) == 0) {
            LOG("quit requested");
            _exit(0);
        }
        close(c);
    }
    return NULL;
}

int main(void)
{
    char token[256];
    int control = claim_instance();
    if (control < 0) {
        LOG("already running");
        return 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, control_loop, (void *)(intptr_t)control);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    if (tls_global_init() != 0) {
        LOG("tls init failed");
        return 1;
    }
    LOG("starting");
    for (;;) {
        if (read_token(token, sizeof token) != 0)
            login(token, sizeof token);
        int code = gateway_run(token);
        if (code != 4004)
            return LOG("stopped (%d)", code), 1;
        LOG("token rejected, logging in again");
        remove(TOKEN_PATH);
    }
}
