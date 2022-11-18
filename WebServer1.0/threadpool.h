#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <cstdio>
#include "locker.h"

// 线程池类
// 定义为模板是为了代码复用
// T 就是任务类
template<typename T>
class threadpool
{

    public:

        // 默认线程数量 8 ，最大请求 10000
        threadpool(int thread_number = 8,int max_requests = 10000);
        ~threadpool();

        bool append(T* request);

    private:

        // 此时由于 worker 为静态函数，所以其没有 this 指针，是无法访问类内的成员的
        // 除非将 this 指针作为 worker 的参数传递给 worker
        static void* worker(void* arg);

        // 启动线程池，一直循环直到 m_stop 置 1
        void run();


    private:

        // 线程的数量
        int m_thread_number;

        // 线程池数组，大小为 m_thread_number
        pthread_t *m_threads;

        // 请求队列中，最多允许的等待处理的请求数量
        int m_max_requests;

        // 请求队列（链表）
        std::list<T*>m_workqueue;

        // 互斥锁
        locker m_queuelocker;

        // 信号量，用来判断是否有任务需要处理
        sem m_queuestat;

        // 是否结束线程
        bool m_stop;

};

template<typename T>
threadpool<T>::threadpool(int thread_number,int max_requests) :
    m_thread_number(thread_number),m_max_requests(max_requests),
    m_stop(false),m_threads(NULL)
    // 对参数进行初始化
{

    if ((thread_number < 0) || (max_requests <= 0))
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
        throw std::exception();
    }

    // 创建 thread_number 个线程，并将它们设置为线程分离
    for (int i = 0; i < thread_number; i++)
    {
        printf("create the %dth thread\n",i);

        // C++ 中 worker 必须是一个静态函数
        /*
            为何pthread_create的第三个参数必须是静态函数？

            这是因为类的成员函数在调用时除了传进参数列表的参数之外，还会再传入一个this指针，
            指向调用此函数的对象。只有传入这个this指针，
            函数在执行过程中才能调用其他非静态成员变量和非静态成员函数
            如果我们的pthread_create函数如果传进了一个形如 void* func(void*)的非静态成员函数，
            那么恭喜你，程序将会报错，因为编译器在你看不见的地方还给你附加了一个参数this*
        */
        if ( pthread_create(m_threads + i,NULL,worker,this) != 0)
        {
            delete [] m_threads;
            throw std::exception();
        }

        // 设置线程分离
        if (pthread_detach(m_threads[i]))
        {
            delete [] m_threads;
            throw std::exception();
        }

    }
    



}


template<typename T>
threadpool<T>::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}


template<typename T>
bool threadpool<T>::append(T *request)
{

    m_queuelocker.lock();

    if (m_workqueue.size() > m_max_requests)
    {
        // 超出最大请求队列长度
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);
    m_queuelocker.unlock();

    // 表示队列中新增了一条待处理的请求
    m_queuestat.post();

    return true;

}


template<typename T>
void* threadpool<T>::worker(void *arg)
{

    // 将 this 指针赋给 pool 局部变量
    threadpool *pool = (threadpool *)arg;
    pool->run();

    return pool;
}


template<typename T>
void threadpool<T>::run()
{

    while (!m_stop)
    {
        // 看当前请求队列中是否有请求，没有就阻塞
        m_queuestat.wait(); 

        m_queuelocker.lock();

        if (m_workqueue.empty())
        {
            // 若请求队列为空
            m_queuelocker.unlock();
            continue;
        }

        // 取请求队头的第一个事件
        T* request = m_workqueue.front();
        m_workqueue.pop_front();

        m_queuelocker.unlock();

        if (!request)
        {
            continue;
        }

        // 执行任务
        request->process();
    }

}

#endif
