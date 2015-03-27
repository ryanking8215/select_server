#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tcp_server.h"

typedef struct EchoClient {
    int is_login;
    int non_login_times;
    int non_writeable_times;
} EchoClient;

static int echo_process(Client *c)
{
    // echo
    EchoClient *ec = (EchoClient *)c->user_data;
    ec->is_login = 1;

    HeadBuffer *read_buf = &c->read_head_buf;
    client_send(c,read_buf->data,read_buf->data_size);
    return read_buf->data_size;
}

static void echo_timeout(Client *c)
{
    EchoClient *ec = (EchoClient *)c->user_data;

    if (!ec->is_login) {
        ec->non_login_times++;
        if (ec->non_login_times>2) {
            printf("client sock:%d login timeout,close it\n",c->sock);
            client_close(c);
            return;
        }
    }

    if (c->is_writing) {
        ec->non_writeable_times++;
        if (ec->non_writeable_times>5) {
            client_close(c);
            return;
        }
    }
}

static void echo_client_init(Client *c)
{
    if (c==NULL)  return;
    EchoClient *ec = (EchoClient *)malloc(sizeof(*ec));
    memset(ec,0,sizeof(*ec));
    c->user_data = (void *)ec;
}

static void echo_client_close(Client *c)
{
    if (c==NULL)  return;
    free(c->user_data);
}

static void echo_on_write(Client *c)
{
    if (c==NULL) return;
    EchoClient *ec = (EchoClient *)malloc(sizeof(*ec));
    ec->non_writeable_times=0;
}

struct ServerHooks EchoServerHook = {
    .client_init = echo_client_init,
    .client_process = echo_process,
    .client_timeout = echo_timeout,
    .client_on_write = echo_on_write,
    .post_client_close = echo_client_close,
};

int main(void)
{
    TcpServer server;
    server_init(&server,5198,5,&EchoServerHook);
    server_run(&server);
    return 0;
}
