#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// -- Includes da câmera
#include "esp_camera.h"
#include "esp_http_server.h"

//========== DEFINA SUA SSID E SENHA AQUI =============
#define WIFI_SSID "EngFlex"
#define WIFI_PASS "tecnoflex@avt"

//========== Definições de pinos para AI THINKER ======
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1 //software reset will be performed
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27

#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

static const char* TAG = "CAM_HTTP";

//========== Configuração da Câmera ===================
static camera_config_t camera_config = {
    .pin_pwdn  = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk  = CAM_PIN_XCLK,
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
    .pin_href  = CAM_PIN_HREF,
    .pin_pclk  = CAM_PIN_PCLK,
    // Frequência do XCLK
    .xclk_freq_hz = 20000000,
    // Configurações de LEDC (para gerar XCLK)
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    // Formato da imagem => JPEG para streaming
    .pixel_format = PIXFORMAT_JPEG,
    // Resolução (por ex. QVGA ou VGA). Ajuste conforme sua PSRAM
    .frame_size = FRAMESIZE_QQVGA,
    // Qualidade do JPEG (0 = melhor, 63 = pior)
    .jpeg_quality = 30,
    // Número de frame buffers (1 ou 2)
    .fb_count = 2,
    // Armazenar os frames em PSRAM
    .fb_location = CAMERA_FB_IN_PSRAM,
    // Modo de captura
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY
};

//========== Inicializar a câmera =====================
static esp_err_t init_camera(void)
{
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao inicializar câmera: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Câmera inicializada com sucesso");
    return ESP_OK;
}

//========== Handler para rota /stream (MJPEG) ========
//
// 1. Define o tipo "multipart/x-mixed-replace;boundary=frame"
// 2. Em loop, captura frames da câmera e envia cada frame como um "chunk".
//
static esp_err_t stream_handler(httpd_req_t *req)
{
    // Define o cabeçalho da resposta para streaming MJPEG
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");

    // Buffer para montar cabeçalho de cada frame
    char part_buf[64];
    
    // Loop infinito para enviar frames até o cliente desconectar
    while (true) {
        // Captura um frame
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Falha ao capturar frame");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        // Cria o cabeçalho do frame (boundary, Content-Type, tamanho etc.)
        size_t hlen = snprintf(part_buf, sizeof(part_buf),
                               "--frame\r\n"
                               "Content-Type: image/jpeg\r\n"
                               "Content-Length: %zu\r\n\r\n",
                               fb->len);
        
        // Envia esse cabeçalho para o cliente
        esp_err_t res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res == ESP_OK) {
            // Envia o próprio buffer da imagem (JPEG)
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        if (res == ESP_OK) {
            // Envia a terminação de linha após o frame
            res = httpd_resp_send_chunk(req, "\r\n", 2);
        }

        // Libera o frame buffer
        esp_camera_fb_return(fb);

        // Se houve erro em algum envio, encerramos o stream
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Erro ao enviar frame: %s", esp_err_to_name(res));
            break;
        }
    }

    return ESP_OK;
}

//========== Inicia o servidor web =====================
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    // Inicia servidor HTTP
    if (httpd_start(&server, &config) == ESP_OK) {
        // Cria a rota /stream
        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stream_uri);

        ESP_LOGI(TAG, "Rota /stream registrada para streaming MJPEG.");
        return server;
    }

    ESP_LOGE(TAG, "Falha ao iniciar o servidor HTTP");
    return NULL;
}

//========== Eventos do Wi-Fi ==========================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi desconectado, tentando reconexão...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        printf("Wi-Fi conectado! Acesse o link abaixo:\n");
        printf("http://%s/stream\n", ip_str);
    }
}

//========== Inicializa Wi-Fi no modo station ==========
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Cria interface station
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Registra handler de eventos
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Configura e inicia Wi-Fi station
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finalizado.");
}

//========== Função principal (app_main) ==============
void app_main(void)
{
    // Inicializa NVS (usada pelo Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES 
        || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicializa Wi-Fi station
    wifi_init_sta();

    // Pequeno delay para conectar
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    // Inicializa a câmera
    if (init_camera() != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicialização da câmera, reiniciando...");
        esp_restart();
    }

    // Inicia servidor Web
    start_webserver();

    // A partir daqui, basta acessar via browser:
    //   http://<ip_esp>/stream
    // para visualizar o stream em MJPEG contínuo.
}
