// QR login (remote auth v2, docs/remote-auth.md). The QR code goes to the log.
#pragma once
#include <stddef.h>

// One login attempt. 0 with the user token in out, -1 on failure, timeout or cancel.
int remote_auth_login(char *out, size_t cap);
