#include<stdio.h>//标准输入输出
#include<stdlib.h>//标准库函数，exit
#include<string.h>
#include<sys/socket.h>//套接字
#include<netinet/in.h>//互联网地址族
#include<arpa/inet.h>//IP地址转换函数
#include<unistd.h>//unix标准库，read、
#include<errno.h>//错误类型
#include<fcntl.h>//文件操作
#include<sys/epoll.h>
#include"locker.h"
#include"threadpool.h"
#include"http_conn.h"
#include <signal.h>
#include <iostream>
#include <cassert>

#define MAX_FD 65536 //最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  //一次监听的最大事件数量
//添加信号捕捉
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;//注册信号的参数
    memset(&sa, '\0', sizeof(sa));  //将sa结构体清零
    sa.sa_handler = handler; // 设置信号函数
    sigfillset(&sa.sa_mask);  //理该信号期间阻塞所有其他信号
    sigaction(sig, &sa, NULL);  //注册信号
}

//添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);

//从epoll删除文件描述符
extern void removefd(int epollfd, int fd);

//修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

//传入参数，第一个是程序，第二次是端口号
int main(int argc, char* argv[]) {

    if(argc <= 1) {//没有输入端口号
        printf("按照如下格式运行： %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    int port = atoi(argv[1]);  //字符串转换为整数

    //对SIGPIE信号进行处理
    //默认情况下，接收到SIGPIPE信号会导致进程终止，会是一个问题，SIG_IGN表示忽略信号
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池，初始化,模板类为http报文，将 http_conn 对象添加到线程池的任务队列中，线程池中的工作线程将处理这个任务
    /*模板类 threadpool 可以通过模板参数 http_conn 使用 http_conn 类的所有公共成员和方法。
    也就是说，threadpool<http_conn> 类的对象 pool 可以调用 http_conn 类的方法，处理 http_conn 类的对象。
    pool->append(users + sockfd pool可以用threadpoolde 函数处理http_conn的对象*/
    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
        
    }

    //创建一个数组用于保存所有的客户端的文件描述符
    http_conn* users = new http_conn[MAX_FD];
    //网络连接
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    //设置端口复用，设置在服务器绑定端口之前，设置监听fd端口复用
    /*1、当服务器程序重新启动时，之前使用的端口可能仍处于 TIME_WAIT 状态。如果不设置端口复用，服务器程序会因为端口被占用而无法绑定到该端口。
        设置 SO_REUSEADDR 选项可以让服务器程序在端口处于 TIME_WAIT 状态时立即重新绑定到该端口。
        2、init中，客户端程序在短时间内频繁地打开和关闭连接，也可能会遇到端口被占用的问题。
        设置 SO_REUSEADDR 选项可以让客户端程序在端口处于 TIME_WAIT 状态时立即重新使用该端口。*/
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    //监听
    listen(listenfd, 5);

    //创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    
    //assert 会使程序终止，并打印出错误信息
    assert(epollfd != -1);
    
    //将监听的文件描述符添加到epoll对象
    addfd(epollfd, listenfd, false);//监听的fd不需要EPOLLONESHOT事件，所以false
    //但监听的不能是ET触发。需要处理

    http_conn::m_epollfd = epollfd;

    //主线程循环检测事件发生
    while(true) {
        //等待文件描述符集合中是否有文件描述符发生变化，返回发生变化的文件描述符个数，-1设置为阻塞
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((num < 0) && (errno != EINTR)) {//如果没有事件或者被信号中断了事件就退出
            printf("epoll failure\n");
            break;
        }
        //循环遍历数组
        for(int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd) {
                //有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);

                if(http_conn::m_user_count >= MAX_FD) {
                    //目前连接数满了
                    //给客户端写一个信息：告诉服务器正忙
                    close(connfd);
                    continue;;
                }
                //将新的数据初始化，放到数组users中
                users[connfd].init(connfd, client_address);
            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //对方异常断开，错误等事件的发生，要关闭连接
                users[sockfd].colse_conn();
            } else if(events[i].events & EPOLLIN) {
                    //判断是否有读事件发生,一次性把事件读出来，模拟proactor模式
                    if(users[sockfd].read()) {
                        //read,一次性读完,检测到读事件，将该事件放入请求队列
                        pool->append(users + sockfd);
                    } else {
                        users[sockfd].colse_conn();//失败就关闭
                    } 
                } else if(events[i].events & EPOLLOUT) {
                    if(!users[sockfd].write()) {//一次性写完数据
                        users[sockfd].colse_conn();
                    }
                }
        }
    }
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;

    return 0;
}