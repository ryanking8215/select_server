#ifndef _HOWELL_SERVER_H
#define _HOWELL_SERVER_H

#include <sys/select.h>

#define MAX_CLIENT_CNT 2
#define CLIENT_RECV_BUF_SIZE 4*1024
#define CLIENT_SEND_BUF_SIZE 4*1024

/* head buffer, 处理完后，将剩余数据copy至开头 */
typedef struct HeadBuffer {
    size_t size;
    size_t data_size;
    unsigned char * data;
} HeadBuffer;

typedef struct TcpServer TcpServer;

/* 客户端 */
typedef struct Client {
    int sock;

    // 读写buffer
    unsigned char read_data[CLIENT_RECV_BUF_SIZE];
    HeadBuffer read_head_buf;
    unsigned char send_data[CLIENT_SEND_BUF_SIZE];
    HeadBuffer write_head_buf;

    int non_writeable_times; // 不可写次数
    int is_writing; // 是否在发送数据
    TcpServer *server;

    void * user_data; // 用户数据
} Client;

/* 服务器 */
typedef struct TcpServer {
    unsigned short port;
    int select_timeout;

    int listen_sock;
    fd_set readfs;
    fd_set writefs;
    fd_set exceptfs;

    Client clients[MAX_CLIENT_CNT];
    struct ServerHooks * hooks;

    void * user_data; // 用户数据
} TcpServer;

// server执行钩子
struct ServerHooks {
    void (* post_init)(TcpServer *server);
    void (* pre_run)(TcpServer *server);

    void (* client_init)(Client *c);
    // 返回消耗的read_buf.data的size
    int (* client_process)(Client *c);
    void (* client_timeout)(Client *c);
    void (* pre_client_close)(Client *c);
    void (* post_client_close)(Client *c);
};

// api for server
int server_init(TcpServer *server, unsigned short port, int select_timeout, struct ServerHooks *hooks);
void server_run(TcpServer *server);

// api for client
int client_send(Client *c, void *data, int len);
void client_close(Client *c);


#endif
