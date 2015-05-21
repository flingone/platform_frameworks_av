#ifndef _HLS_MESSAGE_H_
#define _HLS_MESSAGE_H_

#include "Socket.h"
extern int connect_to_message_server(struct sockaddr_in *addr);
extern void send_message(int sockfd, struct sockaddr_in *addr, const char *msg);

#endif //_HLS_MESSAGE_H_
