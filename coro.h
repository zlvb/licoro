/**
*
* @file     coro.h
* @brief    协程库头文件
*
* @author cppzhang <cppzhang@tencent.com>
* @date 2014-5-7   17:02
*
*
* Copyright (c)  2013-2014, Tencent
* All rights reserved.
*
*/
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
int coro_resume_at(int64_t id, int64_t wake_time);

#define coro_resume(coro) coro_resume_at(coro, 0)

// 挂起一个协程
// wake_time       何时唤醒（微秒）
int coro_yield(int64_t wake_time = 0);

// 协程调度函数
// 返回是否还有协程等待调度
bool coro_schedule(const timeval &tv);
bool coro_schedule(int64_t now);

// 获得状态
void coro_get_statistic(size_t *pMaxCoro, size_t *pSleepCoro, size_t *pTotal, size_t *pFinished, size_t *pTimeout);

// 获得当前栈大小
size_t coro_get_current_stack_used();

// 返回当前的协程id
int64_t coro_getid();
coro_handle *coro_get_current_handle();


// 创建并开始一个协程
#define coro_go(...) coro_resume(coro_new(__VA_ARGS__))

#endif // coro_h__
