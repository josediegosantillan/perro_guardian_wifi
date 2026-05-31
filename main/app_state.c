#include "app_state.h"

#include "esp_log.h"

static const char *TAG = "app_state";
static EventGroupHandle_t s_event_group;

esp_err_t app_state_init(void)
{
    if (s_event_group != NULL) {
        return ESP_OK;
    }

    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el EventGroup de estado");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

EventGroupHandle_t app_state_event_group(void)
{
    return s_event_group;
}

void app_state_set_bits(EventBits_t bits)
{
    if (s_event_group != NULL) {
        xEventGroupSetBits(s_event_group, bits);
    }
}

void app_state_clear_bits(EventBits_t bits)
{
    if (s_event_group != NULL) {
        xEventGroupClearBits(s_event_group, bits);
    }
}

bool app_state_test_bits(EventBits_t bits)
{
    if (s_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_event_group) & bits) == bits;
}
