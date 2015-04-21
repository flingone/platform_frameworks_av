/******************************************************************************
  Copyright (c) 2013 Morten Houmøller Nygaard - www.mortz.dk - admin@mortz.dk

  Permission is hereby granted, free of charge, to any person obtaining a copy of
  this software and associated documentation files (the "Software"), to deal in
  the Software without restriction, including without limitation the rights to
  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
  of the Software, and to permit persons to whom the Software is furnished to do
  so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
 ******************************************************************************/

#include "Handshake.h"
#include "Communicate.h"
#include "Errors.h"
#include "UDP.h"

ws_list *l;
int port = 4567;

/**
 * This function listens for input from STDIN and tries to match it to a 
 * pattern that will trigger different actions.
 */
void *cmdline(void *arg)
{
    int sockfd;
    char buffer[32];
    struct sockaddr_in addr;

    pthread_detach(pthread_self());

    sockfd = setup_message_server(&addr);

    while (1) {
        ws_connection_close status;

        receive_message(sockfd, &addr, buffer, 32);

        ws_message *m = message_new();
        m->len = strlen(buffer);

        char *temp = malloc(m->len+1);
        if (temp == NULL) {
            //FIXME
            break;
        }
        strcpy(temp, buffer);
        m->msg = temp;
        temp = NULL;

        if ( (status = encodeMessage(m)) != CONTINUE) {
            message_free(m);
            free(m);
            //FIXME
            break;;
        }

        list_multicast_all(l, m);
        message_free(m);
        free(m);

        sleep(1);
    }

    pthread_exit((void *) EXIT_SUCCESS);

    return NULL;
}

void *handleClient(void *args) {
    pthread_detach(pthread_self());

    int buffer_length = 0, string_length = 1, reads = 1;

    ws_client *n = args;
    n->thread_id = pthread_self();

    char buffer[BUFFERSIZE];
    n->string = (char *) malloc(sizeof(char));

    if (n->string == NULL) {
        handshake_error("Couldn't allocate memory.", ERROR_INTERNAL, n);
        pthread_exit((void *) EXIT_FAILURE);
    }

    printf("Client connected with the following information:\n"
            "\tSocket: %d\n"
            "\tAddress: %s\n\n", n->socket_id, (char *) n->client_ip);
    printf("Checking whether client is valid ...\n\n");
    fflush(stdout);

    /**
     * Getting headers and doing reallocation if headers is bigger than our
     * allocated memory.
     */
    do {
        memset(buffer, '\0', BUFFERSIZE);
        if ((buffer_length = recv(n->socket_id, buffer, BUFFERSIZE, 0)) <= 0){
            handshake_error("Didn't receive any headers from the client.", 
                    ERROR_BAD, n);
            pthread_exit((void *) EXIT_FAILURE);
        }

        if (reads == 1 && strlen(buffer) < 14) {
            handshake_error("SSL request is not supported yet.", 
                    ERROR_NOT_IMPL, n);
            pthread_exit((void *) EXIT_FAILURE);
        }

        string_length += buffer_length;

        char *tmp = realloc(n->string, string_length);
        if (tmp == NULL) {
            handshake_error("Couldn't reallocate memory.", ERROR_INTERNAL, n);
            pthread_exit((void *) EXIT_FAILURE);
        }
        n->string = tmp;
        tmp = NULL;

        memset(n->string + (string_length-buffer_length-1), '\0', 
                buffer_length+1);
        memcpy(n->string + (string_length-buffer_length-1), buffer, 
                buffer_length);
        reads++;
    } while( strncmp("\r\n\r\n", n->string + (string_length-5), 4) != 0 
            && strncmp("\n\n", n->string + (string_length-3), 2) != 0
            && strncmp("\r\n\r\n", n->string + (string_length-8-5), 4) != 0
            && strncmp("\n\n", n->string + (string_length-8-3), 2) != 0 );

    printf("User connected with the following headers:\n%s\n\n", n->string);
    fflush(stdout);

    ws_header *h = header_new();

    if (h == NULL) {
        handshake_error("Couldn't allocate memory.", ERROR_INTERNAL, n);
        pthread_exit((void *) EXIT_FAILURE);
    }

    n->headers = h;

    if ( parseHeaders(n->string, n, port) < 0 ) {
        pthread_exit((void *) EXIT_FAILURE);
    }

    if ( sendHandshake(n) < 0 && n->headers->type != UNKNOWN ) {
        pthread_exit((void *) EXIT_FAILURE);	
    }	

    list_add(l, n);

    printf("Client has been validated and is now connected\n\n");
    printf("> ");
    fflush(stdout);

    uint64_t next_len = 0;
    char next[BUFFERSIZE];
    memset(next, '\0', BUFFERSIZE);

    while (1) {
        if ( communicate(n, next, next_len) != CONTINUE) {
            break;
        }

        if (n->headers->protocol == CHAT) {
            list_multicast(l, n);
        } else if (n->headers->protocol == ECHO) {
            list_multicast_one(l, n, n->message);
        } else {
            list_multicast_one(l, n, n->message);
        }

        if (n->message != NULL) {
            memset(next, '\0', BUFFERSIZE);
            memcpy(next, n->message->next, n->message->next_len);
            next_len = n->message->next_len;
            message_free(n->message);
            free(n->message);
            n->message = NULL;	
        }
    }

    printf("Shutting client down..\n\n");
    printf("> ");
    fflush(stdout);

    list_remove(l, n);
    pthread_exit((void *) EXIT_SUCCESS);

    return NULL;
}

int main(int argc, char *argv[]) {
    int server_socket, client_socket, on = 1;

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_length;
    pthread_t pthread_id;

    /**
     * Creating new lists, l is supposed to contain the connected users.
     */
    l = list_new();


    printf("Server: \t\tStarted\n");
    printf("Port: \t\t\t%d\n", port);

    /**
     * Opening server socket.
     */
    if ( (server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
        server_error(strerror(errno), server_socket, l);
    }

    printf("Socket: \t\tInitialized\n");

    /**
     * Allow reuse of address, when the server shuts down.
     */
    if ( (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &on, 
                    sizeof(on))) < 0 ){
        server_error(strerror(errno), server_socket, l);
    }

    printf("Reuse Port %d: \tEnabled\n", port);

    memset((char *) &server_addr, '\0', sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    printf("Ip Address: \t\t%s\n", inet_ntoa(server_addr.sin_addr));

    /**
     * Bind address.
     */
    if ( (bind(server_socket, (struct sockaddr *) &server_addr, 
                    sizeof(server_addr))) < 0 ) {
        server_error(strerror(errno), server_socket, l);
    }

    printf("Binding: \t\tSuccess\n");

    /**
     * Listen on the server socket for connections
     */
    if ( (listen(server_socket, 10)) < 0) {
        server_error(strerror(errno), server_socket, l);
    }

    printf("Listen: \t\tSuccess\n\n");

    printf("Server is now waiting for clients to connect ...\n\n");

    /**
     * Create commandline, such that we can do simple commands on the server.
     */
    if ( (pthread_create(&pthread_id, NULL, cmdline, NULL)) < 0 ){
        server_error(strerror(errno), server_socket, l);
    }

    while (1) {
        client_length = sizeof(client_addr);

        /**
         * If a client connects, we observe it here.
         */
        if ( (client_socket = accept(server_socket, 
                        (struct sockaddr *) &client_addr,
                        &client_length)) < 0) {
            server_error(strerror(errno), server_socket, l);
        }

        /**
         * Save some information about the client, which we will
         * later use to identify him with.
         */
        char *temp = (char *) inet_ntoa(client_addr.sin_addr);
        char *addr = (char *) malloc( sizeof(char)*(strlen(temp)+1) );
        if (addr == NULL) {
            server_error(strerror(errno), server_socket, l);
            break;
        }
        memset(addr, '\0', strlen(temp)+1);
        memcpy(addr, temp, strlen(temp));	

        ws_client *n = client_new(client_socket, addr);

        /**
         * Create client thread, which will take care of handshake and all
         * communication with the client.
         */
        if ( (pthread_create(&pthread_id, NULL, handleClient, 
                        (void *) n)) < 0 ){
            server_error(strerror(errno), server_socket, l);
        }
    }

    list_free(l);
    l = NULL;
    close(server_socket);
    return EXIT_SUCCESS;
}
