#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "app_wifi.h"

#include "esp_http_client.h"

#include <dht/dht.h>

//#if CONFIG_SSL_USING_WOLFSSL
#include <time.h>
#include "esp_timer.h"
#include "lwip/apps/sntp.h"
//#endif

#define MAX_HTTP_RECV_BUFFER 1024

#define WEB_SERVER "monitor.utopia.cable.nu"
#define WEB_PORT "8086"
#define WEB_PATH "/write?db=garden"
#define BUFFER 4096
#define INFLUX_URL "http://monitor.utopia.cable.nu:8086/write?db=garden"
#define LOG_SIZE 1024

static const char *TAG = "data_logger";

const dht_sensor_type_t sensor_type = DHT_TYPE_DHT22;
uint8_t const dht_gpio = 4;

char buf[BUFFER];


/* Root cert for howsmyssl.com, taken from howsmyssl_com_root_cert.pem

   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null

   The CA root cert is the last cert given in the chain of certs.

   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
//extern const char howsmyssl_com_root_cert_pem_start[] asm("_binary_howsmyssl_com_root_cert_pem_start");
//extern const char howsmyssl_com_root_cert_pem_end[]   asm("_binary_howsmyssl_com_root_cert_pem_end");

struct sample {
        int16_t temperature;
        int16_t humidity;
        uint16_t flow_pulse_count;
        uint64_t uptime_usec;
} log[LOG_SIZE];

struct stats {
    int16_t sensors_pass;
    int16_t sensors_fail;
    int16_t api_pass;
    int16_t api_fail;
} app_stats;

int log_start = 0;
int log_end = 0;

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Write out data
                // printf("%.*s", evt->data_len, (char*)evt->data);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

static void influxdb_client(int head, int tail)
{
    char *body;
    esp_err_t err;
    uint64_t microseconds = 0;
    struct timeval tv = {0};

    bzero(buf, BUFFER);

    body = buf;

    for(int ptr = head; ptr != tail; ptr = (ptr+1)%LOG_SIZE)
    {
        microseconds = get_boot_time() + log[ptr].uptime_usec;
        tv.tv_sec = microseconds / 1000000;
        tv.tv_usec = microseconds % 1000000;
        body = body + sprintf(body,
            "environment,sensor=humidity value=%.1f %.0lf\n",
            (float)log[ptr].humidity / 10, tv.tv_sec * 1.e+9 + tv.tv_usec);
        body = body + sprintf(body,
            "environment,sensor=temperature value=%.1f %.0lf\n",    
            (float)log[ptr].temperature / 10, tv.tv_sec * 1.e+9 + tv.tv_usec);        
        body = body + sprintf(body,
            "environment,sensor=uptime value=%Lu %.0lf\n",
            esp_timer_get_time(), tv.tv_sec * 1.e+9 + tv.tv_usec);
        body = body + sprintf(body,
            "environment,sensor=heap_size value=%d %.0lf\n",
            esp_get_free_heap_size(), tv.tv_sec * 1.e+9 + tv.tv_usec);
    }
    body = buf;

    esp_http_client_config_t config = {
        .url = INFLUX_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_post_field(client, body, strlen(body));

    while (1) {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) {
            break;
        }
        vTaskDelay(500 / portTICK_RATE_MS);
    }

    if (err == ESP_OK) 
    {
        if (esp_http_client_get_status_code(client) == 204)
        {
            ESP_LOGD(TAG, "HTTP POST Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
            log_start = tail;
            app_stats.api_pass++;
        } else {
            ESP_LOGD(TAG, "HTTP POST Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
            app_stats.api_fail++;
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %d", err);
        app_stats.api_fail++;
    }
    esp_http_client_cleanup(client);
}

void dhtMeasurementTask(void *pvParameters)
{    
    while(1)
        if (dht_read_data(sensor_type, dht_gpio, &log[log_end].humidity, &log[log_end].temperature)) 
        {
            log[log_end].uptime_usec = esp_timer_get_time();
            log_end = (log_end+1)%LOG_SIZE;
            app_stats.sensors_pass++;
        }
        else
        {
            app_stats.sensors_fail++;
        }
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
//    sntp_setoperatingmode(SNTP_OPMODE_POLL);
//    sntp_setservername(0, "1.1.1.1");
    sntp_init();
}

static void obtain_time(void)
{
    app_wifi_wait_connected();
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 100;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    ESP_LOGI(TAG, "Time set with NTP");
}

static void sntp_example_task(void *arg)
{
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];

    time(&now);
    localtime_r(&now, &timeinfo);

    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
    }

    while (1) {
        // update 'now' variable with current time
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year < (2016 - 1900)) {
            ESP_LOGE(TAG, "The current date/time error");
        } else {
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "The current date/time in Sydney is: %s", strftime_buf);
        }

        vTaskDelay(60000 / portTICK_RATE_MS);
    }
}

static void send_results(void *pvParameters)
{
    while ( 1 )
    {
        influxdb_client(log_start, log_end);
        vTaskDelay(100000 / portTICK_PERIOD_MS);
    }
}

static void status(void *pvParameters)
{
    while ( 1 )
    {
        ESP_LOGI(TAG, "Pending: %i, Count: %i: Sensor pass %i fail %i API pass %i fail %i\n", (log_end - log_start), log_end, app_stats.sensors_pass, app_stats.sensors_fail, app_stats.api_pass, app_stats.api_fail);
        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}

void app_main()
{
    xTaskCreate(dhtMeasurementTask, "dhtMeasurementTask", 2048, NULL, 20, NULL);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
    tzset();

    sntp_servermode_dhcp(1);

    app_wifi_initialise();
    obtain_time();

    app_wifi_wait_connected();

    // SNTP service uses LwIP, please allocate large stack space.
    //xTaskCreate(sntp_example_task, "sntp_example_task", 2048, NULL, 2, NULL);
    xTaskCreate(&send_results, "send_results", 10240, NULL, 40, NULL);
    xTaskCreate(&status, "status", 2048, NULL, 45, NULL);
}