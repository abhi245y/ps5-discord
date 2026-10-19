// ps5-discord core: background payload holding the Discord gateway session.
// PoC: token read from a file, session state logged to klog.
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "gateway.h"
#include "log.h"
#include "tls.h"

#ifdef __PROSPERO__
#define TOKEN_PATH "/data/ps5-discord/token"
#else
#define TOKEN_PATH "token.txt"
#endif
// Also the future control port for the UI app; for now it only keeps one instance alive.
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

static int claim_instance(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CONTROL_PORT)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd; // kept open for the process lifetime
}

int main(void)
{
    char token[256];
    if (claim_instance() < 0) {
        LOG("already running");
        return 1;
    }
    if (read_token(token, sizeof token) != 0) {
        LOG("no token at %s", TOKEN_PATH);
        return 1;
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    if (tls_global_init() != 0) {
        LOG("tls init failed");
        return 1;
    }
    LOG("starting");
    int code = gateway_run(token);
    LOG("stopped (%d)", code);
    return 0;
}
