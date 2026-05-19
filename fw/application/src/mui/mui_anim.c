#include "mui_anim.h"
#include "mui_core.h"
#include "app_timer.h"
#include "nrf_log.h"

#define LV_ANIM_RESOLUTION 1024
#define LV_ANIM_RES_SHIFT 10

#define MUI_ANIM_TICK_INTERVAL_MS 20
#define MUI_ANIM_REPEAT_INFINITE 0xFFFF


ARRAY_DEF(mui_anim_ptr_array, mui_anim_t *, M_PTR_OPLIST);

APP_TIMER_DEF(m_anim_tick_tmr);
bool m_anim_tmr_started = false;
mui_anim_ptr_array_t m_anim_ptr_array;


static int32_t lv_anim_path_cubic_bezier(const mui_anim_t * a, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    /*Calculate the current step*/
    uint32_t t = lv_map(a->act_time, 0, a->time, 0, LV_BEZIER_VAL_MAX);
    int32_t step = lv_cubic_bezier(t, x1, y1, x2, y2);

    int32_t new_value;
    new_value = step * (a->end_value - a->start_value);
    new_value = new_value >> LV_BEZIER_VAL_SHIFT;
    new_value += a->start_value;

    return new_value;
}


int32_t lv_anim_path_linear(const mui_anim_t * a)
{
    /*Calculate the current step*/
    int32_t step = lv_map(a->act_time, 0, a->time, 0, LV_ANIM_RESOLUTION);

    /*Get the new value which will be proportional to `step`
     *and the `start` and `end` values*/
    int32_t new_value;
    new_value = step * (a->end_value - a->start_value);
    new_value = new_value >> LV_ANIM_RES_SHIFT;
    new_value += a->start_value;

    return new_value;
}


int32_t lv_anim_path_ease_in(const mui_anim_t * a)
{
    return lv_anim_path_cubic_bezier(a, LV_BEZIER_VAL_FLOAT(0.42), LV_BEZIER_VAL_FLOAT(0),
                                     LV_BEZIER_VAL_FLOAT(1), LV_BEZIER_VAL_FLOAT(1));
}

int32_t lv_anim_path_ease_out(const mui_anim_t * a)
{
    return lv_anim_path_cubic_bezier(a, LV_BEZIER_VAL_FLOAT(0), LV_BEZIER_VAL_FLOAT(0),
                                     LV_BEZIER_VAL_FLOAT(0.58), LV_BEZIER_VAL_FLOAT(1));
}

int32_t lv_anim_path_ease_in_out(const mui_anim_t * a)
{
    return lv_anim_path_cubic_bezier(a, LV_BEZIER_VAL_FLOAT(0.42), LV_BEZIER_VAL_FLOAT(0),
                                     LV_BEZIER_VAL_FLOAT(0.58), LV_BEZIER_VAL_FLOAT(1));
}

int32_t lv_anim_path_overshoot(const mui_anim_t * a)
{
    return lv_anim_path_cubic_bezier(a, 341, 0, 683, 1300);
}

int32_t lv_anim_path_bounce(const mui_anim_t * a)
{
    /*Calculate the current step*/
    int32_t t = lv_map(a->act_time, 0, a->time, 0, LV_BEZIER_VAL_MAX);
    int32_t diff = (a->end_value - a->start_value);

    /*3 bounces has 5 parts: 3 down and 2 up. One part is t / 5 long*/

    if(t < 408) {
        /*Go down*/
        t = (t * 2500) >> LV_BEZIER_VAL_SHIFT; /*[0..1024] range*/
    }
    else if(t >= 408 && t < 614) {
        /*First bounce back*/
        t -= 408;
        t    = t * 5; /*to [0..1024] range*/
        t    = LV_BEZIER_VAL_MAX - t;
        diff = diff / 20;
    }
    else if(t >= 614 && t < 819) {
        /*Fall back*/
        t -= 614;
        t    = t * 5; /*to [0..1024] range*/
        diff = diff / 20;
    }
    else if(t >= 819 && t < 921) {
        /*Second bounce back*/
        t -= 819;
        t    = t * 10; /*to [0..1024] range*/
        t    = LV_BEZIER_VAL_MAX - t;
        diff = diff / 40;
    }
    else if(t >= 921 && t <= LV_BEZIER_VAL_MAX) {
        /*Fall back*/
        t -= 921;
        t    = t * 10; /*to [0..1024] range*/
        diff = diff / 40;
    }

    if(t > LV_BEZIER_VAL_MAX) t = LV_BEZIER_VAL_MAX;
    if(t < 0) t = 0;
    int32_t step = lv_bezier3(t, LV_BEZIER_VAL_MAX, 800, 500, 0);

    int32_t new_value;
    new_value = step * diff;
    new_value = new_value >> LV_BEZIER_VAL_SHIFT;
    new_value = a->end_value - new_value;

    return new_value;
}

//put anim tick into event queue process to avoid race conditions
static void mui_anim_tick_tmr_cb(void* p_context) {
    mui_event_t mui_event = {.id = MUI_EVENT_ID_ANIM};
    mui_post(mui(), &mui_event);
}


static void mui_anim_tick_handler() {
    mui_anim_ptr_array_it_t it;
    int32_t err_code;
    bool mui_update_required = false;
    mui_anim_ptr_array_it(it, m_anim_ptr_array);

    while (!mui_anim_ptr_array_end_p(it)) {
        mui_anim_t *p_anim = *mui_anim_ptr_array_ref(it);
        bool removed = false;   /* v8.2-fix8: 跟踪本次循环里是否做过 remove */

        p_anim->act_time += MUI_ANIM_TICK_INTERVAL_MS;
        if (p_anim->act_time > p_anim->time) {
            if (p_anim->repeat_cnt == MUI_ANIM_REPEAT_INFINITE || (++p_anim->run_cnt) > p_anim->repeat_cnt) {
                // reset
                p_anim->act_time = 0;
                p_anim->current_value = p_anim->start_value;
                p_anim->run_cnt = 0;
            } else {
                if(p_anim->auto_restart) {
                    p_anim->act_time = 0;
                    int32_t start_value = p_anim->start_value;
                    p_anim->current_value = p_anim->end_value;
                    p_anim->start_value = p_anim->end_value;
                    p_anim->end_value = start_value;
                    p_anim->run_cnt = 0;
                }else {
                    // reached end of animation
                    if(p_anim->current_value != p_anim->end_value){
                        p_anim->current_value = p_anim->end_value;
                        p_anim->exec_cb(p_anim->var, p_anim->end_value);
                        mui_update_required = true;
                        NRF_LOG_INFO("ANIM FIX");
                    }

                    mui_anim_ptr_array_remove(m_anim_ptr_array, it);
                    removed = true;
                }
            }
        } else {
            int32_t new_value = p_anim->path_cb(p_anim);
            if (new_value != p_anim->current_value) {
                p_anim->current_value = new_value;
                p_anim->exec_cb(p_anim->var, new_value);
                mui_update_required = true;
            }
        }

        /* v8.2-fix8 关键修复: M-LIB 的 _remove(it) 内部已经把 it 推到了被
         * 移除元素之后的那一个 (跟 STL erase 同义). 旧代码无条件再调一次
         * _next(it), 等于跳过紧邻 remove 后面的那个 anim, 导致它这一轮
         * tick 永远不被处理. 实际症状:
         *
         *   list_view 的 gap_anim 紧跟在某个一次性 text_anim 之后 ——
         *   text_anim 结束被 remove 时, gap_anim 被 next() 跳过, 该轮
         *   item_gap 不更新; 多个 mui_update 排队又触发更多 redraw,
         *   gap_anim 看起来"卡住", 直到下一个 20ms tick 才动一格.
         *   极端情况下 (event_queue 因为上面那个优先级 bug 排满) 这条
         *   anim 几秒都跑不完, 列表就一直处于折叠 (item_gap=0, items
         *   重叠在 y=0) 的状态 —— 用户看到的就是"返回徽章大全后列表空了".
         *
         * 修复: 只有没 remove 时才 next(); remove 已经天然推进了迭代器. */
        if (!removed) {
            mui_anim_ptr_array_next(it);
        }
    }

    // stop timer
    if (mui_anim_ptr_array_size(m_anim_ptr_array) <= 0 && m_anim_tmr_started) {
        m_anim_tmr_started = false;
        err_code = app_timer_stop(m_anim_tick_tmr);
        NRF_LOG_INFO("stop anim timer..");
        APP_ERROR_CHECK(err_code);
    }

    //redraw event
    if(mui_update_required){
        mui_update(mui());
    }
}

static mui_anim_t* mui_anim_remove_ptr(mui_anim_t* p_anim){
    mui_anim_ptr_array_it_t it;
    mui_anim_ptr_array_it(it, m_anim_ptr_array);

    while (!mui_anim_ptr_array_end_p(it)) {
        mui_anim_t *p_cur_anim = *mui_anim_ptr_array_ref(it);

        if(p_anim == p_cur_anim){
            mui_anim_ptr_array_remove(m_anim_ptr_array, it);
            return p_cur_anim;
        }
        mui_anim_ptr_array_next(it);
    }
    /* v8.2-fix8: 没命中也得有显式 return, 否则函数声明的返回值是
     * mui_anim_t* 但实际返回的是 r0 里上一次调用残留的值 (gcc -Os
     * 下落出函数体的行为是 UB). 调用方目前都丢掉返回值, 所以不会
     * 显式炸, 但这是个潜伏的 UB, 不要留. */
    return NULL;
}

void mui_anim_core_init() {
    /* v8.2-fix8: 显式 init 全局 anim 指针数组. 它是 global, BSS 段
     * 会被 zero-initialized, 而 M-LIB 的 ARRAY 容器对全零 buffer 在
     * 大多数操作上能容忍但并不保证 —— 这里花一行调一次 init, 避免
     * 在未来 M-LIB 升级 / 实现细节变化时炸. */
    mui_anim_ptr_array_init(m_anim_ptr_array);

    int32_t err_code = app_timer_create(&m_anim_tick_tmr, APP_TIMER_MODE_REPEATED, mui_anim_tick_tmr_cb);
    APP_ERROR_CHECK(err_code);
}

void mui_anim_core_event(mui_event_t* p_event){
    mui_anim_tick_handler();
}

void mui_anim_init(mui_anim_t *p_anim){
    p_anim->start_value = 0;
    p_anim->end_value = 100;
    p_anim->current_value = 0;
    p_anim->time = 500;
    p_anim->var = NULL;
    p_anim->exec_cb = NULL;
    p_anim->repeat_cnt = 1;
    p_anim->run_cnt = 0;
    p_anim->auto_restart = false;
    p_anim->path_cb = lv_anim_path_ease_in_out;
}

void mui_anim_start(mui_anim_t *p_anim) {
    // todo check var exists
    assert(p_anim);
    assert(p_anim->var != NULL);
    int32_t err_code;
    mui_anim_remove_ptr(p_anim);
    mui_anim_ptr_array_push_back(m_anim_ptr_array, p_anim);

    //fire cb first to set value 
    if(p_anim->exec_cb != NULL){
        p_anim->exec_cb(p_anim->var, p_anim->start_value);
    }

    p_anim->run_cnt = 0;
    p_anim->act_time = 0;
    NRF_LOG_INFO("anim start");
    if (!m_anim_tmr_started) {
        err_code = app_timer_start(m_anim_tick_tmr, APP_TIMER_TICKS(MUI_ANIM_TICK_INTERVAL_MS), NULL);
        APP_ERROR_CHECK(err_code);
        m_anim_tmr_started = true;
    }
}



void mui_anim_stop(mui_anim_t *p_anim) {
    mui_anim_remove_ptr(p_anim);
    if (mui_anim_ptr_array_size(m_anim_ptr_array) <= 0 && m_anim_tmr_started) {
        m_anim_tmr_started = false;
        int32_t err_code = app_timer_stop(m_anim_tick_tmr);
        APP_ERROR_CHECK(err_code);
    }
}