#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>      // 用於 OTA 相關操作
#include <esp_partition.h>    // 用於分區查找
#include "camera_pins.h"      // 假設此檔案定義了所有 CAM_PIN 宏

// 定義 Wi-Fi 連線超時時間 (15 秒，符合文檔要求)
#define WIFI_CONNECT_TIMEOUT_S 15

static httpd_handle_t camera_httpd = NULL;

// 在 src/TFLite_Integration.cpp 中定義的函式原型 (保留)
extern "C" esp_err_t detect_handler(httpd_req_t *req);

// -------------------------------------------------------------------
// 核心容錯函式 1: 跳轉回 Factory App
// -------------------------------------------------------------------

/**
 * @brief 在 Wi-Fi 連線失敗時，強制將下次啟動分區設置為 Factory App。
 * 這是最高級別的容錯機制，確保設備不會因為 User App 的網路故障而鎖死。
 */
void jumpToFactory() {
    Serial.println("FATAL: Wi-Fi connection timed out. Attempting rollback to Factory App...");

    // 1. 尋找 Factory 分區
    const esp_partition_t *factory_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_FACTORY,
        NULL
    );

    if (factory_partition == NULL) {
        Serial.println("ERROR: Factory partition not found! Cannot rollback.");
        return;
    }

    // 2. 設定下次啟動目標為 Factory 分區
    esp_err_t err = esp_ota_set_boot_partition(factory_partition);

    if (err != ESP_OK) {
        Serial.printf("ERROR: Failed to set boot partition to Factory (0x%x).\n", err);
        return;
    }

    Serial.println("INFO: Boot partition set to Factory. Restarting NOW.");
    // 3. 立即重啟
    esp_restart();
}

// -------------------------------------------------------------------
// 核心容錯函式 2: 處理 Wi-Fi 連線與超時
// -------------------------------------------------------------------

/**
 * @brief 嘗試在 15 秒內連線 Wi-Fi，超時則呼叫 jumpToFactory。
 */
void connectToWiFi() {
    Serial.println("INFO: Checking for existing Wi-Fi connection...");
    
    // Launcher App 啟動 User App 時，可能會保持連線
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("INFO: Wi-Fi already connected by Launcher App. IP: ");
        Serial.println(WiFi.localIP());
        return;
    }

    // 如果未連線，開始連線程序
    Serial.println("INFO: Starting Wi-Fi connection attempt...");
    WiFi.mode(WIFI_STA);
    WiFi.begin();

    unsigned long startTime = millis();

    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < (WIFI_CONNECT_TIMEOUT_S * 1000)) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("\nINFO: Wi-Fi STA connected, IP: ");
        Serial.println(WiFi.localIP());
    } else {
        // 連線超時！觸發回滾
        jumpToFactory(); 
        // 程式碼應在 jumpToFactory() 內重啟，如果沒有，則進入無限循環等待
        while (true) { delay(100); }
    }
}


// --- Stream Handler (保留，功能不變) ---
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char part_buf[64];

    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
    static const char* _STREAM_BOUNDARY = "\r\n--frame\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK) return res;

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
        } else {
            if(fb->format != PIXFORMAT_JPEG){
                bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                esp_camera_fb_return(fb);
                fb = NULL;
                if(!jpeg_converted){
                    Serial.println("JPEG compression failed");
                    res = ESP_FAIL;
                }
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        if(res == ESP_OK){
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(fb){
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if(_jpg_buf){
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if(res != ESP_OK) break;
    }
    return res;
}

// --- Snapshot Handler (保留) ---
static esp_err_t capture_handler(httpd_req_t *req){
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return ESP_OK;
}

// --- Index Handler (簡化為移動端友善) ---
static esp_err_t index_handler(httpd_req_t *req){
    // 增加基礎的 viewport 和排版，使其在手機上看起來更好
    const char* html =
        "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
        "<title>ESP32-CAM OV3660</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; text-align: center; margin: 0; background: #f0f0f0; }"
        "h2 { margin-top: 20px; color: #333; }"
        "img { max-width: 90%; height: auto; border: 3px solid #ccc; border-radius: 8px; margin: 10px 0; }"
        "a { text-decoration: none; color: white; background-color: #007bff; padding: 10px 20px; border-radius: 5px; display: inline-block; margin: 5px; }"
        "</style>"
        "</head><body>"
        "<h2>ESP32-CAM OV3660 Live Stream</h2>"
        "<img id=\"stream_img\" src=\"/stream\" />"
        "<p><a href=\"/capture\">Snapshot</a></p>"
        "</body></html>";
    return httpd_resp_send(req, html, strlen(html));
}

// --- 啟動 WebServer (保留) ---
void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
        httpd_register_uri_handler(camera_httpd, &index_uri);

        httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler };
        httpd_register_uri_handler(camera_httpd, &capture_uri);

        httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
        httpd_register_uri_handler(camera_httpd, &stream_uri);

        httpd_uri_t detect_uri = { .uri = "/detect", .method = HTTP_GET, .handler = detect_handler };
        httpd_register_uri_handler(camera_httpd, &detect_uri);

    }
}

// -------------------------------------------------------------------
// --- Setup 函式 (已修正 `esp_ota_mark_app_valid_...` 呼叫) ---
// -------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println("ESP32-CAM OV3660 User App Booting...");

    // 1. 執行連線檢查與容錯跳轉
    // 這會檢查 Wi-Fi 連線，若超時則跳轉回 Factory App。
    connectToWiFi();

    // 2. 相機設定 (保留您的配置)
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    
    // ... (您的 Pin 配置) ...
    config.pin_d0       = Y2_GPIO_NUM; config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM; config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM; config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM; config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM; config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM; config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM; config.pin_reset    = RESET_GPIO_NUM;
    
    // ... (您的記憶體與畫質配置) ...
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 2;

    if(psramFound()){ config.fb_location = CAMERA_FB_IN_PSRAM; } 
    else { config.fb_location = CAMERA_FB_IN_DRAM; }

    if(esp_camera_init(&config) != ESP_OK){
        Serial.println("FATAL: Camera init failed! Cannot proceed.");
        return; 
    }
    
    // 3. OTA 服務設定
    ArduinoOTA.setHostname("esp32car-a882c34c");
    ArduinoOTA.begin();

    // 4. 啟動 WebServer 
    startCameraServer();
    Serial.println("Camera server started.");

    // 5. CRITICAL: 標記當前 User App 為有效 (使用建議的替代函式)
    Serial.println("CRITICAL: Marking current OTA App as VALID.");
    // 修正: 將 esp_ota_mark_app_valid_default_direction 替換為 esp_ota_mark_app_valid_cancel_rollback
    esp_ota_mark_app_valid_cancel_rollback(); 
}

// --- Loop (保留) ---
void loop() {
    ArduinoOTA.handle();
    delay(10);
}
