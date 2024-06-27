
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<pthread.h>
#include<list>
#include"locker.h"
#include<stdio.h>
//线程池类，定义成模板类为了代码的复用,模板参数就是任务类
template<typename T>
class  threadpool {
public:
    threadpool(int m_thread_number = 8, int m_max_requests = 10000);
    ~threadpool();
    bool append(T* request);//向线程池的请求队列中添加请求

private:
    static void* worker(void * arg);//子线程执行的函数
    void run();//线程池一直执行
private:
    //线程数量
    int m_thread_number;

    //线程池数组，大小为m_thread_number
    pthread_t * m_threads;

    //请求队列中最多允许的，等待处理的请求数量
    int m_max_requests;

    //请求队列
    std::list<T*> m_workqueue;

    //线程同步，互斥锁
    locker m_queuelocker;

    //信号量，用来判断是否有任务需要处理
    sem m_queuestat;

    //是否结束线程
    bool m_stop;
};

template<typename T>
//构造函数
threadpool<T>::threadpool(int thread_number, int max_requests) :
        m_thread_number(thread_number), m_max_requests(max_requests),
        m_stop(false), m_threads(NULL) {
            //线程数量和请求队列的请求数量<0
            if(thread_number <= 0 || max_requests <= 0) {
                throw std::exception();
            }
            //实例化线程
            m_threads = new pthread_t[m_thread_number];
            if(!m_threads) {
                throw std::exception();
            }

            //创建thread_num个线程，并设置为线程脱离
            for(int i = 0; i < thread_number; i++) {
                printf("create the &dth th %d\n", i);
                //创建线程去干某事，成功返回0,
                //worker在C++中必须是一个静态函数,不能直接访问非静态的成员变量，通过this传入
                if (pthread_create(m_threads + i, NULL, worker, this) !=0 ) {
                    delete [ ] m_threads;
                    throw std::exception();
                };
                //分离线程，成功返回0
               if(pthread_detach(m_threads[i])) {
                    delete [ ] m_threads;
                    throw std::exception();
                }
            }
        }

template<typename T>
//构造函数
threadpool<T>::~threadpool() {
    delete[ ] m_threads;
    m_stop = true;
} 

//向线程池添加请求
template<typename T>
bool threadpool<T>:: append(T * request) {
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests) {//请求队列的值大于最大请求数，就解锁
        m_queuelocker.unlock();
        return false;
    }
    //没有大于就把当前请求加入到请求队列中，然后在解锁，信号量也增加
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//信号量解锁
    return true;
} 

template<typename T>
void* threadpool<T>:: worker(void* arg) {
    threadpool * pool = (threadpool *) arg;
    pool->run();//当子线程创建出来后，让线程池启动，读取数据
    return pool;
}

template<typename T>
void threadpool<T>:: run() {
    //一直循环,直到线程结束
    while(!m_stop) {
        m_queuestat.wait();//等待是否有信号量
        m_queuelocker.lock();//上锁
        if(m_workqueue.empty()) {//请求队列为空，解锁
            m_queuelocker.unlock();
            continue;
        }

        T* request = m_workqueue.front();//获取请求
        m_workqueue.pop_front();//获取后去掉该请求
        m_queuelocker.unlock();//解锁

        if(!request) {
            continue;
        }

        request->process();//对请求进行处理

    }
}

#endif