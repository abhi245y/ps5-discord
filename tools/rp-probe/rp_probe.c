// SPDX-License-Identifier: AGPL-3.0-or-later
// Remote Play loopback probe for ps5-discord. Payload only, one shot.
// Asks whether the console's own Remote Play server can be reached from a
// payload: which ports are taken, whether TCP connects to the console's own
// address, and whether discovery (SRCH on 9302) and the regist search (SRC3
// on 9295) answer. Never registers, never sends a PIN or account id, never
// starts a session. Packet formats from chiaki-ng lib/src/discovery.c and
// lib/src/regist.c.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define PROBE_DIR "/data/ps5-discord-probe"
#define PROBE_LOG PROBE_DIR "/rp.txt"

static FILE *g_log;

static void log_line(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stdout, "[rp-probe] %s\n", line);
    fprintf(stderr, "[rp-probe] %s\n", line);
    fflush(stdout);
    fflush(stderr);
    if (g_log)
    {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
}

static void timeouts(int s, int ms)
{
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static struct sockaddr_in addr_of(in_addr_t ip, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    return a;
}

// The console's LAN address: connect a UDP socket (sends nothing) and read
// the source address the stack picked.
static in_addr_t lan_ip(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = addr_of(inet_addr("192.168.1.1"), 53), me;
    socklen_t n = sizeof me;
    in_addr_t ip = 0;
    if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(s, (struct sockaddr *)&me, &n) == 0)
        ip = me.sin_addr.s_addr;
    if (s >= 0)
        close(s);
    return ip;
}

// A port someone already holds fails bind with EADDRINUSE. A free port is
// bound for an instant and released.
static void port_taken(int type, int port)
{
    int s = socket(AF_INET, type, 0);
    struct sockaddr_in a = addr_of(htonl(INADDR_ANY), port);
    int r = bind(s, (struct sockaddr *)&a, sizeof a);
    int e = errno;
    log_line("bind %s %d: %s", type == SOCK_STREAM ? "tcp" : "udp", port,
             r == 0 ? "free (nobody listening)" : e == EADDRINUSE ? "IN USE (a server holds it)" : strerror(e));
    close(s);
}

static void tcp_connect(const char *name, in_addr_t ip, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    timeouts(s, 3000);
    struct sockaddr_in a = addr_of(ip, port);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    log_line("tcp connect %s:%d: %s", name, port, r == 0 ? "CONNECTED" : strerror(errno));
    close(s);
}

// Sends one datagram and logs every reply for two seconds.
static void udp_ask(const char *name, in_addr_t ip, int port, const char *msg, size_t len)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    timeouts(s, 2000);
    struct sockaddr_in a = addr_of(ip, port);
    if (sendto(s, msg, len, 0, (struct sockaddr *)&a, sizeof a) < 0)
    {
        log_line("udp %s:%d send: %s", name, port, strerror(errno));
        close(s);
        return;
    }
    int replies = 0;
    for (;;)
    {
        char buf[1500];
        struct sockaddr_in from;
        socklen_t n = sizeof from;
        ssize_t got = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &n);
        if (got < 0)
            break;
        buf[got] = 0;
        for (ssize_t i = 0; i < got; i++)
            if (buf[i] == 0)
                buf[i] = '.';
        replies++;
        log_line("udp %s:%d reply %d from %s (%zd bytes):\n%s", name, port, replies, inet_ntoa(from.sin_addr), got,
                 buf);
    }
    if (!replies)
        log_line("udp %s:%d: no reply in 2 s (%s)", name, port, strerror(errno));
    close(s);
}

int main(void)
{
    mkdir(PROBE_DIR, 0755);
    g_log = fopen(PROBE_LOG, "w");
    log_line("start");

    in_addr_t lo = htonl(INADDR_LOOPBACK), lan = lan_ip();
    struct in_addr l = {lan};
    char lan_name[32];
    snprintf(lan_name, sizeof lan_name, "%s", lan ? inet_ntoa(l) : "unknown");
    log_line("lan ip %s", lan_name);

    // 1. Who holds the Remote Play ports.
    const int ports[] = {9295, 9296, 9302};
    for (unsigned i = 0; i < sizeof ports / sizeof *ports; i++)
    {
        port_taken(SOCK_STREAM, ports[i]);
        port_taken(SOCK_DGRAM, ports[i]);
    }

    // 2. TCP to our own address.
    tcp_connect("127.0.0.1", lo, 9295);
    if (lan)
        tcp_connect(lan_name, lan, 9295);

    // 3. Discovery and regist search.
    static const char srch[] = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
    udp_ask("127.0.0.1", lo, 9302, srch, sizeof srch); // chiaki sends the NUL too
    if (lan)
    {
        udp_ask(lan_name, lan, 9302, srch, sizeof srch);
        udp_ask("broadcast", htonl(INADDR_BROADCAST), 9302, srch, sizeof srch);
    }
    udp_ask("127.0.0.1", lo, 9295, "SRC3", 5);
    if (lan)
        udp_ask(lan_name, lan, 9295, "SRC3", 5);

    log_line("done");
    if (g_log)
        fclose(g_log);
    return 0;
}
