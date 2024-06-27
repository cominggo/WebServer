#ifndef LOCKER_H
#define LOCKER_H

#include<pthread.h>
#include<exception>
#include<semaphore.h>
//线程同步机制封装类

//互斥锁类
class locker{
public:
        locker(){
            //初始化互斥锁
            if(pthread_mutex_init(&m_mutex, NULL) != 0) {
                throw std::exception();//异常对象
            }
        }
        ~locker() {
            //释放锁
            pthread_mutex_destroy(&m_mutex);
        }
        //判断是否上锁
        bool lock() {
            //上锁，成功就返回0
            return pthread_mutex_lock(&m_mutex) == 0;
        }

        bool unlock() {
            //解锁
            return pthread_mutex_unlock(&m_mutex) == 0;
        }
        //获取互斥锁
        pthread_mutex_t * get() {
            return &m_mutex;
        }
        
 private:
        pthread_mutex_t m_mutex;

};

//条件变量类
class cond{
public:
        cond() {
            //初始化，成功返回0
            if(pthread_cond_init(&m_cond, NULL) != 0) {
                throw std::exception();
            }
        }

        ~cond() {
            //销毁
            pthread_cond_destroy(&m_cond);
        }

        bool wait(pthread_mutex_t * mutex) {
            //等待条件变量，成功返回0
            return pthread_cond_wait(&m_cond, mutex) == 0;
        }

        bool timewait(pthread_mutex_t * mutex, struct  timespec t) {
            //超时等待，成功返回0
            return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
        }

        bool signal(pthread_mutex_t * mutex) {
            //唤醒一个等待的线程，成功返回0
            return pthread_cond_signal(&m_cond) == 0;
        }

        bool broadcast() {
            //唤醒所有的线程
            return pthread_cond_broadcast(&m_cond) == 0;
        }

private:
        pthread_cond_t m_cond;

};

//信号量类
class sem{
public:
        sem() {
            //初始化
            if(sem_init(&m_sem, 0, 0) != 0) {
                    throw std::exception();
                }
        }

        sem(int num) {
            //带初始值的初始化
            if(sem_init(&m_sem, 0, num) != 0) {
                    throw std::exception();
                }
        }

        ~sem() {
            //销毁
            sem_destroy(&m_sem);
        }

        //等待信号量
        bool wait() {
            //等待信号量，成功返回0
            return sem_wait(&m_sem) == 0;
        }

        //对信号量解锁
        bool post() {
            //对信号量解锁，成功返回0
            return sem_post(&m_sem) == 0;
        }

private:
        sem_t m_sem;
};

#endif