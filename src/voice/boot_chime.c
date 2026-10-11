/*
 * boot_chime.c — 开机提示音（自检"嘴"：media_player -> /dev/audio/pcm0p -> 喇叭）
 *
 * 目的：不依赖串口控制台就能确认"板子上的喇叭到底会不会响"。
 * 走的是本仓已验证的链路（pcm_media_player.c 把 media_player_* 实现在
 * /dev/audio/pcm0p 上；docs_ble/30 §10/§11 有寄存器级验证）。
 *
 * 三声上行短音（880 / 1175 / 1568 Hz，各 140 ms，间隔 70 ms），总长约 0.6 s。
 * 由 CONFIG_AI_AGENT_BOOT_CHIME 控制（默认开）。
 *
 * Team 181 - Contest 2026
 */

#include <nuttx/config.h>

#ifdef CONFIG_AI_AGENT_BOOT_CHIME

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <media_player.h>

#define CHIME_TAG "boot_chime"
#define CHIME_RATE 16000

static void chime_fill(short* buf, int n, int freq, int phase_ofs)
{
    for (int i = 0; i < n; i++)
    {
        double t = (double)(i + phase_ofs) / (double)CHIME_RATE;
        buf[i] = (short)(sin(2.0 * 3.14159265358979 * (double)freq * t) * 9000.0);
    }
}

int agent_boot_chime(void)
{
    static const int freqs[3] = { 880, 1175, 1568 };
    short buf[CHIME_RATE / 5];              /* 200 ms 上限 */
    void* pb;

    pb = media_player_open(MEDIA_STREAM_MUSIC);

    if (!pb)
    {
        syslog(LOG_ERR, "[" CHIME_TAG "] media_player_open failed\n");
        return -1;
    }

    if (media_player_prepare(pb, NULL,
            "format=s16le:sample_rate=16000:ch_layout=mono") < 0)
    {
        syslog(LOG_ERR, "[" CHIME_TAG "] prepare failed\n");
        media_player_close(pb, 0);
        return -1;
    }

    if (media_player_start(pb) < 0)
    {
        syslog(LOG_ERR, "[" CHIME_TAG "] start failed\n");
        media_player_close(pb, 0);
        return -1;
    }

    for (int k = 0; k < 3; k++)
    {
        int n = CHIME_RATE * 140 / 1000;    /* 140 ms */

        chime_fill(buf, n, freqs[k], 0);
        if (media_player_write_data(pb, buf, (size_t)n * sizeof(short)) < 0)
        {
            break;
        }

        memset(buf, 0, (size_t)(CHIME_RATE * 70 / 1000) * sizeof(short));
        media_player_write_data(pb, buf,
            (size_t)(CHIME_RATE * 70 / 1000) * sizeof(short));
    }

    media_player_stop(pb);
    media_player_close(pb, 0);
    syslog(LOG_INFO, "[" CHIME_TAG "] boot chime played\n");
    return 0;
}

#endif /* CONFIG_AI_AGENT_BOOT_CHIME */
