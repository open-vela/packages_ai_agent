/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Bring-up commands for the on-board MEMS microphone (SF32LB52 AUDCODEC ADC).
 *
 * The capture path has no other openvela reference implementation, so these
 * commands exist to answer the two questions that come up first when it does
 * not behave: is the microphone picking anything up at all (mic_test), and
 * what is actually coming out of the DMA (mic_raw, mic_dump). */

#include "channels/cmd_mic.h"
#include "agent_compat.h"

#include "sf32lb_audcodec.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIC_TEST_SAMPLE_RATE 16000
#define MIC_BLOCK_MS         100
#define MIC_BLOCK_SAMPLES    (MIC_TEST_SAMPLE_RATE / (1000 / MIC_BLOCK_MS))

#define MIC_TEST_MAX_SECONDS 120

/* mic_test [seconds] [pll|xtal] [0|1]
 *
 * The clock source and ADC channel are selectable because neither is settled
 * for this board: the HAL's own reference ADC_CFG value (0x508) selects the
 * crystal, while the analog front-end helper defaults to the PLL, and which
 * ADC channel the on-board microphone lands on is not documented anywhere in
 * the tree.
 */
void cmd_mic_test(int argc, char** argv)
{
    int seconds = (argc >= 2) ? atoi(argv[1]) : 5;
    enum sf32lb_audio_clk_e clk = SF32LB_AUDIO_CLK_PLL;
    int channel = 0;
    int volume = SF32LB_AUDIO_DEFAULT_VOLUME;
    int blocks;
    int16_t* buf;
    int ret;
    int i;

    if (seconds <= 0) {
        seconds = 5;
    }

    if (seconds > MIC_TEST_MAX_SECONDS) {
        seconds = MIC_TEST_MAX_SECONDS;
    }

    if (argc >= 3) {
        clk = (strcmp(argv[2], "xtal") == 0) ? SF32LB_AUDIO_CLK_XTAL
                                             : SF32LB_AUDIO_CLK_PLL;
    }

    if (argc >= 4) {
        channel = atoi(argv[3]) ? 1 : 0;
    }

    if (argc >= 5) {
        volume = atoi(argv[4]);
    }

    if (argc >= 7) {
        sf32lb_audcodec_set_clk((uint8_t)atoi(argv[5]), (uint8_t)atoi(argv[6]));
    }

    if (sf32lb_audcodec_is_open()) {
        printf("mic_test: capture already running, restarting\n");
        sf32lb_audcodec_close();
        usleep(50000);
    }

    ret = sf32lb_audcodec_open_ex(MIC_TEST_SAMPLE_RATE, clk, channel, volume);
    if (ret < 0) {
        printf("mic_test: open failed (%d)\n", ret);
        return;
    }

    buf = malloc(MIC_BLOCK_SAMPLES * sizeof(int16_t));
    if (buf == NULL) {
        printf("mic_test: out of memory\n");
        sf32lb_audcodec_close();
        return;
    }

    {
        uint8_t div, osr;
        sf32lb_audcodec_get_clk(&div, &osr);
        printf("mic_test: %d s, %d ms blocks, %s clock, ADC ch%d, %d dB, "
               "clk_div=%d osr_sel=%d -- make some noise\n"
               "          (block timing is nominal; check mic_rate for the "
               "true sample rate)\n",
            seconds, MIC_BLOCK_MS,
            (clk == SF32LB_AUDIO_CLK_XTAL) ? "xtal" : "pll", channel, volume,
            div, osr);
    }

    /* Print what the block actually latched, not what we asked for. */
    cmd_mic_dump(0, NULL);

    /* Let the analog front end and the DMA settle before the first read. */
    usleep(200000);

    blocks = seconds * (1000 / MIC_BLOCK_MS);

    for (i = 0; i < blocks; i++) {
        struct sf32lb_audio_stats_s stats;
        ssize_t n = sf32lb_audcodec_read(buf, MIC_BLOCK_SAMPLES);
        int peak;

        if (n < 0) {
            printf("mic_test: read failed (%d)\n", (int)n);
            break;
        }

        /* Analyse what the read actually returned, which is not the request
         * size: a read may hand back less as soon as a useful amount is in
         * hand, so the tail of 'buf' can be stale. */

        sf32lb_audcodec_analyze(buf, (size_t)n, &stats);
        peak = (stats.min < 0 ? -stats.min : stats.min);
        if (stats.max > peak) {
            peak = stats.max;
        }

        printf("[mic] %5d ms  rms=%-6ld peak=%-6d min=%-6d max=%-6d "
               "mean=%-5ld zero=%zu\n",
            (i + 1) * MIC_BLOCK_MS, (long)stats.rms, peak,
            stats.min, stats.max, (long)stats.mean, stats.nzero);
    }

    free(buf);
    sf32lb_audcodec_close();
    printf("mic_test: done\n");
}

void cmd_mic_raw(int argc, char** argv)
{
    int words = (argc >= 2) ? atoi(argv[1]) : 32;
    uint32_t raw[128];
    ssize_t n;
    int ret;
    int i;

    if (words <= 0 || words > (int)(sizeof(raw) / sizeof(raw[0]))) {
        words = 32;
    }

    if (!sf32lb_audcodec_is_open()) {
        ret = sf32lb_audcodec_open(MIC_TEST_SAMPLE_RATE);
        if (ret < 0) {
            printf("mic_raw: open failed (%d)\n", ret);
            return;
        }
    }

    usleep(200000);
    n = sf32lb_audcodec_read_raw(raw, words);
    if (n < 0) {
        printf("mic_raw: read failed (%d)\n", (int)n);
        sf32lb_audcodec_close();
        return;
    }

    printf("mic_raw: %d DMA words "
           "(lo16 / hi16 both shown -- tells us where the sample sits)\n",
        (int)n);

    for (i = 0; i < (int)n; i++) {
        printf("  [%3d] 0x%08lx  lo=%6d  hi=%6d\n", i,
            (unsigned long)raw[i],
            (int)(int16_t)(raw[i] & 0xffff),
            (int)(int16_t)(raw[i] >> 16));
    }
}

void cmd_mic_dump(int argc, char** argv)
{
    struct sf32lb_audcodec_regs_s r;

    (void)argc;
    (void)argv;

    if (sf32lb_audcodec_get_regs(&r) < 0) {
        printf("mic_dump: capture is not open (run mic_test first)\n");
        return;
    }

    /* Decoded against the vendor bit definitions so the values can be read
     * without a register map to hand:
     *   ADC_CFG   bits0-2 OSR_SEL, bits3-4 OP_MODE, bit6 CLK_SRC_SEL,
     *             bits8-15 CLK_DIV
     *   CFG       bit0 ADC_ENABLE, bit1 DAC_ENABLE
     *   CH0/CH1   bit0 ENABLE, bit7 DMA_EN, bits8-11 ROUGH_VOL, bit16 FMT
     * The reference the HAL documents is ADC_CFG == 0x508.
     */
    printf("mic_dump: AUDCODEC registers\n");
    printf("  CFG         = 0x%08lx  (adc_en=%lu dac_en=%lu)\n",
        (unsigned long)r.cfg,
        (unsigned long)(r.cfg & 1), (unsigned long)((r.cfg >> 1) & 1));
    printf("  ADC_CFG     = 0x%08lx  (osr=%lu opmode=%lu clksrc=%lu div=%lu)\n",
        (unsigned long)r.adc_cfg,
        (unsigned long)(r.adc_cfg & 0x7),
        (unsigned long)((r.adc_cfg >> 3) & 0x3),
        (unsigned long)((r.adc_cfg >> 6) & 0x1),
        (unsigned long)((r.adc_cfg >> 8) & 0xff));
    printf("  ADC_CH0_CFG = 0x%08lx  (en=%lu dma_en=%lu vol=%lu fmt=%lu)\n",
        (unsigned long)r.adc_ch0_cfg,
        (unsigned long)(r.adc_ch0_cfg & 1),
        (unsigned long)((r.adc_ch0_cfg >> 7) & 1),
        (unsigned long)((r.adc_ch0_cfg >> 8) & 0xf),
        (unsigned long)((r.adc_ch0_cfg >> 16) & 1));
    printf("  ADC_CH1_CFG = 0x%08lx\n", (unsigned long)r.adc_ch1_cfg);
    printf("  ADC_ANA_CFG = 0x%08lx  (micbias_en=%lu)\n",
        (unsigned long)r.adc_ana_cfg,
        (unsigned long)((r.adc_ana_cfg >> 1) & 1));
    printf("  DMA CNDTR   = %lu / %d   CCR=0x%08lx  pos=%lu\n",
        (unsigned long)r.dma_cndtr, SF32LB_AUDIO_DMA_WORDS,
        (unsigned long)r.dma_ccr, (unsigned long)r.dma_pos);
}

/* mic_rate [seconds]
 *
 * Measure the sample rate the ADC actually produces, rather than the one we
 * asked for. ADC_CFG's clk_div/osr_sel fields do not map onto a sample rate
 * in any way the tree documents, and the HAL's own values (5 / 0) turn out to
 * land near 6.4 kHz rather than the 16 kHz everything downstream assumes.
 * Frames on the wire are whole and evenly spaced either way, so a wrong rate
 * looks exactly like correct capture until speech recognition mangles it --
 * this command is what makes the discrepancy visible.
 */
void cmd_mic_rate(int argc, char** argv)
{
    int seconds = (argc >= 2) ? atoi(argv[1]) : 3;
    struct timespec t0, t1;
    unsigned int total = 0;
    uint8_t div, osr;
    int16_t* buf;
    double elapsed;
    int ret;

    if (seconds <= 0 || seconds > 30) {
        seconds = 3;
    }

    if (sf32lb_audcodec_is_open()) {
        sf32lb_audcodec_close();
        usleep(50000);
    }

    if (argc >= 4) {
        sf32lb_audcodec_set_clk((uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]));
    }

    ret = sf32lb_audcodec_open(MIC_TEST_SAMPLE_RATE);
    if (ret < 0) {
        printf("mic_rate: open failed (%d)\n", ret);
        return;
    }

    buf = malloc(MIC_BLOCK_SAMPLES * sizeof(int16_t));
    if (buf == NULL) {
        sf32lb_audcodec_close();
        printf("mic_rate: out of memory\n");
        return;
    }

    sf32lb_audcodec_get_clk(&div, &osr);

    /* Let the analog path settle so the first reads are not the slow ones. */
    usleep(200000);

    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        ssize_t n = sf32lb_audcodec_read(buf, MIC_BLOCK_SAMPLES);

        if (n < 0) {
            break;
        }

        /* Count what came back, not what was asked for: a read returns as
         * soon as it has a useful amount, so assuming the request size here
         * would over-report the rate. */

        total += (unsigned int)n;

        clock_gettime(CLOCK_MONOTONIC, &t1);
        elapsed = (t1.tv_sec - t0.tv_sec)
                + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed >= (double)seconds) {
            break;
        }
    }

    free(buf);
    sf32lb_audcodec_close();

    if (elapsed <= 0.0) {
        printf("mic_rate: no samples\n");
        return;
    }

    printf("mic_rate: clk_div=%d osr_sel=%d -> %u samples in %.2f s "
           "= %.0f Hz\n", div, osr, total, elapsed, total / elapsed);
}
