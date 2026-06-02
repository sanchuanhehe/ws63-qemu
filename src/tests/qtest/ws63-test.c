/*
 * QTest for the HiSilicon WS63 machine (hw/riscv/ws63.c).
 *
 * Register-level, BOOT-FREE regression: drives the peripheral models directly
 * over MMIO (the test process plays the role of the CPU via qtest_writel/readl)
 * without running any firmware. This complements scripts/smoke-test.sh, which
 * boots real ws63-rs / C-SDK ELFs end-to-end — here we assert the fine-grained
 * register semantics of GPIO, UART, the timer, the interrupt wiring into the
 * WS63 INTC, and a DMA mem->mem round-trip, and they run in milliseconds.
 *
 * Injected into qemu/tests/qtest/ and registered in qtests_riscv32 by
 * scripts/build.sh; run via scripts/qtest.sh (or `meson test ... ws63-test`).
 */
#include "qemu/osdep.h"
#include "libqtest-single.h"

/* Peripheral bases — must match hw/riscv/ws63.c. */
#define TIMER_BASE   0x44002000u
#define UART0_BASE   0x44010000u
#define GPIO0_BASE   0x44028000u
#define DMA0_BASE    0x4A000000u
#define SRAM_BASE    0x00A00000u   /* 0x90000 bytes of system RAM */

/* GPIO register offsets. */
#define GPIO_DATA    0x00   /* pin level (out | external drive), read-only-ish */
#define GPIO_OEN     0x04
#define GPIO_INT_EN  0x0C
#define GPIO_INT_MSK 0x10
#define GPIO_DATA_SET 0x30  /* w1s */
#define GPIO_DATA_CLR 0x34  /* w1c */

/* UART register offsets + status bits. */
#define UART_LINE_STATUS 0x34
#define UART_FIFO_STATUS 0x44
#define FIFO_TX_EMPTY (1u << 1)
#define FIFO_RX_EMPTY (1u << 3)
#define LSR_TX_READY  (3u << 5)

/* Timer: per-channel block at base + 0x100*(ch+1). */
#define T0           (TIMER_BASE + 0x100)
#define TMR_LOAD     0x00
#define TMR_CURRENT  0x08
#define TMR_CONTROL  0x10
#define TMR_EOI      0x14
#define TMR_RAW_ST   0x18
#define TMR_MIS_ST   0x1C
#define TMR_EN       (1u << 0)
#define WS63_IRQ_TIMER0 26

/* DMA channel block: base + 0x100 + ch*0x20. */
#define DMA_CH0      (DMA0_BASE + 0x100)
#define DMA_ORI_INT_ST (DMA0_BASE + 0x0C)
#define DMA_DST      0x04
#define DMA_CFG      0x08   /* bit0 ch_enable triggers the transfer; fc_tt[9:11] */
#define DMA_SRC      0x10
#define DMA_CTRL     0x14   /* transfersize[0:11], swsize[18:20], dwsize[21:23],
                             * src_inc[26], dest_inc[27], tc_int_en[31] */

/* ---- GPIO: DATA set/clear, output-enable + interrupt-enable read-back ---- */
static void test_gpio(void)
{
    QTestState *qts = qtest_init("-machine ws63");

    /* DATA_SET pins 0 and 2; DATA reflects the driven level. */
    qtest_writel(qts, GPIO0_BASE + GPIO_DATA_SET, 0x5);
    g_assert_cmphex(qtest_readl(qts, GPIO0_BASE + GPIO_DATA) & 0x5, ==, 0x5);

    /* DATA_CLR pin 0 only -> pin 2 stays high. */
    qtest_writel(qts, GPIO0_BASE + GPIO_DATA_CLR, 0x1);
    g_assert_cmphex(qtest_readl(qts, GPIO0_BASE + GPIO_DATA) & 0x5, ==, 0x4);

    /* OEN / INT_EN / INT_MASK are plain read/write shadow registers. */
    qtest_writel(qts, GPIO0_BASE + GPIO_OEN, 0xF0);
    g_assert_cmphex(qtest_readl(qts, GPIO0_BASE + GPIO_OEN), ==, 0xF0);
    qtest_writel(qts, GPIO0_BASE + GPIO_INT_EN, 0x2);
    g_assert_cmphex(qtest_readl(qts, GPIO0_BASE + GPIO_INT_EN), ==, 0x2);
    qtest_writel(qts, GPIO0_BASE + GPIO_INT_MSK, 0x2);
    g_assert_cmphex(qtest_readl(qts, GPIO0_BASE + GPIO_INT_MSK), ==, 0x2);

    qtest_quit(qts);
}

/* ---- UART: reset-value reads of the FIFO / line-status registers ---- */
static void test_uart(void)
{
    QTestState *qts = qtest_init("-machine ws63");

    /* No chardev attached: TX FIFO empty, RX FIFO empty, TX always ready. */
    g_assert_cmphex(qtest_readl(qts, UART0_BASE + UART_FIFO_STATUS), ==,
                    FIFO_TX_EMPTY | FIFO_RX_EMPTY);
    g_assert_cmphex(qtest_readl(qts, UART0_BASE + UART_LINE_STATUS), ==,
                    LSR_TX_READY);

    qtest_quit(qts);
}

/* ---- Timer: load/enable/decrement, fire, and delivery into the INTC ---- */
static void test_timer_and_intc(void)
{
    QTestState *qts = qtest_init("-machine ws63");

    /* Latch the INTC's input lines so we can observe what the timer delivers. */
    qtest_irq_intercept_in(qts, "/machine/intc");

    qtest_writel(qts, T0 + TMR_LOAD, 100000);
    g_assert_cmpuint(qtest_readl(qts, T0 + TMR_LOAD), ==, 100000);

    /* Disabled -> CURRENT reads 0, no interrupt pending. */
    g_assert_cmpuint(qtest_readl(qts, T0 + TMR_CURRENT), ==, 0);
    g_assert_false(qtest_get_irq(qts, WS63_IRQ_TIMER0));

    /* Enable (unmasked). The model arms a QEMU_CLOCK_VIRTUAL timer. */
    qtest_writel(qts, T0 + TMR_CONTROL, TMR_EN);
    g_assert_false(qtest_get_irq(qts, WS63_IRQ_TIMER0));

    /* Jump to the timer's deadline -> it fires. */
    qtest_clock_step_next(qts);
    g_assert_cmpuint(qtest_readl(qts, T0 + TMR_RAW_ST), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, T0 + TMR_MIS_ST), ==, 1);
    g_assert_true(qtest_get_irq(qts, WS63_IRQ_TIMER0));  /* delivered to INTC #26 */

    /* EOI clears the raw status and lowers the line into the INTC. */
    qtest_writel(qts, T0 + TMR_EOI, 1);
    g_assert_cmpuint(qtest_readl(qts, T0 + TMR_RAW_ST), ==, 0);
    g_assert_false(qtest_get_irq(qts, WS63_IRQ_TIMER0));

    qtest_quit(qts);
}

/* ---- DMA: channel-0 mem->mem block copy + raw completion status ---- */
static void test_dma_mem2mem(void)
{
    QTestState *qts = qtest_init("-machine ws63");

    const uint32_t src = SRAM_BASE + 0x1000;
    const uint32_t dst = SRAM_BASE + 0x2000;
    const uint32_t pat[4] = {0xdeadbeefu, 0x01234567u, 0xa5a5a5a5u, 0xfeedfaceu};

    for (int i = 0; i < 4; i++) {
        qtest_writel(qts, src + i * 4, pat[i]);
        qtest_writel(qts, dst + i * 4, 0);   /* clear destination */
    }

    /* Program channel 0: dest, src, then ctrl, then cfg (cfg.ch_enable runs it).
     * ctrl: count=4 items, swsize=dwsize=2 (1<<2 = 4 bytes), src_inc + dest_inc. */
    qtest_writel(qts, DMA_CH0 + DMA_DST, dst);
    qtest_writel(qts, DMA_CH0 + DMA_SRC, src);
    qtest_writel(qts, DMA_CH0 + DMA_CTRL,
                 4u | (2u << 18) | (2u << 21) | (1u << 26) | (1u << 27));
    qtest_writel(qts, DMA_CH0 + DMA_CFG, 0x1);  /* ch_enable, fc_tt=0 (mem->mem) */

    for (int i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(qts, dst + i * 4), ==, pat[i]);
    }
    /* Raw transfer-complete status latches channel 0 regardless of IRQ mask. */
    g_assert_cmphex(qtest_readl(qts, DMA_ORI_INT_ST) & 0x1, ==, 0x1);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ws63/gpio", test_gpio);
    qtest_add_func("/ws63/uart", test_uart);
    qtest_add_func("/ws63/timer_intc", test_timer_and_intc);
    qtest_add_func("/ws63/dma_mem2mem", test_dma_mem2mem);
    return g_test_run();
}
