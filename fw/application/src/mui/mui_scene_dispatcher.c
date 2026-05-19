#include "mui_scene_dispatcher.h"

mui_scene_dispatcher_t *mui_scene_dispatcher_create() {
    mui_scene_dispatcher_t *p_dispatcher = mui_mem_malloc(sizeof(mui_scene_dispatcher_t));
    scene_id_stack_init(p_dispatcher->scene_id_stack);
    p_dispatcher->p_scene_defines = NULL;
    p_dispatcher->scene_num = 0;
    p_dispatcher->user_data = NULL;
    p_dispatcher->default_scene_id = 0;
    return p_dispatcher;
}

void mui_scene_dispatcher_free(mui_scene_dispatcher_t *p_dispatcher) {
    // call last sence exit to free resources
    //  if (scene_id_stack_size(p_dispatcher->scene_id_stack) > 0) {
    //      uint32_t cur_scene_id = *scene_id_stack_back(p_dispatcher->scene_id_stack);
    //      p_dispatcher->p_scene_defines[cur_scene_id].exit_cb(p_dispatcher->user_data);
    //  }
    scene_id_stack_clear(p_dispatcher->scene_id_stack);
    mui_mem_free(p_dispatcher);
}

void mui_scene_dispatcher_exit(mui_scene_dispatcher_t *p_dispatcher) {
    // call last sence exit to free resources
    if (scene_id_stack_size(p_dispatcher->scene_id_stack) > 0) {
        uint32_t cur_scene_id = *scene_id_stack_back(p_dispatcher->scene_id_stack);
        p_dispatcher->p_scene_defines[cur_scene_id].exit_cb(p_dispatcher->user_data);
    }
}

void mui_scene_dispatcher_set_scene_defines(mui_scene_dispatcher_t *p_dispatcher, const mui_scene_t *p_scene_defines,
                                            uint32_t scene_num) {
    p_dispatcher->p_scene_defines = p_scene_defines;
    p_dispatcher->scene_num = scene_num;
}

void mui_scene_dispatcher_set_user_data(mui_scene_dispatcher_t *p_dispatcher, void *user_data) {
    p_dispatcher->user_data = user_data;
}

void mui_scene_dispatcher_next_scene(mui_scene_dispatcher_t *p_dispatcher, uint32_t scene_id) {
    if (scene_id_stack_size(p_dispatcher->scene_id_stack) > 0) {
        uint32_t cur_scene_id = *scene_id_stack_back(p_dispatcher->scene_id_stack);
        p_dispatcher->p_scene_defines[cur_scene_id].exit_cb(p_dispatcher->user_data);
    }
    scene_id_stack_push_back(p_dispatcher->scene_id_stack, scene_id);
    p_dispatcher->p_scene_defines[scene_id].enter_cb(p_dispatcher->user_data);
}

void mui_scene_dispatcher_back_scene(mui_scene_dispatcher_t *p_dispatcher, uint32_t step) {
    /* v8.2-fix8 关键修复:
     *
     * 原版只对最初的栈顶调 exit_cb, 中间被一并 pop 掉的场景 (例如 step=2 时
     * 的中间一层) 的 exit 永远不会触发. 在本工程里最直接的副作用是从
     * amiibo_detail_menu 选"返回徽章大全" (调 back_scene(2)) 时, 中间那层
     * amiibo_detail 的 on_exit 被跳过, 它本该做的:
     *
     *   ntag_emu_set_update_cb(NULL, NULL);
     *   app_timer_stop(m_amiibo_gen_delay_timer);
     *
     * 都没跑. 后果: 用户回到 badge_list 后, 模拟器侧仍然把 ntag_update_cb
     * 当成活跃回调, 任何手机贴卡写入会再次触发 ntag_update -> reload_files,
     * 在用户已经不在 detail 视图的情况下乱搅 app->amiibo_files. 并发 + UI
     * 状态错乱叠加, 跟用户报告的"返回徽章大全列表都没了"现象高度吻合.
     *
     * 修法: 一边 pop 一边对每个 popped 场景调 exit_cb (栈顶 -> 栈底顺序),
     * 全部 pop 完再对新的栈顶 (或 default_scene) 调 enter_cb 一次. 单步
     * previous_scene() 的语义不变 (走的还是这条 loop, 只是只跑一轮). */

    uint32_t popped_scene_id;
    while (scene_id_stack_size(p_dispatcher->scene_id_stack) > 0 && step > 0) {
        scene_id_stack_pop_back(&popped_scene_id, p_dispatcher->scene_id_stack);
        /* 对每一个被 pop 掉的场景按"栈顶到栈底"顺序逐个 exit. */
        p_dispatcher->p_scene_defines[popped_scene_id].exit_cb(p_dispatcher->user_data);
        step--;
    }

    // last scene
    if (scene_id_stack_size(p_dispatcher->scene_id_stack) == 0) {
        p_dispatcher->p_scene_defines[p_dispatcher->default_scene_id].enter_cb(p_dispatcher->user_data);
    } else {
        uint32_t prev_scene_id = *scene_id_stack_back(p_dispatcher->scene_id_stack);
        p_dispatcher->p_scene_defines[prev_scene_id].enter_cb(p_dispatcher->user_data);
    }
}

void mui_scene_dispatcher_previous_scene(mui_scene_dispatcher_t *p_dispatcher) {
    /* v8.2-fix8: 复用 back_scene 的"按层调 exit"路径, 保持单步退栈跟多步
     * 退栈的清理语义一致. 行为上跟原版相同 (单 step). */
    mui_scene_dispatcher_back_scene(p_dispatcher, 1);
}

uint32_t mui_scene_dispatcher_current_scene(mui_scene_dispatcher_t *p_dispatcher) {
    uint32_t *p_scene_id = scene_id_stack_back(p_dispatcher->scene_id_stack);
    if (p_scene_id) {
        return *p_scene_id;
    }
    return 0;
}