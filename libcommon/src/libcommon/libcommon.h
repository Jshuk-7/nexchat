#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

typedef struct nexchat_client_state_t
{
    int32_t sockfd;
    char username[64];
    pthread_t recv_thread;
    bool connected;
} nexchat_client_state_t;

typedef struct nexchat_inet_id_t
{
    const char* ipaddr;
    const char* service;
} nexchat_inet_id_t;
