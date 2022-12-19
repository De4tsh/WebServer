#ifndef __HTTPCONNEC__H
#define __HTTPCONNEC__H

#include <sys/epoll.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>
#include <string.h>


class http_conn
{

    public:

        // 所有的 socket 事件都被注册到一个 epoll 事件中
        static int m_epollfd;

        // 当前在线用户数量
        static int m_user_count;

        // 读缓冲区的大小
        static const int READ_BUFFER_SIZE = 2048;

        // 写缓冲区的大小
        static const int WRITE_BUFFER_SIZE = 2048;

        // 文件名程字符串空间
        static const int FILENAME_LEN = 200;


        
        // HTTP 请求方法
        enum METHOD
        {
            GET = 0,
            POST,
            HEAD,
            PUT,
            DELETE,
            TRACE,
            OPTIONS,
            CONNECT
        };


        // HTTP 主状态机状态
        enum CHECK_STATE
        {
            CHECK_STATE_REQUESTLINE = 0,    // 解析 请求首行 GET /index.html HTTP/1.1
            CHECK_STATE_HEADER,             // 解析 HTTP 头
            CHECK_STATE_CONTENT            // 解析 HTTP body
        };

        // 从状态机的三种状态
        enum LINE_STATUS
        {
            LINE_OK = 0,                    // 读取到一个完整行
            LINE_BAD,                       // 行出错
            LINE_OPEN                       // 行数据不完整
        };


        // HTTP 响应结果（返回状态码）
        enum HTTP_CODE
        {
            NO_REQUEST,                     // 请求不完整，需要继续读取客户数据
            GET_REQUEST,                    // 获得了一个完整的 GET 请求
            BAD_REQUEST,                    // 客户端请求语法错误
            NO_RESOURCE,                    // 请求资源服务器不存在
            FORBIDDEN_REQUEST,              // 客户端对请求资源没有访问权限
            FILE_REQUEST,                   // 文件请求成功
            INTERNAL_ERROR,                 // 服务器内部错误
            CLOSED_CONNECTION               // 客户端关闭连接
        };


        http_conn(){}
        ~http_conn(){}


        // 处理客户端的请求
        // 解析 HTTP 请求报文
        void process();

        // 初始化新接收的连接
        void init(int sockfd,const sockaddr_in & addr);

        // 关闭连接
        void close_conn();

        // 非阻塞读
        bool read();

        // 非阻塞写
        bool write();


    private:

        // 当前 HTTP 连接的 socket
        int m_sockfd;

        // 通信的socket地址
        sockaddr_in m_address;

        // 读缓冲区
        char m_read_buf[READ_BUFFER_SIZE];

        // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
        int m_read_idx;

        // 当前正在分析的字符在读缓冲区的位置
        int m_checked_index;

        // 当前正在解析行的起始的位置
        int m_start_line;

        // 从此处开始考虑是否要归为一个结构体

        // 请求目标文件的文件名
        char *m_url;
        // 协议版本，只支持 HTTP1.1
        char *m_version;
        // 请求方法
        METHOD m_method;
        // 主机名
        char *m_host;
        // http 请求是否要保持连接
        bool m_linger;
        // Content_length
        long m_content_length;
        // 要访问资源路径名称
        char m_real_file[FILENAME_LEN];



        // 写缓冲区
        char m_write_buf[WRITE_BUFFER_SIZE];  
        // 写缓冲区中待发送的字节数
        int m_write_idx;         
        // 客户请求的目标文件被mmap到内存中的起始位置              
        char* m_file_address;                   
        // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
        struct stat m_file_stat;                
        // 我们将采用writev来执行写操作，所以定义下面两个成员
        struct iovec m_iv[2];                   
        // m_iv_count表示被写内存块的数量
        int m_iv_count;

        // 将要发送的数据的字节数
        int bytes_to_send;            
        // 已经发送的字节数
        int bytes_have_send;            

	// 这一组函数被process_write调用以填充HTTP应答。
   	 void unmap();
   	 bool add_response( const char* format, ... );
   	 bool add_content( const char* content );
   	 bool add_content_type();
   	 bool add_status_line( int status, const char* title );
   	 bool add_headers( int content_length );
   	 bool add_content_length( int content_length );
   	 bool add_linger();
   	 bool add_blank_line();



        // 主状态机当前所处的状态
        CHECK_STATE m_check_state;

        // 初始化连接所需要的其余的信息
        void init();


        // 解析 HTTP 请求
        HTTP_CODE process_read();                   // 解析 HTTP 请求
        HTTP_CODE parse_request_line(char *text);   // 解析 HTTP 首行
        HTTP_CODE parse_headers(char *text);        // 解析 HTTP 头
        HTTP_CODE parse_content(char *text);        // 解析 HTTP 体
	
	// 填充 HTTP 应答
	bool process_write(HTTP_CODE ret);

        // 按照 /r/n 解析行
        LINE_STATUS parse_line();

        // 获取一行数据
        inline char *get_line() {return m_read_buf + m_start_line; };
        

        HTTP_CODE do_request();

};




#endif
