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
    size_t connected_clients;
    pthread_mutex_t clients_mutex;
    bool running;
} nexchat_server_state_t;

typedef struct nexchat_client_thread_data_t
{
    nexchat_server_state_t* server_state;
    size_t client_index;
} nexchat_client_thread_data_t;

typedef struct nexchat_conn_accept_result_t
{
    int32_t connfd;
    char username[64];
} nexchat_conn_accept_result_t;

typedef enum nexchat_client_command_t
{
    CMD_NONE,
    CMD_SETUSERNAME,
    CMD_LISTUSERS,
    CMD_KICKUSER,
    CMD_MAXCOMMANDS,
} nexchat_client_command_t;

const char* nexchat_client_command_to_str(nexchat_client_command_t cmd)
{
    switch (cmd)
    {
        case CMD_SETUSERNAME: return "set-username";
        case CMD_LISTUSERS: return "users";
        case CMD_KICKUSER: return "kick";
    }

    return "none";
}

const char* nexchat_client_command_get_desc(nexchat_client_command_t cmd)
{
    switch (cmd)
    {
        case CMD_SETUSERNAME: return "Set a new username";
        case CMD_LISTUSERS: return "List all users in the chat";
        case CMD_KICKUSER: return "Vote to kick a user from the chat. If the majority agrees, the user will be kicked.";
    }

    return "none";
}

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
void nexchat_server_send_cmdlist_to_client(int32_t sockfd);
void nexchat_server_exec_cmd(nexchat_server_state_t* state, nexchat_client_state_t* client, nexchat_client_command_t cmd, size_t argc, const char* argv);
void nexchat_server_broadcast_msg(nexchat_server_state_t* state, nexchat_client_state_t* sender, const char* username, const char* msg);
void nexchat_server_disconnect_client(nexchat_server_state_t* state, int32_t sockfd);
void nexchat_server_kick_client(nexchat_server_state_t* state, int32_t sockfd);

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
    state->connected_clients = 0;
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

            // launch client recv thread
            nexchat_client_thread_data_t client_thread_data = {.server_state=state, .client_index=i};
            pthread_create(&client->recv_thread, NULL, nexchat_server_handle_client, &client_thread_data);
            pthread_detach(client->recv_thread);
            
            state->connected_clients++;

            pthread_mutex_unlock(&state->clients_mutex);

            char sendbuf[1024];
            snprintf(sendbuf, sizeof(sendbuf) - 1, "%s connected\0", client->username);
            nexchat_server_broadcast_msg(state, client, NULL, sendbuf);

            memset(sendbuf, 0, sizeof sendbuf);
            snprintf(sendbuf, sizeof(sendbuf) - 1, "type /commands to see a list of commands.\0");
            nexchat_server_sendmsg(client->sockfd, sendbuf);

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

    char ipstr[INET6_ADDRSTRLEN];
    const void* addr = nexchat_get_inet_addr((struct sockaddr*)&conninfo);

    inet_ntop(conninfo.ss_family, addr, ipstr, sizeof ipstr);
    printf("server: '%s' connected from (%s)\n", result.username, ipstr);

    return result;
}

void* nexchat_server_handle_client(void* arg)
{
    nexchat_client_thread_data_t* data = (nexchat_client_thread_data_t*)arg;
    nexchat_client_state_t* client = &data->server_state->clients[data->client_index];

    char recvbuf[1024];

    while (client->connected && data->server_state->running)
    {
        int32_t bytesread = nexchat_server_recvmsg(client->sockfd, recvbuf, sizeof recvbuf);

        if (strlen(client->username) == 6)
        {
            int a = 10;
        }

        if (bytesread == -1)
        {
            perror("recv");
            continue;
        }
        else if (bytesread == 0) // client disconnected
        {
            nexchat_server_disconnect_client(data->server_state, client->sockfd);
            break;
        }

        recvbuf[bytesread] = '\0';

        if (recvbuf[0] == '/')
        {
            if (strcmp(recvbuf, "/commands") == 0)
            {
                nexchat_server_send_cmdlist_to_client(client->sockfd);
            }
            else
            {
                bool foundcmd = false;

                for (size_t i = CMD_NONE + 1; i < CMD_MAXCOMMANDS; i++)
                {
                    nexchat_client_command_t cmd = (nexchat_client_command_t)i;
					const char* cmdstr = nexchat_client_command_to_str(cmd);
					size_t len = strlen(cmdstr);

                    if (memcmp(&recvbuf[1], cmdstr, len) != 0)
                    {
                        continue;
                    }

					const char* args = NULL;
					const char* space = strchr(recvbuf, ' ');
					size_t argc = 0;
					
					if (space)
					{
						args = &recvbuf[(size_t)(space - recvbuf) + 1];
						argc++;
					}
					
                    nexchat_server_exec_cmd(data->server_state, client, cmd, argc, args);
                    foundcmd = true;
                    break;
                }

                if (!foundcmd)
                {
                    char sendbuf[1024];
                    snprintf(sendbuf, sizeof(sendbuf) - 1, "server: unknown command '%s'\0", recvbuf);
                    nexchat_server_sendmsg(client->sockfd, sendbuf);
                }
            }
        }
        else
        {
            printf("%s: %s\n", client->username, recvbuf);
            nexchat_server_broadcast_msg(data->server_state, client, client->username, recvbuf);
        }
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
}

int32_t nexchat_server_recvmsg(int32_t sockfd, char* recvbuf, size_t size)
{
    return recv(sockfd, recvbuf, size, 0);
}

void nexchat_server_send_cmdlist_to_client(int32_t sockfd)
{
    char sendbuf[1024];
    size_t offset = 0;

    for (size_t i = CMD_NONE + 1; i < CMD_MAXCOMMANDS; i++)
    {
        nexchat_client_command_t cmd = (nexchat_client_command_t)i;
        const char* cmdstr = nexchat_client_command_to_str(cmd);
        const char* cmddesc = nexchat_client_command_get_desc(cmd);
        offset += snprintf(sendbuf + offset, sizeof(sendbuf) - 1, "  /%s - %s\n", cmdstr, cmddesc);
    }

    nexchat_server_sendmsg(sockfd, sendbuf);
}

void nexchat_server_exec_cmd(nexchat_server_state_t* state, nexchat_client_state_t* client, nexchat_client_command_t cmd, size_t argc, const char* args)
{
    char sendbuf[1024];

    switch (cmd)
    {
        case CMD_SETUSERNAME:
        {
            if (argc != 1)
			{
				const char* cmdstr = nexchat_client_command_to_str(cmd);
				snprintf(sendbuf, sizeof(sendbuf) + 1, "server: expected '1' argument to /%s got '%s'\0", cmdstr, argc);
				nexchat_server_sendmsg(client->sockfd, sendbuf);
			}
			else
			{
				size_t usernamelen = strlen(args);
				if (usernamelen >= sizeof client->username)
				{
					nexchat_server_sendmsg(client->sockfd, "server: username is too long");
				}
				else
				{
					size_t oldusernamelen = strlen(client->username);
					char oldusername[64];
					memcpy(oldusername, client->username, strlen(client->username));
					oldusername[oldusernamelen] = '\0';

					pthread_mutex_lock(&state->clients_mutex);

					memcpy(client->username, args, usernamelen);

					pthread_mutex_unlock(&state->clients_mutex);

					printf("server: '%s' set username -> '%s'\n", oldusername, client->username);
					nexchat_server_sendmsg(client->sockfd, "server: new username set");

					snprintf(sendbuf, sizeof(sendbuf) - 1, "'%s' set username -> '%s'\0", oldusername, client->username);
					nexchat_server_broadcast_msg(state, client, "server", sendbuf);
				}
			}
        } break;
        case CMD_LISTUSERS:
        {
            size_t offset = 0;

            for (size_t i = 0; i < MAXCLIENTS; i++)
            {
                nexchat_client_state_t* c = &state->clients[i];
                
                if (!c->connected)
                {
                    continue;
                }

				const char* fmt = c->sockfd == client->sockfd ? "%s (you)\n" : "%s\n";
                offset += snprintf(sendbuf + offset, sizeof(sendbuf) - 1, fmt, c->username);
            }

			nexchat_server_sendmsg(client->sockfd, sendbuf);
        } break;
        case CMD_KICKUSER:
        {
            if (argc != 1)
			{
				const char* cmdstr = nexchat_client_command_to_str(cmd);
				snprintf(sendbuf, sizeof(sendbuf) - 1, "server: expected 1 argument to /%s, got '%zu'\0", cmdstr, argc);
				nexchat_server_sendmsg(client->sockfd, sendbuf);
			}
			else
			{
				bool foundclient = false;

				for (size_t i = 0; i < MAXCLIENTS; i++)
				{
					nexchat_client_state_t* c = &state->clients[i];

					if (!c->connected || c->sockfd == client->sockfd)
					{
						continue;
					}

					if (strcmp(c->username, args) != 0)
					{
						continue;
					}

					c->kicks_requested++;
					size_t majority = (state->connected_clients / 2) + state->connected_clients % 2;

					if (c->kicks_requested >= majority)
					{
						nexchat_server_kick_client(state, c->sockfd);
					}

					foundclient = true;
					break;
				}

				if (!foundclient)
				{
					snprintf(sendbuf, sizeof(sendbuf) + 1, "server: no users named '%s' in the chat\0", args);
					nexchat_server_sendmsg(client->sockfd, sendbuf);
				}
			}
        } break;
    }
}

void nexchat_server_broadcast_msg(nexchat_server_state_t* state, nexchat_client_state_t* sender, const char* username, const char* msg)
{
    char sendbuf[1024];
	if (username)
	{
    	snprintf(sendbuf, sizeof(sendbuf) - 1, "%s: %s\0", username, msg);
	}
	else
	{
		snprintf(sendbuf, sizeof(sendbuf) - 1, "%s\0", msg);
	}

    for (size_t i = 0; i < MAXCLIENTS; i++)
    {
        nexchat_client_state_t* client = &state->clients[i];

        if (!client->connected || client->sockfd == sender->sockfd)
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

        printf("server: %s disconnected\n", client->username);
        char sendbuf[1024];
        snprintf(sendbuf, sizeof(sendbuf) - 1, "%s disconnected\0", client->username);
        nexchat_server_broadcast_msg(state, client, NULL, sendbuf);

        pthread_mutex_lock(&state->clients_mutex);

        client->connected = false;
        pthread_join(client->recv_thread, NULL);
        close(client->sockfd);
        client->sockfd = 0;
        memset(client->username, 0, sizeof(client->username));

        state->connected_clients--;

        pthread_mutex_unlock(&state->clients_mutex);
    }
}

void nexchat_server_kick_client(nexchat_server_state_t* state, int32_t sockfd)
{
    for (size_t i = 0; i < MAXCLIENTS; i++)
    {
        nexchat_client_state_t* client = &state->clients[i];

        if (!client->connected || client->sockfd != sockfd)
        {
            continue;
        }

        printf("server: kicked '%s' from chat\n", client->username);
        char sendbuf[1024];
        snprintf(sendbuf, sizeof(sendbuf) - 1, "kicked '%s' from chat\0", client->username);
        nexchat_server_broadcast_msg(state, client, "server", sendbuf);

		nexchat_server_sendmsg(client->sockfd, "server: you have been kicked from chat");

        pthread_mutex_lock(&state->clients_mutex);

        client->connected = false;
        pthread_join(client->recv_thread, NULL);
        close(client->sockfd);
        client->sockfd = 0;
        memset(client->username, 0, sizeof(client->username));

        state->connected_clients--;

        pthread_mutex_unlock(&state->clients_mutex);

        break;
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