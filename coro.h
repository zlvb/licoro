#ifndef coro_h__
#define coro_h__

#include <stdint.h>
#include <stddef.h>

#define CORO_OK 0
#define CORO_TIMEOUT 1
#define CORO_ERROR_ID -5
#define CORO_ALREDY_RUN -3
#define CORO_NOT_IN_CORO -1
#define CORO_RESUME_MAIN -2
// 协程句柄
struct coro_handle;
struct timeval;
typedef int64_t CORO_ID;
// 协程函数
// custom_data      自定义数据
typedef void (*corofunc)(void *custom_data);

// 创建协程
// coro             协程函数
// custom_data      自定义数据
// stack_size       栈大小
// 返回协程句柄
coro_handle *coro_new(corofunc coro, void *custom_data = 0, uint32_t stack_size = 65536);

// 切换到一个挂起的协程使之继续执行
// hcoro 协程句柄
int coro_resume_at(coro_handle *hcoro, int64_t wake_time);
int coro_resume_at(CORO_ID id, int64_t wake_time);

#define coro_resume(coro) coro_resume_at(coro, 0)

// 挂起一个协程
// sleep_time       唤醒的时刻（微秒）
int coro_yield(int64_t wake_time);
#define coro_yield_dangerous() coro_yield(0) // 永久挂起，除非主动唤醒

// 协程调度函数
// 返回是否还有协程等待调度
bool coro_schedule(const timeval &tv);
bool coro_schedule(int64_t now);

// 获得状态
void coro_get_statistic(size_t *pMaxCoro, size_t *pSleepCoro, size_t *pTotal);

// 获得当前栈大小
size_t coro_get_current_stack_used();

// 返回当前的协程id
CORO_ID coro_getid();
coro_handle *coro_get_current_handle();


// 创建并开始一个协程
#define coro_go(...) coro_resume(coro_new(__VA_ARGS__))

#define SEC2WAKETIME(sec, tv_now) ((tv_now).tv_sec * 1000000 + (tv_now).tv_usec + (sec) * 1000000)
#define MSEC2WAKETIME(msec, tv_now) ((tv_now).tv_sec * 1000000 + (tv_now).tv_usec + (msec) * 1000)
#endif // coro_h__
