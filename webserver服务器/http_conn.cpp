#include"http_conn.h"
using namespace std;

// 定义HTTP响应的一些状态信息 
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Server Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

int http_conn::m_epollfd = -1;//所以socket上的事件都被注册到同一个socket
int http_conn::m_user_count = 0;//用来统计用户的数量

//网站的根目录
const char* doc_root = "/home/jhb/webserver服务器/resources";

//设置文件描述符非阻塞
int setnonblockin(int fd){
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);//设置文件为非阻塞
    return old_flag;  //返回 old_flag 是为了在需要时恢复文件描述符的原始状态
}

//向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;//先用水平触发LT ，EPOLLET边沿触发,EPOLLRDHUP挂起
    //但监听的不能是ET触发。需要处理
    if(one_shot) {
        event.events | EPOLLONESHOT;
        //添加EPOLLONESHOT事件，当一个线程在操作某个socket时，其他线程不会操作该socket，用完后要重置
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符非阻塞
    setnonblockin(fd);
}
//从epoll删除文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改文件描述符,重置socket上 EPOLLONESHOT事件，确保下一次可读时，EPOLLONESHOT事件可以被触发。
extern void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_MOD,fd, &event);  // 将epollfd集合中fd修改
}

//初始化新接收的连接
void http_conn:: init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;

    //设置端口复用，客户端端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //添加到epoll对象中，设置文件描述符非阻塞
    addfd(m_epollfd, sockfd, true);
    m_user_count++;//总用户数+1

    init();
}

//其他连接初始化
void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;//初始化状态为解析请求首行
    m_checked_index = 0;
    m_start_line = 0;
    m_read_idx = 0;

    //请求头初始化
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_host = 0;
    m_linger = false;
    m_content_length = 0;
    m_write_idx = 0;
    m_cgi = 0;
    //初始化读缓冲区
    bzero(m_read_buff, READ_BUFFRE_SIZE);
    bzero(m_write_buf, WRITER_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

//关闭连接
void http_conn::colse_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);//从集合中删除本次fd，并关闭fd
        m_sockfd = -1;//没有用
        m_user_count--;//关闭一个连接，客户总数量-1
    }
}

//一次性读完，保存到m_read_buff
bool http_conn::read() {

    if(m_read_idx >= READ_BUFFRE_SIZE) {
        return false;
    }

    //读取到的字节
    int bytes_read = 0;
    while(1) {
        //保存到buff中，保存位置从 读缓存首地址+最后一字节的下一个位置 开始保存，bytes_read是返回的字节数，非阻塞读
        bytes_read = recv(m_sockfd, m_read_buff + m_read_idx, READ_BUFFRE_SIZE - m_read_idx, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                //当前没有数据就退出read函数
                break;
            } 
            return false;
        } else if(bytes_read == 0) {
            //对方关闭
            return false;
        }
        //更新m_read_idx的读取字节数
        m_read_idx += bytes_read;
    }
    //printf("读取到的数据：\n%s\n", m_read_buff);
    return true;
}

//解析HTTP请求,主状态机解析请求
http_conn::HTTP_CODE http_conn::process_read(){
    //初始状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char* text = 0;
    /*为什么这么判断？
    在GET请求中，每一行都是\r\n结尾，所以仅用从状态机的line_status = pares_line() == LINE_OK就可以
    但在POST中，GET是没有消息体的，POST有消息体，且消息体末尾没有任何字符，应该使用主状态机作为循环条件
    为什么要&&Line_status == LINE_OK？
    因为解析为消息体后，报文的完整解析完成了，此时主状态机还是CHECK_STATE_CONTENT，还会进入该循环
    因此要在完成解析消息体后，把line_status变成LINE_OPEN，跳出循环*/
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
     || ((line_status = pares_line()) == LINE_OK)) {
        //解析到了一行完整的数据，或者解析到了请求体，也是完整的数据
        
        //获取一行数据
        text = get_line();

        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_index;//更新下一次正在解析的起始位置是这一次正在分析的位置
        printf("get 1 http line: %s\n", text);
        //根据不同的情况，处理
        switch ((m_check_state))
        {
            //init中设置了m_check_state为CHECK_STATE_REQUESTLINE
            case CHECK_STATE_REQUESTLINE:
            {
                ret = pares_request_line(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;//表示客户请求语法错误,结束
                }
                break;
            }

            case CHECK_STATE_HEADER:
            {
                ret = pares_headers(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {//表示获得了一个完成的客户请求,开始解析数据
                    return do_request();//解析具体的内容
                }
                break;
            }

            case CHECK_STATE_CONTENT:
            {
                //解析消息体
                ret = pares_content(text);
                if(ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;//行数据尚且不完整
                break;
            }
            default:
            {
                return INTERNAL_ERROR;//服务器内部错误
            }
        }
     }
    return NO_REQUEST;//请求不完整，需要继续读取客户数据
}

// //解析http请求行，获得请求方法，目标url，http版本
// http_conn::HTTP_CODE http_conn::pares_request_line(char * text){
//     // GET\t/index.html  HTTP/1.1
//     //可以用正则表达式
//     m_url = strpbrk(text,"\t");//查找 text 中第一次出现制表符（空格）的位置。
//     // GET\0/index.html  HTTP/1.1
//     *m_url++ = '\0';

//     char* method = text;
//     if(strcasecmp(method, "GET") == 0) {
//         m_method = GET;
//     } else {
//         return BAD_REQUEST;
//     }
//     // GET\0/index.html\tHTTP/1.1
//     m_version = strpbrk(m_url, "\t");
//     if(!m_version) {
//         return BAD_REQUEST;
//     }
//     *m_version++ = '\0';
//     // GET\0/index.html\0HTTP/1.1
//     if(strcasecmp(m_version, "HTTP/1.1") != 0) {
//         return BAD_REQUEST;
//     }
//     // 有些是http://192/168/88:1000/index.html，所以找前7个字符
//     if(strncasecmp(m_url, "http://",7) == 0) {
//         m_url += 7;//http://192/168/88:1000/index.html
//         m_url = strchr(m_url, '/');// /index.html
//     }

//     if(!m_url || m_url[0] != '/') {
//         return BAD_REQUEST;
//     }

//     m_check_state = CHECK_STATE_HEADER;//主状态机检测状态转换成检测状态头
//     return NO_REQUEST;
// }

//解析http请求行，获得请求方法，目标url，http版本
http_conn::HTTP_CODE http_conn::pares_request_line(char * text){
    // GET /index.html  HTTP/1.1
    //请求方法 空格 URL 空格 协议版本 回车符 换行符
    //可以用正则表达式
    string request_line(text);
    regex request_line_pattern("^(\\S+)\\s+(\\S+)\\s+(HTTP/\\d\\.\\d)$");//定义正则表达式模型
    smatch matches;//保存结果

    if(regex_match(request_line, matches, request_line_pattern)) {
        string method = matches[1].str();
        string url = matches[2].str();
        string version = matches[3].str();

        if(strcasecmp(method.c_str(), "GET") == 0) {
            m_method = GET;
        } else if(strcasecmp(method.c_str(), "POST") == 0 ) {
            m_method = POST;
            m_cgi = 1;
        }
        else {
            return BAD_REQUEST;
        }

        // 检查URL是否以"http://"开头，如果是，则去掉
        if (url.compare(0, 7, "http://") == 0) {
            url = url.substr(7); // 去掉"http://"
        }
        // 释放先前分配的内存
        if (m_url != nullptr) {
            delete[] m_url;
        }
        // 分配足够的空间来存储URL，并复制字符串
        m_url = new char[url.length() + 1];
        strcpy(m_url, url.c_str());

        // 释放先前分配的内存
        if (m_version != nullptr) {
            delete[] m_version;
        }
        // 分配足够的空间来存储URL，并复制字符串
        m_version = new char[version.length() + 1];
        strcpy(m_version, version.c_str());
        
        if(strcasecmp(m_version, "HTTP/1.1") != 0) {
            return BAD_REQUEST;
        }
    }
    m_check_state = CHECK_STATE_HEADER;//主状态机检测状态转换成检测状态头
    return NO_REQUEST;
}
    //解析HTTP请求头
http_conn::HTTP_CODE http_conn::pares_headers(char * text){
    //遇到空行，表示头部字段解析完毕
    if(text[0] == '\0') {
        //如果HTTP请求有消息体，则需要读取内容字节的消息体
        if(m_content_length != 0){
            //说明请求头有消息，状态机转移到CHECK_STATE_CONTENT状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则就有了一个完整的HTTP请求，不需要解析
        return GET_REQUEST;
    } 

    //定义正则表达式
    //定义Connection头部字段的正则表达式模式，忽略大小写。
    regex connection_pattern(R"(^Connection:\s*(\S+))", regex::icase);
    regex content_length_pattern(R"(^Content-Length::\s*(\S+))", regex::icase);
    regex host_pattern(R"(^host::\s*(\S+))", regex::icase);

    cmatch matches;
    if(regex_match(text, matches, connection_pattern)) {
        if(strcasecmp(matches[1].str().c_str(), "keep-alive") == 0) {
            m_linger = true;
        }
    }
    if(regex_match(text, matches, content_length_pattern) ) {
        m_content_length = atol(matches[1].str().c_str());//将匹配到的Content-Length值转换为长整数，并赋值给m_content_length
    }
    if(regex_match(text, matches, host_pattern) ) {
        m_host = strdup(matches[1].str().c_str());//将匹配到的Host值复制到m_host
    } else {
        printf("unknow header %s\n", text);
    }
    return NO_REQUEST;
}
    //解析HTTP请求数据,没有解析，只是判断是否被完整读入
http_conn::HTTP_CODE http_conn::pares_content(char * text){
    if(m_read_idx >= (m_content_length + m_checked_index)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//从状态机，解析具体的一行，返回给主状态机 HTTP_CODE
http_conn::LINE_STATUS http_conn::pares_line(){
    char temp;
    //一个字符一个字符遍历到已经读入到读缓冲区的数据
    for( ;m_checked_index < m_read_idx; ++m_checked_index) {
        temp = m_read_buff[m_checked_index];
        //判断\r\n是否连起来，如果是就说明取到一行
        if( temp == '\r') {
            if((m_checked_index +1) == m_read_idx) {
                return LINE_OPEN;//说明一行不完整
            } else if(m_read_buff[m_checked_index + 1] == '\n') {
                //说明\r\n连在一起，读取到了一行
                m_read_buff[m_checked_index++] = '\0';
                m_read_buff[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;  //行出错
        } else if( temp == '\n') {
            //判断\n的前面一个是不是\r
            if((m_checked_index > 1) && (m_read_buff[m_checked_index - 1] == '\r')) {
                m_read_buff[m_checked_index-1] = '\0';
                m_read_buff[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//当得到一个完整正确的HTTP请求时，分析目标文件属性
//如果目标文件存在，对所有用户可读，且不是目录，
//则使用mmap映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    //"/home/nowcoder/webserver/resources";
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN-len-1);//在m_real_file末尾加入url
    //获取m_real_file文件的相关状态信息，-1失败，0成功
    if(stat(m_real_file, &m_file_stat) < 0) {//stat函数用于获取文件的属性信息，返回值小于0表示获取文件属性失败
        return NO_RESOURCE;
    }

    //判断访问权限
    if(!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    //判断是否是目录
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    //以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    //创建内存映射
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    
    return FILE_REQUEST;//文件请求,获取文件成功
}

//解除映射
void http_conn::unmap() {
    //检查文件地址是否为空，如果不为空则解除文件映射并将地址指针置空
    if(m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
//写HTTP响应
bool http_conn::write() {
    int temp = 0;//临时变量
    int bytes_have_send = 0;//已经发送的字节
    int bytes_to_send = m_write_idx;//将要发送的字节，写缓冲区待发送的字节数

    if(bytes_to_send == 0) {
        //说明发送字节为0，响应结束，初始化，重置套接字的EPOLLONESHOT事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1) {
        //分散写,将多个缓冲区的数据一次发送出去
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if(temp <= -1) {
            //TCP写缓存没有空间了，则等待下一轮EPOLLOUT事件，在此期间
            //服务器无法立即收到同一客户的下一个请求，但可以保证连接的完整性
            if(errno == EAGAIN) {//如果错误是 EAGAIN，表示 TCP 写缓冲区没有空间,等待下一次可写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            //解除映射，释放文件映射到内存的空间
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if(bytes_to_send <= bytes_have_send) {
            //发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            } else {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;//返回 false，表示连接关闭
            }
        }
    }
} 

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers( strlen(error_500_form));
            if(!add_content(error_500_form)) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers( strlen(error_400_form));
            if(!add_content(error_400_form)) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers( strlen(error_404_form));
            if(!add_content(error_404_form)) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers( strlen(error_403_form));
            if(!add_content(error_403_form)) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;

}

//填入状态行
bool http_conn::add_status_line(int status, const char* title){
    return add_response("%s %d %s\r\n","HTTP/1.1", status, title);
}

//填入响应头部
bool http_conn::add_headers(int content_length) {
    //添加content-length头部
    if(!add_response("Content-Length: %d\r\n", content_length)) return false;

    //添加content-type
    if(!add_response("Content-Type: %s\r\n", "text/html")) return false;

    //添加Connection
    if(!add_response("Connection: %s\r\n", (m_linger ? "keep-alive" : "close"))) return false;

    //添加空行
    if(!add_response("%s", "\r\n")) return false;

    return true;
}

//填入响应正文
bool http_conn::add_content(const char* content){
    return add_response("%s", content);
}

bool http_conn::add_response(const char* format,...) {

    //检测当前写入缓冲区的索引是否已经超过缓冲区的大小
    if(m_write_idx >= WRITER_BUFFER_SIZE) {
        return false;
    }
    //定义一个va_list 类型的变量，用来访问可变参数列表
    va_list arg_list;

    //初始化arg_list，指向第一个参数
    va_start(arg_list, format);

    //使用vsnprintf函数格式化写入到缓冲区m_write_buf中,返回写入的字节数
    //-1通常是为了确保缓冲区中始终保留一个未使用的字节，这个字节用于存储字符串的结束符\0
    int len = vsnprintf(m_write_buf + m_write_idx, WRITER_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    //检测写入的字符是否超过了缓冲区的剩余空间
    if(len >= (WRITER_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    //更新缓冲区的写入位置
    m_write_idx += len;

    //结束可变参数
    va_end(arg_list);

    return true;
}


//由线程池中的工资线程调用，处理HTTP请求的入口函数
void http_conn::process() {
    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST) {
        //请求不完整，需要继续读取客户数据
        modfd(m_epollfd, m_sockfd, EPOLLIN);//重新读取客户数据
        return;
    }

    //生成响应
    bool writer_ret = process_write(read_ret);
    if(!writer_ret) {
        colse_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}