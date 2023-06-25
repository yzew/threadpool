#ifndef THREADPOOL_H
#define THREADPOOL_H
// ʹ�� #ifndef ���� #pragma once ����Ϊ������Ҫ������֧�֣���linux�²�֧��
#include<iostream>
#include<vector>
#include<queue>
#include<memory>  // ����ָ��
#include<atomic>  // ԭ�����ͣ�ʵ���̻߳���
#include<mutex>  // ��
#include<condition_variable>  // ����������ʵ���߳�ͨ��
#include<functional>
#include<thread>
#include<unordered_map>
#include<future>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 10;  // �ȴ���ʱ�䣬s

struct normal   {};  // normal task (for type inference)
struct urgent   {};  // urgent task (for type inference)

// �̳߳�֧�ֵ�ģʽ
// ����ʹ����c++11���޶��������ö������
// ʹ��ʱ������ʾ�ķ��ʣ�PoolMode p = PoolMode::MODE_FIXED;
enum class PoolMode {
	MODE_FIXED,  // �̶��������߳�
	MODE_CACHED,  // �߳������ɶ�̬����
};

//////////////////////////////////   �߳�����
class Thread {
public:
	// �̺߳����������ͣ�Ϊfunction��������
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func)
        : func_(func)
        , threadId_(generateId_++)
    {}
	
    ~Thread() = default;
	// �����߳�
    void start() {
        // �����߳�
        // ThreadPool::start����������thread��������func_
        // ��˴����̵߳�ʱ��ֱ�Ӱ�func_�Ž�ȥ����
        // ����thread�Ĺ��캯��ͨ������ת�����������ݸ��̺߳���
        
        std::thread t(func_, threadId_);
        // ע�⣬�����ǽ��̺߳���д���̳߳����д��
        // ���ǽ��̺߳���д��Thread���У���Ӧ��дΪ
        // std::thread t(&Thread::threadFunc, &thread1);
        // thread1��ʾһ���̶߳�����Ӧ��start����ҲӦ�����βΣ�start(Thread& thread1)

        // ע�⣬����̶߳���t�������������ͻ��Զ�����
        // ���Ҫ����Ϊ�����̣߳�ʹ�ô��̱߳�Ϊ�ػ��̣߳�פ����̨����
        t.detach();
    }

	// ��ȡ�߳�id
	int getId() const {
        return threadId_;
    }
private:
	ThreadFunc func_;
	static int generateId_;  // ������ж�����̬��Ա������ͨ�������õ��仯��threadId_
	int threadId_;  // �����߳�ID
};
// ��̬��Ա����������Ҫ��ʼ��
int Thread::generateId_ = 0;

///////////////////////////////////   �̳߳�����

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
        // �ȴ��̳߳������е��̷߳���
        // ����״̬������ / ִ����
        // �������Ҫ�����̵߳�ͨ��
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
    }
	// ��ֹ�̳߳صĿ����͸���
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// �����̳߳�ģʽ
	void setMode(PoolMode mode) {
        // ����̳߳��Ѿ������ˣ��������ù���ģʽ��
        if (checkRunningState()) return;
        poolMode_ = mode;
    }

	// �����̳߳أ������ó�ʼ���߳�������Ϊcpuϵͳ�ĺ�������
	void start(size_t initThreadSize = std::thread::hardware_concurrency()) {
        // �����̳߳ص�����״̬
        isPoolRunning_ = true;

        // ���ó�ʼ���߳�����
        initThreadSize_ = initThreadSize;
        // ��¼�߳�����
        curThreadSize_ = initThreadSize;

        // �����̶߳���
        for (size_t i = 0; i < initThreadSize_; i++) {
            // ����thread�̶߳����ʱ����Ҫ���̺߳�������thread��
            // ������Thread::start�в�����������̺߳���
            // ͨ�����캯������threadFunc�Ž�ȥ
            // ��������ʹ��bind��������ĳ�Ա������ǰ��Ҫȡ����ַ
            // ����Ҫ��һ����������ʹ�ã��������this����ǰ����
            // ����ʹ������ָ�봴���̶߳����������Զ�����
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));  // c++14
            // ʹ��move����ֵת��Ϊ��Ӧ����ֵ��������
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // unique_ptr�ǲ�����������ģ���֧��һ������ָ��ָ����
            // ��� emplace_back(ptr) �Ĳ����ᱨ��
            // unique_ptr��Ȼ�ر�����ֵ���õĿ����͸�ֵ����֧����ֵ���õĲ�����
        }

        // ���������߳�
        for (size_t i = 0; i < initThreadSize_; i++) {
            // ע�⣬����threads_[i]��ָ��
            // �������߳������start���������������߳�
            threads_[i]->start();
            idleThreadSize_++;
        }
    }

	// ����cachedģʽ���߳���ֵ
	void setThreadSizeThreshHold(int threshhold) {
        if (checkRunningState()) return;
        if (poolMode_ == PoolMode::MODE_CACHED) {
            threadSizeThreshHold_ = threshhold;
        }
    }
	
    // �������
	// ����task�������������ֵ
	void setTaskQueMaxThreshHold(int threshhold) {
        // ����̳߳��Ѿ������ˣ�����������
        if (checkRunningState()) return;
        taskQueMaxThreshHold_ = threshhold;
    }
	
    // ���̳߳��ύ����
    // ����T���ڵ����������ȼ�
    template<typename T = normal, typename Func, typename... Args> 
    // Func&&�����۵������Խ�����ֵ����ֵ����
    // ���autoд��β�÷�������
    // ����ֵΪfuture<>���ͣ���future�е�������Ҫdecltype�����Ƶ�
	auto submitTask(Func&& func, Args&&... args) 
        -> std::future<decltype(func(args...))>
    {  
        bool isNorm;
        if (std::is_same<T, normal>::value) isNorm = true;
        else isNorm = false;
        // ������񣬷����������
        using RType = decltype(func(args...));
        // ͨ������ָ���ӳ���������
        // packaged_task��Ҫ���뷵��ֵ���ͺͲ������ͣ������������ֱ�Ӱ���
        // ��Ҫʹ��std::forward��������ת��
        auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        // ԭ����submitTask������
        // ��ȡ��
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        // �̵߳�ͨ�� �ȴ���������п���
        // û�п���ŵȴ��������Ҫ�ȴ�notFull_
        // ����ʹ����lambda���ʽ����ʽ��������ò����ñ��������ݺ������д����ƶϲ����б�
        //// notFull_.wait(lock, [&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; });
        // ���������еȼ�
        //while (taskQue_.size() == taskQueMaxThreshHold_) {
        //	notFull_.wait(lock);
        //}
        // ����һ��Ҫ���û��ύ�����������������1s�������ж��ύ����ʧ�ܣ�����
        // wait()��һֱ�ȴ����������㣻wait_for()����һ��ʱ�䣻wait_until()���ȵ�һ��ʱ���
        /*notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; });*/
        if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; })) {
            // �ȴ�1s�������Բ�����
            std::cerr << "task queue is full, submit task fail." << std::endl;
            // �������ύʧ�ܣ��򷵻�һ����ֵ
            auto task = std::make_shared<std::packaged_task<RType()>>(
                []()->RType { return RType(); }
                );
            (*task)();
            return task->get_future();
        }

        // �п��࣬����������������
        // ��������е���������д�������µ���ʽ����һ��û�з���ֵ��
        // using Task = std::function<void()>;
        // ����ͨ��д���޷���ֵ��lambda���ʽ����ʽ����task��ʵ��
        if (isNorm) {
            taskQue_.emplace_back([task]() {
            // ִ�����������
            (*task)();  // task�����ú����packaged_task�ĺ�������
            } );
        } else {
            taskQue_.emplace_front([task]() {
            // ִ�����������
            (*task)();  // task�����ú����packaged_task�ĺ�������
            } );
        }
        
        taskSize_++;

        // ������в��գ�֪ͨnotEmpty_
        notEmpty_.notify_all();

        // cachedģʽ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳���
        // ����Ҫ��ǰ�߳�����С�������趨������߳���
        if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_) {
            std::cout << "create new thread" << std::endl;
            // �������߳�
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));  // c++14
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // �����߳�
            threads_[threadId]->start();
            // �޸��̸߳�����صı���
            curThreadSize_++;
            idleThreadSize_++;
        }
        // ����Result������ͨ��task������ĺ������أ�Ҳ������Result����з�װ
        // return task->getResult();  // ���У�task�����߳�ȡ���������������ˣ�����ʹ�����task�����ǲ��е�
        // return Result(task);  // ���ͨ��Result��ά��task���������ڣ���֤������Ҫtask����ֵ��ʱ��task������
        return result;
    }

private:
	// �����̺߳���
    // threadFunc�������̳߳��л�ȡ��ִ������
    // ���̳߳����ж����̺߳��������߳���ִ��
    // �̺߳�����Ҫ����Щ�����������������������̳߳���
    // �����̺߳������Ƕ�����Thread���У��򲻷������ThreadPool�����Щ˽�е�������������
    // ��˽��̺߳���������ThreadPool����
    void threadFunc(int threadid) {
        auto lastTime = std::chrono::high_resolution_clock().now();

        // �����������ִ����ɣ��̳߳زſ��Ի��������߳���Դ�����Ի��Ǳ�����for(;;)
        // �൱��while(true)���߳�ִ��threadFunc���һֱ�ڴ˺����г��Ի�ȡ����
        // while (isPoolRunning_){
        for (;;) {
            // ע�⣬���ǽ���Ҫ��ȡ�����ʱ���ȡ����ȡ��������ͷ�������ִ������
            Task task;
            {
                // ��ȡ��
                std::unique_lock<std::mutex> lock(taskQueMtx_);

                std::cout << "tid:" << std::this_thread::get_id()
                    << "try to get the task!" << std::endl;

                // cachedģʽ�£��п����Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬��60s
                // ���ڳ�����ʼ�߳�����initThreadSize_���̣߳���Ҫ��������л���
                while (taskQue_.size() == 0) {
                    // �̳߳�Ҫ�����������߳���Դ���������Щ����ִ���ˣ�ֱ������whileѭ������ִ��ɾ���̲߳���
                    if (!isPoolRunning_) {
                        // �����̳߳ؽ���ʱ��������ûִ���꣬��������wait״̬������whileѭ�����������������
                        threads_.erase(threadid);
                        std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                        // ����ʱ֪ͨ��pool���ڵ����߳�
                        exitCond_.notify_all();
                        return;
                    }
                    // ÿһ�뷵��һ��
                    // while�������ǳ�ʱ���ػ����������ִ�з���
                    // ������������񣬾�����whileȥ���ѣ�û����ŵȴ�����
                    if (poolMode_ == PoolMode::MODE_CACHED) {
                        // wait_for�����ķ���ֵcv_status������״̬����ʱ�Ͳ���ʱ
                        if (std::cv_status::timeout == 
                            notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
                            auto now = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME
                                && curThreadSize_ > initThreadSize_) {
                                // �����߳�
                                // ��¼�߳���������ر�����ֵ
                                idleThreadSize_--;
                                curThreadSize_--;

                                // ���̶߳�����߳��б���ɾ�� ��û����threadFunc��Ӧ��thread����
                                // threadid => thread���� => ɾ��
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

                // Ϊʲô�����������������أ�����ڽ��и���ϸ�Ĳ���
                // ��������������������֪ͨ����wait��notEmpty_���߳�ִ������
                if (taskQue_.size() > 0) {
                    notEmpty_.notify_all();
                }
                
                // ֪ͨ wait��notFull_�ϵ������ߣ����Լ����ύ����
                notFull_.notify_all();
            }  // ȡ�������ͷ���
            if (task != nullptr) {
                // ִ������
                task();
            }
            idleThreadSize_++;
            lastTime = std::chrono::high_resolution_clock().now();
        }
    }

	// ����̳߳ص�����״̬
	bool checkRunningState() const {
        return isPoolRunning_;
    }
private:
	

	// �߳����
	// �߳��б������̵߳Ĵ���������ThreadPool::start��new�����ģ�����Ҫdelete
	// ���ֱ��ʹ������ָ�롣�̵߳Ļ�unique����
	// std::vector<std::unique_ptr<Thread>> threads_;
	// Ϊ��ʵ��ͨ��threadId_��ѯ����Ӧ���̣߳��������ʹ����map
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;

	size_t threadSizeThreshHold_;  // �߳�����������
	// Ϊʲô����һ��������������threads_.size()�أ���Ϊvector�����̰߳�ȫ��
	std::atomic_int curThreadSize_;  // ��¼��ǰ�̳߳������̵߳�������
	std::atomic_int idleThreadSize_;  // ��¼�����̵߳�����
	int initThreadSize_;  // ��ʼ���߳�������size_t��ǿ�˿���ֲ�ԣ���ʾ�κζ������ܴﵽ����󳤶�
	
	// Task������Ǹ���������
    using Task = std::function<void()>;
	std::deque<Task> taskQue_;  // �������
	std::atomic_uint taskSize_;  // ��������������ǵ��̰߳�ȫ���⣬ʹ����������ԭ������ʵ���̻߳���
	size_t taskQueMaxThreshHold_;  // �����������������

	// ʵ���߳�ͨ��
	std::mutex taskQueMtx_;  // ��֤������е��̰߳�ȫ
	std::condition_variable notEmpty_;  // ������в���
	std::condition_variable notFull_;  // ������в���
	std::condition_variable exitCond_;  // �ȴ��߳���Դȫ������

	PoolMode poolMode_;  // ��ǰ�̳߳صĹ���ģʽ

	std::atomic_bool isPoolRunning_;  // ��ʾ��ǰ�̳߳ص�����״̬
	
};


#endif // !THREADPOOL_H
