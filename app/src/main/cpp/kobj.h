#pragma once
/* Kernel object handles. A handle is (generation << 15) | index; 0 is never valid. */

#include <stdint.h>

enum { KO_NONE = 0, KO_SESSION, KO_EVENT_W, KO_EVENT_R, KO_THREAD, KO_TMEM, KO_SHMEM, KO_PROCESS, KO_GS_SERVER, KO_GS_CLIENT };

#define HANDLE_CUR_THREAD  0xFFFF8000u
#define HANDLE_CUR_PROCESS 0xFFFF8001u

#define RES_OK              0u
#define RES_TIMEOUT         0xEA01u
#define RES_CANCELLED       0xEC01u
#define RES_INVALID_HANDLE  0xE401u
#define RES_INVALID_STATE   0xFA01u
#define RES_INVALID_MEMORY  0xD401u
#define RES_INVALID_ADDRESS 0xCC01u
#define RES_INVALID_SIZE    0xCA01u
#define RES_INVALID_ENUM    0xF001u
#define RES_OUT_OF_MEMORY   0xD001u

uint32_t kobj_new(int kind, void *obj);
void *kobj_get(uint32_t handle, int kind); /* NULL when the handle is bad or another kind */
int kobj_kind(uint32_t handle);
void *kobj_take(uint32_t handle, int *kind); /* removes the handle; returns its object */
