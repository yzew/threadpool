#ifndef THREADPOOL_H
#define THREADPOOL_H
// 使用 #ifndef 而非 #pragma once ，因为后者需要编译器支持，在linux下不支持
#include<iostream>
#include<vector>
#include<queue>
#include<memory>  // 智能指针
#include<atomic>  // 原子类型，实现线程互斥
#include<mutex>  // 锁
#include<condition_variable>  // 条件变量，实现线程通信
#include<functional>
#include<thread>
#include<unordered_map>
#include<future>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 10;  // 等待的时间，s

struct normal   {};  // normal task (for type inference)
struct urgent   {};  // urgent task (for type inference)

// 线程池支持的模式
// 这里使用了c++11中限定作用域的枚举类型
// 使用时必须显示的访问，PoolMode p = PoolMode::MODE_FIXED;
enum class PoolMode {
	MODE_FIXED,  // 固定数量的线程
	MODE_CACHED,  // 线程数量可动态增长
};

//////////////////////////////////   线程类型
class Thread {
public:
	// 线程函数对象类型，为function函数对象
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func)
        : func_(func)
        , threadId_(generateId_++)
    {}
	
    ~Thread() = default;
	// 启动线程
    void start() {
        // 创建线程
        // ThreadPool::start函数创建了thread，传入了func_
        // 因此创建线程的时候直接把func_放进去就行
        // 这里thread的构造函数通过完美转发将参数传递给线程函数
        
        std::thread t(func_, threadId_);
        // 注意，上面是将线程函数写到线程池类的写法
        // 若是将线程函数写到Thread类中，则应该写为
        // std::thread t(&Thread::threadFunc, &thread1);
        // thread1表示一个线程对象，相应的start函数也应该有形参：start(Thread& thread1)

        // 注意，这个线程对象t出了这个作用域就会自动销毁
        // 因此要设置为分离线程，使得此线程变为守护线程，驻留后台运行
        t.detach();
    }

	// 获取线程id
	int getId() const {
        return threadId_;
    }
private:
	ThreadFunc func_;
	static int generateId_;  // 类的所有对象共享静态成员变量，通过它来得到变化的threadId_
	int threadId_;  // 保存线程ID
};
// 静态成员变量在类外要初始化
int Thread::generateId_ = 0;

///////////////////////////////////   线程池类型

class ThreadPool {
public:
	ThreadPool()
        : initThreadSize_(4)
        , taskSize_(0)
        , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
        , curThreadSize_(0)
        , idleThreadSize_(0)
    {}
	~ThreadPool() {
        isPoolRunning_ = false;
        // 等待线程池中所有的线程返回
        // 两种状态：阻塞 / 执行中
        // 这里就需要进行线程的通信
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
    }
	// 禁止线程池的拷贝和复制
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 设置线程池模式
	void setMode(PoolMode mode) {
        // 如果线程池已经启动了，则不能设置工作模式了
        if (checkRunningState()) return;
        poolMode_ = mode;
    }

	// 开启线程池，并设置初始的线程数量，为cpu系统的核心数量
	void start(size_t initThreadSize = std::thread::hardware_concurrency()) {
        // 设置线程池的启动状态
        isPoolRunning_ = true;

        // 设置初始的线程数量
        initThreadSize_ = initThreadSize;
        // 记录线程总数
        curThreadSize_ = initThreadSize;

        // 创建线程对象
        for (size_t i = 0; i < initThreadSize_; i++) {
            // 创建thread线程对象的时候，需要把线程函数给到thread类
            // 这样在Thread::start中才能启动这个线程函数
            // 通过构造函数，将threadFunc放进去
            // 这里我们使用bind。对于类的成员函数，前面要取个地址
            // 且需要绑定一个类对象才能使用，这里绑定了this即当前对象
            // 这里使用智能指针创建线程对象，让其能自动析构
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));  // c++14
            // 使用move将左值转换为对应的右值引用类型
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // unique_ptr是不允许拷贝构造的，仅支持一个智能指针指向它
            // 因此 emplace_back(ptr) 的操作会报错
            // unique_ptr虽然关闭了左值引用的拷贝和赋值，但支持右值引用的操作！
        }

        // 启动所有线程
        for (size_t i = 0; i < initThreadSize_; i++) {
            // 注意，这里threads_[i]是指针
            // 调用了线程类里的start函数创建并启动线程
            threads_[i]->start();
            idleThreadSize_++;
        }
    }

	// 设置cached模式下线程阈值
	void setThreadSizeThreshHold(int threshhold) {
        if (checkRunningState()) return;
        if (poolMode_ == PoolMode::MODE_CACHED) {
            threadSizeThreshHold_ = threshhold;
        }
    }
	
    // 任务相关
	// 设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(int threshhold) {
        // 如果线程池已经启动了，则不能设置了
        if (checkRunningState()) return;
        taskQueMaxThreshHold_ = threshhold;
    }
	
    // 给线程池提交任务
    // 这里T用于调整任务优先级
    template<typename T = normal, typename Func, typename... Args> 
    // Func&&引用折叠，可以接收左值和右值引用
    // 配合auto写成尾置返回类型
    // 返回值为future<>类型，而future中的类型需要decltype进行推导
	auto submitTask(Func&& func, Args&&... args) 
        -> std::future<decltype(func(args...))>
    {  
        bool isNorm;
        if (std::is_same<T, normal>::value) isNorm = true;
        else isNorm = false;
        // 打包任务，放入任务队列
        using RType = decltype(func(args...));
        // 通过智能指针延长生命周期
        // packaged_task需要传入返回值类型和参数类型，这里参数类型直接绑定上
        // 需要使用std::forward进行完美转发
        auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        // 原来的submitTask的内容
        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        // 线程的通信 等待任务队列有空余
        // 没有空余才等待，因此需要等待notFull_
        // 这里使用了lambda表达式的隐式捕获的引用捕获，让编译器根据函数体中代码推断捕获列表
        //// notFull_.wait(lock, [&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; });
        // 与下面三行等价
        //while (taskQue_.size() == taskQueMaxThreshHold_) {
        //	notFull_.wait(lock);
        //}
        // 增加一个要求：用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
        // wait()：一直等待到条件满足；wait_for()：等一段时间；wait_until()：等到一个时间点
        /*notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; });*/
        if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; })) {
            // 等待1s，条件仍不满足
            std::cerr << "task queue is full, submit task fail." << std::endl;
            // 若任务提交失败，则返回一个空值
            auto task = std::make_shared<std::packaged_task<RType()>>(
                []()->RType { return RType(); }
                );
            (*task)();
            return task->get_future();
        }

        // 有空余，将任务放入任务队列
        // 任务队列中的类型我们写成了如下的形式，是一个没有返回值的
        // using Task = std::function<void()>;
        // 我们通过写成无返回值的lambda表达式的形式调用task来实现
        if (isNorm) {
            taskQue_.emplace_back([task]() {
            // 执行下面的任务
            (*task)();  // task解引用后就是packaged_task的函数对象
            } );
        } else {
            taskQue_.emplace_front([task]() {
            // 执行下面的任务
            (*task)();  // task解引用后就是packaged_task的函数对象
            } );
        }
        
        taskSize_++;

        // 任务队列不空，通知notEmpty_
        notEmpty_.notify_all();

        // cached模式需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
        // 且需要当前线程总数小于我们设定的最大线程数
        if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_) {
            std::cout << "create new thread" << std::endl;
            // 创建新线程
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));  // c++14
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // 启动线程
            threads_[threadId]->start();
            // 修改线程个数相关的变量
            curThreadSize_++;
            idleThreadSize_++;
        }
        // 对于Result，可以通过task类里面的函数返回，也可以用Result类进行封装
        // return task->getResult();  // 不行！task对象被线程取出来用完后就析构了，再想使用这个task对象是不行的
        // return Result(task);  // 因此通过Result来维持task的生命周期，保证我们想要task返回值的时候task对象还在
        return result;
    }

private:
	// 定义线程函数
    // threadFunc负责在线程池中获取并执行任务
    // 在线程池类中定义线程函数，由线程类执行
    // 线程函数需要的那些锁和条件变量，都定义在线程池中
    // 这里线程函数若是定义在Thread类中，则不方便访问ThreadPool里的那些私有的锁和条件变量
    // 因此将线程函数定义在ThreadPool类中
    void threadFunc(int threadid) {
        auto lastTime = std::chrono::high_resolution_clock().now();

        // 所有任务必须执行完成，线程池才可以回收所有线程资源，所以还是必须用for(;;)
        // 相当于while(true)。线程执行threadFunc后便一直在此函数中尝试获取任务
        // while (isPoolRunning_){
        for (;;) {
            // 注意，我们仅需要在取任务的时候获取锁，取到任务后释放锁，再执行任务
            Task task;
            {
                // 获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_);

                std::cout << "tid:" << std::this_thread::get_id()
                    << "try to get the task!" << std::endl;

                // cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s
                // 对于超过初始线程数量initThreadSize_的线程，需要看情况进行回收
                while (taskQue_.size() == 0) {
                    // 线程池要结束，回收线程资源，后面的这些都不执行了，直接跳出while循环，来执行删除线程操作
                    if (!isPoolRunning_) {
                        // 若是线程池结束时还有任务没执行完，则等其结束wait状态后，跳出while循环，在这里进行析构
                        threads_.erase(threadid);
                        std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                        // 结束时通知下pool所在的主线程
                        exitCond_.notify_all();
                        return;
                    }
                    // 每一秒返回一次
                    // while来区分是超时返回还是有任务待执行返回
                    // 任务队列有任务，就跳过while去消费，没任务才等待任务
                    if (poolMode_ == PoolMode::MODE_CACHED) {
                        // wait_for函数的返回值cv_status有两个状态，超时和不超时
                        if (std::cv_status::timeout == 
                            notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
                            auto now = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME
                                && curThreadSize_ > initThreadSize_) {
                                // 回收线程
                                // 记录线程数量的相关变量的值
                                idleThreadSize_--;
                                curThreadSize_--;

                                // 将线程对象从线程列表中删除 但没法将threadFunc对应到thread对象
                                // threadid => thread对象 => 删除
                                threads_.erase(threadid);
                                std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                                return;
                            }
                        }
                    }
                    else {
                        notEmpty_.wait(lock);
                    }
                }

                idleThreadSize_--;

                std::cout << "tid:" << std::this_thread::get_id()
                    << "obtain the task!" << std::endl;

                task = taskQue_.front(); taskQue_.pop_front();
                taskSize_--;

                // 为什么用了两个条件变量呢，这便于进行更精细的操作
                // 如果任务队列仍有任务，则通知其他wait在notEmpty_的线程执行任务
                if (taskQue_.size() > 0) {
                    notEmpty_.notify_all();
                }
                
                // 通知 wait在notFull_上的生产者，可以继续提交任务
                notFull_.notify_all();
            }  // 取完任务，释放锁
            if (task != nullptr) {
                // 执行任务
                task();
            }
            idleThreadSize_++;
            lastTime = std::chrono::high_resolution_clock().now();
        }
    }

	// 检查线程池的运行状态
	bool checkRunningState() const {
        return isPoolRunning_;
    }
private:
	

	// 线程相关
	// 线程列表。对于线程的创建，是在ThreadPool::start中new出来的，还需要delete
	// 因此直接使用智能指针。线程的话unique就行
	// std::vector<std::unique_ptr<Thread>> threads_;
	// 为了实现通过threadId_查询到对应的线程，这里最后使用了map
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;

	size_t threadSizeThreshHold_;  // 线程数量的上限
	// 为什么新增一个变量而不是用threads_.size()呢，因为vector不是线程安全的
	std::atomic_int curThreadSize_;  // 记录当前线程池里面线程的总数量
	std::atomic_int idleThreadSize_;  // 记录空闲线程的数量
	int initThreadSize_;  // 初始的线程数量。size_t增强了可移植性，表示任何对象所能达到的最大长度
	
	// Task任务就是个函数对象
    using Task = std::function<void()>;
	std::deque<Task> taskQue_;  // 任务队列
	std::atomic_uint taskSize_;  // 任务的数量。考虑到线程安全问题，使用轻量化的原子类型实现线程互斥
	size_t taskQueMaxThreshHold_;  // 任务队列数量的上限

	// 实现线程通信
	std::mutex taskQueMtx_;  // 保证任务队列的线程安全
	std::condition_variable notEmpty_;  // 任务队列不空
	std::condition_variable notFull_;  // 任务队列不满
	std::condition_variable exitCond_;  // 等待线程资源全部回收

	PoolMode poolMode_;  // 当前线程池的工作模式

	std::atomic_bool isPoolRunning_;  // 表示当前线程池的启动状态
	
};


#endif // !THREADPOOL_H
