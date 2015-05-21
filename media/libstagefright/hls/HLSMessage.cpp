#include "Socket.h"

int connect_to_message_server(struct sockaddr_in *addr)
{
    int sockfd;

    sockfd = _socket(AF_INET, SOCK_DGRAM, 0);
    make_sockaddr(addr, 8888);

    return sockfd;
}

void send_message(int sockfd, struct sockaddr_in *addr, const char *msg)
{
    int addr_len = sizeof(*addr);
    sendto(sockfd, msg, strlen(msg) + 1, 0, (const struct sockaddr *)addr, addr_len);
}

