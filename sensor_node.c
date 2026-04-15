#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

// --- BLUETOOTH & MESH LIBRARIES ---
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "dht.h"

static const char *TAG = "mesh_sensor";

#define DHT_GPIO 4
#define SENSOR_TYPE DHT_TYPE_AM2301

// --- MESH DEFINITIONS ---
#define CID_ESP 0x02E5 
#define ESP_BLE_MESH_VND_MODEL_ID_SENSOR 0x0001

// The valid Opcode (Command) so the framework doesn't crash on init
#define ESP_BLE_MESH_VND_MODEL_OP_SEND ESP_BLE_MESH_MODEL_OP_3(0x00, CID_ESP)

static uint8_t dev_uuid[16] = { 0x32, 0x10, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 
                                0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x99 };

static esp_ble_mesh_cfg_srv_t config_server = {
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .default_ttl = 7,
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
};

// --- THE FIXED VENDOR MODEL ---
static esp_ble_mesh_model_op_t vnd_op[] = { 
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_SEND, 2), 
    ESP_BLE_MESH_MODEL_OP_END 
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(vendor_pub, 20 + 3, ROLE_NODE); 

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_VND_MODEL_ID_SENSOR, vnd_op, &vendor_pub, NULL),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models), 
};
// ------------------------------

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .elements = elements,
    .element_count = ARRAY_SIZE(elements),
};

static esp_ble_mesh_prov_t provision = { .uuid = dev_uuid, };

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param) {
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Unprovisioned Sensor Beacon broadcasting! Open your nRF Mesh App.");
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "SUCCESS! Sensor Node provisioned. Address: 0x%04x", param->node_prov_complete.addr);
        break;
    default:
        break;
    }
}

void ble_mesh_init(void) {
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));
    ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
}

void app_main(void) {
    // --- MEMORY WIPE REMOVED ---
    // The chip will now permanently remember its network keys after rebooting!
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    // ---------------------------

    ESP_LOGI(TAG, "--- STARTING NODE 2 (FINAL SENSOR BUILD) ---");

    ble_mesh_init();

    ESP_LOGI(TAG, "Waiting 3 seconds for Bluetooth to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    float temperature = 0.0;
    float humidity = 0.0;

    while (1) {
        ESP_LOGI(TAG, "Attempting to read DHT22..."); 
        bool success = false;

        for(int i = 0; i < 3; i++) {
            if (dht_read_float_data(SENSOR_TYPE, DHT_GPIO, &humidity, &temperature) == ESP_OK) {
                ESP_LOGI(TAG, "Read DHT22 - Hum: %.1f%% | Temp: %.1f°C", humidity, temperature);
                success = true;
                
                // --- PUBLISH TO MESH ---
                char data_str[20];
                snprintf(data_str, sizeof(data_str), "T:%.1f,H:%.1f", temperature, humidity);
                
                esp_err_t err = esp_ble_mesh_model_publish(&vnd_models[0], ESP_BLE_MESH_VND_MODEL_OP_SEND, strlen(data_str), (uint8_t *)data_str, ROLE_NODE);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Mesh Publish Initiated: %s", data_str);
                } else {
                    ESP_LOGW(TAG, "Mesh Publish waiting for App configuration...");
                }
                // -----------------------
                break; 
            }
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }

        if (!success) {
            ESP_LOGE(TAG, "Failed to read DHT22 data after 3 attempts!");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}