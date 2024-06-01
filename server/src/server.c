#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include "libcommon/libcommon.h"

#define IPADDR     "127.0.0.1"
#define PORT       "3490"
#define MAXCLIENTS 20

typedef struct nexchat_server_state_t
{
    int32_t sockfd;
    nexchat_client_state_t clients[MAXCLIENTS];
    pthread_mutex_t clients_mutex;
    bool running;
} nexchat_server_state_t;

typedef struct nexchat_client_thread_data_t
{
    nexchat_server_state_t* server_state;
    nexchat_client_state_t* client_state;
} nexchat_client_thread_data_t;

typedef struct nexchat_conn_accept_result_t
{
    int32_t connfd;
    char username[64];
} nexchat_conn_accept_result_t;

const void* nexchat_get_inet_addr(struct sockaddr* sa)
{
    if (sa->sa_family == AF_INET) // IPv4
    {
        return &((struct sockaddr_in*)sa)->sin_addr;
    }

    return &((struct sockaddr_in6*)sa)->sin6_addr;
}

int32_t nexchat_server_bind(nexchat_server_state_t* state, const nexchat_inet_id_t* id);
void nexchat_server_launch(nexchat_server_state_t* state);
void nexchat_server_shutdown(nexchat_server_state_t* state);
nexchat_conn_accept_result_t nexchat_server_accept_connection(nexchat_server_state_t* state);
void* nexchat_server_handle_client(void* arg);
void nexchat_server_sendmsg(int32_t sockfd, const char* msg);
int32_t nexchat_server_recvmsg(int32_t sockfd, char* recvbuf, size_t size);
void nexchat_server_broadcast_msg(nexchat_server_state_t* state, nexchat_client_state_t* sender, const char* username, const char* msg);
void nexchat_server_disconnect_client(nexchat_server_state_t* state, int32_t sockfd);

int32_t nexchat_server_bind(nexchat_server_state_t* state, const nexchat_inet_id_t* id)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* res = NULL;
    int32_t status = getaddrinfo(id->ipaddr, id->service, &hints, &res);

    if (status != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    struct addrinfo* it = NULL;
    int32_t yes = 1;
    
    for (it = res; it != NULL; it = it->ai_next)
    {
        state->sockfd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (state->sockfd == -1)
        {
            perror("socket");
            continue;
        }

        if (setsockopt(state->sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int32_t)) == -1)
        {
            perror("setsockopt");
            close(state->sockfd);
            continue;
        }

        if (bind(state->sockfd, it->ai_addr, it->ai_addrlen) == -1)
        {
            perror("bind");
            close(state->sockfd);
            continue;
        }

        break;
    }

    if (it == NULL)
    {
        fprintf(stderr, "server: failed to bind\n");
        return -1;
    }

    freeaddrinfo(res);

    return 0;
}

void nexchat_server_launch(nexchat_server_state_t* state)
{
    if (listen(state->sockfd, MAXCLIENTS) == -1)
    {
        perror("listen");
        fprintf(stderr, "server: failed to start listening\n");
        return;
    }
    
    state->running = true;
    pthread_mutex_init(&state->clients_mutex, NULL);

    for (size_t i = 0; i < MAXCLIENTS; i++)
    {
        nexchat_client_state_t* client = &state->clients[i];
        client->connected = false;
        client->sockfd = 0;
        memset(client->username, 0, sizeof(client->username));
    }
    
    printf("server: listening for connections...\n");

    while (state->running)
    {
        nexchat_conn_accept_result_t result = nexchat_server_accept_connection(state);

        if (result.connfd == -1)
        {
            fprintf(stderr, "server: failed to accept connection\n");
            continue;
        }

        bool found_empty_slot = false;

        for (size_t i = 0; i < MAXCLIENTS; i++)
        {
            nexchat_client_state_t* client = &state->clients[i];

            if (client->connected)
            {
                continue;
            }

            pthread_mutex_lock(&state->clients_mutex);
            
            client->sockfd = result.connfd;
            snprintf(client->username, sizeof(client->username) - 1, "%s\0", result.username);
            client->connected = true;

            char sendbuf[1024];
            snprintf(sendbuf, sizeof(sendbuf) - 1, "%s connected\0", client->username);
            nexchat_server_broadcast_msg(state, client, "server", sendbuf);

            // launch client recv thread
            nexchat_client_thread_data_t client_thread_data;
            client_thread_data.server_state = state;
            client_thread_data.client_state = client;
            pthread_create(&client->recv_thread, NULL, nexchat_server_handle_client, &client_thread_data);
            
            pthread_mutex_unlock(&state->clients_mutex);

            found_empty_slot = true;
            break;
        }

        if (!found_empty_slot)
        {
            printf("server: reached maximum number of clients, failed to accept new connection\n");
            continue;
        }
    }
}

void nexchat_server_shutdown(nexchat_server_state_t* state)
{
    pthread_mutex_destroy(&state->clients_mutex);

    printf("server: shutting down...\n");
}

nexchat_conn_accept_result_t nexchat_server_accept_connection(nexchat_server_state_t* state)
{
    nexchat_conn_accept_result_t result;

    struct sockaddr_storage conninfo;
    socklen_t conninfo_size = sizeof(struct sockaddr_storage);
    
    result.connfd = accept(state->sockfd, (struct sockaddr*)&conninfo, &conninfo_size);
    if (result.connfd == -1)
    {
        perror("accept");
        return result;
    }

    // get the clients username
    char recvbuf[1024];
    int32_t bytesread = nexchat_server_recvmsg(result.connfd, recvbuf, sizeof recvbuf);
    if (bytesread == -1)
    {
        perror("recv");
        result.connfd = -1;
        return result;
    }
    else if (bytesread == 0)
    {
        // client disconnected
    }

    recvbuf[bytesread] = '\0';
    snprintf(result.username, sizeof(result.username) - 1, "%s\0", recvbuf);

    printf("bytesread: %zu, username: %s, strlen: %zu\n", bytesread, result.username, strlen(result.username));

    char ipstr[INET6_ADDRSTRLEN];
    const void* addr = nexchat_get_inet_addr((struct sockaddr*)&conninfo);

    inet_ntop(conninfo.ss_family, addr, ipstr, sizeof ipstr);
    printf("server: '%s' connected from (%s)\n", result.username, ipstr);

    return result;
}

void* nexchat_server_handle_client(void* arg)
{
    nexchat_client_thread_data_t* data = (nexchat_client_thread_data_t*)arg;

    char recvbuf[1024];

    while (data->server_state->running)
    {
        int32_t bytesread = nexchat_server_recvmsg(data->client_state->sockfd, recvbuf, sizeof recvbuf);

        if (bytesread == -1)
        {
            perror("recv");
            continue;
        }
        else if (bytesread == 0) // client disconnected
        {
            nexchat_server_disconnect_client(data->server_state, data->client_state->sockfd);
            break;
        }

        recvbuf[bytesread] = '\0';

        printf("%s: %s\n", data->client_state->username, recvbuf);
        nexchat_server_broadcast_msg(data->server_state, data->client_state, data->client_state->username, recvbuf);
    }

    return NULL;
}

void nexchat_server_sendmsg(int32_t sockfd, const char* msg)
{
    int32_t bytessent = send(sockfd, msg, strlen(msg), 0);
    if (bytessent == -1)
    {
        perror("send");
        return;
    }

    printf("server: sending packet to client, contents: %s\n", msg);
}

int32_t nexchat_server_recvmsg(int32_t sockfd, char* recvbuf, size_t size)
{
    return recv(sockfd, recvbuf, size, 0);
}

void nexchat_server_broadcast_msg(nexchat_server_state_t* state, nexchat_client_state_t* sender, const char* username, const char* msg)
{
    char sendbuf[1024];
    snprintf(sendbuf, sizeof(sendbuf) - 1, "%s: %s\0", username, msg);

    for (size_t i = 0; i < MAXCLIENTS; i++)
    {
        nexchat_client_state_t* client = &state->clients[i];

        if (!client->connected)
        {
            continue;
        }

        if (client->sockfd == sender->sockfd)
        {
            continue;
        }

        nexchat_server_sendmsg(client->sockfd, sendbuf);
    }
}

void nexchat_server_disconnect_client(nexchat_server_state_t* state, int32_t sockfd)
{
    for (size_t i = 0; i < MAXCLIENTS; i++)
    {
        nexchat_client_state_t* client = &state->clients[i];

        if (!client->connected || client->sockfd != sockfd)
        {
            continue;
        }

        pthread_mutex_lock(&state->clients_mutex);

        printf("server: %s disconnected\n", client->username);
        char sendbuf[1024];
        snprintf(sendbuf, sizeof(sendbuf) - 1, "%s disconnected\0", client->username);
        nexchat_server_broadcast_msg(state, client, "server", sendbuf);

        client->connected = false;
        pthread_cancel(client->recv_thread);
        close(client->sockfd);

        pthread_mutex_unlock(&state->clients_mutex);
    }
}

int main(int argc, char** argv)
{
    nexchat_server_state_t server;
    nexchat_inet_id_t id = {.ipaddr=IPADDR, .service=PORT};

    if (nexchat_server_bind(&server, &id) == -1)
    {
        return 1;
    }

    nexchat_server_launch(&server);

    nexchat_server_shutdown(&server);
}