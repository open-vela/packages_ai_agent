/****************************************************************************
 * apps/packages/ai_agent/src/tools/tool_2k0300.c
 *
 * LS2K0300 (LOONG-HAT) peripheral control tools for the AI Agent.
 * Driver logic ported from apps/examples/ls_driver_test.
 *
 * LEDs:   GPIO72 red / GPIO73 green, HIGH level lights the LED
 * PWM:    /dev/pwm2 on GPIO88 (second function), duty 0..65536
 * Buzzer: GPIO75 output, 1kHz software square wave
 * Keys:   GPIO87 / GPIO86 inputs, active-low
 * Thermal: /dev/thermal0, read() -> int32 milli-Celsius
 * ADC:    MCP3204 CH0-3 on SPI2 (base 0x1610c000, uncached),
 *         software CS = GPIO67, Vref 3300 mV
 * Light:  BH1750 on I2C1 at 0x23 (alt 0x5C)
 * OLED:   SSD1306 128x64 on I2C1 at 0x3C
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "tools/tool_2k0300.h"
#include "agent_compat.h"

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/ioexpander/gpio.h>
#include <nuttx/pinctrl/pinctrl.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/i2c/i2c_master.h>

#include "cJSON.h"

#define TAG "board"

/****************************************************************************
 * Pin Definitions (from ls_driver_test README pin map)
 ****************************************************************************/

#define LED_RED_PIN       72    /* HIGH level = lit */
#define LED_GREEN_PIN     73    /* HIGH level = lit */
#define PWM_PIN           88    /* second function = PWM2 */
#define BUZZER_PIN        75
#define KEY1_PIN          87    /* active-low */
#define KEY2_PIN          86    /* active-low */
#define ADC_CS_PIN        67

#define I2C1_PATH         "/dev/i2c1"
#define THERMAL_PATH      "/dev/thermal0"

#define BH1750_ADDR_LOW   0x23
#define BH1750_ADDR_HIGH  0x5c
#define BH1750_POWER_ON   0x01
#define BH1750_RESET      0x07
#define BH1750_H_MODE     0x10

#define OLED_ADDR         0x3c
#define OLED_WIDTH        128
#define OLED_PAGES        8
#define OLED_I2C_CMD      0x00
#define OLED_I2C_DATA     0x40

#define PWM_DEVICE        "/dev/pwm2"
#define PWM_FUNCTION      2      /* second function */
#define PWM_FREQ_DEFAULT  1000
#define PWM_DUTY_MAX      65536

#define BUZZER_HALF_US    500    /* 1 kHz tone */

/* MCP3204 via SPI2 bare registers (mirror test_adc.c) */

#define SPI2_BASE         (0x8000000000000000UL | 0x1610c000UL)
#define CFG5_ADDR         (0x8000000000000000UL | 0x16000114UL)

#define LS_SPI_IO_CR1     0x00
#define LS_SPI_IO_CR3     0x08
#define LS_SPI_IO_CR4     0x0c
#define LS_SPI_IO_SR1     0x14
#define LS_SPI_IO_CFG1    0x20
#define LS_SPI_IO_CFG2    0x24
#define LS_SPI_IO_CFG3    0x28
#define LS_SPI_IO_DR      0x40

#define LS_SPI_CR1_SPE    (1U << 0)
#define LS_SPI_CR1_CSTART (1U << 1)
#define LS_SPI_CR1_AUTOSUS (1U << 2)

#define LS_SPI_CFG1_DSIZE_8BIT (7U << 8)
#define LS_SPI_CFG2_BRINT_SHIFT 8

#define LS_SPI_CFG3_MSTR  (1U << 0)
#define LS_SPI_CFG3_DIE   (1U << 2)
#define LS_SPI_CFG3_DOE   (1U << 3)
#define LS_SPI_CFG3_SSMODE_SW (1U << 8)

#define LS_SPI_SR1_RXA    (1U << 0)
#define LS_SPI_SR1_RXE    (1U << 4)
#define LS_SPI_SR1_EOT    (1U << 15)
#define LS_SPI_SR1_W1C    (LS_SPI_SR1_EOT | (1U << 11) | (1U << 10) | \
                           (1U << 9) | (1U << 8))

#define SPI_APB_HZ        (200UL * 1000000UL)
#define SPI_CLOCK_HZ      1000000UL
#define SPI_TIMEOUT_US    10000U

#define MCP3204_MAX       4095U
#define MCP3204_VREF_MV   3300U
#define MCP3204_SAMPLES   8

/* Font data: 6x8 subset of oled_fonts.h (printable ASCII) */

#include "tool_2k0300_fonts.h"

#define SPI_REG32(off) (*(volatile uint32_t *)(SPI2_BASE + (off)))
#define SPI_REG8(off)  (*(volatile uint8_t *)(SPI2_BASE + (off)))
#define CFG5_REG       (*(volatile uint32_t *)CFG5_ADDR)

/****************************************************************************
 * Private Helpers
 ****************************************************************************/

static int pinctrl_select_gpio(int pin)
{
  int fd = open("/dev/pinctrl0", O_RDWR);
  int ret;

  if (fd < 0)
    {
      return -errno;
    }

  ret = ioctl(fd, PINCTRLC_SELECTGPIO, (unsigned long)pin);
  close(fd);
  return ret < 0 ? -errno : OK;
}

static int pinctrl_set_function(int pin, int func)
{
  struct pinctrl_param_s param;
  int fd = open("/dev/pinctrl0", O_RDWR);
  int ret;

  if (fd < 0)
    {
      return -errno;
    }

  param.pin = pin;
  param.para.function = func;
  ret = ioctl(fd, PINCTRLC_SETFUNCTION, (unsigned long)(uintptr_t)&param);
  close(fd);
  return ret < 0 ? -errno : OK;
}

static int gpio_open_output(int pin)
{
  char path[32];
  int fd;
  int ret;

  if (pinctrl_select_gpio(pin) < 0)
    {
      return -1;
    }

  snprintf(path, sizeof(path), "/dev/gpio%d", pin);
  fd = open(path, O_RDWR);
  if (fd < 0)
    {
      return -1;
    }

  ret = ioctl(fd, GPIOC_SETPINTYPE, (unsigned long)GPIO_OUTPUT_PIN);
  if (ret < 0)
    {
      close(fd);
      return -1;
    }

  return fd;
}

static int gpio_open_input(int pin)
{
  char path[32];
  int fd;
  int ret;

  if (pinctrl_select_gpio(pin) < 0)
    {
      return -1;
    }

  snprintf(path, sizeof(path), "/dev/gpio%d", pin);
  fd = open(path, O_RDWR);
  if (fd < 0)
    {
      return -1;
    }

  ret = ioctl(fd, GPIOC_SETPINTYPE, (unsigned long)GPIO_INPUT_PIN);
  if (ret < 0)
    {
      close(fd);
      return -1;
    }

  return fd;
}

static int gpio_write_level(int fd, int level)
{
  return ioctl(fd, GPIOC_WRITE, (unsigned long)level);
}

static int gpio_read_level(int fd, int *level)
{
  bool value = false;
  int ret = ioctl(fd, GPIOC_READ, (unsigned long)(uintptr_t)&value);
  if (ret < 0)
    {
      return ret;
    }

  *level = value ? 1 : 0;
  return OK;
}

static void json_reply(cJSON *resp, char *output, size_t output_size)
{
  char *json_str = cJSON_PrintUnformatted(resp);
  if (json_str)
    {
      strncpy(output, json_str, output_size - 1);
      output[output_size - 1] = '\0';
      free(json_str);
    }
  else
    {
      snprintf(output, output_size, "{\"error\":\"json serialize failed\"}");
    }

  cJSON_Delete(resp);
}

static int json_fail(char *output, size_t output_size, const char *fmt,
                     const char *arg)
{
  snprintf(output, output_size, fmt, arg ? arg : "");
  return ERROR;
}

/****************************************************************************
 * Private: OLED (SSD1306 via I2C1)
 ****************************************************************************/

static int oled_write_byte(int i2c_fd, uint8_t reg, uint8_t data)
{
  struct i2c_msg_s msg;
  struct i2c_transfer_s trans;
  uint8_t buf[2];

  buf[0] = reg;
  buf[1] = data;

  msg.frequency = I2C_SPEED_STANDARD;
  msg.addr      = OLED_ADDR;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  trans.msgv = &msg;
  trans.msgc = 1;

  return ioctl(i2c_fd, I2CIOC_TRANSFER, (unsigned long)(uintptr_t)&trans);
}

static int oled_write_cmd(int i2c_fd, uint8_t cmd)
{
  return oled_write_byte(i2c_fd, OLED_I2C_CMD, cmd);
}

static int oled_write_data(int i2c_fd, uint8_t data)
{
  return oled_write_byte(i2c_fd, OLED_I2C_DATA, data);
}

static int oled_init_device(int i2c_fd)
{
  static const uint8_t init_cmds[] =
  {
    0xae, 0x00, 0x10, 0x40, 0xb0, 0x81, 0xff, 0xa1,
    0xa6, 0xa8, 0x3f, 0xc8, 0xd3, 0x00, 0xd5, 0x80,
    0xd8, 0x05, 0xd9, 0xf1, 0xda, 0x12, 0xdb, 0x30,
    0x8d, 0x14, 0xaf,
  };
  size_t i;

  for (i = 0; i < sizeof(init_cmds); i++)
    {
      if (oled_write_cmd(i2c_fd, init_cmds[i]) < 0)
        {
          return -EIO;
        }
    }

  return OK;
}

static void oled_set_position(int i2c_fd, uint8_t x, uint8_t y)
{
  oled_write_cmd(i2c_fd, 0xb0 + y);
  oled_write_cmd(i2c_fd, x & 0x0f);
  oled_write_cmd(i2c_fd, ((x & 0xf0) >> 4) | 0x10);
}

static void oled_fill_screen(int i2c_fd, uint8_t fill)
{
  uint8_t page;
  uint8_t col;

  for (page = 0; page < OLED_PAGES; page++)
    {
      oled_set_position(i2c_fd, 0, page);
      for (col = 0; col < OLED_WIDTH; col++)
        {
          oled_write_data(i2c_fd, fill);
        }
    }
}

static void oled_show_char(int i2c_fd, uint8_t x, uint8_t y, char ch)
{
  uint8_t i;
  int c;

  if (ch < ' ' || ch > '~')
    {
      ch = ' ';
    }

  c = ch - ' ';

  oled_set_position(i2c_fd, x, y);
  for (i = 0; i < 8; i++)
    {
      oled_write_data(i2c_fd, g_font_8x16[c][i]);
    }

  oled_set_position(i2c_fd, x, y + 1);
  for (i = 0; i < 8; i++)
    {
      oled_write_data(i2c_fd, g_font_8x16[c][8 + i]);
    }
}

static void oled_show_string(int i2c_fd, uint8_t x, uint8_t y,
                             const char *str)
{
  if (!str)
    {
      return;
    }

  while (*str)
    {
      oled_show_char(i2c_fd, x, y, *str);
      x += 8;
      if (x > (OLED_WIDTH - 8))
        {
          x = 0;
          y += 2;
          if (y >= OLED_PAGES - 1)
            {
              break;
            }
        }

      str++;
    }
}

/****************************************************************************
 * Private: BH1750 light sensor via I2C1
 *
 * Mirrors test_oled.c: pinmux for GPIO50(SCL)/GPIO51(SDA) to main
 * function (I2C1) is re-applied on every open, matching the driver
 * test behavior rather than relying on boot-time bringup only.
 ****************************************************************************/

#define I2C1_SCL_PIN  50
#define I2C1_SDA_PIN  51
#define I2C1_FUNC     3      /* MAIN_FUNC = I2C1 */

static int i2c1_pinctrl_setup(void)
{
  int ret1 = pinctrl_set_function(I2C1_SCL_PIN, I2C1_FUNC);
  int ret2 = pinctrl_set_function(I2C1_SDA_PIN, I2C1_FUNC);

  return (ret1 == OK && ret2 == OK) ? OK : -EIO;
}

static int i2c_write_byte(int fd, uint8_t addr, uint8_t data)
{
  struct i2c_msg_s msg;
  struct i2c_transfer_s trans;

  msg.frequency = I2C_SPEED_STANDARD;
  msg.addr      = addr;
  msg.flags     = 0;
  msg.buffer    = &data;
  msg.length    = 1;

  trans.msgv = &msg;
  trans.msgc = 1;

  return ioctl(fd, I2CIOC_TRANSFER, (unsigned long)(uintptr_t)&trans);
}

static int i2c_read_bytes(int fd, uint8_t addr, uint8_t *buf, int len)
{
  struct i2c_msg_s msg;
  struct i2c_transfer_s trans;

  msg.frequency = I2C_SPEED_STANDARD;
  msg.addr      = addr;
  msg.flags     = I2C_M_READ;
  msg.buffer    = buf;
  msg.length    = len;

  trans.msgv = &msg;
  trans.msgc = 1;

  return ioctl(fd, I2CIOC_TRANSFER, (unsigned long)(uintptr_t)&trans);
}

static int bh1750_init(int fd, uint8_t *addr)
{
  *addr = BH1750_ADDR_LOW;
  if (i2c_write_byte(fd, *addr, BH1750_POWER_ON) < 0)
    {
      *addr = BH1750_ADDR_HIGH;
      if (i2c_write_byte(fd, *addr, BH1750_POWER_ON) < 0)
        {
          return -EIO;
        }
    }

  usleep(20000);
  i2c_write_byte(fd, *addr, BH1750_RESET);
  usleep(20000);

  if (i2c_write_byte(fd, *addr, BH1750_H_MODE) < 0)
    {
      return -EIO;
    }

  usleep(180000);
  return OK;
}

static int bh1750_read_lux(int fd, uint8_t addr, unsigned int *lux_x100)
{
  uint8_t buf[2] = { 0, 0 };
  unsigned int raw;
  int ret = i2c_read_bytes(fd, addr, buf, 2);

  if (ret < 0)
    {
      return ret;
    }

  raw = ((unsigned int)buf[0] << 8) | buf[1];
  *lux_x100 = (raw * 1000 + 6) / 12;
  return OK;
}

/****************************************************************************
 * Private: MCP3204 ADC via SPI2 bare registers (mirror test_adc.c)
 ****************************************************************************/

static int g_fd_adc_cs = -1;

static void spi2_abort(void)
{
  uint32_t val;
  uint32_t i;

  val = SPI_REG32(LS_SPI_IO_CR1);
  val &= ~(LS_SPI_CR1_CSTART | LS_SPI_CR1_AUTOSUS);
  SPI_REG32(LS_SPI_IO_CR1) = val;

  SPI_REG32(LS_SPI_IO_CR4) = 0;
  SPI_REG32(LS_SPI_IO_CR3) = 0;
  up_udelay(10);

  SPI_REG32(LS_SPI_IO_SR1) = LS_SPI_SR1_W1C;

  for (i = 0; i < 4; i++)
    {
      if (SPI_REG32(LS_SPI_IO_SR1) & LS_SPI_SR1_RXE)
        {
          break;
        }

      (void)SPI_REG8(LS_SPI_IO_DR);
    }

  ioctl(g_fd_adc_cs, GPIOC_WRITE, 1);
  up_udelay(1);
}

static int spi2_xfer_byte(uint8_t tx, uint8_t *rx)
{
  uint32_t val;
  uint32_t sr1;
  uint32_t i;

  val = SPI_REG32(LS_SPI_IO_CR1);
  val |= LS_SPI_CR1_SPE;
  SPI_REG32(LS_SPI_IO_CR1) = val;

  val = SPI_REG32(LS_SPI_IO_CR1);
  val &= ~(LS_SPI_CR1_CSTART | LS_SPI_CR1_AUTOSUS);
  SPI_REG32(LS_SPI_IO_CR1) = val;

  SPI_REG32(LS_SPI_IO_CR4) = 0;
  SPI_REG32(LS_SPI_IO_CR3) = 0;
  up_udelay(5);

  SPI_REG32(LS_SPI_IO_SR1) = LS_SPI_SR1_W1C;
  up_udelay(1);

  for (i = 0; i < 4; i++)
    {
      sr1 = SPI_REG32(LS_SPI_IO_SR1);
      if (sr1 & LS_SPI_SR1_RXE)
        {
          break;
        }

      (void)SPI_REG8(LS_SPI_IO_DR);
    }

  SPI_REG32(LS_SPI_IO_CR3) = 0;
  SPI_REG8(LS_SPI_IO_DR) = tx;

  val = SPI_REG32(LS_SPI_IO_CR1);
  val |= LS_SPI_CR1_CSTART;
  SPI_REG32(LS_SPI_IO_CR1) = val;

  for (i = 0; i < SPI_TIMEOUT_US; i++)
    {
      sr1 = SPI_REG32(LS_SPI_IO_SR1);
      if (sr1 & LS_SPI_SR1_EOT)
        {
          SPI_REG32(LS_SPI_IO_SR1) = LS_SPI_SR1_EOT;
          break;
        }

      up_udelay(1);
    }

  if (i >= SPI_TIMEOUT_US)
    {
      spi2_abort();
      return -1;
    }

  for (i = 0; i < SPI_TIMEOUT_US; i++)
    {
      sr1 = SPI_REG32(LS_SPI_IO_SR1);
      if (sr1 & LS_SPI_SR1_RXA)
        {
          if (rx != NULL)
            {
              *rx = SPI_REG8(LS_SPI_IO_DR);
            }
          else
            {
              (void)SPI_REG8(LS_SPI_IO_DR);
            }

          val = SPI_REG32(LS_SPI_IO_CR1);
          val &= ~(LS_SPI_CR1_CSTART | LS_SPI_CR1_AUTOSUS);
          SPI_REG32(LS_SPI_IO_CR1) = val;
          return 0;
        }

      up_udelay(1);
    }

  spi2_abort();
  return -1;
}

static int mcp3204_init(void)
{
  int fd;
  uint32_t brint;
  int ret;

  if (g_fd_adc_cs >= 0)
    {
      return OK;  /* already initialized */
    }

  ret = pinctrl_select_gpio(ADC_CS_PIN);
  if (ret < 0)
    {
      return ret;
    }

  fd = open("/dev/gpio67", O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  ret = ioctl(fd, GPIOC_SETPINTYPE, (unsigned long)GPIO_OUTPUT_PIN);
  if (ret < 0)
    {
      close(fd);
      return -errno;
    }

  ioctl(fd, GPIOC_WRITE, 1);
  g_fd_adc_cs = fd;

  CFG5_REG |= (0x3u << 17);

  SPI_REG32(LS_SPI_IO_CFG3) = LS_SPI_CFG3_MSTR | LS_SPI_CFG3_DIE |
                              LS_SPI_CFG3_DOE | LS_SPI_CFG3_SSMODE_SW;
  SPI_REG32(LS_SPI_IO_CFG1) = LS_SPI_CFG1_DSIZE_8BIT;

  brint = SPI_APB_HZ / SPI_CLOCK_HZ;
  if (brint < 2)
    {
      brint = 2;
    }
  if (brint > 255)
    {
      brint = 255;
    }

  SPI_REG32(LS_SPI_IO_CFG2) = brint << LS_SPI_CFG2_BRINT_SHIFT;
  SPI_REG32(LS_SPI_IO_CR1) = LS_SPI_CR1_SPE;

  return OK;
}

static int mcp3204_read_raw(uint8_t channel, uint16_t *raw)
{
  uint8_t rx_hi = 0;
  uint8_t rx_lo = 0;

  ioctl(g_fd_adc_cs, GPIOC_WRITE, 0);
  up_udelay(1);

  if (spi2_xfer_byte(0x06, NULL) != 0 ||
      spi2_xfer_byte((uint8_t)(channel << 6), &rx_hi) != 0 ||
      spi2_xfer_byte(0x00, &rx_lo) != 0)
    {
      ioctl(g_fd_adc_cs, GPIOC_WRITE, 1);
      return -1;
    }

  ioctl(g_fd_adc_cs, GPIOC_WRITE, 1);
  up_udelay(1);

  if (rx_hi == 0xff || rx_lo == 0xff)
    {
      return -1;  /* MISO stuck high: no device */
    }

  *raw = (uint16_t)((((uint16_t)rx_hi & 0x0fu) << 8) | rx_lo);
  return 0;
}

static int mcp3204_read_average(uint8_t channel, uint16_t *raw)
{
  uint32_t sum = 0;
  uint16_t value = 0;
  int i;
  int valid = 0;

  for (i = 0; i < MCP3204_SAMPLES * 8; i++)
    {
      if (mcp3204_read_raw(channel, &value) == 0)
        {
          sum += value;
          valid++;
          if (valid >= MCP3204_SAMPLES)
            {
              break;
            }
        }

      up_mdelay(2);
    }

  if (valid == 0)
    {
      return -1;
    }

  *raw = (uint16_t)((sum + (valid / 2)) / valid);
  return 0;
}

/****************************************************************************
 * Tool: board_led
 *
 * Input: { "led": "red"|"green"|"both", "action": "on"|"off"|"toggle"|"blink",
 *          "times": N, "interval_ms": M }
 ****************************************************************************/

int tool_board_led_execute(const char *input_json,
                           char *output, size_t output_size)
{
  int fds[2] = { -1, -1 };
  int pins[2] = { LED_RED_PIN, LED_GREEN_PIN };
  bool led_on[2] = { false, false };
  char led_name[16];
  char action_name[16];
  const char *led = "red";
  const char *action = "on";
  int times = 3;
  int interval_ms = 500;
  int count = 1;
  int i;
  cJSON *root;
  cJSON *j_led;
  cJSON *j_action;
  cJSON *j_times;
  cJSON *j_interval;
  cJSON *resp;

  if (!input_json || !output || output_size < 32)
    {
      return json_fail(output, output_size, "{\"error\":\"invalid arguments\"}", NULL);
    }

  root = cJSON_Parse(input_json);
  if (!root)
    {
      return json_fail(output, output_size, "{\"error\":\"cannot parse input JSON\"}", NULL);
    }

  j_led = cJSON_GetObjectItem(root, "led");
  j_action = cJSON_GetObjectItem(root, "action");
  j_times = cJSON_GetObjectItem(root, "times");
  j_interval = cJSON_GetObjectItem(root, "interval_ms");

  if (cJSON_IsString(j_led))
    {
      led = j_led->valuestring;
    }

  if (!cJSON_IsString(j_action))
    {
      cJSON_Delete(root);
      return json_fail(output, output_size,
                       "{\"error\":\"missing required field: action\"}", NULL);
    }

  action = j_action->valuestring;

  /* Copy strings out of the parsed tree: cJSON_Delete(root) below
   * frees them, and they are still needed when building the reply.
   * (Use-after-free here produced "led":"","action":"" in replies.) */
  strncpy(led_name, led, sizeof(led_name) - 1);
  strncpy(action_name, action, sizeof(action_name) - 1);
  led = led_name;
  action = action_name;

  if (cJSON_IsNumber(j_times) && j_times->valuedouble >= 1 &&
      j_times->valuedouble <= 10)
    {
      times = (int)j_times->valuedouble;
    }

  if (cJSON_IsNumber(j_interval) && j_interval->valuedouble >= 50 &&
      j_interval->valuedouble <= 5000)
    {
      interval_ms = (int)j_interval->valuedouble;
    }

  if (strcmp(led, "red") == 0)
    {
      count = 1;
    }
  else if (strcmp(led, "green") == 0)
    {
      count = 1;
      pins[0] = LED_GREEN_PIN;
    }
  else if (strcmp(led, "both") == 0 || strcmp(led, "all") == 0)
    {
      count = 2;
    }
  else
    {
      cJSON_Delete(root);
      return json_fail(output, output_size,
                       "{\"error\":\"unknown led '%s'. valid: red, green, both\"}", led);
    }

  if (strcmp(action, "on") != 0 && strcmp(action, "off") != 0 &&
      strcmp(action, "toggle") != 0 && strcmp(action, "blink") != 0)
    {
      cJSON_Delete(root);
      return json_fail(output, output_size,
                       "{\"error\":\"unknown action '%s'. valid: on, off, toggle, blink\"}",
                       action);
    }

  /* Open the selected LEDs */

  for (i = 0; i < count; i++)
    {
      fds[i] = gpio_open_output(pins[i]);
      if (fds[i] < 0)
        {
          int j;
          for (j = 0; j < i; j++)
            {
              close(fds[j]);
            }

          cJSON_Delete(root);
          snprintf(output, output_size,
                   "{\"error\":\"open GPIO%d failed (errno %d)\"}", pins[i], errno);
          return ERROR;
        }
    }

  /* Execute action. LEDs are active-HIGH: written level 1 = lit. */

  if (strcmp(action, "on") == 0)
    {
      for (i = 0; i < count; i++)
        {
          gpio_write_level(fds[i], 1);        /* high = ON */
          led_on[i] = true;
        }
    }
  else if (strcmp(action, "off") == 0)
    {
      for (i = 0; i < count; i++)
        {
          gpio_write_level(fds[i], 0);        /* low = OFF */
          led_on[i] = false;
        }
    }
  else if (strcmp(action, "toggle") == 0)
    {
      int level[2] = { 1, 1 };

      for (i = 0; i < count; i++)
        {
          gpio_read_level(fds[i], &level[i]);
          level[i] = !level[i];
          gpio_write_level(fds[i], level[i]);
          led_on[i] = (level[i] != 0);        /* high = ON */
        }
    }
  else /* blink */
    {
      int n;

      for (n = 0; n < times; n++)
        {
          for (i = 0; i < count; i++)
            {
              gpio_write_level(fds[i], 1);   /* on (high) */
            }

          up_mdelay(interval_ms);

          for (i = 0; i < count; i++)
            {
              gpio_write_level(fds[i], 0);   /* off (low) */
            }

          if (n < times - 1)
            {
              up_mdelay(interval_ms);
            }
        }

      for (i = 0; i < count; i++)
        {
          led_on[i] = false;   /* blink ends off */
        }
    }

  for (i = 0; i < count; i++)
    {
      close(fds[i]);
    }

  cJSON_Delete(root);

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddStringToObject(resp, "led", led);
  cJSON_AddStringToObject(resp, "action", action);
  if (strcmp(action, "blink") == 0)
    {
      cJSON_AddNumberToObject(resp, "blinks", times);
    }
  else
    {
      cJSON_AddStringToObject(resp, "state", led_on[0] ? "on" : "off");
    }

  json_reply(resp, output, output_size);
  syslog(LOG_INFO, "[%s] led=%s action=%s\n", TAG, led, action);
  return OK;
}

/****************************************************************************
 * Tool: board_pwm
 *
 * Input: { "duty": 0-100, "frequency": Hz (default 1000) }
 * duty=0 stops PWM. Controls blue LED on GPIO88.
 ****************************************************************************/

int tool_board_pwm_execute(const char *input_json,
                           char *output, size_t output_size)
{
  int fd;
  int ret;
  uint32_t duty;
  uint32_t freq = PWM_FREQ_DEFAULT;
  struct pwm_info_s info;
  cJSON *root;
  cJSON *j_duty;
  cJSON *j_freq;
  cJSON *resp;

  if (!input_json || !output || output_size < 32)
    {
      return json_fail(output, output_size, "{\"error\":\"invalid arguments\"}", NULL);
    }

  root = cJSON_Parse(input_json);
  if (!root)
    {
      return json_fail(output, output_size, "{\"error\":\"cannot parse input JSON\"}", NULL);
    }

  j_duty = cJSON_GetObjectItem(root, "duty");
  j_freq = cJSON_GetObjectItem(root, "frequency");

  if (!cJSON_IsNumber(j_duty))
    {
      cJSON_Delete(root);
      return json_fail(output, output_size,
                       "{\"error\":\"missing required field: duty (0-100)\"}", NULL);
    }

  duty = (uint32_t)j_duty->valuedouble;
  if (duty > 100)
    {
      duty = 100;
    }

  if (cJSON_IsNumber(j_freq) && j_freq->valuedouble >= 10 &&
      j_freq->valuedouble <= 100000)
    {
      freq = (uint32_t)j_freq->valuedouble;
    }

  cJSON_Delete(root);

  /* Pinmux to PWM2 second function */

  ret = pinctrl_set_function(PWM_PIN, PWM_FUNCTION);
  if (ret < 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"pinctrl set GPIO88 to PWM failed (%d)\"}", ret);
      return ERROR;
    }

  fd = open(PWM_DEVICE, O_RDONLY);
  if (fd < 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"open %s failed (errno %d)\"}", PWM_DEVICE, errno);
      return ERROR;
    }

  info.frequency = freq;
  info.duty = (uint32_t)((uint64_t)duty * PWM_DUTY_MAX / 100);

  ret = ioctl(fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)(uintptr_t)&info);
  if (ret < 0)
    {
      close(fd);
      snprintf(output, output_size,
               "{\"error\":\"PWMIOC_SETCHARACTERISTICS failed (errno %d)\"}", errno);
      return ERROR;
    }

  if (duty == 0)
    {
      ioctl(fd, PWMIOC_STOP, 0);
    }
  else
    {
      ioctl(fd, PWMIOC_START, 0);
    }

  close(fd);

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddNumberToObject(resp, "duty_percent", duty);
  cJSON_AddNumberToObject(resp, "frequency_hz", freq);
  cJSON_AddStringToObject(resp, "state", duty > 0 ? "on" : "stopped");
  json_reply(resp, output, output_size);

  syslog(LOG_INFO, "[%s] pwm duty=%u%% freq=%uHz\n", TAG, duty, freq);
  return OK;
}

/****************************************************************************
 * Tool: board_buzzer
 *
 * Input: { "duration_ms": 100 (1-5000), "count": 1 }
 * Passive buzzer on GPIO75, software 1 kHz square wave.
 ****************************************************************************/

int tool_board_buzzer_execute(const char *input_json,
                              char *output, size_t output_size)
{
  int fd;
  int duration_ms = 200;
  int count = 1;
  int n;
  long cycles;
  long c;
  cJSON *root;
  cJSON *j_dur;
  cJSON *j_count;
  cJSON *resp;

  if (!input_json || !output || output_size < 32)
    {
      return json_fail(output, output_size, "{\"error\":\"invalid arguments\"}", NULL);
    }

  root = cJSON_Parse(input_json);
  if (!root)
    {
      return json_fail(output, output_size, "{\"error\":\"cannot parse input JSON\"}", NULL);
    }

  j_dur = cJSON_GetObjectItem(root, "duration_ms");
  j_count = cJSON_GetObjectItem(root, "count");

  if (cJSON_IsNumber(j_dur) && j_dur->valuedouble >= 1 &&
      j_dur->valuedouble <= 5000)
    {
      duration_ms = (int)j_dur->valuedouble;
    }

  if (cJSON_IsNumber(j_count) && j_count->valuedouble >= 1 &&
      j_count->valuedouble <= 10)
    {
      count = (int)j_count->valuedouble;
    }

  cJSON_Delete(root);

  fd = gpio_open_output(BUZZER_PIN);
  if (fd < 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"open buzzer GPIO%d failed (errno %d)\"}", BUZZER_PIN, errno);
      return ERROR;
    }

  gpio_write_level(fd, 0);

  cycles = (long)duration_ms * 1000 / (BUZZER_HALF_US * 2);

  for (n = 0; n < count; n++)
    {
      for (c = 0; c < cycles; c++)
        {
          gpio_write_level(fd, 1);
          up_udelay(BUZZER_HALF_US);
          gpio_write_level(fd, 0);
          up_udelay(BUZZER_HALF_US);
        }

      if (n < count - 1)
        {
          up_mdelay(200);
        }
    }

  close(fd);

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddNumberToObject(resp, "duration_ms", duration_ms);
  cJSON_AddNumberToObject(resp, "count", count);
  cJSON_AddNumberToObject(resp, "frequency_hz", 1000);
  json_reply(resp, output, output_size);

  syslog(LOG_INFO, "[%s] buzzer %dms x%d\n", TAG, duration_ms, count);
  return OK;
}

/****************************************************************************
 * Tool: board_key_read
 *
 * Input: {} (no params). Reads KEY1 (GPIO87) and KEY2 (GPIO86).
 * Output: { "key1": "pressed"|"released", "key2": "pressed"|"released" }
 ****************************************************************************/

int tool_board_key_read_execute(const char *input_json,
                                char *output, size_t output_size)
{
  int fd1;
  int fd2;
  int lvl1 = 1;
  int lvl2 = 1;
  cJSON *resp;

  fd1 = gpio_open_input(KEY1_PIN);
  if (fd1 >= 0)
    {
      gpio_read_level(fd1, &lvl1);
      close(fd1);
    }

  fd2 = gpio_open_input(KEY2_PIN);
  if (fd2 >= 0)
    {
      gpio_read_level(fd2, &lvl2);
      close(fd2);
    }

  if (fd1 < 0 || fd2 < 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"open key GPIO failed (errno %d)\"}", errno);
      return ERROR;
    }

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  /* active-low: level 0 = pressed */
  cJSON_AddStringToObject(resp, "key1", lvl1 == 0 ? "pressed" : "released");
  cJSON_AddStringToObject(resp, "key2", lvl2 == 0 ? "pressed" : "released");
  json_reply(resp, output, output_size);

  syslog(LOG_INFO, "[%s] key1=%d key2=%d\n", TAG, lvl1, lvl2);
  return OK;
}

/****************************************************************************
 * Tool: board_thermal_read
 *
 * Input: {} (no params). Reads on-die thermal sensor.
 * Output: { "temperature_celsius": 45.1 }
 ****************************************************************************/

int tool_board_thermal_read_execute(const char *input_json,
                                    char *output, size_t output_size)
{
  int fd;
  int32_t temp;
  cJSON *resp;

  fd = open(THERMAL_PATH, O_RDONLY);
  if (fd < 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"open %s failed (errno %d)\"}", THERMAL_PATH, errno);
      return ERROR;
    }

  if (read(fd, &temp, sizeof(temp)) != sizeof(temp))
    {
      close(fd);
      snprintf(output, output_size,
               "{\"error\":\"read thermal failed (errno %d)\"}", errno);
      return ERROR;
    }

  close(fd);

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddNumberToObject(resp, "temperature_celsius", temp / 1000.0);
  json_reply(resp, output, output_size);

  syslog(LOG_INFO, "[%s] thermal %d mC\n", TAG, temp);
  return OK;
}

/****************************************************************************
 * Tool: board_adc_read
 *
 * Input: { "channel": 0-3 (default 0) }
 * Output: { "raw": 2048, "voltage_mv": 1650, "voltage_v": 1.65 }
 ****************************************************************************/

int tool_board_adc_read_execute(const char *input_json,
                                char *output, size_t output_size)
{
  int channel = 0;
  uint16_t raw = 0;
  uint32_t mv;
  cJSON *root;
  cJSON *j_ch;
  cJSON *resp;

  if (!input_json || !output || output_size < 32)
    {
      return json_fail(output, output_size, "{\"error\":\"invalid arguments\"}", NULL);
    }

  root = cJSON_Parse(input_json);
  if (!root)
    {
      return json_fail(output, output_size, "{\"error\":\"cannot parse input JSON\"}", NULL);
    }

  j_ch = cJSON_GetObjectItem(root, "channel");
  if (cJSON_IsNumber(j_ch) && j_ch->valuedouble >= 0 && j_ch->valuedouble <= 3)
    {
      channel = (int)j_ch->valuedouble;
    }

  cJSON_Delete(root);

  if (mcp3204_init() < 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"MCP3204 SPI2 init failed\"}");
      return ERROR;
    }

  if (mcp3204_read_average((uint8_t)channel, &raw) != 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"MCP3204 CH%d read failed (device present?)\"}", channel);
      return ERROR;
    }

  mv = ((uint32_t)raw * MCP3204_VREF_MV + (MCP3204_MAX / 2)) / MCP3204_MAX;

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddNumberToObject(resp, "channel", channel);
  cJSON_AddNumberToObject(resp, "raw", raw);
  cJSON_AddNumberToObject(resp, "voltage_mv", mv);
  cJSON_AddNumberToObject(resp, "voltage_v", mv / 1000.0);
  json_reply(resp, output, output_size);

  syslog(LOG_INFO, "[%s] adc ch%d raw=%u mv=%u\n", TAG, channel, raw, mv);
  return OK;
}

/****************************************************************************
 * Tool: board_light_read
 *
 * Input: {} (no params). Reads BH1750 ambient light.
 * Output: { "lux": 123.45 }
 ****************************************************************************/

int tool_board_light_read_execute(const char *input_json,
                                  char *output, size_t output_size)
{
  int fd;
  uint8_t addr;
  unsigned int lux_x100 = 0;
  cJSON *resp;

  i2c1_pinctrl_setup();

  fd = open(I2C1_PATH, O_RDWR);
  if (fd < 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"open %s failed (errno %d)\"}", I2C1_PATH, errno);
      return ERROR;
    }

  if (bh1750_init(fd, &addr) < 0)
    {
      close(fd);
      snprintf(output, output_size,
               "{\"error\":\"BH1750 not detected on I2C1 (0x23/0x5C)\"}");
      return ERROR;
    }

  if (bh1750_read_lux(fd, addr, &lux_x100) < 0)
    {
      close(fd);
      snprintf(output, output_size,
               "{\"error\":\"BH1750 read failed\"}");
      return ERROR;
    }

  close(fd);

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddNumberToObject(resp, "lux", lux_x100 / 100.0);
  json_reply(resp, output, output_size);

  syslog(LOG_INFO, "[%s] light %u.%02u lux\n", TAG,
         lux_x100 / 100, lux_x100 % 100);
  return OK;
}

/****************************************************************************
 * Tool: board_oled_show
 *
 * Input: { "text": "Hello" }   (ASCII only; multiple lines separated by \n)
 * Output: { "ok": true }
 ****************************************************************************/

int tool_board_oled_show_execute(const char *input_json,
                                 char *output, size_t output_size)
{
  int fd;
  char text[257];
  const char *p;
  uint8_t x;
  uint8_t y;
  cJSON *root;
  cJSON *j_text;
  cJSON *resp;

  if (!input_json || !output || output_size < 32)
    {
      return json_fail(output, output_size, "{\"error\":\"invalid arguments\"}", NULL);
    }

  root = cJSON_Parse(input_json);
  if (!root)
    {
      return json_fail(output, output_size, "{\"error\":\"cannot parse input JSON\"}", NULL);
    }

  /* Copy text out before cJSON_Delete frees the tree. Reading the
   * dangling pointer afterwards produced an empty/garbled string and
   * a blank screen with stray pixels. */
  text[0] = '\0';
  j_text = cJSON_GetObjectItem(root, "text");
  if (cJSON_IsString(j_text))
    {
      strncpy(text, j_text->valuestring, sizeof(text) - 1);
    }

  cJSON_Delete(root);

  if (strlen(text) == 0)
    {
      return json_fail(output, output_size,
                       "{\"error\":\"missing required field: text\"}", NULL);
    }

  i2c1_pinctrl_setup();

  fd = open(I2C1_PATH, O_RDWR);
  if (fd < 0)
    {
      snprintf(output, output_size,
               "{\"error\":\"open %s failed (errno %d)\"}", I2C1_PATH, errno);
      return ERROR;
    }

  if (oled_init_device(fd) < 0)
    {
      close(fd);
      snprintf(output, output_size,
               "{\"error\":\"SSD1306 not detected on I2C1 (0x3C)\"}");
      return ERROR;
    }

  oled_fill_screen(fd, 0x00);

  x = 0;
  y = 0;
  for (p = text; *p; p++)
    {
      if (*p == '\n')
        {
          x = 0;
          y += 2;
          if (y >= OLED_PAGES - 1)
            {
              break;
            }

          continue;
        }

      oled_show_char(fd, x, y, *p);
      x += 8;
      if (x > (OLED_WIDTH - 8))
        {
          x = 0;
          y += 2;
          if (y >= OLED_PAGES - 1)
            {
              break;
            }
        }
    }

  close(fd);

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddStringToObject(resp, "displayed", text);
  json_reply(resp, output, output_size);

  syslog(LOG_INFO, "[%s] oled show\n", TAG);
  return OK;
}

/****************************************************************************
 * Tool: smart_light_control (smarthome-skill compatible)
 *
 * Maps the skill's virtual devices onto real board hardware:
 *   living_room -> red LED (GPIO72, active-high)
 *   bedroom     -> green LED (GPIO73, active-high)
 *   kitchen     -> blue LED via PWM2 (GPIO88)
 *   any other device name falls back to the red LED
 *
 * Actions:
 *   turn_on / turn_off: switch the mapped device
 *   set_brightness: kitchen -> PWM duty; red/green -> on(>0)/off(0)
 *   set_color: cross-device scene (warm=red, cool=blue,
 *              daylight/white=all on) - only meaningful when the
 *              caller does not pin a single device
 ****************************************************************************/

#define BLUE_LED_DUTY_MAX 100

static int smarthome_led_onoff(int pin, bool on)
{
  int fd = gpio_open_output(pin);
  int ret;

  if (fd < 0)
    {
      return -1;
    }

  ret = gpio_write_level(fd, on ? 1 : 0);  /* active-high */
  close(fd);
  return ret < 0 ? -1 : 0;
}

static int smarthome_pwm_set(int percent)
{
  int fd;
  int ret;
  struct pwm_info_s info;

  if (pinctrl_set_function(PWM_PIN, PWM_FUNCTION) < 0)
    {
      return -1;
    }

  fd = open(PWM_DEVICE, O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }

  info.frequency = PWM_FREQ_DEFAULT;
  info.duty = (uint32_t)((uint64_t)percent * PWM_DUTY_MAX / 100);

  ret = ioctl(fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)(uintptr_t)&info);
  if (ret == 0)
    {
      if (percent == 0)
        {
          ioctl(fd, PWMIOC_STOP, 0);
        }
      else
        {
          ioctl(fd, PWMIOC_START, 0);
        }
    }

  close(fd);
  return ret < 0 ? -1 : 0;
}

int tool_smart_light_control_execute(const char *input_json,
                                     char *output, size_t output_size)
{
  cJSON *root;
  cJSON *j_device;
  cJSON *j_action;
  cJSON *j_brightness;
  cJSON *j_color;
  char device_name[32];
  char action_name[24];
  const char *device;
  const char *action;
  int pin = LED_RED_PIN;
  bool is_pwm = false;
  bool on = false;
  int brightness = 100;
  cJSON *resp;

  if (!input_json || !output || output_size < 32)
    {
      return json_fail(output, output_size, "{\"error\":\"invalid arguments\"}", NULL);
    }

  root = cJSON_Parse(input_json);
  if (!root)
    {
      return json_fail(output, output_size, "{\"error\":\"cannot parse input JSON\"}", NULL);
    }

  j_device = cJSON_GetObjectItem(root, "device");
  j_action = cJSON_GetObjectItem(root, "action");

  if (!cJSON_IsString(j_device) || !cJSON_IsString(j_action))
    {
      cJSON_Delete(root);
      return json_fail(output, output_size,
                       "{\"error\":\"missing required fields: device, action\"}", NULL);
    }

  device = j_device->valuestring;
  action = j_action->valuestring;

  /* Copy strings out of the parsed tree before any cJSON_Delete:
   * they are referenced in the reply JSON built after deletion. */
  strncpy(device_name, device, sizeof(device_name) - 1);
  strncpy(action_name, action, sizeof(action_name) - 1);
  device = device_name;
  action = action_name;

  /* Map device name to hardware */

  if (strcmp(device, "bedroom") == 0)
    {
      pin = LED_GREEN_PIN;
    }
  else if (strcmp(device, "kitchen") == 0)
    {
      is_pwm = true;
    }
  /* living_room and everything else -> red LED */

  if (strcmp(action, "turn_on") == 0)
    {
      on = true;
      if (is_pwm)
        {
          if (smarthome_pwm_set(100) < 0)
            {
              cJSON_Delete(root);
              snprintf(output, output_size,
                       "{\"error\":\"PWM device open failed\"}");
              return ERROR;
            }
        }
      else if (smarthome_led_onoff(pin, true) < 0)
        {
          cJSON_Delete(root);
          snprintf(output, output_size,
                   "{\"error\":\"GPIO%d open/write failed\"}", pin);
          return ERROR;
        }
    }
  else if (strcmp(action, "turn_off") == 0)
    {
      on = false;
      if (is_pwm)
        {
          smarthome_pwm_set(0);
        }
      else if (smarthome_led_onoff(pin, false) < 0)
        {
          cJSON_Delete(root);
          snprintf(output, output_size,
                   "{\"error\":\"GPIO%d open/write failed\"}", pin);
          return ERROR;
        }
    }
  else if (strcmp(action, "set_brightness") == 0)
    {
      j_brightness = cJSON_GetObjectItem(root, "brightness");
      if (!cJSON_IsNumber(j_brightness))
        {
          cJSON_Delete(root);
          return json_fail(output, output_size,
                           "{\"error\":\"set_brightness requires 'brightness' (0-100) field\"}", NULL);
        }

      brightness = (int)j_brightness->valuedouble;
      if (brightness < 0)
        {
          brightness = 0;
        }
      if (brightness > 100)
        {
          brightness = 100;
        }

      if (is_pwm)
        {
          if (smarthome_pwm_set(brightness) < 0)
            {
              cJSON_Delete(root);
              snprintf(output, output_size,
                       "{\"error\":\"PWM device open failed\"}");
              return ERROR;
            }

          on = (brightness > 0);
        }
      else
        {
          /* red/green LEDs have no dimming: brightness > 0 = full on */
          on = (brightness > 0);
          if (smarthome_led_onoff(pin, on) < 0)
            {
              cJSON_Delete(root);
              snprintf(output, output_size,
                       "{\"error\":\"GPIO%d open/write failed\"}", pin);
              return ERROR;
            }
        }
    }
  else if (strcmp(action, "set_color") == 0)
    {
      j_color = cJSON_GetObjectItem(root, "color");
      if (!cJSON_IsString(j_color))
        {
          cJSON_Delete(root);
          return json_fail(output, output_size,
                           "{\"error\":\"set_color requires 'color' field\"}", NULL);
        }

      /* Scene mapping: light the corresponding LED(s) */
      const char *color = j_color->valuestring;
      bool all_ok = true;

      if (strcmp(color, "warm") == 0)
        {
          all_ok = smarthome_led_onoff(LED_RED_PIN, true) == 0;
          smarthome_led_onoff(LED_GREEN_PIN, false);
          smarthome_pwm_set(0);
        }
      else if (strcmp(color, "cool") == 0)
        {
          smarthome_led_onoff(LED_RED_PIN, false);
          smarthome_led_onoff(LED_GREEN_PIN, false);
          all_ok = smarthome_pwm_set(100) == 0;
        }
      else if (strcmp(color, "daylight") == 0 || strcmp(color, "white") == 0)
        {
          all_ok = smarthome_led_onoff(LED_RED_PIN, true) == 0;
          all_ok &= smarthome_led_onoff(LED_GREEN_PIN, true) == 0;
          all_ok &= smarthome_pwm_set(100) == 0;
        }
      else
        {
          cJSON_Delete(root);
          return json_fail(output, output_size,
                           "{\"error\":\"unknown color '%s'. valid: warm, cool, daylight\"}", color);
        }

      if (!all_ok)
        {
          cJSON_Delete(root);
          snprintf(output, output_size,
                   "{\"error\":\"LED/PWM device access failed\"}");
          return ERROR;
        }

      on = true;
    }
  else
    {
      cJSON_Delete(root);
      return json_fail(output, output_size,
                       "{\"error\":\"unknown action '%s'. valid: turn_on, turn_off, set_brightness, set_color\"}",
                       action);
    }

  cJSON_Delete(root);

  resp = cJSON_CreateObject();
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddStringToObject(resp, "device", device);
  cJSON_AddStringToObject(resp, "hardware",
                          is_pwm ? "blue LED (PWM GPIO88)"
                                 : (pin == LED_GREEN_PIN ? "green LED (GPIO73)"
                                                         : "red LED (GPIO72)"));
  cJSON_AddStringToObject(resp, "state", on ? "on" : "off");
  if (strcmp(action, "set_brightness") == 0)
    {
      cJSON_AddNumberToObject(resp, "brightness", brightness);
    }

  json_reply(resp, output, output_size);
  syslog(LOG_INFO, "[%s] smart_light device=%s action=%s -> %s\n",
         TAG, device, action, on ? "ON" : "OFF");
  return OK;
}

/****************************************************************************
 * Tool: env_sensor_read (smarthome-skill compatible)
 *
 * temperature -> on-die thermal sensor (real)
 * light       -> BH1750 (real; error JSON if module absent)
 * humidity    -> not available on this board (null + note)
 ****************************************************************************/

int tool_env_sensor_read_execute(const char *input_json,
                                 char *output, size_t output_size)
{
  cJSON *root;
  cJSON *j_type;
  cJSON *j_device;
  char device_name[32];
  char type_name[16];
  const char *sensor_type = "all";
  const char *device = "board";
  bool want_temp;
  bool want_hum;
  bool want_light;
  cJSON *resp;
  cJSON *readings;
  bool any_ok = false;
  bool any_fail = false;

  if (!input_json || !output || output_size < 32)
    {
      return json_fail(output, output_size, "{\"error\":\"invalid arguments\"}", NULL);
    }

  root = cJSON_Parse(input_json);
  if (!root)
    {
      return json_fail(output, output_size, "{\"error\":\"cannot parse input JSON\"}", NULL);
    }

  j_device = cJSON_GetObjectItem(root, "device");
  j_type = cJSON_GetObjectItem(root, "sensor_type");

  if (cJSON_IsString(j_device))
    {
      device = j_device->valuestring;
    }

  if (cJSON_IsString(j_type))
    {
      sensor_type = j_type->valuestring;
    }

  /* Copy out before cJSON_Delete frees the tree (device is used in
   * the reply JSON after deletion). */
  strncpy(device_name, device, sizeof(device_name) - 1);
  strncpy(type_name, sensor_type, sizeof(type_name) - 1);
  device = device_name;
  sensor_type = type_name;

  want_temp = (strcmp(sensor_type, "all") == 0 ||
               strcmp(sensor_type, "temperature") == 0);
  want_hum = (strcmp(sensor_type, "all") == 0 ||
              strcmp(sensor_type, "humidity") == 0);
  want_light = (strcmp(sensor_type, "all") == 0 ||
                strcmp(sensor_type, "light") == 0);

  if (!want_temp && !want_hum && !want_light)
    {
      cJSON_Delete(root);
      return json_fail(output, output_size,
                       "{\"error\":\"unknown sensor_type '%s'. valid: temperature, humidity, light, all\"}",
                       sensor_type);
    }

  cJSON_Delete(root);

  resp = cJSON_CreateObject();
  readings = cJSON_CreateObject();

  if (want_temp)
    {
      int fd = open(THERMAL_PATH, O_RDONLY);
      int32_t temp = 0;

      if (fd >= 0 && read(fd, &temp, sizeof(temp)) == sizeof(temp))
        {
          cJSON_AddNumberToObject(readings, "temperature_celsius",
                                  temp / 1000.0);
          any_ok = true;
        }
      else
        {
          any_fail = true;
        }

      if (fd >= 0)
        {
          close(fd);
        }
    }

  if (want_hum)
    {
      /* No humidity sensor on LOONG-HAT */
      cJSON_AddNullToObject(readings, "humidity_percent");
      cJSON_AddStringToObject(readings, "humidity_note",
                              "no humidity sensor on this board");
    }

  if (want_light)
    {
      i2c1_pinctrl_setup();
      int fd = open(I2C1_PATH, O_RDWR);
      uint8_t addr;
      unsigned int lux_x100 = 0;

      if (fd >= 0 && bh1750_init(fd, &addr) == OK &&
          bh1750_read_lux(fd, addr, &lux_x100) == OK)
        {
          cJSON_AddNumberToObject(readings, "light_lux", lux_x100 / 100.0);
          any_ok = true;
        }
      else
        {
          any_fail = true;
        }

      if (fd >= 0)
        {
          close(fd);
        }
    }

  cJSON_AddItemToObject(resp, "readings", readings);
  cJSON_AddStringToObject(resp, "device", device);

  if (!any_ok && any_fail)
    {
      /* Nothing readable at all: report as failure */
      cJSON_Delete(resp);
      snprintf(output, output_size,
               "{\"error\":\"sensor read failed (thermal/BH1750 not accessible)\"}");
      return ERROR;
    }

  cJSON_AddBoolToObject(resp, "ok", true);
  if (any_fail)
    {
      cJSON_AddBoolToObject(resp, "partial", true);
    }

  json_reply(resp, output, output_size);
  syslog(LOG_INFO, "[%s] env_sensor type=%s\n", TAG, sensor_type);
  return OK;
}
