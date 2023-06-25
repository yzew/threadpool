#include"threadpool.h"
#include<chrono>

class MyTask : public Task
{
public:
	MyTask(int begin, int end)
		: begin_(begin)
		, end_(end)
	{}
	Any run() {
		std::cout << "tid:" << std::this_thread::get_id()
			<< "begin!" << std::endl;
		std::this_thread::sleep_for(std::chrono::seconds(5));
		int sum = 0;
		for (int i = begin_; i <= end_; i++)
			sum += i;
		std::cout << "tid:" << std::this_thread::get_id()
			<< "end!" << std::endl;
		return sum;
	}
private:
	int begin_;
	int end_;
};

// const int TASK_MAX_THRESHHOLD = 4;�������б���Ϊ������ĸ�����
// ��ʱ�̳߳�����4���̡߳�ÿ������ִ��5s���ύ���ɸ�����󣬻���һЩ�����ύʧ�ܣ��ȴ�������1s��

int main() {
	{
		ThreadPool pool;
		// �û��Լ������̳߳صĹ���ģʽ
		//pool.setMode(PoolMode::MODE_CACHED);
		// �����̳߳�
		pool.start(2);
		// Master - Slave�߳�ģ��
		// Master�߳������ֽ�����Ȼ�������Slave�̷߳�������
		// �ȴ�����Slave�߳�ִ�������񣬷��ؽ��
		// Master�̺߳ϲ����������������
		Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100));
		Result res2 = pool.submitTask(std::make_shared<MyTask>(101, 200));
		Result res3 = pool.submitTask(std::make_shared<MyTask>(201, 300));
		/*int sum = res1.get().cast_<int>() + res2.get().cast_<int>() + res3.get().cast_<int>();
		std::cout << sum << std::endl;
		int sum2 = 0;
		for (int i = 1; i <= 300; i++) {
			sum2 += i;
		}
		std::cout << "answer:" << sum2 << std::endl;*/
	}
	getchar();
}