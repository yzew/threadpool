#ifndef THREADPOOL_H
#define THREADPOOL_H
// 使用 #ifndef 而非 #pragma once，因为后者需要编译器支持，在linux下不支持
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

// Any类型：可以接收任意数据的类型
// 将其作为函数返回值类型时，会看Any有没有一个合适的构造函数，来接收return返回的对象
class Any {
public:
	// 将任意类型的data包在派生类中，用基类指针指向
	template<typename T>
	Any(T data) : base_(std::make_unique<Derive<T>>(data)) {}

	// 定义一个方法，将Any对象存储的data_数据提取出来
	template<typename T>
	T cast_() {
		// 从base_指针中找到所指向的派生类对象，取出data_成员变量
		// 用智能指针提供的get方法取得裸指针，用 dynamic_cast 进行自动的类型转换至派生类指针
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
		if (pd == nullptr) {
			// dynamic_cast 转换失败，比如人家是int，你以为是long，就cast_<long>来调用，结果人家是Derive<int>，就转换失败了
			throw "type is unmatch";
		}
		return pd->data_;
	}
	Any() = default;
	~Any() = default;
	// Any类中的成员变量是unique_ptr类型的，对于这个智能指针，禁止了拷贝构造和赋值，仅支持移动构造
	// 因此Any类中也是这样的。这就是默认实现，不写也无所谓
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;
private:
	// 基类类型
	class Base {
	public:
		// 对于多态用途的基类，需要虚析构函数
		// 若不是虚析构，在delete一个指向派生类的基类指针的时候，可能仅将派生类对象的基类部分西沟了，因此其结果将是未定义的
		// 定义为虚析构，才能确保delete基类指针时运行正确的析构函数版本
		virtual ~Base() = default;
	};
	// 派生类类型
	template<typename T>
	class Derive : public Base {
	public:
		Derive(T data) : data_(data) {};
		T data_;
	};
private:
	// 定义一个基类的指针
	std::unique_ptr<Base> base_;
};

// 实现一个信号量类，来实现Result类的线程通信
// 这个信号量默认的资源数为0
class Semaphore {
public:
	Semaphore(int count = 0) : resLimit_(count), isExit_(false) {}
	~Semaphore() {
		isExit_ = true;
	}
	// 获取一个信号量资源
	void wait() {
		if (isExit_) return;
		std::unique_lock<std::mutex> lock(mtx_);
		// 等待信号量有资源
		cond_.wait(lock, [&]()->bool { return resLimit_ > 0; });
		resLimit_--;
	}
	// 增加一个信号量资源
	void post() {
		if (isExit_) return;
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;
		cond_.notify_all();
	}
private:
	// 对于linux下的程序，由于锁和条件变量不会自动析构，会引起死锁
	// 因此增加了isExit_状态位
	std::atomic_bool isExit_;
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

class Task;  // Task对象的前置声明
// Result类：作为 submitTask 的返回值
// 实现接收提交到线程池的task任务执行完成后的返回值类型
// 主线程中的Result类通过get来获取任务线程的返回值，因此需要与任务线程进行线程通信
class Result {
public:
	Result(std::shared_ptr<Task> task, bool isValid = true);
	// 获取任务执行完的返回值，将task_ run()的返回值放到any_里
	void setVal(Any any);
	// 用户调用这个方法获取task的返回值
	Any get();

private:
	Any any_;  // 存储任务的返回值
	Semaphore sem_;  // 线程通信的信号量
	std::shared_ptr<Task> task_;  // 指向对应的需要获取返回值的task对象
	std::atomic_bool isValid_;  // 返回值是否有效
};


//////////////////////////////    任务抽象基类
class Task {
public:
	// 用户可以自定义任意任务类型，从 Task 继承，重写 run 方法，实现自定义任务处理
	Task();
	virtual Any run() = 0;
	void exec();
	void setResult(Result* res);
private:
	Result* result_;  // 这里用普通的指针就行，因为result的生命周期比task长
};


// 线程池支持的模式
// 这里使用了限定作用域的枚举类型
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

	Thread(ThreadFunc func);
	~Thread();
	void start();

	// 获取线程id
	int getId() const;
private:
	ThreadFunc func_;
	static int generateId_;  // 类的所有对象共享静态成员变量，通过它来得到变化的threadId_
	int threadId_;  // 保存线程ID
};


///////////////////////////////////   线程池类型
/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
	public:
		void run() { // 线程代码... }
};

pool.submitTask(std::make_shared<MyTask>());
*/
class ThreadPool {
public:
	ThreadPool();
	~ThreadPool();
	// 禁止线程池的拷贝和复制
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 设置线程池模式
	void setMode(PoolMode mode);

	// 开启线程池，并设置初始的线程数量，为cpu系统的核心数量
	void start(size_t initThreadSize = std::thread::hardware_concurrency());

	// 设置cached模式下线程阈值
	void setThreadSizeThreshHold(int threshhold);
	// 任务相关
	// 设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(int threshhold);
	// 给线程池提交任务
	Result submitTask(std::shared_ptr<Task> sp);

private:
	// 定义线程函数
	void threadFunc(int threadid);

	// 检查线程池的运行状态
	bool checkRunningState() const;
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
	
	// 需要使用基类的指针或引用才能实现多态
	// 而用户传入的通常会是临时的一个任务对象，出了submitTask语句后就析构了，用指针指向一个析构了的对象是没有意义的
	// 而我们是需要考虑来保持这个任务的生命周期的，当这个任务run执行以后在析构
	// 因此需要使用智能指针
	std::queue<std::shared_ptr<Task>> taskQue_;  // 任务队列
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
