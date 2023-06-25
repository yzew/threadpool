#include"threadpool.h"

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 10;  // �ȴ���ʱ�䣬s
//////////////////////  Task����ʵ��
Task::Task(): result_(nullptr) {}
// ��task��Ӧ��result����й����ʱ����ã����´�����result�󶨵���ǰ��task��
// ��Ϊtask��ָ��Result�ĳ�Ա����result_��ֵ��֮��Ϳ���ͨ�����ָ�룬��task�Ľ���浽Result��������
void Task::setResult(Result* res) {
	result_ = res;
}
void Task::exec() {
	// run����������ķ���ֵ������Result����
	if (result_ != nullptr) {
		result_->setVal(run());  // run�����﷢����̬����
	}
}

//////////////////////  �̳߳ط���ʵ��
ThreadPool::ThreadPool()
	: initThreadSize_(4)
	, taskSize_(0)
	, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
	, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
	, poolMode_(PoolMode::MODE_FIXED)
	, isPoolRunning_(false)
	, curThreadSize_(0)
	, idleThreadSize_(0)
{}

ThreadPool::~ThreadPool() {
	isPoolRunning_ = false;
	// �ȴ��̳߳������е��̷߳���
	// ����״̬������ / ִ����
	// �������Ҫ�����̵߳�ͨ��
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all();
	exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
}

// ����cachedģʽ���߳���ֵ
void ThreadPool::setThreadSizeThreshHold(int threshhold) {
	if (checkRunningState()) return;
	if (poolMode_ == PoolMode::MODE_CACHED) {
		threadSizeThreshHold_ = threshhold;
	}
}
// �����̳߳ع���ģʽ
void ThreadPool::setMode(PoolMode mode) {
	// ����̳߳��Ѿ������ˣ��������ù���ģʽ��
	if (checkRunningState()) return;
	poolMode_ = mode;
}

// ����̳߳ص�����״̬
bool ThreadPool::checkRunningState() const {
	return isPoolRunning_;
}

// �������
// ����task�������������ֵ
void ThreadPool::setTaskQueMaxThreshHold(int threshhold) {
	// ����̳߳��Ѿ������ˣ�����������
	if (checkRunningState()) return;
	taskQueMaxThreshHold_ = threshhold;
}

// �����̳߳أ������ó�ʼ���߳�����
void ThreadPool::start(size_t initThreadSize) {
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
		// unique_ptr�ǲ�������������ģ���֧��һ������ָ��ָ����
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

// threadFunc�������̳߳��л�ȡ��ִ������
// ���̳߳����ж����̺߳��������߳���ִ��
// �̺߳�����Ҫ����Щ�����������������������̳߳���
// �����̺߳������Ƕ�����Thread���У��򲻷������ThreadPool�����Щ˽�е�������������
// ��˽��̺߳���������ThreadPool����
void ThreadPool::threadFunc(int threadid) {
	auto lastTime = std::chrono::high_resolution_clock().now();

	// �����������ִ����ɣ��̳߳زſ��Ի��������߳���Դ�����Ի��Ǳ�����for(;;)
	// �൱��while(true)���߳�ִ��threadFunc���һֱ�ڴ˺����г��Ի�ȡ����
	// while (isPoolRunning_){
	for (;;) {
		// ע�⣬���ǽ���Ҫ��ȡ�����ʱ���ȡ����ȡ��������ͷ�������ִ������
		std::shared_ptr<Task> task;
		{
			// ��ȡ��
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid:" << std::this_thread::get_id()
				<< "���Ի�ȡ����!" << std::endl;

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
				//// �ж�һ����������ű����ѵģ�������Ϊ�̳߳ؽ����Ż��ѵ�
				//if (!isPoolRunning_) {
				//	// ���̶߳�����߳��б���ɾ�� ��û����threadFunc��Ӧ��thread����
				//	// threadid => thread���� => ɾ��
				//	threads_.erase(threadid);
				//	std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
				//	// ֪ͨ�����̵߳�������������������������߳�
				//	exitCond_.notify_all();
				//	return;
				//}
			}

			idleThreadSize_--;

			std::cout << "tid:" << std::this_thread::get_id()
				<< "�ɹ���ȡ����!" << std::endl;

			task = taskQue_.front(); taskQue_.pop();
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
			// ִ�����񣬲�������ķ���ֵsetVal��������Result
			// task->run();
			// run�Ǹ���Ҫ��д�ķ��������ǲ����ܽ����ӵ��ⲿ�ֹ���д��run����
			// ����������Task����������exec�����������������ִ��run�������ӵĹ���
			task->exec();
		}
		idleThreadSize_++;
		lastTime = std::chrono::high_resolution_clock().now();
	}
}

// ���̳߳��ύ����
// �����ߣ���ȡ����while(��) {wait}���ύ����notify_all
Result ThreadPool::submitTask(std::shared_ptr<Task> sp) {
	// ��ȡ��
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	// �̵߳�ͨ�� �ȴ���������п���
	// û�п���ŵȴ��������Ҫ�ȴ�notFull_
	// ����ʹ����lambda����ʽ����ʽ��������ò����ñ��������ݺ������д����ƶϲ����б�
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
		// �������ύʧ�ܣ���
		return Result(sp, false);
	}

	// �п��࣬����������������
	taskQue_.emplace(sp);
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
	return Result(sp);
}


/////////////////////////////////// �̷߳���ʵ��
int Thread::generateId_ = 0;

Thread::Thread(ThreadFunc func)
	: func_(func)
	, threadId_(generateId_++)
{}

Thread::~Thread() {

}

int Thread::getId() const {
	return threadId_;
}

// �����߳�
void Thread::start() {
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



/////////////////////////////  Result��ʵ��
Result::Result(std::shared_ptr<Task> task, bool isValid) 
	: task_(task), isValid_(isValid){
	// ˳�㽫result����󶨵�task��
	task_->setResult(this);
}

Any Result::get() {
	if (!isValid_) return "";  // �������ֵ��Ч������g����
	sem_.wait();  // task�������û��ִ���꣬������û����߳�
	return std::move(any_);

}

void Result::setVal(Any any) {
	// �洢task�ķ���ֵ
	this->any_ = std::move(any);
	sem_.post();
}