
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD          65535 // 最大文件描述符个数
#define MAX_EVENT_NUM   10000 // 一次监听的最大事件数量

// 网络通信中若一端已经断开链接了，本端却还在往过写数据就会产生 SIGPIPE 信号
// 信号处理函数(添加信号捕捉)
/// @@para sig 信号
/// @@para handler 信号处理函数
void addsig(int sig,void(handler)(int))
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));

    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);

    sigaction(sig,&sa,NULL);
}

// 添加文件描述符到 epoll
extern void addfd(int epollfd,int fd,bool one_shot);
// 从 epoll 中删除文件描述符
extern void removefd(int epollfd,int fd);
// 修改文件描述符
extern void modfd(int epollfd,int fd,int ev);

int main(int argc,char ** argv)
{

    if (argc <= 1)
    {
        fprintf(stderr,"Usage.. ./%s port_num\n",basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对 SIGPIPE 信号处理
    addsig(SIGPIPE,SIG_IGN);

    // 创建线程池，初始化线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch(...)
    {
        exit(-1);
    }

    //  创建一个数组用于保存所有的客户端信息  
    http_conn *users = new http_conn[MAX_FD];

    // 创建监听套接字
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    if (listenfd < 0)
    {
	perror("socket()");
	exit(-1);
    }

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse));

    // 绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);


    bind(listenfd,(struct sockaddr *)&address,sizeof(address));
   // if ()
   // {
   //
   // }

    // 监听
    listen(listenfd,5);

    // 创建 epoll 对象、事件数组、添加
    epoll_event events[MAX_EVENT_NUM];

    int epollfd = epoll_create(200);

    // 将监听的文件描述符加入 epoll 中
    addfd(epollfd,listenfd,false);

    http_conn::m_epollfd = epollfd;


    while (true)
    {

        int num = epoll_wait(epollfd,events,MAX_EVENT_NUM - 1,-1);
        if (num < 0 && (errno != EINTR))
        {
            // EINTR 是被信号打断，属于正常情况，再循环阻塞即可
            perror("epoll()");
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < num; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {
                // 有客户端连接进来

                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);

                // 目前可接受的连接数已经满了
                if (http_conn::m_user_count >= MAX_FD)
                {
                    // 向客户端返回信息，表示服务器正忙

                    close(connfd);
                    continue;
                }

                // 将新的客户的数据初始化，放入数组中
                users[connfd].init(connfd,client_address);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 对方异常断开或错误等事件发生,关闭连接
                users[sockfd].close_conn();


            }
            else if (events[i].events & EPOLLIN)
            {
                // 有读的事件发生
                if (users[sockfd].read())
                {
                    // 一次性将数据读完
                    // 数组首地址 users + 该fd的偏移 sockfd 定位到该对象在数组中的起始地址
                    pool->append(users + sockfd);
                }
                else
                {
                    // 读失败
                    users[sockfd].close_conn(); 
                }
            }else if (events[i].events & EPOLLOUT)
            {
                // 有写的事件发生
                if ( !users[sockfd].write() ) 
                {
                    // 一次性写完所有事件
                    // 若写失败
                    users[sockfd].close_conn();
                }
            }
        }
        

    }


    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;

    return 0;
}
