#pragma once
#include <stddef.h>

void browser_auth_allow(int allowed);
int browser_auth_request(void); // accepted only while signed out, once per attempt
void browser_auth_cancel(void);
int browser_auth_pending(void);
int browser_auth_keepalive(void); // keep core alive while browser covers the app
int browser_auth_login(char *out, size_t cap); // called only by main login thread
