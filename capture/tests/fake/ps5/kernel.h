#pragma once
#include <stdint.h>
#include <sys/types.h>
uint64_t kernel_get_ucred_authid(pid_t pid);
int kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);
int kernel_dynlib_handle(pid_t pid, const char *name, uint32_t *handle);
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *sym);
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid);
intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle);
int kernel_get_vmem_protection(pid_t pid, intptr_t addr, size_t len);
int kernel_set_vmem_protection(pid_t pid, intptr_t addr, size_t len, int prot);
