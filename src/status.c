#include "status.h"

#include "build_config.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <cJSON.h>
#include <qrcodegen.h>

#define LOG_LINES 200

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char state[32] = "starting", user[64];
static int guilds, dms;
static long events;
static char qr_url[128], note[256];
static char qr_bits[177 * 177 + 1]; // row-major '1' = dark module
static int qr_size;
static char log_ring[LOG_LINES][160];
static unsigned log_next;
static time_t started;

void status_state(const char *s)
{
    pthread_mutex_lock(&lock);
    snprintf(state, sizeof state, "%s", s);
    if (!started)
        started = time(NULL);
    pthread_mutex_unlock(&lock);
}

void status_qr(const char *url)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    int size = 0;
    if (url && qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                    qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        size = qrcodegen_getSize(qr);
    pthread_mutex_lock(&lock);
    qr_size = size;
    snprintf(qr_url, sizeof qr_url, "%s", size ? url : "");
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            qr_bits[y * size + x] = qrcodegen_getModule(qr, x, y) ? '1' : '0';
    qr_bits[size * size] = 0;
    pthread_mutex_unlock(&lock);
}

void status_user(const char *name, int g, int d)
{
    pthread_mutex_lock(&lock);
    snprintf(user, sizeof user, "%s", name ? name : "");
    guilds = g;
    dms = d;
    pthread_mutex_unlock(&lock);
}

void status_event(void)
{
    pthread_mutex_lock(&lock);
    events++;
    pthread_mutex_unlock(&lock);
}

void status_log(const char *line)
{
    pthread_mutex_lock(&lock);
    snprintf(log_ring[log_next % LOG_LINES], sizeof log_ring[0], "%s", line);
    log_next++;
    pthread_mutex_unlock(&lock);
}

void status_note(const char *text)
{
    pthread_mutex_lock(&lock);
    snprintf(note, sizeof note, "%s", text ? text : "");
    pthread_mutex_unlock(&lock);
}

char *status_json(void)
{
    cJSON *j = cJSON_CreateObject();
    pthread_mutex_lock(&lock);
    cJSON_AddNumberToObject(j, "api_version", CORE_API_VERSION);
    cJSON_AddStringToObject(j, "version", CORE_VERSION);
    cJSON_AddStringToObject(j, "state", state);
    cJSON_AddStringToObject(j, "user", user);
    cJSON_AddNumberToObject(j, "guilds", guilds);
    cJSON_AddNumberToObject(j, "dms", dms);
    cJSON_AddNumberToObject(j, "events", (double)events);
    cJSON_AddNumberToObject(j, "uptime", started ? (double)(time(NULL) - started) : 0);
    cJSON_AddStringToObject(j, "qr_url", qr_url);
    cJSON_AddNumberToObject(j, "qr_size", qr_size);
    cJSON_AddStringToObject(j, "qr", qr_bits);
    cJSON_AddStringToObject(j, "note", note);
    cJSON *log = cJSON_AddArrayToObject(j, "log");
    unsigned first = log_next > LOG_LINES ? log_next - LOG_LINES : 0;
    for (unsigned i = first; i < log_next; i++)
        cJSON_AddItemToArray(log, cJSON_CreateString(log_ring[i % LOG_LINES]));
    pthread_mutex_unlock(&lock);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}
