/*
 * ESP-IDF Camera Web Server for ESP32-S3 with OV3660
 *
 * Mode: "On-Demand Router Mode" (Safe STA)
 * - Always boots in AP mode (Hotspot: ESP32-CAM)
 * - Connects to Router ONLY when requested via Web UI
 * - Uses Static IP for reliability
 * - Retry Logic: Tries to connect 3 times before falling back to AP
 * - strict Fail-Safe: Reverts to AP immediately purely on failure after retries
 * - GPIO 19: Toggle between AP and STA
 */

#include "esp_camera.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "nvs_flash.h"
#include <string.h>

#include "camera_pins.h"

static const char *TAG = "cam_main";

/* AP Configuration */
#define AP_WIFI_SSID CONFIG_AP_WIFI_SSID
#define AP_WIFI_PASS CONFIG_AP_WIFI_PASSWORD
#define AP_WIFI_CHANNEL 1
#define AP_MAX_CONNECTIONS 4

/* NVS Keys */
#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "sta_ssid"
#define NVS_KEY_PASS "sta_pass"
#define NVS_KEY_IP "sta_ip"
#define NVS_KEY_GW "sta_gw"
#define NVS_KEY_NETMASK "sta_netmask"

/* GPIO */
#define BUTTON_GPIO 19
#define LED_GPIO 2

/* WiFi Event Group */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/* Retry Logic */
#define MAX_RETRY 12
static int s_retry_num = 0;

/* Global State */
bool g_is_ap_mode = true;
esp_netif_t *g_netif_ap = NULL;
esp_netif_t *g_netif_sta = NULL;

/* Forward declarations */
void start_camera_server(void);
static void start_mdns(void);

/* ─── NVS Helpers ─── */
/* ─── NVS Helpers ─── */
void nvs_save_config(const char *ssid, const char *pass) {
  nvs_handle_t h;
  ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
  nvs_set_str(h, NVS_KEY_SSID, ssid);
  nvs_set_str(h, NVS_KEY_PASS, pass);
  nvs_commit(h);
  nvs_close(h);
}

bool nvs_load_config(char *ssid, char *pass) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    return false;

  size_t len = 32;
  nvs_get_str(h, NVS_KEY_SSID, ssid, &len);
  len = 64;
  nvs_get_str(h, NVS_KEY_PASS, pass, &len);
  nvs_close(h);
  return (strlen(ssid) > 0);
}

/* ─── WiFi Handlers ─── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    s_retry_num = 0;
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (!g_is_ap_mode) {
      if (s_retry_num < MAX_RETRY) {
        esp_wifi_connect();
        s_retry_num++;
        ESP_LOGW(TAG, "STA Disconnected! Retrying... (%d/%d)", s_retry_num,
                 MAX_RETRY);
      } else {
        ESP_LOGE(TAG, "STA Failed after %d retries. Reverting to AP.",
                 MAX_RETRY);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      }
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG,
             "Station connected to AP - MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             event->mac[0], event->mac[1], event->mac[2], event->mac[3],
             event->mac[4], event->mac[5]);
  }
}

/* ─── Mode Switching ─── */
void start_ap_mode(void) {
  ESP_LOGI(TAG, "Starting AP Mode...");
  g_is_ap_mode = true;

  esp_wifi_stop();
  esp_wifi_set_mode(WIFI_MODE_AP);

  wifi_config_t wifi_config = {
      .ap =
          {
              .ssid = AP_WIFI_SSID,
              .ssid_len = strlen(AP_WIFI_SSID),
              .channel = AP_WIFI_CHANNEL,
              .password = AP_WIFI_PASS,
              .max_connection = AP_MAX_CONNECTIONS,
              .authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg = {.required = true},
          },
  };
  if (strlen(AP_WIFI_PASS) == 0)
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(70));

  ESP_LOGI(TAG, "AP Started. SSID: %s, IP: 192.168.4.1", AP_WIFI_SSID);

  // Blink LED to indicate AP mode
  for (int i = 0; i < 3; i++) {
    gpio_set_level(LED_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void start_sta_mode(const char *ssid, const char *pass) {
  ESP_LOGI(TAG, "Attempting STA Connection to %s with DHCP...", ssid);
  g_is_ap_mode = false;
  s_retry_num = 0;

  esp_wifi_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, ssid, 32);
  strncpy((char *)wifi_config.sta.password, pass, 64);

  // DHCP is enabled by default for STA mode, so no netif config needed

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(70));

  // Wait for connection
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
      pdMS_TO_TICKS(45000)); // Increased timeout for retries

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to %s!", ssid);
    start_mdns();
  } else {
    ESP_LOGE(TAG, "Connection Failed! Reverting to AP...");
    start_ap_mode();
  }
}

/* ─── Initialization ─── */
static void init_wifi_stack(void) {
  s_wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  g_netif_ap = esp_netif_create_default_wifi_ap();
  g_netif_sta = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
}

static esp_err_t init_camera(void) {
  camera_config_t camera_config = {
      .pin_pwdn = CAM_PIN_PWDN,
      .pin_reset = CAM_PIN_RESET,
      .pin_xclk = CAM_PIN_XCLK,
      .pin_sccb_sda = CAM_PIN_SIOD,
      .pin_sccb_scl = CAM_PIN_SIOC,
      .pin_d7 = CAM_PIN_D7,
      .pin_d6 = CAM_PIN_D6,
      .pin_d5 = CAM_PIN_D5,
      .pin_d4 = CAM_PIN_D4,
      .pin_d3 = CAM_PIN_D3,
      .pin_d2 = CAM_PIN_D2,
      .pin_d1 = CAM_PIN_D1,
      .pin_d0 = CAM_PIN_D0,
      .pin_vsync = CAM_PIN_VSYNC,
      .pin_href = CAM_PIN_HREF,
      .pin_pclk = CAM_PIN_PCLK,
      .xclk_freq_hz = 20000000,
      .ledc_timer = LEDC_TIMER_0,
      .ledc_channel = LEDC_CHANNEL_0,
      .pixel_format = PIXFORMAT_JPEG,
      .frame_size = FRAMESIZE_VGA,
      .jpeg_quality = 12,
      .fb_count = 2,
      .fb_location = CAMERA_FB_IN_PSRAM,
      .grab_mode = CAMERA_GRAB_LATEST,
  };
  return esp_camera_init(&camera_config);
}

static void start_mdns(void) {
  mdns_init();
  mdns_hostname_set("esp32-cam");
  mdns_instance_name_set("ESP32-S3 Camera");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

/* ─── Input Task ─── */
void input_task(void *arg) {
  gpio_config_t io_conf = {.pin_bit_mask = (1ULL << BUTTON_GPIO),
                           .mode = GPIO_MODE_INPUT,
                           .pull_up_en = 1,
                           .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&io_conf);

  while (1) {
    if (gpio_get_level(BUTTON_GPIO) == 0) {
      vTaskDelay(pdMS_TO_TICKS(100)); // Debounce
      if (gpio_get_level(BUTTON_GPIO) == 0) {
        ESP_LOGI(TAG, "Button Pressed! Toggling Mode...");
        if (g_is_ap_mode) {
          // Try to load config and connect
          char ssid[33] = {0}, pass[65] = {0};
          if (nvs_load_config(ssid, pass)) {
            start_sta_mode(ssid, pass);
          } else {
            ESP_LOGW(TAG, "No STA Config found!");
            // Flicker LED error
            for (int i = 0; i < 5; i++) {
              gpio_set_level(LED_GPIO, 1);
              vTaskDelay(50 / portTICK_PERIOD_MS);
              gpio_set_level(LED_GPIO, 0);
              vTaskDelay(50 / portTICK_PERIOD_MS);
            }
          }
        } else {
          start_ap_mode();
        }
        while (gpio_get_level(BUTTON_GPIO) == 0)
          vTaskDelay(10); // Wait release
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/* ─── Main ─── */
void app_main(void) {
  // Init NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // Init GPIO
  gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(LED_GPIO, 0);

  // Init Camera
  if (init_camera() != ESP_OK) {
    ESP_LOGE(TAG, "Camera Init Failed");
    return;
  }

  // Init WiFi
  init_wifi_stack();

  // Always Boot AP
  start_ap_mode();

  // Start Server
  start_camera_server();

  // Start Input Task
  xTaskCreate(input_task, "input_task", 2048, NULL, 10, NULL);
}
