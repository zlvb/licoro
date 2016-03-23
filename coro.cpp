#include "coro.h"
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <deque>
#include <vector>
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

template <class T>
class VectorBase
{
protected:
    typedef typename std::deque<T> CntType;
public:
    typedef typename CntType::iterator iterator;
    size_t size() const
    {
        return m_data.size();
    }

    iterator begin()
    {
        return m_data.begin();
    }

    iterator end()
    {
        return m_data.end();
    }

    void clear()
    {
        m_data.clear();
    }

    bool empty() const
    {
        return m_data.empty();
    }

    void push(const T &v)
    {
        m_data.push_back(v);
    }

protected:
    CntType m_data;
};

struct sleep_compare
{
    bool operator()(const std::pair<int64_t, int64_t>& x, const std::pair<int64_t, int64_t>& y) const
    {
        return x.first < y.first;
    }
};

template <class T, class Compare>
class OrderedVector : public VectorBase<T>
{
public:
    typedef typename VectorBase<T>::iterator iterator;
    iterator begin() { return VectorBase<T>::m_data.begin(); }
    iterator end() { return VectorBase<T>::m_data.end(); }

    void add(const T& t)
    {
        iterator i = std::lower_bound(begin(), end(), t, cmp);
        if (i == end() || cmp(t, *i))
            VectorBase<T>::m_data.insert(i, t);
    }
    void remove(int idx)
    {
        VectorBase<T>::m_data.erase(begin() + idx);
    }

    void erase(const T &value)
    {
        auto bounds = std::equal_range(begin(), end(), value);
        auto last = end() - std::distance(bounds.first, bounds.second);
        std::swap_ranges(bounds.first, bounds.second, last);
        VectorBase<T>::m_data.erase(last, end());
    }

    T &operator[](size_t idx)
    {
        return VectorBase<T>::m_data[idx];
    }

private:
    Compare cmp;
};

typedef OrderedVector<std::pair<int64_t, int64_t>, sleep_compare> SleepList;

static ucontext_t uctx_main;
static ucontext_t *gs_uctx_current = &uctx_main;
static coro_handle *gs_current_handle = NULL;
static std::vector<coro_handle*> gs_FreeList;
static SleepList gs_SleepList;
static int64_t gs_id = 1;
static CoroMap gs_coromap;
static size_t gs_maxcorocount = 0;
static size_t gs_finished = 0;
static size_t gs_timeout = 0;
static const struct INITFinish
{
    INITFinish()
    {
        printf("coro init finish\n");
    }
}gs_fi;

static void coro_agent(uint32_t low32, uint32_t hi32)
{
    uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
    coro_handle *hcoro = (coro_handle*)ptr;
    hcoro->func(hcoro->custom_data);
    gs_FreeList.push_back(hcoro);
    gs_uctx_current = &uctx_main;
    gs_current_handle = NULL;
    gs_finished++;
    if (0 == _setjmp(hcoro->jmpbuf))
    {
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
        return CORO_OK;
    }

    if (hcoro->sleepsec == 0 && hcoro->started)
    {
        return CORO_ALREDY_RUN;
    }

    if (_setjmp(hcoro->last_jmpbuf) == 0)
    {
        if (!hcoro->started)
        {
            getcontext(&hcoro->uctx);
            hcoro->uctx.uc_stack.ss_sp = hcoro->stack_mem;
            hcoro->uctx.uc_stack.ss_size = hcoro->stack_size;
            hcoro->uctx.uc_link = &uctx_main;

            uintptr_t ptr = (uintptr_t)hcoro;
            makecontext(&hcoro->uctx, (void (*)(void))coro_agent, 2, (uint32_t)ptr, (uint32_t)(ptr >> 32));
            hcoro->started = true;
            gs_current_handle = hcoro;
            ucontext_t *uctx_parent = gs_uctx_current;
            gs_uctx_current = &hcoro->uctx;
            swapcontext(uctx_parent, &hcoro->uctx);
        }
        else
        {
            gs_current_handle = hcoro;
            if (gs_current_handle->sleepsec > 0)
            {
                gs_SleepList.erase(std::make_pair(hcoro->sleepsec, hcoro->id));
            }
            gs_current_handle->sleepsec = 0;
            _longjmp(hcoro->jmpbuf, 1);
        }
    }
    return CORO_OK;
}

int coro_yield(int64_t wake_time)
{
    if (!gs_current_handle)
        return CORO_NOT_IN_CORO;

    if (wake_time > 0)
    {
        gs_current_handle->sleepsec = wake_time;
        gs_SleepList.add(std::make_pair(gs_current_handle->sleepsec, gs_current_handle->id));
    }
    else
    {
        gs_current_handle->sleepsec = TIME_INFINITY;
    }
    if (0 == _setjmp(gs_current_handle->jmpbuf))
    {
        _longjmp(gs_current_handle->last_jmpbuf, 1);
    }

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
        const auto &n = gs_SleepList[0];
        if (now < n.first)
            break;

        auto it = gs_coromap.find(n.second);
        gs_SleepList.remove(0);
        if (it != gs_coromap.end() && it->second->sleepsec > 0)
        {
            gs_timeout++;
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

void coro_get_statistic(size_t *pMaxCoro, size_t *pSleepCoro, size_t *pTotal, size_t *pFinished, size_t *pTimeout)
{
    *pSleepCoro = gs_SleepList.size();
    *pTotal = gs_coromap.size();
    *pMaxCoro = gs_maxcorocount;
    *pFinished = gs_finished;
    *pTimeout = gs_timeout;
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
