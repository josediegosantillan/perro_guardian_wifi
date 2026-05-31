#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define TELEGRAM_BOT_TOKEN_MAX_LEN 96
#define TELEGRAM_CHAT_ID_MAX_LEN 32

typedef struct {
    bool enabled;
    bool bot_token_configured;
    char bot_token[TELEGRAM_BOT_TOKEN_MAX_LEN];
    char chat_id[TELEGRAM_CHAT_ID_MAX_LEN];
    bool notify_on_internet_fail;
    bool notify_on_router_reset;
    bool notify_on_cooldown;
    bool allow_commands;
} telegram_bot_config_t;

esp_err_t telegram_bot_init(void);
esp_err_t telegram_bot_get_config(telegram_bot_config_t *config);
esp_err_t telegram_bot_save_config(const telegram_bot_config_t *config, bool update_token);
esp_err_t telegram_bot_send_test_message(const char *message);
esp_err_t telegram_bot_notify(const char *message);

#endif
