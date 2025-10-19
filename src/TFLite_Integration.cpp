#include <Arduino.h>
#include <esp_http_server.h>
#include <esp_camera.h>

/**
 * @brief 影像偵測處理器 (Placeholder)
 * * 此函式用於處理 /detect 網址請求，未來將實作 TFLite 模型推理和物件偵測邏輯。
 * * 備註: 這裡使用 extern "C" 宣告，因此實作時必須確保函式名稱和呼叫約定正確。
 * * @param req HTTP 請求結構
 * @return esp_err_t 成功返回 ESP_OK
 */
extern "C" esp_err_t detect_handler(httpd_req_t *req) {
    // -----------------------------------------------------------
    // TODO: 在此處新增您的 TFLite 影像擷取、模型載入與推理邏輯
    // -----------------------------------------------------------

    // 獲取一個相機幀 (如果需要即時偵測)
    // camera_fb_t *fb = esp_camera_fb_get();
    
    // 目前僅作為 Placeholder，返回成功訊息
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Detection endpoint is active. TFLite logic pending implementation.");
    
    // esp_camera_fb_return(fb); // 記得歸還 framebuffer

    return ESP_OK;
}
