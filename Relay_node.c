#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
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
#include "esp_ble_mesh_generic_model_api.h" 

static const char *TAG = "mesh_relay";

// --- RELAY HARDWARE PIN ---
#define RELAY_PIN 5

// --- MESH DEFINITIONS ---
#define CID_ESP 0x02E5 

static uint8_t dev_uuid[16] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22, 
                                0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00 };

static esp_ble_mesh_cfg_srv_t config_server = {
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .default_ttl = 7,
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
};

// --- THE GENERIC ONOFF SERVER MODEL ---
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub, 2 + 3, ROLE_NODE);
static esp_ble_mesh_gen_onoff_srv_t onoff_server = {
    .rsp_ctrl.get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    .rsp_ctrl.set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
};

// Put BOTH standard models in the same root list!
static esp_ble_mesh_model_t root_models[] = { 
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server), 
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub, &onoff_server),
};

// Notice ESP_BLE_MESH_MODEL_NONE. We have NO vendor models on this node!
static esp_ble_mesh_elem_t elements[] = { 
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE), 
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .elements = elements,
    .element_count = ARRAY_SIZE(elements),
};

static esp_ble_mesh_prov_t provision = { .uuid = dev_uuid, };

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param) {
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Relay Beacon broadcasting! Open your nRF Mesh App.");
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "SUCCESS! Relay Node provisioned. Address: 0x%04x", param->node_prov_complete.addr);
        break;
    default: break;
    }
}

// --- STANDARD SIG MODEL LISTENER (ACTIVE LOW UPDATED) ---
static void example_ble_mesh_generic_server_cb(esp_ble_mesh_generic_server_cb_event_t event, esp_ble_mesh_generic_server_cb_param_t *param) {
    if (event == ESP_BLE_MESH_GENERIC_SERVER_STATE_CHANGE_EVT) {
        if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
            param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK) {
            
            // Extract the logical state (1 = Turn ON, 0 = Turn OFF)
            uint8_t logical_state = param->value.state_change.onoff_set.onoff;
            
            // INVERT FOR ACTIVE LOW: 
            // If logical_state is 1 (ON), hardware_pin_state becomes 0 (LOW).
            // If logical_state is 0 (OFF), hardware_pin_state becomes 1 (HIGH).
            uint8_t hardware_pin_state = logical_state ? 0 : 1;
            
            ESP_LOGI(TAG, "====== COMMAND RECEIVED! ======");
            ESP_LOGI(TAG, "Mesh Command: %s", logical_state ? "ON" : "OFF");
            ESP_LOGI(TAG, "Physical Pin Output: %s", hardware_pin_state ? "HIGH (1)" : "LOW (0)");
            ESP_LOGI(TAG, "===============================");
            
            // Toggle the physical hardware pin!
            gpio_set_level(RELAY_PIN, hardware_pin_state);
        }
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
    esp_ble_mesh_register_generic_server_callback(example_ble_mesh_generic_server_cb); 
    
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));
    ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "--- STARTING NODE 3 (ACTIVE LOW RELAY ACTUATOR) ---");

    // Initialize the GPIO Pin for the Relay
    gpio_reset_pin(RELAY_PIN);
    gpio_set_direction(RELAY_PIN, GPIO_MODE_OUTPUT_OD);
    
    // START OFF (Active Low means output 1 to turn OFF)
    gpio_set_level(RELAY_PIN, 1); 

    // Start Bluetooth
    ble_mesh_init();
}