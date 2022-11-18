#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";


// 初始化静态变量
int http_conn::m_epollfd = -1;

int http_conn::m_user_count = 0;


// 网站根目录
// 要请求资源的目录
const char* doc_root = "/home/de4tsh/Desktop";



// 设置文件描述符非阻塞
int setnonblocking( int fd)
{
    int old_flag = fcntl(fd,F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;

    fcntl(fd,F_SETFL,new_flag);

    return old_flag;


}



// 添加需要监听的文件描述符到 epoll
void addfd(int epollfd,int fd,bool one_shot)
{

    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP; // 默认为 LT 模式 若要设置为 ET 自己指定
    /*
        EPOLLRDHUP 事件

        在使用epoll时，对端正常断开连接（调用close())，在服务器端会触发一个epoll事件。在低于2.6.17版本的内核中，这个epoll事件一般是EPOLLIN，即0x1表示连接可读。

        连接池检测到某个连接发生EPOLLIN事件且没有错误后，会认为有请求到来，将连接交给上层进行处理。这样以来，上层尝试在对端已经close()的连接上读取请求，只能读到EOF，会认为发生异常，报告一个错误。

        因此在使用2.6.17之前版本内核的系统中，我们无法依赖封装epoll的底层连接库来实现对对端关闭连接事件的检测，只能通过上层读取数据进行区分处理。

        因此在使用2.6.7版本内核中增加EPOLLRDHUP事件，表示对端断开连接
    
    */

    /*
        即使使用ET模式，一个socket上的同一事件还是可能被触发多次
        （将TCP缓冲区设置的小一点，发送一个较大的数据，接受的数据会多次填满缓冲区，导致会被触发多次），
        当某个线程在读取完某个socket的数据后开始处理这些数据，但在处理这些数据的时候这个socket上又有数据来了，
        此时另外一个线程被唤醒来读取这些新的数据，于是出现了两个线程同时操作一个socket的局面。
        但我们期望一个socket在任意时刻值被一个socket处理。此时可以用epoll的EPOLLONESHOT事件来解决
    
    */

    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);

    // 设置文件描述符非阻塞
    setnonblocking(fd);

}



// 从 epoll 中删除监听的文件描述符
void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

// 修改文件描述符
// 记得重置 EPOLLONESHOT 事件，确保下一次可读时，EPOLLIN 事件能被触发
void modfd(int epollfd,int fd,int ev)
{
    epoll_event event;

    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);

}


// 初始化新接收的连接
void http_conn::init(int sockfd,const sockaddr_in & addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse));

    // 添加到 epoll
    addfd(m_epollfd,m_sockfd,true);
    m_user_count++; // 总用户数 +1

    init();
}


// 关闭连接
void http_conn::close_conn()
{

    if (m_sockfd != -1)
    {
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;

    }
}

void http_conn::init()
{
    // 初始化状态为解析请求首行
    m_check_state = CHECK_STATE_REQUESTLINE; 
    // 开始从第 0 个字符开始解析
    m_checked_index = 0;   
    // 开始从第 0 行开始解析
    m_start_line = 0;
    // 初始接收数据也是从 0 开始
    m_read_idx = 0;
    // 默认不保持链接  Connection : keep-alive保持连接
    m_linger = false;       

    // 默认为 GET 请求
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_index = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    bytes_to_send = 0;
    bytes_have_send = 0;

    bzero(m_read_buf,READ_BUFFER_SIZE);
    bzero(m_write_buf,WRITE_BUFFER_SIZE);
    bzero(m_real_file,FILENAME_LEN);
}


// 非阻塞读
// 循环读取客户端数据，直到无数据可读或对方关闭连接
bool http_conn::read()
{

    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        // 读缓冲区已经满了
        return false;
    }

    // 读取到的字节
    int bytes_read = 0;

    while (true)
    {
        bytes_read = recv(m_sockfd,m_read_buf + m_read_idx,READ_BUFFER_SIZE - m_read_idx,0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 没有数据了 读完了
                break;
            }

            return false;
        }
        else if (bytes_read == 0)
        {
            // 对端关闭
            return false;
        }

        m_read_idx += bytes_read;
    }

    printf("读取到的数据为：%s\n",m_read_buf);
    return true;
}

// 解析 HTTP 请求
// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read()                   // 解析 HTTP 请求
{

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST; 

    char *text = 0;

    while ( ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK)) 
    {
        // 解析到了请求体的完整数据
        // 或解析到了一行完整数据
        // 则进入 while 循环

        text = get_line();

        m_start_line = m_checked_index;
        printf("get 1 http line : %s\n",text);

        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                {
                    // 语法错误直接退出丢弃
                    return BAD_REQUEST;
                }
                break;
            }

            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                {
                    // 语法错误直接退出丢弃
                    return BAD_REQUEST;
                }
                else if (ret == GET_REQUEST)
                {
                    // 进一步解析具体的请求信息
                    return do_request();
                }
                break;
            }

            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                {
                    // 此处存疑，GET 请求没有请求体
                    return do_request();
                }

                // content 数据不完整
                line_status = LINE_OPEN;
                break;

            }

            default:
            {
                return INTERNAL_ERROR;
            }
        }
        
    }  

    return NO_REQUEST;

}

// 解析 HTTP 请求行
// 获得 请求方法、目标URL、HTTP版本
// GET /index.html HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)   // 解析 HTTP 首行
{

    // GET /index.html HTTP/1.1
    m_url = strpbrk(text," \t");

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';

    char *method = text;
    // 忽略大小写的比较
    if ( strcasecmp(method,"GET") == 0)
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }
    
    // /index.html HTTP/1.1
    m_version = strpbrk(m_url," \t");
    if (!m_version)
    {
        // 若没有找到 m_version 字段
        return BAD_REQUEST;
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }

    // 例如 http://xxxx.xxxx.xxxx.xx:xxxx/index.html
    if (strncasecmp(m_url,"http://",7) == 0)
    {
        m_url += 7; // xxxx.xxxx.xxxx.xx:xxxx/index.html
        m_url = strchr(m_url,'/'); // /index.html
    }

    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }

    // 至此 http 包的请求首行解析完毕
    // 将状态机推至检查请求头
    m_check_state = CHECK_STATE_HEADER;
    
    return NO_REQUEST;

}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)        // 解析 HTTP 头
{

    if (text[0] == '\0')
    {
        // 若请求首行后的第一个字符为 \0 就说明没有请求头
        // 直接切换状态机为 CHECK_STATE_CONTENT 即可
        
        //// 疑惑 - 没有请求头是否还会有 body
        // 若存在 body
        if ( m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        // 若既不存在请求头也不存在body
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Connection:",11) == 0)
    {
        // 处理 Connection 头部字段
        // Connection:keep-alive
        text += 11;
        text += strspn(text," \t");

        if (strcasecmp(text,"keep-alive") == 0)
        {
            m_linger = true;
        }
       
    }
    else if (strncasecmp(text,"Content-Length:",15) == 0)
    {
        // 处理 Content-Length 头部字段
        text += 15;
        text += strspn(text," \t");
        m_content_length = atol(text);
    }   
    else if (strncasecmp(text,"Host:",5) == 0)
    {
        // 处理 Host 头部字段
        text += 5;
        text += strspn(text," \t");
        m_host = text;
    }
    else
    {
        printf("unkonwn headers %s\n",text);
    }

    return NO_REQUEST;

}
http_conn::HTTP_CODE http_conn::parse_content(char *text)        // 解析 HTTP 体
{

    if (m_read_idx >= (m_content_length + m_checked_index))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;


}


// 按照 /r/n 解析行
http_conn::LINE_STATUS http_conn::parse_line()
{

    char temp;

    for( ; m_checked_index < m_read_idx; ++m_checked_index)
    {
        temp = m_read_buf[m_checked_index];
        if (temp == '\r')
        {
            if ((m_checked_index + 1) == m_read_idx)
            {
                // 读到当前读入缓冲区的末尾没有读到与 \r 匹配的 \n 故说明数据没读完
                // 返回 LINE_OPEN 状态
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_index + 1] == '\n')
            {
                // 截断字符串方便读取
                m_read_buf[m_checked_index++] = '\0'; // \r 变为 0
                m_read_buf[m_checked_index++] = '\0'; // \n 变为 0
                
                return LINE_OK;

            }

            return LINE_BAD;
        }
        // 为了应对第一种情况中先读取到了一个 '\r' 
        // 然后又重新从 '\r' 下一位读取的情况
        else if ( temp == '\n') 
        {
            if ((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r'))
            {
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';

                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{

    // /Desktop
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) 
    {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) 
    {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) 
    {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;

}


// 对内存映射区执行munmap操作
void http_conn::unmap() 
{
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    
    if ( bytes_to_send == 0 ) 
    {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) 
    {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) 
            {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    }

    
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) 
{
    if( m_write_idx >= WRITE_BUFFER_SIZE ) 
    {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) 
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) 
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) 
{
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) 
{
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) 
{
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) 
            {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) 
            {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) 
            {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) 
            {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}



// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
// 任务放入队列中由线程池到队列中取任务，取到了以后由工作线程调用 process 解析 HTTP 请求
void http_conn::process()
{

    // 解析 HTTP 请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        // 请求不完整，需要继续读取客户数据
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) 
    {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}

