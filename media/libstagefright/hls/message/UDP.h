#ifndef _UDP_H_
#define _UDP_H_

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>


extern int setup_message_server(struct sockaddr_in *addr);
extern void receive_message(int sockfd, struct sockaddr_in *addr, char *msg, int len);
extern int connect_to_message_server(struct sockaddr_in *addr);
extern void send_message(int sockfd, struct sockaddr_in *addr, const char *msg);

#endif //_UDP_H_

