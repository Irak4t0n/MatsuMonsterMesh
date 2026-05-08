#include "custom_certificates.h"
#include <stdint.h>
#include "esp_log.h"
#include "esp_tls.h"

static const char* TAG = "Custom certificates";

#ifdef CONFIG_CUSTOM_CA_LETSENCRYPT_X1
extern const uint8_t _binary_letsencrypt_isrg_root_x1_pem_start[];
extern const uint8_t _binary_letsencrypt_isrg_root_x1_pem_end[];
#endif

#ifdef CONFIG_CUSTOM_CA_LETSENCRYPT_X2
extern const uint8_t _binary_letsencrypt_isrg_root_x2_pem_start[];
extern const uint8_t _binary_letsencrypt_isrg_root_x2_pem_end[];
#endif

#ifdef CONFIG_CUSTOM_CA_MCH2022_OTA
extern const uint8_t _binary_mch2022_ota_pem_start[];
extern const uint8_t _binary_mch2022_ota_pem_end[];
#endif

#ifdef CONFIG_CUSTOM_CA_TANMATSU_APPS
extern const uint8_t _binary_tanmatsu_apps_pem_start[];
extern const uint8_t _binary_tanmatsu_apps_pem_end[];
#endif

#ifdef CONFIG_CUSTOM_CA_TANMATSU_OTA
extern const uint8_t _binary_tanmatsu_ota_pem_start[];
extern const uint8_t _binary_tanmatsu_ota_pem_end[];
#endif

#ifdef CONFIG_CUSTOM_CA_BADGETEAM_OTA
extern const uint8_t _binary_badgeteam_ota_pem_start[];
extern const uint8_t _binary_badgeteam_ota_pem_end[];
#endif

// Note: there is no need to call esp_tls_init_global_ca_store as this function is called implicitly by
// esp_tls_set_global_ca_store if it has not already been called

static esp_err_t add_certificate(const uint8_t* start, const uint8_t* end) {
    return esp_tls_set_global_ca_store(start, end - start);
}

esp_err_t initialize_custom_ca_store(void) {
    esp_err_t res = ESP_OK;

#ifdef CONFIG_CUSTOM_CA_LETSENCRYPT_X1
    ESP_LOGI(TAG, "Adding Letsencrypt ISRG ROOT X1 certificate to CA store");
    res = add_certificate(_binary_letsencrypt_isrg_root_x1_pem_start, _binary_letsencrypt_isrg_root_x1_pem_end);
    if (res != ESP_OK) return res;
#endif

#ifdef CONFIG_CUSTOM_CA_LETSENCRYPT_X2
    ESP_LOGI(TAG, "Adding Letsencrypt ISRG ROOT X2 certificate to CA store");
    res = add_certificate(_binary_letsencrypt_isrg_root_x2_pem_start, _binary_letsencrypt_isrg_root_x2_pem_end);
    if (res != ESP_OK) return res;
#endif

#ifdef CONFIG_CUSTOM_CA_MCH2022_OTA
    ESP_LOGI(TAG, "Adding Badge.Team MCH2022 OTA certificate to CA store");
    res = add_certificate(_binary_mch2022_ota_pem_start, _binary_mch2022_ota_pem_end);
    if (res != ESP_OK) return res;
#endif

#ifdef CONFIG_CUSTOM_CA_TANMATSU_APPS
    ESP_LOGI(TAG, "Adding Tanmatsu APPS certificate to CA store");
    res = add_certificate(_binary_tanmatsu_apps_pem_start, _binary_tanmatsu_apps_pem_end);
    if (res != ESP_OK) return res;
#endif

#ifdef CONFIG_CUSTOM_CA_TANMATSU_OTA
    ESP_LOGI(TAG, "Adding Tanmatsu OTA certificate to CA store");
    res = add_certificate(_binary_tanmatsu_ota_pem_start, _binary_tanmatsu_ota_pem_end);
    if (res != ESP_OK) return res;
#endif

#ifdef CONFIG_CUSTOM_CA_BADGETEAM_OTA
    ESP_LOGI(TAG, "Adding Badge.Team OTA certificate to CA store");
    res = add_certificate(_binary_badgeteam_ota_pem_start, _binary_badgeteam_ota_pem_end);
    if (res != ESP_OK) return res;
#endif

    return res;
}
