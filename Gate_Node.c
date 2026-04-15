#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" 
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "lwip/dns.h" 
#include "cJSON.h" 

// --- BLUETOOTH & MESH LIBRARIES ---
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h" 

static const char *TAG = "master_gateway";

// ==========================================
// 🔴 YOUR PLATFORM CREDENTIALS 🔴
// ==========================================
#define WIFI_SSID "TR"
#define WIFI_PASS "BLE@ANEDYA"
#define ANEDYA_CONNECTION_KEY "a103ae9e31d4a38a137e25df64f8a341"

// ⚠️ PASTE YOUR API KEY HERE!
#define ANEDYA_API_KEY "2d56e0a5bce625a52602c3024c062e5277dd6af3fa2f57acdb25f4f5b4c74ddd" 

// The true Node UUID from Anedya Dashboard URL
#define GATEWAY_NODE_ID "019d4324-939b-723c-a8b3-0864b1e24b4f" 
// ==========================================

volatile bool wifi_connected = false;

#define CID_ESP 0x02E5 
#define ESP_BLE_MESH_VND_MODEL_ID_GATEWAY 0x0002
#define ESP_BLE_MESH_VND_MODEL_OP_RECEIVE ESP_BLE_MESH_MODEL_OP_3(0x00, CID_ESP)

typedef struct { float temp; float hum; } sensor_data_t;
QueueHandle_t cloud_queue; 

static uint8_t dev_uuid[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 
                                0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00 };

static esp_ble_mesh_cfg_srv_t config_server = {
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .default_ttl = 7,
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
};

// Publisher buffer increased to 20 to prevent memory allocation errors
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_cli_pub, 20, ROLE_NODE);
static esp_ble_mesh_client_t onoff_client;

static esp_ble_mesh_model_op_t vnd_op[] = { 
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_RECEIVE, 2), 
    ESP_BLE_MESH_MODEL_OP_END 
};

static esp_ble_mesh_model_t vnd_models[] = { ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_VND_MODEL_ID_GATEWAY, vnd_op, NULL, NULL), };
static esp_ble_mesh_model_t root_models[] = { ESP_BLE_MESH_MODEL_CFG_SRV(&config_server), ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_cli_pub, &onoff_client), };
static esp_ble_mesh_elem_t elements[] = { ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models), };
static esp_ble_mesh_comp_t composition = { .cid = CID_ESP, .elements = elements, .element_count = ARRAY_SIZE(elements), };
static esp_ble_mesh_prov_t provision = { .uuid = dev_uuid, };

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param) {
    if (event == ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT) { ESP_LOGI(TAG, "Gateway Beacon broadcasting!"); } 
    else if (event == ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT) { ESP_LOGI(TAG, "Gateway provisioned. Address: 0x%04x", param->node_prov_complete.addr); }
}

static void example_ble_mesh_generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event, esp_ble_mesh_generic_client_cb_param_t *param) {
    if (event == ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT) { ESP_LOGI(TAG, "✅ RELAY NODE ACKNOWLEDGED COMMAND!"); }
}

// ===================================================================
// ⬇️ DOWNLINK SYSTEM: PLATFORM API -> GATEWAY -> MESH RELAY
// ===================================================================

void send_mesh_command(uint8_t state) {
    // Pure 2-byte raw array to bypass C structure padding
    // Byte 0: State (1=ON, 0=OFF)
    // Byte 1: Transaction ID (Random to prevent replay attacks)
    uint8_t raw_payload[2] = { state, (uint8_t)(esp_random() % 255) };
    
    esp_err_t err = esp_ble_mesh_model_publish(onoff_client.model, 
                                               ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK, 
                                               sizeof(raw_payload), 
                                               raw_payload, 
                                               ROLE_NODE);
                                               
    if(err == ESP_OK) { 
        ESP_LOGI(TAG, "🚀 MESH COMMAND FIRED TO RELAY: %s", state ? "ON" : "OFF"); 
    } else { 
        ESP_LOGE(TAG, "Failed to send! Error Code: %d", err); 
    }
}

esp_err_t _http_poll_handler(esp_http_client_event_t *evt) {
    static char response_buffer[2048]; 
    static int response_len = 0;
    static char last_command_id[100] = ""; 
    
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (response_len + evt->data_len < sizeof(response_buffer) - 1) {
            memcpy(response_buffer + response_len, evt->data, evt->data_len);
            response_len += evt->data_len;
            response_buffer[response_len] = '\0';
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        if (response_len > 0) {
            
            ESP_LOGW(TAG, "RAW PLATFORM API RESPONSE: %s", response_buffer);
            
            cJSON *root = cJSON_Parse(response_buffer);
            if (root) {
                cJSON *data_array = cJSON_GetObjectItem(root, "data");
                if (cJSON_IsArray(data_array) && cJSON_GetArraySize(data_array) > 0) {
                    
                    cJSON *cmd_item = cJSON_GetArrayItem(data_array, 0);
                    if (cmd_item) {
                        cJSON *id_json = cJSON_GetObjectItem(cmd_item, "commandId");
                        cJSON *cmd_json = cJSON_GetObjectItem(cmd_item, "command");
                        
                        if (id_json && cmd_json) {
                            if (strcmp(id_json->valuestring, last_command_id) != 0) {
                                strcpy(last_command_id, id_json->valuestring); 
                                
                                ESP_LOGI(TAG, "☁️ NEW CLOUD COMMAND CAUGHT: %s", cmd_json->valuestring);
                                
                                if (strstr(cmd_json->valuestring, "ON") || strstr(cmd_json->valuestring, "on")) {
                                    send_mesh_command(1);
                                } else if (strstr(cmd_json->valuestring, "OFF") || strstr(cmd_json->valuestring, "off")) {
                                    send_mesh_command(0);
                                } else {
                                    ESP_LOGW(TAG, "Unrecognized command name. Failsafe triggering ON.");
                                    send_mesh_command(1); 
                                }
                            }
                        }
                    }
                }
                cJSON_Delete(root); 
            }
        }
        response_len = 0; 
    }
    return ESP_OK;
}

void anedya_poller_task(void *pvParameters) {
    while (1) {
        if (wifi_connected) {
            char post_data[256];
            snprintf(post_data, sizeof(post_data), "{\"filter\":{\"nodeId\":\"%s\",\"status\":[\"pending\"]},\"limit\":5}", GATEWAY_NODE_ID);

            esp_http_client_config_t config = {
                .url = "https://api.ap-in-1.anedya.io/v1/commands/list", 
                .method = HTTP_METHOD_POST,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .event_handler = _http_poll_handler,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            
            char auth_header[150];
            snprintf(auth_header, sizeof(auth_header), "Bearer %s", ANEDYA_API_KEY);
            esp_http_client_set_header(client, "Authorization", auth_header);
            
            esp_http_client_set_post_field(client, post_data, strlen(post_data));
            esp_http_client_perform(client);
            esp_http_client_cleanup(client);
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    }
}

// ===================================================================
// ⬆️ UPLINK SYSTEM: MESH SENSOR -> GATEWAY -> CLOUD
// ===================================================================

void send_to_anedya(float temp, float hum) {
    if (!wifi_connected) return;
    char post_data[256];
    snprintf(post_data, sizeof(post_data), "{\"data\":[{\"variable\":\"temperature\",\"value\":%.1f},{\"variable\":\"humidity\",\"value\":%.1f}]}", temp, hum);

    esp_http_client_config_t config = {
        .url = "https://device.ap-in-1.anedya.io/v1/submitData",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Auth-mode", "key");
    esp_http_client_set_header(client, "Authorization", ANEDYA_CONNECTION_KEY); 
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    if (esp_http_client_perform(client) == ESP_OK) { ESP_LOGI(TAG, "☁️ SUCCESS! Sensor Telemetry pushed to Anedya Cloud!"); }
    esp_http_client_cleanup(client);
}

void cloud_upload_task(void *pvParameters) {
    sensor_data_t received_data;
    while (1) {
        if (xQueueReceive(cloud_queue, &received_data, portMAX_DELAY)) { send_to_anedya(received_data.temp, received_data.hum); }
    }
}

static void example_ble_mesh_custom_model_cb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *param) {
    if (event == ESP_BLE_MESH_MODEL_OPERATION_EVT && param->model_operation.opcode == ESP_BLE_MESH_VND_MODEL_OP_RECEIVE) {
        char received_string[20];
        snprintf(received_string, sizeof(received_string), "%.*s", param->model_operation.length, param->model_operation.msg);
        float parsed_temp = 0.0, parsed_hum = 0.0;
        if (sscanf(received_string, "T:%f,H:%f", &parsed_temp, &parsed_hum) == 2) {
            ESP_LOGI(TAG, "====== MESH SENSOR DATA RECEIVED: Temp %.1f, Hum %.1f ======", parsed_temp, parsed_hum);
            sensor_data_t new_data = { .temp = parsed_temp, .hum = parsed_hum };
            xQueueSend(cloud_queue, &new_data, 0); 
        }
    }
}

// ===================================================================
// SYSTEM INITIALIZATION
// ===================================================================

void ble_mesh_init(void) {
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    
    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_custom_model_callback(example_ble_mesh_custom_model_cb);
    esp_ble_mesh_register_generic_client_callback(example_ble_mesh_generic_client_cb);
    
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));
    ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) { esp_wifi_connect(); } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) { wifi_connected = false; esp_wifi_connect(); } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_addr_t dns_server; ipaddr_aton("8.8.8.8", &dns_server); dns_setserver(0, &dns_server); wifi_connected = true; 
    }
}

void wifi_init_sta(void) {
    esp_netif_init(); esp_event_loop_create_default(); esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, }, };
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wifi_config); esp_wifi_start();
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }

    ESP_LOGI(TAG, "--- STARTING NODE 1 (FULL STACK GATEWAY) ---");
    cloud_queue = xQueueCreate(10, sizeof(sensor_data_t));
    
    xTaskCreate(&cloud_upload_task, "upload_task", 8192, NULL, 5, NULL); 
    xTaskCreate(&anedya_poller_task, "poll_task", 8192, NULL, 5, NULL); 

    wifi_init_sta();
    while (!wifi_connected) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    ESP_LOGI(TAG, "WI-FI CONNECTED!");
    ble_mesh_init();
}