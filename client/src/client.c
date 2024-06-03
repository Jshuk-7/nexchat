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

int32_t nexchat_client_connect_to_host(nexchat_client_state_t* state, nexchat_inet_id_t* id);
int32_t nexchat_client_send_username_to_host(nexchat_client_state_t* state);
void nexchat_client_launch(nexchat_client_state_t* state);
void* nexchat_client_handle_incoming_msgs(void* arg);
void nexchat_client_sendmsg(int32_t sockfd, const char* msg);

int32_t nexchat_client_connect_to_host(nexchat_client_state_t* state, nexchat_inet_id_t* id)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* res = NULL;
    int32_t status = getaddrinfo(IPADDR, PORT, &hints, &res);

    if (status != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    struct addrinfo* it = NULL;

    for (it = res; it != NULL; it = it->ai_next)
    {
        state->sockfd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (state->sockfd == -1)
        {
            perror("socket");
            continue;
        }

        if (connect(state->sockfd, it->ai_addr, it->ai_addrlen) == -1)
        {
            perror("connect");
            close(state->sockfd);
            continue;
        }

        printf("client: connected to host\n");

        if (nexchat_client_send_username_to_host(state) == -1)
        {
            continue;
        }

        break;
    }

    if (it == NULL)
    {
        fprintf(stderr, "client: failed to connect to host\n");
        return -1;
    }

    freeaddrinfo(res);

    return 0;
}

int32_t nexchat_client_send_username_to_host(nexchat_client_state_t* state)
{
    // send the host our username
    bool first_attempt = true;

    memset(state->username, 0, sizeof(state->username));

    do 
    {
        if (!first_attempt)
        {
            printf("Username must not be empty!\n\n");
        }
        printf("Enter username: ");
        fgets(state->username, sizeof(state->username) - 1, stdin);
        first_attempt = false;
    } while (strcmp(state->username, "\n") == 0);

    for (size_t i = 0; state->username[i] != '\0'; i++)
    {
        if (state->username[i] != '\n')
        {
            continue;
        }
    
        state->username[i] = '\0';
        break;
    }

    int32_t bytessent = send(state->sockfd, state->username, strlen(state->username), 0);
    if (bytessent == -1)
    {
        perror("send");
        fprintf(stderr, "client: failed to send username to host\n");
        return -1;
    }

    return 0;
}

void nexchat_client_launch(nexchat_client_state_t* state)
{
    state->connected = true;

    pthread_create(&state->recv_thread, NULL, nexchat_client_handle_incoming_msgs, state);
    pthread_detach(state->recv_thread);

    char sendbuf[1024];

    while (state->connected)
    {
        fgets(sendbuf, sizeof(sendbuf) - 1, stdin);
        for (size_t i = 0; sendbuf[i] != '\0'; i++)
        {
            if (sendbuf[i] == '\n')
            {
                sendbuf[i] = '\0';
                break;
            }
        }

        nexchat_client_sendmsg(state->sockfd, sendbuf);
    }
}

void* nexchat_client_handle_incoming_msgs(void* arg)
{
    nexchat_client_state_t* client = (nexchat_client_state_t*)arg;

    char recvbuf[1024];

    while (client->connected)
    {
        int32_t bytesread = recv(client->sockfd, recvbuf, sizeof recvbuf, 0);

        if (bytesread == -1)
        {
            perror("recv");
            continue;
        }
        else if (bytesread == 0) // server disconnected
        {
            printf("client: host disconnected\n");
            break;
        }

        recvbuf[bytesread] = '\0';

        printf("%s\n", recvbuf);
    }

    return NULL;
}

void nexchat_client_sendmsg(int32_t sockfd, const char* msg)
{
    int32_t bytessent = send(sockfd, msg, strlen(msg), 0);
    if (bytessent == -1)
    {
        perror("send");
        return;
    }
}

int main(int argc, char** argv)
{
    nexchat_client_state_t client;
    nexchat_inet_id_t id = {.ipaddr=IPADDR, .service=NULL};

    if (nexchat_client_connect_to_host(&client, &id) == -1)
    {
        return 1;
    }

    nexchat_client_launch(&client);
}