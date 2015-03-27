// #include "nucleus.h"
// #include "lwip/sockets.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <errno.h>
#include <semaphore.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "tcp_server.h"

// #define LOCK(lock)
// #define UNLOCK(lock)

#if 1
    #define app_debug       printf
#else
    #define app_debug( format, arg... )
#endif

inline void HeadBuffer_init(HeadBuffer *buf, unsigned char *data, size_t size)
{
    memset(buf,0,sizeof(*buf));
    buf->data = data;
    buf->size = size;
}

inline size_t HeadBuffer_freesize(HeadBuffer *buf)
{
    return buf->size-buf->data_size;
}

inline void HeadBuffer_consume(HeadBuffer *buf, size_t len)
{
    assert(len<=buf->data_size);
    size_t left = len-buf->data_size;
    if (left>0) {
        memcpy(buf->data,buf->data+len,left);
    }
    buf->data_size = left;
}

static int client_init(Client *c, int newsock, TcpServer *server)
{
    memset(c,0,sizeof(*c));
    c->sock = newsock;
    c->server = server;
    HeadBuffer_init(&c->read_head_buf,c->read_data,CLIENT_RECV_BUF_SIZE);
    HeadBuffer_init(&c->write_head_buf,c->send_data,CLIENT_SEND_BUF_SIZE);

    // 将客户端加入select
    FD_SET(c->sock,&server->readfs);
    FD_SET(c->sock,&server->exceptfs);

    if (server->hooks && server->hooks->client_init) {
        server->hooks->client_init(c);
    }
    return 0;
}

int client_send(Client *c, void *data, int len)
{
    if (data==NULL || len==0) {
        return -1;
    }
    HeadBuffer *buf = &c->write_head_buf;
    // lock
    size_t freesize = HeadBuffer_freesize(buf);
    if (freesize<len) {
        app_debug("data size is bigger than buffer size\n");
        // unlock
        return -2;
    }
    memcpy(buf->data+buf->data_size,data,len);
    buf->data_size+=len;

    // trigger the write event
    c->is_writing = 1;
    FD_SET(c->sock,&c->server->writefs);
    //unlock

    return 0;
}

static void client_real_write(Client *c)
{
    HeadBuffer *buf = &c->write_head_buf;

    TcpServer *server = c->server;

    if (server->hooks && server->hooks->client_on_write) {
        server->hooks->client_on_write(c);
    }

    //lock
    size_t data_size = buf->data_size;

    if (data_size == 0) {
        // 没有发送数据，取消发送事件
        FD_CLR(c->sock,&c->server->writefs);
        c->is_writing = 0;
        //unlock
        return;
    }

    int len = send(c->sock,buf->data,buf->data_size,0);
    if (len>0) {
        HeadBuffer_consume(buf,len);
    }
    //unlock
}

void client_close(Client *c)
{
    TcpServer * server = c->server;

    if (server->hooks && server->hooks->pre_client_close) {
        server->hooks->pre_client_close(c);
    }

    FD_CLR(c->sock,&server->readfs);
    FD_CLR(c->sock,&server->writefs);
    FD_CLR(c->sock,&server->exceptfs);
    close(c->sock);

    if (server->hooks && server->hooks->post_client_close) {
        server->hooks->post_client_close(c);
    }

    c->sock = 0; // free the slots
}

static void client_timeout(Client *c)
{

    TcpServer * server = c->server;
    if (server->hooks && server->hooks->client_timeout) {
        server->hooks->client_timeout(c);
    }
}


static int client_recv(Client *c)
{
    HeadBuffer *buf = &c->read_head_buf;
    size_t freesize = HeadBuffer_freesize(buf);
    /*app_debug("read buf freesize:%d\n",freesize);*/
    int len = recv(c->sock,buf->data+buf->data_size,freesize,0);
    if (len<=0) {
        app_debug("client close %d\n",c->sock);
        return -1;
    }
    app_debug("recv len:%d\n",len);
    buf->data_size+=len;

    app_debug("\n");
    return 0;
}

static void client_process(Client *c)
{
    // 处理协议
    // HeadBuffer *write_buf = &c->write_head_buf;

    // echo
    /*client_send(c,read_buf->data,read_buf->data_size);*/

    TcpServer * server = c->server;
    int consume_len=0;
    if (server->hooks && server->hooks->client_process) {
        consume_len = server->hooks->client_process(c);
    }

    if (consume_len>0) {
        HeadBuffer *read_buf = &c->read_head_buf;
        HeadBuffer_consume(read_buf,consume_len);
    }
}

int server_init(TcpServer *server, unsigned short port, int select_timeout, struct ServerHooks *hooks)
{
    if (server==NULL) {
        return -1;
    }
    memset(server,0,sizeof(*server));
    server->port = port;
    server->select_timeout = select_timeout;
    server->hooks = hooks;

    if (server->hooks && server->hooks->post_init) {
        server->hooks->post_init(server);
    }
    return 0;
}

int server_create_client(TcpServer *server, int newsock)
{
    int i;
    for (i=0; i<MAX_CLIENT_CNT; i++) {
        Client *c = &server->clients[i];
        if (c->sock==0) {
            app_debug("add client, sock:%d index:%d\n",newsock,i);
            return client_init(c,newsock,server);
        }
    }
    app_debug("There are too many clients\n");
    return -1;
}

static void set_nonblock(int sock)
{
    // int val = 1;
    // ioctlsocket(sock,FIONBIO,&val);

    int val = fcntl(sock,F_GETFL,0);
    fcntl(sock,F_SETFL,val | O_NONBLOCK);
}

void server_run(TcpServer *server)
{
    int i;
    int listen_sock,newsock;
    int status;
    struct sockaddr_in servaddr;
    struct sockaddr_in client_addr;
    unsigned int client_len = sizeof(client_addr);
    int maxsocket;
    struct timeval timeout;

    listen_sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(listen_sock < 0)
    {
        app_debug("create socket fail\n");
        assert(0);
    }
    server->listen_sock = listen_sock;

	int reuseAddr = 1;
	status = setsockopt(listen_sock,SOL_SOCKET,SO_REUSEADDR,(char*)&reuseAddr,sizeof(reuseAddr));
	if(status != 0)
	{
		app_debug("start_client_listen# set SO_REUSEADDR error\n");
        return;
	}

    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(server->port);
    //servaddr.sin_addr.s_addr = 0; //作为服务器端， IP地址置零就可以了！
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    status = bind(listen_sock,(struct sockaddr *)&servaddr, sizeof(servaddr));
    if(status != 0)
    {
        app_debug("socket bind fail\n");
        assert(0);
    }

    status = listen(listen_sock, 20);
    if(status != 0)
    {
        app_debug("socket listen fail\n");
        assert(0);
    }
    else
    {
        status = 0;
    }

    if (status != 0)
    {
        app_debug("listen socket error!\n");
        return;
    }

    if (server->hooks && server->hooks->pre_run) {
        server->hooks->pre_run(server);
    }

    maxsocket = listen_sock;

    FD_ZERO(&server->readfs);
    FD_ZERO(&server->writefs);
    FD_ZERO(&server->exceptfs);
    //设置select 在readfs中， 要监听的套接字。
    FD_SET(listen_sock,&server->readfs);

    fd_set r_cache,w_cache,e_cache;

    while (1)
    {
        r_cache = server->readfs;
        w_cache = server->writefs;
        e_cache = server->exceptfs;
        timeout.tv_sec = server->select_timeout; //设置select的超时时间；
        timeout.tv_usec = 0;

        status = select(maxsocket+1, &r_cache, &w_cache, &e_cache, &timeout);

        /*status = select(maxsocket+1, &r_cache, NULL, NULL, NULL);*/
        app_debug("select status:%d\n",status);
        if (status>0) {
            // select fds
            if (FD_ISSET(listen_sock,&r_cache)) {
                newsock = accept(listen_sock,(struct sockaddr*)&client_addr,&client_len);
                if (newsock>0) {
                    //make sock be non-blocking
                    set_nonblock(newsock);

                    if (newsock>maxsocket) {
                        maxsocket = newsock;
                    }
                    //create new client
                    if (server_create_client(server,newsock)!=0) {
                        close(newsock);
                    }
                }
            }
            for (i=0; i<MAX_CLIENT_CNT; i++) {
                Client *c = &server->clients[i];
                if (c->sock==0) {
                    continue;
                }
                // read event
                if (FD_ISSET(c->sock,&r_cache)) {
                    status = client_recv(c);
                    if (status==0) {
                        client_process(c);
                    }
                    else {
                        /*app_debug("socket recv failed, sock:%d\n",c->sock);*/
                        client_close(c);
                        continue;
                    }
                }

                // write event
                if (FD_ISSET(c->sock,&w_cache)) {
                    client_real_write(c); // trigger sending
                }

                // except event
                if (FD_ISSET(c->sock,&e_cache)) {
                    app_debug("socket error sock:%d\n",c->sock);
                    client_close(c);
                }
            }
        }
        else if (status==0) {
            // select timeout
            /*app_debug("select timeout\n");*/
            for (i=0; i<MAX_CLIENT_CNT; i++) {
                Client *c = &server->clients[i];
                if (c->sock==0) {
                    continue;
                }

                client_timeout(c);
            }
        }
        else {
            // select failed
            app_debug("NU_Select failed ret=%d \n",status);
            assert(0);
        }
    }
}


