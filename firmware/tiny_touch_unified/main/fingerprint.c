#include "fingerprint.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "fingerprint";

static const uart_port_t FP_UART = UART_NUM_1;
static const int FP_TX_PIN = 43;
static const int FP_RX_PIN = 44;
static const int FP_INT_PIN = 2;
static const int INT_ACTIVE_VALUE = 1;
static const uint16_t START_SLOT = 1;
static const uint16_t END_SLOT = 5;
// This timeout returns the LED to idle when an interactive request is abandoned.
// macOS does not send tinyTouch a dedicated popup-cancel event.
static const uint32_t FINGER_WAIT_MS = 7000;
static const uint8_t FP_LED_BLUE = 0x01;
static const uint8_t FP_LED_GREEN = 0x02;
static const uint8_t FP_LED_RED = 0x04;
static const uint8_t FP_LED_YELLOW = 0x06;
static const uint8_t FP_LED_AURA_COMMAND = 0x3c;
static const uint8_t FP_LED_CONTROL_MODE_COMMAND = 0x60;
static const uint8_t FP_LED_MANUAL_CONTROL_MODE = 0x00;
static const uint8_t FP_LED_MANUAL_CONTROL_MODE_CONFIGURED = 0x01;
static const uint8_t FP_LED_FUNC_BREATHING = 0x01;
static const uint8_t FP_LED_FUNC_FLASH = 0x02;
static const uint8_t FP_LED_CYCLES_FOREVER = 0;
static const uint8_t FP_LED_FLASH_COUNT = 2;
static const uint8_t FP_LED_FLASH_DUTY_HALF = 0x11;
static const uint8_t FP_LED_IDLE_PERIOD_TENTHS = 100;
static const uint8_t FP_LED_PROMPT_PERIOD_TENTHS = 10;
static const uint8_t FP_LED_FLASH_PERIOD_TENTHS = 5;
static const uint32_t FP_LED_TENTH_SECOND_MS = 100;
// A host-requested prompt is held for this long and then falls back to idle.
// macOS emits no event when its PIN dialog is dismissed, so an abandoned prompt
// would otherwise leave the LED breathing yellow indefinitely.
static const uint32_t FP_LED_PROMPT_HOLD_MS = 30000;
static const char *FP_LED_NVS_NAMESPACE = "fingerprint";
static const char *FP_LED_MANUAL_MODE_KEY = "led_manual_mode";
static const uint32_t FP_IMAGE_COMMAND_MAX_WAIT_MS = 1000;
static const uint32_t FP_IMAGE_RETRY_DELAY_MS = 150;
static const uint8_t FP_AUTO_ENROLL_COMMAND = 0x31;
static const uint8_t FP_CANCEL_COMMAND = 0x30;
static const uint16_t FP_AUTO_ENROLL_ALLOW_OVERWRITE = 1U << 3;
static const uint32_t FP_ENROLL_TIMEOUT_MS = 90000;
static const uint32_t FP_ENROLL_INITIAL_LIFT_TIMEOUT_MS = 10000;
static const uint32_t FP_ENROLL_LIFT_SETTLE_MS = 250;
static const uint8_t FP_RESPONSE_ACK_PACKET_ID = 0x07;
static const uint8_t FP_CONFIRM_SUCCESS = 0x00;
static const uint8_t FP_CONFIRM_FEATURE_FAILURE = 0x07;
static const uint8_t FP_AUTO_ENROLL_FINAL_MERGE = 0xf0;
static const uint8_t FP_AUTO_ENROLL_FINAL_DUPLICATE_CHECK = 0xf1;
static const uint8_t FP_AUTO_ENROLL_FINAL_STORE = 0xf2;

enum {
  FP_ENROLL_CAPTURE_COUNT = 5,
  FP_RESPONSE_HEADER_SIZE = 9,
  FP_RESPONSE_BODY_CAPACITY = 32,
};

typedef enum {
  FP_AUTO_ENROLL_STAGE_VALIDATE = 0x00,
  FP_AUTO_ENROLL_STAGE_IMAGE = 0x01,
  FP_AUTO_ENROLL_STAGE_FEATURE = 0x02,
  FP_AUTO_ENROLL_STAGE_LIFT = 0x03,
  FP_AUTO_ENROLL_STAGE_MERGE = 0x04,
  FP_AUTO_ENROLL_STAGE_DUPLICATE_CHECK = 0x05,
  FP_AUTO_ENROLL_STAGE_STORE = 0x06,
} fp_auto_enroll_stage_t;

typedef enum {
  FP_LED_STATE_UNKNOWN,
  FP_LED_STATE_IDLE,
  FP_LED_STATE_PROMPT,
  FP_LED_STATE_REJECTED,
  FP_LED_STATE_ACCEPTED,
} fp_led_state_t;

typedef struct {
  uint8_t function;
  uint8_t start_color;
  uint8_t end_color_or_duty;
  uint8_t cycles;
  uint8_t period_tenths;
} fp_led_command_t;

static const fp_led_command_t FP_LED_IDLE_COMMAND = {
  .function = FP_LED_FUNC_BREATHING,
  .start_color = FP_LED_BLUE,
  .end_color_or_duty = FP_LED_BLUE,
  .cycles = FP_LED_CYCLES_FOREVER,
  .period_tenths = FP_LED_IDLE_PERIOD_TENTHS,
};
static const fp_led_command_t FP_LED_PROMPT_COMMAND = {
  .function = FP_LED_FUNC_BREATHING,
  .start_color = FP_LED_YELLOW,
  .end_color_or_duty = FP_LED_YELLOW,
  .cycles = FP_LED_CYCLES_FOREVER,
  .period_tenths = FP_LED_PROMPT_PERIOD_TENTHS,
};
static const fp_led_command_t FP_LED_REJECTED_COMMAND = {
  .function = FP_LED_FUNC_FLASH,
  .start_color = FP_LED_RED,
  .end_color_or_duty = FP_LED_FLASH_DUTY_HALF,
  .cycles = FP_LED_FLASH_COUNT,
  .period_tenths = FP_LED_FLASH_PERIOD_TENTHS,
};
static const fp_led_command_t FP_LED_ACCEPTED_COMMAND = {
  .function = FP_LED_FUNC_FLASH,
  .start_color = FP_LED_GREEN,
  .end_color_or_duty = FP_LED_FLASH_DUTY_HALF,
  .cycles = FP_LED_FLASH_COUNT,
  .period_tenths = FP_LED_FLASH_PERIOD_TENTHS,
};

static fp_led_state_t led_state = FP_LED_STATE_UNKNOWN;
static SemaphoreHandle_t fp_mutex;
// Tick at which a host-requested prompt returns to idle; 0 when none is armed.
static TickType_t led_prompt_deadline;

static uint16_t fp_checksum(uint8_t packet_id, const uint8_t *payload, size_t payload_len) {
  uint16_t length = payload_len + 2;
  uint32_t total = packet_id + (length >> 8) + (length & 0xff);
  for (size_t i = 0; i < payload_len; i++) total += payload[i];
  return (uint16_t)total;
}

static bool fp_send_command(uint8_t instruction, const uint8_t *params, size_t param_len) {
  uint8_t drain[64];
  while (uart_read_bytes(FP_UART, drain, sizeof(drain), 0) > 0) {}

  uint8_t payload[32];
  if (param_len + 1 > sizeof(payload)) return false;
  payload[0] = instruction;
  if (param_len) memcpy(payload + 1, params, param_len);

  const size_t payload_len = param_len + 1;
  const uint16_t length = payload_len + 2;
  const uint16_t sum = fp_checksum(0x01, payload, payload_len);
  const uint8_t header[] = {
    0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01,
    (uint8_t)(length >> 8), (uint8_t)(length & 0xff)
  };
  const uint8_t sum_bytes[] = {(uint8_t)(sum >> 8), (uint8_t)(sum & 0xff)};

  return uart_write_bytes(FP_UART, header, sizeof(header)) == (int)sizeof(header) &&
         uart_write_bytes(FP_UART, payload, payload_len) == (int)payload_len &&
         uart_write_bytes(FP_UART, sum_bytes, sizeof(sum_bytes)) == (int)sizeof(sum_bytes);
}

static bool fp_command(uint8_t instruction, const uint8_t *params, size_t param_len,
                       uint8_t *confirm, uint8_t *data, size_t *data_len,
                       uint32_t timeout_ms) {
  if (!fp_send_command(instruction, params, param_len)) return false;

  uint8_t response[96];
  size_t pos = 0;
  const size_t data_cap = (data && data_len) ? *data_len : 0;
  size_t out_len = 0;
  bool saw_ack = false;
  TickType_t post_ack_until = 0;
  TickType_t start = xTaskGetTickCount();
  TickType_t deadline = pdMS_TO_TICKS(timeout_ms);
  if (data && data_len) *data_len = 0;

  while ((xTaskGetTickCount() - start) < deadline) {
    int n = uart_read_bytes(FP_UART, response + pos, sizeof(response) - pos, pdMS_TO_TICKS(10));
    if (n <= 0) continue;
    pos += (size_t)n;

    while (pos >= 2 && !(response[0] == 0xef && response[1] == 0x01)) {
      memmove(response, response + 1, --pos);
    }
    if (pos < 9) continue;

    uint8_t packet_id = response[6];
    uint16_t resp_len = ((uint16_t)response[7] << 8) | response[8];
    size_t expected = 9 + resp_len;
    if (expected > sizeof(response)) return false;
    if (pos < expected) continue;

    if (packet_id == 0x07) {
      *confirm = response[9];
      saw_ack = true;
      size_t actual_len = resp_len > 3 ? resp_len - 3 : 0;
      if (data && data_len && actual_len) {
        size_t copy_len = actual_len;
        if (copy_len > data_cap - out_len) copy_len = data_cap - out_len;
        memcpy(data + out_len, response + 10, copy_len);
        out_len += copy_len;
        *data_len = out_len;
      }
      if (*confirm != 0x00 || !data || !data_len || out_len >= data_cap) return true;
      post_ack_until = xTaskGetTickCount() + pdMS_TO_TICKS(120);
    } else if (packet_id == 0x02 && data && data_len) {
      size_t actual_len = resp_len > 2 ? resp_len - 2 : 0;
      if (actual_len) {
        size_t copy_len = actual_len;
        if (copy_len > data_cap - out_len) copy_len = data_cap - out_len;
        memcpy(data + out_len, response + 9, copy_len);
        out_len += copy_len;
        *data_len = out_len;
      }
      if (saw_ack && out_len >= data_cap) return true;
    }

    size_t remaining = pos - expected;
    if (remaining) memmove(response, response + expected, remaining);
    pos = remaining;

    if (saw_ack && post_ack_until && xTaskGetTickCount() > post_ack_until) return true;
  }

  return saw_ack;
}

static bool fp_take(uint32_t timeout_ms) {
  return fp_mutex && xSemaphoreTake(fp_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void fp_give(void) {
  if (fp_mutex) xSemaphoreGive(fp_mutex);
}

static bool led_manual_mode_provisioned(void) {
  nvs_handle_t handle;
  if (nvs_open(FP_LED_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

  uint8_t configured = 0;
  esp_err_t result = nvs_get_u8(handle, FP_LED_MANUAL_MODE_KEY, &configured);
  nvs_close(handle);
  return result == ESP_OK && configured == FP_LED_MANUAL_CONTROL_MODE_CONFIGURED;
}

// The fingerprint mutex must be held by the caller for the whole UART exchange.
static bool configure_led_manual_mode(void) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open(FP_LED_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "could not prepare manual LED mode err=%s", esp_err_to_name(result));
    return false;
  }

  const uint8_t params[] = {FP_LED_MANUAL_CONTROL_MODE};
  uint8_t confirm = 0xff;
  if (!fp_command(FP_LED_CONTROL_MODE_COMMAND, params, sizeof(params), &confirm, NULL, NULL,
                  1000) || confirm != 0x00) {
    nvs_close(handle);
    ESP_LOGW(TAG, "manual LED mode rejected confirm=0x%02x", confirm);
    return false;
  }

  result = nvs_set_u8(handle, FP_LED_MANUAL_MODE_KEY, FP_LED_MANUAL_CONTROL_MODE_CONFIGURED);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "could not record manual LED mode err=%s; LED effects remain disabled",
             esp_err_to_name(result));
    return false;
  }

  ESP_LOGW(TAG, "manual LED mode configured; power-cycle the ZW111 before using LED effects");
  return true;
}

static const fp_led_command_t *led_command_for_state(fp_led_state_t state) {
  switch (state) {
    case FP_LED_STATE_IDLE:
      return &FP_LED_IDLE_COMMAND;
    case FP_LED_STATE_PROMPT:
      return &FP_LED_PROMPT_COMMAND;
    case FP_LED_STATE_REJECTED:
      return &FP_LED_REJECTED_COMMAND;
    case FP_LED_STATE_ACCEPTED:
      return &FP_LED_ACCEPTED_COMMAND;
    case FP_LED_STATE_UNKNOWN:
    default:
      return NULL;
  }
}

static uint32_t led_command_duration_ms(const fp_led_command_t *command) {
  return (uint32_t)command->cycles * command->period_tenths * FP_LED_TENTH_SECOND_MS;
}

// The fingerprint mutex must be held by the caller for the whole UART exchange.
static bool led_apply(fp_led_state_t state) {
  if (state == led_state) return true;

  const fp_led_command_t *command = led_command_for_state(state);
  if (command == NULL) return false;
  const uint8_t params[] = {
    command->function,
    command->start_color,
    command->end_color_or_duty,
    command->cycles,
    command->period_tenths,
  };
  uint8_t confirm = 0xff;
  bool ok = fp_command(FP_LED_AURA_COMMAND, params, sizeof(params), &confirm, NULL, NULL,
                       1000) && confirm == 0x00;
  if (!ok) {
    ESP_LOGW(TAG, "LED state %d rejected confirm=0x%02x", state, confirm);
    return false;
  }

  led_state = state;
  return true;
}

static void show_result(bool ok) {
  fp_led_state_t result_state = ok ? FP_LED_STATE_ACCEPTED : FP_LED_STATE_REJECTED;
  const fp_led_command_t *command = led_command_for_state(result_state);
  // A result supersedes any pending prompt: the touch it was asking for
  // has happened.
  led_prompt_deadline = 0;
  if (led_apply(result_state)) {
    vTaskDelay(pdMS_TO_TICKS(led_command_duration_ms(command)));
  }
  led_apply(FP_LED_STATE_IDLE);
}

void fingerprint_led_idle(void) {
  led_prompt_deadline = 0;
  if (!fp_take(1000)) return;
  led_apply(FP_LED_STATE_IDLE);
  fp_give();
}

bool fingerprint_led_prompt(void) {
  if (!fp_take(1000)) return false;
  bool applied = led_apply(FP_LED_STATE_PROMPT);
  fp_give();
  if (!applied) return false;
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(FP_LED_PROMPT_HOLD_MS);
  // A deadline of zero is the "disarmed" marker, so the one tick value that
  // would collide with it is nudged forward.
  led_prompt_deadline = deadline ? deadline : 1;
  return true;
}

void fingerprint_led_tick(void) {
  TickType_t deadline = led_prompt_deadline;
  if (deadline == 0) return;
  // Signed difference so the comparison survives the tick counter wrapping.
  if ((int32_t)(xTaskGetTickCount() - deadline) < 0) return;
  led_prompt_deadline = 0;
  if (!fp_take(0)) {
    // The sensor is busy, most likely matching the finger this prompt asked
    // for. Whatever that exchange concludes will set the LED itself.
    return;
  }
  led_apply(FP_LED_STATE_IDLE);
  fp_give();
}

static bool finger_present(void) {
  return gpio_get_level(FP_INT_PIN) == INT_ACTIVE_VALUE;
}

bool fingerprint_present_hint(void) {
  return finger_present();
}

static bool fingerprint_match_captured(bool quiet) {
  uint8_t confirm = 0xff;
  uint8_t img2tz[] = {0x01};
  if (!fp_command(0x02, img2tz, sizeof(img2tz), &confirm, NULL, NULL, 2000) || confirm != 0x00) {
    if (!quiet) {
      ESP_LOGW(TAG, "img2tz failed confirm=0x%02x", confirm);
    }
    show_result(false);
    return false;
  }

  uint16_t count = END_SLOT - START_SLOT + 1;
  uint8_t search_params[] = {
    0x01,
    (uint8_t)(START_SLOT >> 8), (uint8_t)(START_SLOT & 0xff),
    (uint8_t)(count >> 8), (uint8_t)(count & 0xff)
  };
  uint8_t search_data[4];
  size_t search_len = sizeof(search_data);
  if (!fp_command(0x04, search_params, sizeof(search_params), &confirm, search_data, &search_len, 2000)) {
    if (!quiet) ESP_LOGW(TAG, "search command failed");
  } else if (confirm == 0x00 && search_len == sizeof(search_data)) {
    uint16_t score = ((uint16_t)search_data[2] << 8) | search_data[3];
    bool ok = score > 0;
    ESP_LOGI(TAG, "fingerprint search: %s score=%u", ok ? "ok" : "failed", score);
    show_result(ok);
    return ok;
  } else if (!quiet) {
    ESP_LOGW(TAG, "search failed confirm=0x%02x len=%u", confirm, (unsigned)search_len);
  }

  for (uint16_t slot = START_SLOT; slot <= END_SLOT; slot++) {
    uint8_t load_params[] = {0x02, (uint8_t)(slot >> 8), (uint8_t)(slot & 0xff)};
    confirm = 0xff;
    if (!fp_command(0x07, load_params, sizeof(load_params), &confirm, NULL, NULL, 1000) ||
        confirm != 0x00) {
      if (!quiet) ESP_LOGW(TAG, "load slot %u failed confirm=0x%02x", slot, confirm);
      continue;
    }

    uint8_t match_data[2];
    size_t match_len = sizeof(match_data);
    confirm = 0xff;
    if (!fp_command(0x03, NULL, 0, &confirm, match_data, &match_len, 1000)) {
      if (!quiet) ESP_LOGW(TAG, "match slot %u command failed", slot);
      continue;
    }
    if (confirm == 0x00 && match_len == sizeof(match_data)) {
      uint16_t score = ((uint16_t)match_data[0] << 8) | match_data[1];
      if (score > 0) {
        ESP_LOGI(TAG, "fingerprint match: ok slot=%u score=%u", slot, score);
        show_result(true);
        return true;
      }
    }
    if (!quiet) {
      ESP_LOGW(TAG, "match slot %u failed confirm=0x%02x len=%u", slot, confirm, (unsigned)match_len);
    }
  }

  show_result(false);
  return false;
}

bool fingerprint_authorize_poll_once(void) {
  if (!fp_take(0)) return false;
  uint8_t confirm = 0xff;
  if (!fp_command(0x01, NULL, 0, &confirm, NULL, NULL, 350) || confirm != 0x00) {
    fp_give();
    return false;
  }
  bool ok = fingerprint_match_captured(true);
  fp_give();
  return ok;
}

void fingerprint_init(void) {
  gpio_config_t io = {
    .pin_bit_mask = 1ULL << FP_INT_PIN,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_ENABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);

  uart_config_t cfg = {
    .baud_rate = 57600,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  uart_driver_install(FP_UART, 1024, 0, 0, NULL, 0);
  uart_param_config(FP_UART, &cfg);
  uart_set_pin(FP_UART, FP_TX_PIN, FP_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  fp_mutex = xSemaphoreCreateMutex();
  if (fp_mutex == NULL) {
    ESP_LOGE(TAG, "could not create fingerprint mutex");
    return;
  }

  uint8_t params[] = {0x00, 0x00, 0x00, 0x00};
  uint8_t confirm = 0xff;
  fp_take(2000);
  bool ok = fp_command(0x13, params, sizeof(params), &confirm, NULL, NULL, 2000) && confirm == 0x00;
  bool manual_mode_already_configured = led_manual_mode_provisioned();
  bool manual_mode_configured = ok &&
                                (manual_mode_already_configured || configure_led_manual_mode());
  fp_give();
  ESP_LOGI(TAG, "sensor verify: %s", ok ? "ok" : "failed");
  if (ok && manual_mode_already_configured) {
    fingerprint_led_idle();
  } else if (manual_mode_configured) {
    ESP_LOGW(TAG, "LED effects will start after the required ZW111 power cycle");
  }
}

bool fingerprint_authorize_once(void) {
  if (!fp_take(FINGER_WAIT_MS + 1000)) return false;
  uint8_t confirm = 0xff;
  ESP_LOGI(TAG, "finger present hint=%d", finger_present());
  led_apply(FP_LED_STATE_PROMPT);

  TickType_t start = xTaskGetTickCount();
  TickType_t deadline = pdMS_TO_TICKS(FINGER_WAIT_MS);
  bool got_image = false;
  while ((xTaskGetTickCount() - start) < deadline) {
    TickType_t elapsed = xTaskGetTickCount() - start;
    TickType_t remaining = deadline - elapsed;
    uint32_t remaining_ms = pdTICKS_TO_MS(remaining);
    if (remaining_ms == 0) break;
    uint32_t command_timeout_ms = remaining_ms < FP_IMAGE_COMMAND_MAX_WAIT_MS
                                    ? remaining_ms
                                    : FP_IMAGE_COMMAND_MAX_WAIT_MS;
    if (fp_command(0x01, NULL, 0, &confirm, NULL, NULL, command_timeout_ms) &&
        confirm == 0x00) {
      got_image = true;
      break;
    }
    elapsed = xTaskGetTickCount() - start;
    if (elapsed >= deadline) break;
    remaining = deadline - elapsed;
    TickType_t retry_delay = pdMS_TO_TICKS(FP_IMAGE_RETRY_DELAY_MS);
    vTaskDelay(remaining < retry_delay ? remaining : retry_delay);
  }
  if (!got_image) {
    ESP_LOGW(TAG, "gen image failed confirm=0x%02x", confirm);
    // No image means an abandoned request, not a rejected fingerprint.
    led_apply(FP_LED_STATE_IDLE);
    fp_give();
    return false;
  }

  bool ok = fingerprint_match_captured(false);
  fp_give();
  return ok;
}

int fingerprint_count(void) {
  if (!fp_take(2000)) return -1;
  uint8_t confirm = 0xff;
  uint8_t data[2];
  size_t data_len = sizeof(data);
  bool ok = fp_command(0x1d, NULL, 0, &confirm, data, &data_len, 2000) &&
            confirm == 0x00 && data_len == sizeof(data);
  fp_give();
  return ok ? ((int)data[0] << 8) | data[1] : -1;
}

static bool wait_finger_state(bool present, uint32_t timeout_ms) {
  TickType_t start = xTaskGetTickCount();
  TickType_t deadline = pdMS_TO_TICKS(timeout_ms);
  while ((xTaskGetTickCount() - start) < deadline) {
    if (finger_present() == present) return true;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return false;
}

static bool fp_read_exact(uint8_t *output, size_t output_length, TickType_t start,
                          TickType_t timeout_ticks) {
  size_t output_offset = 0;
  while (output_offset < output_length) {
    TickType_t elapsed = xTaskGetTickCount() - start;
    if (elapsed >= timeout_ticks) return false;
    TickType_t remaining = timeout_ticks - elapsed;
    int bytes_read = uart_read_bytes(FP_UART, output + output_offset,
                                     output_length - output_offset, remaining);
    if (bytes_read <= 0) return false;
    output_offset += (size_t)bytes_read;
  }
  return true;
}

static bool fp_read_auto_enroll_response(uint8_t *confirm, uint8_t *stage,
                                         uint8_t *capture_or_result, TickType_t start,
                                         TickType_t timeout_ticks) {
  uint8_t header[FP_RESPONSE_HEADER_SIZE] = {0};
  bool saw_header_prefix = false;
  while (true) {
    uint8_t next_byte = 0;
    if (!fp_read_exact(&next_byte, 1, start, timeout_ticks)) return false;
    if (saw_header_prefix && next_byte == 0x01) {
      header[0] = 0xef;
      header[1] = 0x01;
      break;
    }
    saw_header_prefix = next_byte == 0xef;
  }

  if (!fp_read_exact(header + 2, sizeof(header) - 2, start, timeout_ticks)) return false;
  if (header[6] != FP_RESPONSE_ACK_PACKET_ID) return false;

  uint16_t response_length = ((uint16_t)header[7] << 8) | header[8];
  if (response_length < 5 || response_length > FP_RESPONSE_BODY_CAPACITY) return false;

  uint8_t body[FP_RESPONSE_BODY_CAPACITY];
  if (!fp_read_exact(body, response_length, start, timeout_ticks)) return false;

  size_t payload_length = response_length - 2;
  uint32_t calculated_checksum = header[6] + header[7] + header[8];
  for (size_t index = 0; index < payload_length; index++) {
    calculated_checksum += body[index];
  }
  uint16_t received_checksum = ((uint16_t)body[payload_length] << 8) |
                               body[payload_length + 1];
  if ((uint16_t)calculated_checksum != received_checksum || payload_length < 3) return false;

  *confirm = body[0];
  *stage = body[1];
  *capture_or_result = body[2];
  return true;
}

static const char *enrollment_touch_prompt(uint8_t capture_number) {
  static const char *const prompts[FP_ENROLL_CAPTURE_COUNT] = {
    "TOUCH_CENTER",
    "TOUCH_LEFT_EDGE",
    "TOUCH_RIGHT_EDGE",
    "TOUCH_TIP",
    "TOUCH_BASE",
  };
  if (capture_number == 0 || capture_number > FP_ENROLL_CAPTURE_COUNT) return NULL;
  return prompts[capture_number - 1];
}

static void prompt_enrollment_touch(void (*prompt)(const char *message),
                                    uint8_t capture_number) {
  const char *message = enrollment_touch_prompt(capture_number);
  if (prompt && message) prompt(message);
}

static void cancel_auto_enroll(void) {
  uint8_t confirm = 0xff;
  if (!fp_command(FP_CANCEL_COMMAND, NULL, 0, &confirm, NULL, NULL, 1000) ||
      confirm != FP_CONFIRM_SUCCESS) {
    ESP_LOGW(TAG, "auto enroll cancel failed confirm=0x%02x", confirm);
  }
}

static bool auto_enroll(uint16_t slot, void (*prompt)(const char *message)) {
  const uint8_t params[] = {
    (uint8_t)(slot >> 8),
    (uint8_t)slot,
    FP_ENROLL_CAPTURE_COUNT,
    (uint8_t)(FP_AUTO_ENROLL_ALLOW_OVERWRITE >> 8),
    (uint8_t)FP_AUTO_ENROLL_ALLOW_OVERWRITE,
  };
  if (!fp_send_command(FP_AUTO_ENROLL_COMMAND, params, sizeof(params))) return false;

  TickType_t start = xTaskGetTickCount();
  TickType_t timeout_ticks = pdMS_TO_TICKS(FP_ENROLL_TIMEOUT_MS);
  while ((xTaskGetTickCount() - start) < timeout_ticks) {
    uint8_t confirm = 0xff;
    uint8_t stage_value = 0xff;
    uint8_t capture_or_result = 0xff;
    if (!fp_read_auto_enroll_response(&confirm, &stage_value, &capture_or_result,
                                      start, timeout_ticks)) {
      ESP_LOGW(TAG, "auto enroll response timed out or was invalid");
      cancel_auto_enroll();
      return false;
    }

    fp_auto_enroll_stage_t stage = (fp_auto_enroll_stage_t)stage_value;
    ESP_LOGI(TAG, "auto enroll confirm=0x%02x stage=0x%02x value=0x%02x",
             confirm, stage_value, capture_or_result);

    if (stage == FP_AUTO_ENROLL_STAGE_FEATURE &&
        confirm == FP_CONFIRM_FEATURE_FAILURE) {
      if (prompt) prompt("RETRY_CAPTURE");
      prompt_enrollment_touch(prompt, capture_or_result);
      continue;
    }
    if (confirm != FP_CONFIRM_SUCCESS) {
      cancel_auto_enroll();
      return false;
    }

    switch (stage) {
      case FP_AUTO_ENROLL_STAGE_VALIDATE:
        prompt_enrollment_touch(prompt, 1);
        break;
      case FP_AUTO_ENROLL_STAGE_IMAGE:
        break;
      case FP_AUTO_ENROLL_STAGE_FEATURE:
        if (capture_or_result == 0 || capture_or_result > FP_ENROLL_CAPTURE_COUNT) {
          cancel_auto_enroll();
          return false;
        }
        if (capture_or_result < FP_ENROLL_CAPTURE_COUNT && prompt) prompt("LIFT");
        break;
      case FP_AUTO_ENROLL_STAGE_LIFT:
        if (capture_or_result == 0 || capture_or_result >= FP_ENROLL_CAPTURE_COUNT) {
          cancel_auto_enroll();
          return false;
        }
        prompt_enrollment_touch(prompt, capture_or_result + 1);
        break;
      case FP_AUTO_ENROLL_STAGE_MERGE:
        if (capture_or_result != FP_AUTO_ENROLL_FINAL_MERGE) {
          cancel_auto_enroll();
          return false;
        }
        break;
      case FP_AUTO_ENROLL_STAGE_DUPLICATE_CHECK:
        if (capture_or_result != FP_AUTO_ENROLL_FINAL_DUPLICATE_CHECK) {
          cancel_auto_enroll();
          return false;
        }
        break;
      case FP_AUTO_ENROLL_STAGE_STORE:
        return capture_or_result == FP_AUTO_ENROLL_FINAL_STORE;
      default:
        cancel_auto_enroll();
        return false;
    }
  }

  cancel_auto_enroll();
  return false;
}

bool fingerprint_enroll(uint16_t slot, void (*prompt)(const char *message)) {
  if (slot < START_SLOT || slot > END_SLOT || !fp_take(1000)) return false;
  led_apply(FP_LED_STATE_PROMPT);
  if (finger_present()) {
    if (prompt) prompt("LIFT");
    if (!wait_finger_state(false, FP_ENROLL_INITIAL_LIFT_TIMEOUT_MS)) {
      show_result(false);
      fp_give();
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(FP_ENROLL_LIFT_SETTLE_MS));
  }

  bool ok = auto_enroll(slot, prompt);
  show_result(ok);
  fp_give();
  return ok;
}

bool fingerprint_delete(uint16_t slot) {
  if (slot < START_SLOT || slot > END_SLOT || !fp_take(1000)) return false;
  uint8_t params[] = {(uint8_t)(slot >> 8), (uint8_t)slot, 0x00, 0x01};
  uint8_t confirm = 0xff;
  bool ok = fp_command(0x0c, params, sizeof(params), &confirm, NULL, NULL, 2000) && confirm == 0x00;
  fp_give();
  return ok;
}

bool fingerprint_delete_all(void) {
  if (!fp_take(1000)) return false;
  uint8_t confirm = 0xff;
  bool ok = fp_command(0x0d, NULL, 0, &confirm, NULL, NULL, 2000) && confirm == 0x00;
  fp_give();
  return ok;
}
