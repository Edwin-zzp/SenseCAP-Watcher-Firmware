/*
 * SPDX-FileCopyrightText: 2024 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "sensecap-watcher.h"
#include "task_flow_module/common/tf_module_util.h"
#include "task_flow_module/tf_module_ai_camera.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define CAMERA_FRAME_QUEUE_LENGTH      4
#define CAMERA_FRAME_QUEUE_ITEM_SIZE   sizeof(tf_module_ai_camera_preview_info)
#define CAMERA_DISPATCH_TASK_STACK     4096
#define CAMERA_UPLOADER_TASK_STACK     6144

static const char *TAG = "camera_uploader";

static EventGroupHandle_t s_wifi_event_group;
static QueueHandle_t s_frame_queue;
static int s_retry_num;

static void release_preview_info(tf_module_ai_camera_preview_info *preview)
{
    tf_data_image_free(&preview->img);
    tf_data_inference_free(&preview->inference);
    memset(preview, 0, sizeof(*preview));
}

static esp_err_t camera_acquire_latest_frame(tf_module_ai_camera_preview_info *preview)
{
    /*
     * TODO: integrate with tf_module_ai_camera so that this function copies
     * the most recent preview frame (base64 JPEG plus inference metadata) into
     * the provided structure. For now it simply reports that the functionality
     * is not ready.
     */
    (void)preview;
    return ESP_ERR_NOT_SUPPORTED;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 10) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection (%d/10)", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect to AP");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Obtained IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* TODO: replace with configurable SSID/password via menuconfig or provisioning */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS");
        ret = nvs_flash_init();
    }
    return ret;
}

static void camera_frame_dispatch_task(void *arg)
{
    (void)arg;
    tf_module_ai_camera_preview_info preview_info = {0};

    for (;;) {
        esp_err_t err = camera_acquire_latest_frame(&preview_info);
        if (err == ESP_OK) {
            if (xQueueSend(s_frame_queue, &preview_info, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "Frame queue full, dropping frame");
                release_preview_info(&preview_info);
            }
            memset(&preview_info, 0, sizeof(preview_info));
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            /* Camera integration not ready yet; wait before retrying. */
        } else if (err != ESP_ERR_NOT_FOUND) {
            /* ESP_ERR_NOT_FOUND means no new frame yet; any other error should be logged */
            ESP_LOGW(TAG, "Unable to acquire camera frame: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void uploader_task(void *arg)
{
    (void)arg;
    tf_module_ai_camera_preview_info preview_info;

    for (;;) {
        if (xQueueReceive(s_frame_queue, &preview_info, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Dequeued frame with size %u bytes", preview_info.img.len);
            /*
             * TODO: convert preview_info.img (currently base64 JPEG) into the format
             * required by the target image hosting service and POST it via esp_http_client.
             * Remember to free resources after the HTTP request completes.
             */
            release_preview_info(&preview_info);
        }
    }
}

static void initialise_bsp(void)
{
    ESP_ERROR_CHECK(bsp_io_expander_init());
    ESP_ERROR_CHECK(bsp_exp_io_set_level(BSP_PWR_AI_CHIP, 1));
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    initialise_bsp();

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_event_group ? ESP_OK : ESP_ERR_NO_MEM);

    initialise_wifi();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Wi-Fi connection failed, restarting");
        esp_restart();
    }

    s_frame_queue = xQueueCreate(CAMERA_FRAME_QUEUE_LENGTH, CAMERA_FRAME_QUEUE_ITEM_SIZE);
    ESP_ERROR_CHECK(s_frame_queue ? ESP_OK : ESP_ERR_NO_MEM);

    BaseType_t created = xTaskCreatePinnedToCore(camera_frame_dispatch_task, "frame_dispatch", CAMERA_DISPATCH_TASK_STACK, NULL, 5, NULL, tskNO_AFFINITY);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_FAIL);

    created = xTaskCreatePinnedToCore(uploader_task, "uploader", CAMERA_UPLOADER_TASK_STACK, NULL, 5, NULL, tskNO_AFFINITY);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "Camera uploader example initialised");
}
