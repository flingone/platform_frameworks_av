#ifndef _SOCKET_H_
#define _SOCKET_H_

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#define SYS_LOGE(...) \
    do \
    { \
        printf("%s(%d): ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf(": %s.\n", strerror(errno)); \
        exit(0); \
    }while(0)

static inline int _socket(int domain, int type, int protocol)
{
    int fd;

    if ((fd = socket(domain, type, protocol)) == -1) {
        SYS_LOGE("Create socket failed");
    }

    return fd;
}

static inline void make_sockaddr(struct sockaddr_in *addr, int port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = htonl(INADDR_ANY);
}

static inline int _bind(int sockfd, const struct sockaddr_in *addr,
                        int addrlen)
{
    if (bind(sockfd, (const struct sockaddr *)addr, (socklen_t)addrlen) == -1) {
        SYS_LOGE("Bind failed");
    }

    return 0;
}

static inline int set_reuse_socket_option(int sockfd)
{
    int reuse = 1;

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        SYS_LOGE("set reuse socket option failed");
    }

    return 0;
}

static inline int _listen(int sockfd, int backlog)
{
    if (listen(sockfd, backlog)) {
        SYS_LOGE("listen failed");
    }

    return 0;
}


#endif
