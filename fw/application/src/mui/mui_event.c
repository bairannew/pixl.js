#include "mui_event.h"
#include "nrf_log.h"

void mui_event_queue_init(mui_event_queue_t *p_queue) {
    mui_event_deque_init(p_queue->event_deque);
}
void mui_event_set_callback(mui_event_queue_t *p_queue, mui_event_handler_t dispatcher,
                            void *context) {
    p_queue->dispatch_context = context;
    p_queue->dispatcher = dispatcher;
}

void mui_event_post(mui_event_queue_t *p_queue, mui_event_t *p_event) {
    // CRTIAL_ENTER
    /* v8.2-fix8: 修运算符优先级 bug. 原版写的是
     *   if (!mui_event_deque_size(p_queue->event_deque) > MAX_EVENT_MSG)
     * 由于 '!' 比 '>' 优先级高, 实际语义是
     *   if ((!size) > MAX_EVENT_MSG)
     * !size 只可能是 0 或 1, 永远 <= MAX_EVENT_MSG (MAX_EVENT_MSG 通常 > 1),
     * 这个判断**永远为假**, 队列溢出保护从来没生效过. BLE 大包高频上传
     * 时事件队列只增不减, 长时间运行会吃光 deque 池.
     *
     * 正确写法: 显式判断 size >= MAX_EVENT_MSG, 满了就丢掉. */
    if (mui_event_deque_size(p_queue->event_deque) >= MAX_EVENT_MSG) {
        NRF_LOG_WARNING("event buffer is FULL!!");
        return;
    }
    mui_event_t *p_new = mui_event_deque_push_back_new(p_queue->event_deque);
    memcpy(p_new, p_event, sizeof(mui_event_t));
}

void mui_event_dispatch(mui_event_queue_t *p_queue) {
    mui_event_t event;
    while (!mui_event_deque_empty_p(p_queue->event_deque)) {
        mui_event_deque_pop_front(&event, p_queue->event_deque);
        p_queue->dispatcher(p_queue->dispatch_context, &event);
    }
}

void mui_event_dispatch_now(mui_event_queue_t *p_queue, mui_event_t* p_event){
    p_queue->dispatcher(p_queue->dispatch_context, p_event);
}