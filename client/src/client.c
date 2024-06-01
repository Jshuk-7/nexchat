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

int main(int argc, char** argv)
{
	struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* res = NULL;
    int32_t status = getaddrinfo(IPADDR, PORT, &hints, &res);

    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    int32_t sockfd;
    struct addrinfo* it = NULL;

    for (it = res; it != NULL; it = it->ai_next) {
        sockfd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sockfd == -1) {
            perror("socket");
            continue;
        }

        if (connect(sockfd, it->ai_addr, it->ai_addrlen) == -1) {
            perror("connect");
            close(sockfd);
            continue;
        }

        printf("client: connected to host\n");

        // send the host our username
        char username[64];
        bool first_try = true;

        while (strlen(username) == 0) {
            if (!first_try) {
                printf("Username must not be empty!\n\n");
            }
            printf("Enter username: ");
            fgets(username, sizeof(username) - 1, stdin);
            first_try = false;
        }

        for (size_t i = 0; username[i] != '\0'; i++) {
            if (username[i] == '\n') {
                username[i] = '\0';
                break;
            }
        }

        int32_t bytessent = send(sockfd, username, strlen(username) + 1, 0);
        if (bytessent == -1) {
            perror("send");
            fprintf(stderr, "client: failed to send username to host\n");
            return -1;
        }

        break;
    }

    if (it == NULL) {
        fprintf(stderr, "client: failed to connect to host\n");
        return -1;
    }

    freeaddrinfo(res);
}