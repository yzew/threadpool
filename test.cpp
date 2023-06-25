#include"threadpool.h"

int sum1(int a, int b) {
    std::cout << a + b << std::endl;
	return a + b;
}

int main() {
    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHED);
    pool.start(2);
    std::future<int> result = pool.submitTask(sum1, 1, 2);
    std::future<int> result1 = pool.submitTask<normal>(sum1, 1, 2);
    std::future<int> result2 = pool.submitTask(sum1, 1, 2);
    std::future<int> result3 = pool.submitTask(sum1, 1, 2);
    std::future<int> result4 = pool.submitTask(sum1, 1, 2);
    std::future<int> result5 = pool.submitTask<urgent>(sum1, 2, 2);
    // std::cout << result.get() << result1.get() << result2.get() << result3.get() << result4.get() << result5.get() << std::endl;
}