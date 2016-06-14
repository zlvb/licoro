#include "coro.h"
#include "common/Log.h"
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <vector>
#include <map>
#include <algorithm>
#include <setjmp.h>
#include <tr1/unordered_map>
#include <sys/time.h>

#define TIME_INFINITY -1
#define TIME_TIMEOUT -2

struct coro_handle
{
    ucontext_t uctx;
    void *stack_mem;
    void *custom_data;
    uint32_t stack_size;
    corofunc func;
    bool started;
    int64_t id;
    int64_t sleepsec;
    jmp_buf last_jmpbuf;
    jmp_buf jmpbuf;
};

typedef std::tr1::unordered_map<int64_t, coro_handle*> CoroMap;
typedef std::map<int64_t, int64_t> SleepList;

static ucontext_t uctx_main;
static ucontext_t *gs_uctx_current = &uctx_main;
static coro_handle *gs_current_handle = NULL;
static std::vector<coro_handle*> gs_FreeList;
static SleepList gs_SleepList;
static int64_t gs_id = 1;
static CoroMap gs_coromap;
static size_t gs_maxcorocount = 0;

static void coro_agent(uint32_t low32, uint32_t hi32)
{
    uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
    coro_handle *hcoro = (coro_handle*)ptr;
    hcoro->func(hcoro->custom_data);    
    gs_FreeList.push_back(hcoro);  
    gs_uctx_current = hcoro->uctx.uc_link;
    gs_current_handle = NULL;
    if (0 == _setjmp(hcoro->jmpbuf))
    {
		LOG_DEBUG("CORO out: %p %ld cur=%p", hcoro, hcoro->id, gs_current_handle);
        _longjmp(hcoro->last_jmpbuf, 1);
    }
}

coro_handle *coro_new( corofunc coro, void *custom_data, uint32_t stack_size )
{
    coro_handle *hcoro = (coro_handle*)malloc(sizeof(coro_handle));
    hcoro->func = coro;
    hcoro->stack_size = stack_size;
    hcoro->custom_data = custom_data;
    hcoro->stack_mem = malloc(stack_size);
    hcoro->started = false;
    hcoro->id = gs_id++;
    hcoro->sleepsec = 0;
    gs_coromap[hcoro->id] = hcoro;
    if (gs_coromap.size() > gs_maxcorocount)
    {
        gs_maxcorocount = gs_coromap.size();
    }
    return hcoro;
}

static void coro_del( coro_handle *hcoro )
{
	LOG_DEBUG("CORO DEL: %p %ld cur=%p", hcoro, hcoro->id, gs_current_handle);
    gs_coromap.erase(hcoro->id);
    free(hcoro->stack_mem);
    free(hcoro);
}

int coro_resume_at(coro_handle *hcoro, int64_t wake_time)
{	
    if (!hcoro->started)
    {
        if (&hcoro->uctx == gs_uctx_current)
            return CORO_RESUME_MAIN;
    }

    if (wake_time > 0)
    {
		hcoro->sleepsec = wake_time;
		gs_SleepList.insert(std::make_pair(hcoro->sleepsec, hcoro->id));
        return CORO_OK;
    }

    if (hcoro->sleepsec == 0 && hcoro->started)
    {
        return CORO_ALREDY_RUN;
    }

	coro_handle *old_hcoro = gs_current_handle;
    if (_setjmp(hcoro->last_jmpbuf) == 0)
    {        
        if (!hcoro->started)
        {
            getcontext(&hcoro->uctx);    
            hcoro->uctx.uc_stack.ss_sp = hcoro->stack_mem;
            hcoro->uctx.uc_stack.ss_size = hcoro->stack_size;
            hcoro->uctx.uc_link = gs_uctx_current;

            uintptr_t ptr = (uintptr_t)hcoro;
            makecontext(&hcoro->uctx, (void (*)(void))coro_agent, 2, (uint32_t)ptr, (uint32_t)(ptr >> 32));   
            hcoro->started = true;            
            gs_current_handle = hcoro;
            ucontext_t *uctx_parent = gs_uctx_current;
            gs_uctx_current = &hcoro->uctx;
			LOG_DEBUG("CORO in: %p %ld cur=%p", hcoro, hcoro->id, gs_current_handle);
            swapcontext(uctx_parent, &hcoro->uctx);
        }   
        else
        {
            gs_current_handle = hcoro;
            if (gs_current_handle->sleepsec > 0)
            {
				auto it = gs_SleepList.find(hcoro->sleepsec);
				for (; it->first == hcoro->sleepsec; ++it)
				{
					if (it->second == hcoro->id)
					{
						gs_SleepList.erase(it);
						break;
					}
				}          
            }
            gs_current_handle->sleepsec = 0;
			LOG_DEBUG("CORO in: %p %ld cur=%p", hcoro, hcoro->id, gs_current_handle);
            _longjmp(hcoro->jmpbuf, 1);
        } 
    }

	gs_current_handle = old_hcoro;
	LOG_DEBUG("CORO out: %p %ld cur=%p", hcoro, hcoro->id, gs_current_handle);
    return CORO_OK;
}

int coro_yield(int64_t wake_time)
{
    if (!gs_current_handle)
        return CORO_NOT_IN_CORO;
	
    if (wake_time > 0)
    {
        gs_current_handle->sleepsec = wake_time;
        gs_SleepList.insert(std::make_pair(gs_current_handle->sleepsec, gs_current_handle->id));
    }
    else
    {
        gs_current_handle->sleepsec = TIME_INFINITY;
    }
	
    if (0 == _setjmp(gs_current_handle->jmpbuf))
    {
		LOG_DEBUG("CORO out: %p %ld cur=%p", gs_current_handle, gs_current_handle->id, gs_current_handle);
        _longjmp(gs_current_handle->last_jmpbuf, 1);
    }
	LOG_DEBUG("CORO in: %p %ld cur=%p", gs_current_handle, gs_current_handle->id, gs_current_handle);

    if (gs_current_handle->sleepsec == TIME_TIMEOUT)
    {
        gs_current_handle->sleepsec = 0;
        return CORO_TIMEOUT;
    }
    return CORO_OK;
}

int coro_resume_at(int64_t cid, int64_t wake_time)
{
    auto it = gs_coromap.find(cid);
    if (it != gs_coromap.end())
    {
        return coro_resume_at(it->second, wake_time);
    }
    return CORO_ERROR_ID;
}


bool coro_schedule(const timeval &tv)
{    
    int64_t now = tv.tv_sec * 1000000 + tv.tv_usec;
    return coro_schedule(now);
}

bool coro_schedule(int64_t now)
{
    while (!gs_SleepList.empty())
    {
        const auto &n = gs_SleepList.begin();
        if (now < n->first)
            break;

        auto it = gs_coromap.find(n->second);
		int64_t waketime = n->first;
        gs_SleepList.erase(n);		
        if (it != gs_coromap.end() && it->second->sleepsec > 0 && it->second->sleepsec == waketime)
        {
			LOG_DEBUG("%ld %ld", it->second->sleepsec, waketime);
            it->second->sleepsec = TIME_TIMEOUT;
            int ret = coro_resume_at(it->second, 0);
            assert(ret == 0);
            (void)ret;
        }
    }

    for (auto it = gs_FreeList.begin(); it != gs_FreeList.end(); ++it)
    {
        coro_del(*it);
    }
    gs_FreeList.clear();
    return !gs_coromap.empty();
}


int64_t coro_getid()
{
    if (!gs_current_handle)
        return 0;
    return gs_current_handle->id;
}

coro_handle *coro_get_current_handle()
{
    return gs_current_handle;
}

void coro_get_statistic(size_t *pMaxCoro, size_t *pSleepCoro, size_t *pTotal)
{
    *pSleepCoro = (int)gs_SleepList.size();
    *pTotal = gs_coromap.size();
    *pMaxCoro = gs_maxcorocount;
}

size_t coro_get_current_stack_used()
{
    char curstacp = 0;
    if (!gs_current_handle)
        return 0;
    char* top = (char*)gs_current_handle->stack_mem + gs_current_handle->stack_size;
    size_t ssize = top - &curstacp;
    return ssize;
}
