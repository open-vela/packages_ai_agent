/*
 * local_lm.h - On-device language model fallback for the agent loop.
 *
 * Wraps the velaAI TFLite Micro model (ai_lm.cxx in the team repo) behind a
 * small C API so agent_loop.c does not need to know whether TFLM is compiled
 * in.  With CONFIG_TFLITEMICRO off every call reports "unavailable" and the
 * caller keeps its previous behaviour.
 *
 * LOCAL-FAST A（2026-11-04，docs_ble/108 §2.A/§2.B）在本文件新增三个入口：
 *   local_lm_fast_reply()      —— 模板/规则快路径（不跑模型，毫秒级）
 *   local_lm_is_control_intent() —— 设备/日程控制意图判定（deny-list，供
 *                                  agent_loop 阻止控制类请求进本地模型）
 *   local_lm_warmup_start()    —— 一次性预热（detached 16KB 线程只做 ai_lm_init）
 */

#ifndef AI_AGENT_LOCAL_LM_H
#define AI_AGENT_LOCAL_LM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether an on-device model is compiled into this image.
 */
bool local_lm_available(void);

/**
 * LOCAL-FAST A-FIX（2026-11-04）：是否为**设备/日程控制意图**。
 *
 * 复用快路径同一份 deny-list（打开/关闭/设置/定个/闹钟/提醒/播放/暂停/
 * 音量/亮度/空调/灯/音乐/打电话/导航/静音/勿扰/测心率…）。命中返回 true。
 *
 * 调用方（agent_loop）用它把控制类请求挡在本地模板快路径/本地模型之外，
 * 避免"帮我定个闹钟"被本地模板回一句"你好"（本地规则不做设备控制）。
 * 纯字符串匹配，无副作用；任何线程可调。
 *
 * @param text 用户消息（UTF-8，可为 NULL → 返回 false）
 * @return true = 控制/日程类意图；false = 非控制类（或 NULL）
 */
bool local_lm_is_control_intent(const char *text);

/**
 * LOCAL-FAST A（docs_ble/108 §2.A）：模板/规则快路径 —— 不跑模型。
 *
 * 覆盖（固定短句）：
 *   - 问候 / 你是谁 / 我有点累 / 心情不好 / 夸夸我 / 谢谢 / 再见；
 *   - 现在几点 / 今天几号 / 星期几（读 RTC，本地时间）；
 *   - 讲个笑话 / 再讲一个（两条固定的本地笑话）。
 *
 * 红线：**不做设备控制**（开灯/空调/闹钟/播放…一律不接），也不编造动作；
 * 含这类词或明显更长的句子一律返回 -1，交回模型/云端处理。
 * 纯本地：只读 RTC，不联网、不写 /data、不起线程。
 *
 * @param user_text 用户消息（UTF-8）
 * @param out       命中时写入 malloc 的回复，调用方负责 free
 * @return 0 命中；-1 未命中或参数错误
 */
int local_lm_fast_reply(const char *user_text, char **out);

/**
 * Answer user_text: 先走模板快路径，未命中再跑端侧模型。
 *
 * Initialises the interpreter on first use (that allocates the tensor arena,
 * which lands in PSRAM).  Inference takes seconds, so call it from a worker
 * thread -- never from the LVGL render thread.  Fast-path hits are cheap and
 * safe from any thread; the model path has the usual cost.
 *
 * @param user_text  The user's message.
 * @param out        On success, receives a malloc'd reply the caller frees.
 * @return 0 on success, -1 if unavailable or the model produced nothing.
 */
int local_lm_reply(const char *user_text, char **out);

/**
 * LOCAL-FAST A（docs_ble/108 §2.B）：一次性预热端侧模型解释器。
 *
 * 起一个 detached **16KB** 线程，只调 ai_lm_init()（加载模型 +
 * AllocateTensors），不做生成；幂等（只尝试一次）。失败只记日志，之后
 * local_lm_reply() 的按需初始化会再试一次。
 *
 * 调用约束：在 ai_agent 主进程上下文调用（agent_loop_init），不要放到 NSH
 * 短任务组里 —— 任务组退出会把线程清掉（demo_voice 踩过同类坑）。
 */
void local_lm_warmup_start(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_LOCAL_LM_H */
