#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include<sys/epoll.h>
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<fcntl.h>
#include<arpa/inet.h>
#include<sys/stat.h>
#include<sys/mman.h>
#include<stdarg.h>
#include<errno.h>
#include"locker.h"
#include<sys/uio.h>
#include<string>
#include<regex>//正则表达式

//http报文类
class http_conn {
public:
    static int m_epollfd;//所有socket上的事件都被注册到同一个socket
    static int m_user_count;//用来统计用户的数量
    static const int READ_BUFFRE_SIZE = 2048;//读缓冲区大小
    static const int WRITER_BUFFER_SIZE = 2048;//写缓冲区大小
    static const int FILENAME_LEN = 200;//文件名的最大长度

    //状态机中的状态枚举
     // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};   
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };

    http_conn(){}
    ~http_conn(){}

    void process();  //处理客户端的请求
    void init(int sockfd, const sockaddr_in &addr);  //初始化新接收的连接
    void colse_conn();  //关闭连接
    bool read();  //一次性读
    bool write();  //一次性写

private:
    void init();  //初始化连接其余的数据
    HTTP_CODE process_read();  //解析HTTP请求
    bool process_write(HTTP_CODE ret);  //填充HTTP应答

    //process_read（）调用分析HTTP请求
    HTTP_CODE pares_request_line(char * text);//解析请求首行
    HTTP_CODE pares_headers(char * text);//解析HTTP请求头
    HTTP_CODE pares_content(char * text);//解析HTTP内容
    HTTP_CODE do_request();//解析具体的数据
    char* get_line() {return m_read_buff + m_start_line; }      //获取一行数据
    LINE_STATUS pares_line();//从状态机 解析具体的一行，返回给主状态机 HTTP_CODE

    //process_write()调用填充HTTP应答
    void unmap();
    bool add_response(const char* format,...);//接受可变参数的函数
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content(const char* content);
  

private:
    int m_sockfd;//这次HTTP连接的socket
    sockaddr_in m_address;//通信的socket地址

    char m_read_buff[READ_BUFFRE_SIZE];//读缓冲
    int m_read_idx;//标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    int m_checked_index;//当前正在分析的字符在缓冲区的位置
    int m_start_line;//当前正在解析的行的起始位置

    METHOD m_method;//请求方法
    CHECK_STATE m_check_state;//主状态机当前所处的状态

    char m_real_file[FILENAME_LEN];//客户请求的目标文件的完整路径，其内容等于doc_root+m_rul，doc_root是网站根目录
    //http请求报文中的各信息
    char* m_url;                              //请求目标文件的文件名
    char* m_version;                        //协议版本，只支持http1.1
    char* m_host;                            //主机名
    bool m_linger;                                 //判断http请求是否要保持连接
    int m_content_length;                           //HTTP请求的消息总长度
    
    char m_write_buf[WRITER_BUFFER_SIZE];//写缓冲区
    int m_write_idx;                          //写缓冲区待发送的字节数
    char* m_file_address;               //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
                                                      //struct stat 是一个结构体，用于存储文件的属性信息。它包含了许多关于文件的详细信息，例如文件大小、文件类型、权限、所有者、修改时间等。
    struct iovec m_iv[2];              // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;                 //iovec 用于描述分散/聚集I/O操作的数据结构,在一次函数调用中读写多个非连续缓冲区
};
#endif