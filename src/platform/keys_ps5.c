// NOT BUILT YET: on 13.00 sceKeyboardInit crashes the calling payload (the
// process stops; sceKeyboardMbusInit returns 0 first, Open without Init says
// 0x80da0005 not initialised). Wire keys_start() into main.c and the Makefile
// (-lSceKeyboard -lSceUserService) once Init is understood.
//
// A USB or Bluetooth keyboard on the console, read by the core: the native app
// cannot reach libSceKeyboard itself (a title's sceKernelDlsym finds nothing,
// and its imports of libraries the system did not load for it resolve to 0).
// Each key that goes down, and the newest one held as it repeats, becomes a
// "KEY" event the app reads with its messages.
//
// libSceKeyboard has no header in the SDK: the state block follows the layout
// emulators and OpenOrbis reverse (96 bytes, held keys as USB HID usage codes).
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "chat.h"
#include "log.h"

typedef struct {
    uint64_t timestamp;
    uint32_t intercepted; // the system has the keyboard (a dialog is up)
    uint8_t reserve1[4];
    int32_t connected;
    int32_t length; // held keys in key_code
    uint32_t led;   // 1 num, 2 caps, 4 scroll lock
    uint32_t modifier_key;
    uint16_t key_code[16];
    uint8_t reserve2[32];
} keyboard_data;
_Static_assert(sizeof(keyboard_data) == 96, "keyboard state layout");

int sceKeyboardInit(void);
int sceKeyboardOpen(int user, int type, int index, void *param);
int sceKeyboardReadState(int handle, keyboard_data *data);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetInitialUser(int *user);

#define REPEAT_DELAY_US 450000
#define REPEAT_EVERY_US 40000

static int held(const keyboard_data *k, uint16_t key)
{
    for (int i = 0; i < k->length && i < 16; i++)
        if (k->key_code[i] == key)
            return 1;
    return 0;
}

static void *keys_loop(void *arg)
{
    (void)arg;
    LOG("keyboard: user service");
    sceUserServiceInitialize(NULL);
    int user = -1;
    const int user_rc = sceUserServiceGetInitialUser(&user);
    LOG("keyboard: user=0x%x rc=0x%08x; init", user, (unsigned)user_rc);
    const int init_rc = sceKeyboardInit();
    LOG("keyboard: init=0x%08x; open", (unsigned)init_rc);
    const int handle = sceKeyboardOpen(user, 0, 0, NULL);
    LOG("keyboard: user=0x%x init=0x%08x open=0x%08x", user, (unsigned)init_rc, (unsigned)handle);
    if (handle < 0)
        return NULL;
    keyboard_data last = {0};
    // Room past the 96 bytes in case this firmware's block is larger.
    union {
        keyboard_data k;
        uint8_t room[1024];
    } buf;
    keyboard_data now;
    int first = 1;
    uint16_t repeat_key = 0;
    long repeat_at = 0;
    int connected = -1;
    for (long t = 0;; t += 10000) {
        usleep(10000);
        memset(&buf, 0, sizeof buf);
        if (first)
            LOG("keyboard: first read");
        const int rc = sceKeyboardReadState(handle, &buf.k);
        if (first)
            LOG("keyboard: first read rc=0x%08x", (unsigned)rc);
        first = 0;
        now = buf.k;
        const int on = rc == 0 && now.connected;
        if (on != connected) {
            LOG("keyboard: %s (rc=0x%08x)", on ? "connected" : "not connected", (unsigned)rc);
            connected = on;
        }
        if (!on || now.intercepted) {
            memset(&last, 0, sizeof last);
            repeat_key = 0;
            continue;
        }
        for (int i = 0; i < now.length && i < 16; i++) {
            const uint16_t key = now.key_code[i];
            if (key && !held(&last, key)) {
                chat_key(key, (int)now.modifier_key, (int)now.led);
                repeat_key = key;
                repeat_at = t + REPEAT_DELAY_US;
            }
        }
        if (repeat_key && !held(&now, repeat_key))
            repeat_key = 0;
        else if (repeat_key && t >= repeat_at) {
            chat_key(repeat_key, (int)now.modifier_key, (int)now.led);
            repeat_at = t + REPEAT_EVERY_US;
        }
        last = now;
    }
}

void keys_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, keys_loop, NULL) == 0)
        pthread_detach(t);
}
