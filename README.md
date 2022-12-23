# 服务器基本框架

| 模块             | 功能                       |
| ---------------- | -------------------------- |
| **I/O处理单元**  | 处理客户连接，读写网络数据 |
| **逻辑单元**     | 业务进程或线程             |
| **网络存储单元** | 数据库、文件或缓存         |
| **请求队列**     | 各单元之间的通信方式       |

- **`I/O` 处理单元**是服务器管理客户连接的模块，它要完成以下工作：

  - 等待并接收新的客户连接
  - 接收客户数据，将服务器响应数据返回给客户端

  但**是数据的收发不一定在 `I/O` 处理单元中执行，也可能在逻辑单元中执行**，具体在何处执行取决于事件处理模式

- 一个**逻辑处理单元**通常时一个进程或线程，它要完成以下工作：

  - 分析并处理客户数据
  - 将处理结果传递给 `I/O` 处理单元 或 直接发送给客户端（具体使用哪种方式取决于事件的处理模式）

  服务器通常拥有多个逻辑处理单元，以实现对多个客户任务的并发处理

- 网络存储单元可以是数据库、缓存和文件，但不是必须的

- **请求队列**是各单元之间的通信方式的抽象

  `I/O` 处理单元接收到客户请求时，需要以某种方式通知一个逻辑单元来处理该请求

  同样，多个逻辑单元同时访问一个存储单元时，也需要采用这种机制来协调竞态条件。请求队列通常被实现位池的一部分 

  # 两种高效的事件处理模式

  服务器程序通常需要处理三类事件：

  - `I/O` 事件
  - 信号
  - 定时事件

  对此，有两种高效的**事件处理模式**：

  - `Reactor`	**同步 I/O** 模式通常用于实现 `Reactor` 模式
  - `Proactor`   **异步 I/O** 模式通常用于实现 `Proactor` 模式

## Reactor 模式

```
Redis Nginx 采用 Reactor 模式
```

要求主线程（`I/O` 处理单元）只负责监听文件描述符上是否有事件发生，有的话则立即将该事件通知工作线程（逻辑单元），将 `socket` 可读可写事件放入请求队列，交给工作线程处理。

除此之外，主线程不做任何其他实质性的工作。

读写数据、接收新的连接、处理客户的请求均在工作线程中完成。

1. 主线程往 `epoll` 内核时间表中注册 `socket` 上的读就绪事件
2. 主线程调用 `epoll_wait` 等待 `socket` 上有数据可读
3. 当 `socket` 上有数据可读时，`epoll_wait` 通知主线程。主线程则将 `socket` 可读事件放入请求队列
4. 睡眠在请求队列上的某个工作线程被唤醒，它从 `socket` 获取数据，并处理客户请求，然后往 `epoll` 内核事件表中注册该 `socket` 上的写就绪事件
5. 当主线程调用 `epoll_wait` 等待 `socket` 可写
6. 当 `socket` 可写时，`epoll_wait` 通知主线程。主线程将 `socket` 可写事件放入请求队列
7. 睡眠在请求队列上的某个工作线程被唤醒，它往 `socket` 上写入服务器处理客户请求的结果

**单进程多线程 Reactor 概念**

![img](https://raw.githubusercontent.com/De4tsh/typoraPhoto/main/img/202211161901677.png)

- `Reactor` 对象的作用是监听和分发事件
- `Acceptor` 对象的作用是获取连接
- `Handler` 对象的作用是处理业务

**过程：**

- `Reactor` 对象通过 `select/poll/epoll` （ `IO` 多路复用接口 ） 监听事件，收到事件后通过 `dispatch` 进行分发，具体分发给 `Acceptor` 对象还是 `Handler` 对象，还要看收到的事件类型
- 如果是连接建立的事件，则交由 `Acceptor` 对象进行处理，`Acceptor` 对象会通过 `accept` 方法 获取连接，并创建一个 `Handler` 对象来处理后续的响应事件
- 如果不是连接建立事件， 则交由当前连接对应的 `Handler` 对象来进行响应

## Proactor 模式

`Proactpr` 模式将所有 `I/O` 操作都交给主线程和内核来处理（进行读、写），工作线程仅负责业务逻辑，使用异步 `I/O` 模型（ `aio_read` 和 `aio_write` ）实现的 `Proactor` 模式的工作流程为：

1. 主线程调用 `aio_read` 函数向内核注册 `socket` 上的读完成事件，并告诉内核，用户读缓冲区的位置，以及读操作完成时如何通知应用程序
2. 主线程继续处理其他逻辑
3. 当 `socket` 上的数据被内核读入用户缓冲区后，内核将向应用程序发送一个信号，以通知应用程序数据已经可用
4. 应用程序预先定义好的信号处理函数选择一个工作线程来处理客户端的请求
5. 工作线程处理完客户请求后，调用 `aio_write` 函数向内核注册 `socket` 上的写完成事件，并告诉内核，用户写缓冲区的位置，以及写操作完成时如何通知应用程序
6. 主线程继续处理其他逻辑
7. 当用户缓冲区的数据被写入 `socket` 后，内核将向应用程序发送一个信号，以通知应用程序数据已经发送完毕
8. 应用程序预先定义好的信号处理函数选择一个工作线程来做善后处理，比如决定是否关闭 `socket`



## 模拟 Proactor 模式（本项目中使用）

使用同步 `I/O` 模拟出 `Proactor` 模式，原理是：主线程执行数据读写操作，读写完成之后，主线程向工作线程通知这一 “完成事件”。那么从工作线程的角度来看，它们就直接获得了数据读写的结果，接下来要做的只是对读写的结果进行逻辑处理

<!-- 但这无疑会增大主线程的压力 -->

1. 主线程往 `epoll` 内核事件表中注册 `socket` 上的读就绪事件
2. 主线程调用 `epoll_wait` 等待 `socket` 上有数据可读
3. 当 `socket` 上有数据可读时，`epoll_wait` 通知主线程。主线程从 `socket` 循环读取数据，直到没有更多的数据可读，然后将独到的数据封装成一个请求对象并插入请求队列
4. 睡眠在请求队列上的某个工作线程被唤醒，它获得请求对象并处理客户请求，然后往 `epoll` 内核事件表中注册 `socket` 上的写就绪事件
5. 主线程调用 `epoll_wait` 等待 `socket` 可写
6. 当 `socket` 可写时，`epoll_wait` 通知主线程。主线程往 `socket` 上写入服务器处理客户请求的结果



# 线程池

线程池是由服务器预先创建的一组子线程，线程池中的线程数量应该和 CPU 数量差不多。线程池中的所有子线程都运行着相同的代码。当有新的任务到来时，主线程将通过某种方式选择线程池中某一个子线程来为之服务。

相比动态的创建子线程，选择一个已经存在的子线程的代价显然要小得多。至于主线程选择哪个子线程来为新任务服务，有多种方式：

- 主线程使用某种算法来选择子线程
  - 随机算法
  - 轮询算法（`Round Robin`）
  - 以上两种算法数据最简单的分配算法

主线程和所有子线程通过一个共享的工作队列来同步，子线程都睡眠在该工作队列上，当有新的任务到来时，主线程将任务添加到工作队列中。这将唤醒正在等待任务的子线程，不过只有一个子线程将获得新任务的“接管权”，它可以从工作队列中取出任务并执行，而其他子线程将继续睡眠在工作队列上

## 本项目中线程池的整体调用流程
https://raw.githubusercontent.com/De4tsh/typoraPhoto/main/img/202212232350809.png



**locker.h**

```C++
#ifndef __LOCKER_H
#define __LOCKER_H

#include <pthread.h>
#include <exception>
#include <semaphore.h>

// 线程同步

// 互斥锁类
class locker
{
    public:
    
    	locker()
       {
            if (pthread_mutex_init(&m_mutex,NULL) != 0)
             {
            	throw std::exception();
			    }
        }
    
    	~locker()
        {
        	   pthread_mutex_destory(&m_mutex); 
		  }
    
    	 bool lock()
        {
             return pthread_mutex_lock(&m_mutex) == 0;
        }
    
    	bool unlock()
        {
            return pthread_mutex_unlock(&m_mutex) == 0;
        }
    
    	pthread_mutex_t *get()
        {
       		return &mutex;     
        }
    
    private:
    
    	pthread_mutex_t m_mutex;
}

// 条件变量类
class cond 
{
    public:
    
    	cond()
        {
            if (pthread_cond_init(&m_cond,NULL) != 0)
            {
              throw std::exception();  
            }
        }
    
    	~cond()
        {
            pthread_cond_destory(&m_cond)
        }
    
    	bool wait(pthread_mutex_t *mutex)
        {
            return pthread_cond_wait(&m_cond,mutex) == 0;
        }
    
    	bool timewait(pthread_mutex_t *mutex,struct timespec t)
        {
            return pthread_cond_timedwait(&m_cond,mutex,&t) == 0;
        }
    
    	bool signal(pthread_mutex_t *mutex)
        {
            return pthread_cond_signal(&m_cond) == 0;
        }
    
    	bool broadcast()
        {
            return pthread_cond_broadcast(&m_cond) == 0;
        }
    
    private:
    
    	pthread_cond_t m_cond;
}

// 信号量类
class sem
{
    public:
    	
       sem()
       {
       		if (sem_init(&m_sem,0,0) != 0)
            {
                throw std::exception();
			}
       }
    
       sem(int num)
       {
           if (sem_init(&m_sem,0,num) != 0)
           {
                throw std::exception();
		   }
       }
    
    	~sem()
        {
            sem_destroy(&m_sem);
        }
    
    
    	// 等待信号量
        bool wait()
        {
            return sem_wait(&m_sem) == 0;
        }


        // 增加信号量
        bool post()
        {
            return sem_post(&m_sem) == 0;
        }
    
    private:
    
    	sem_t m_sem;
}


#endif

```


