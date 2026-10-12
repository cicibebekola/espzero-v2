#pragma once
#include "esp_http_server.h"
#include "file_server.h"

esp_err_t file_manager_register(httpd_handle_t server, const char *mount_point,
                                file_server_transfer_cb_t notify);
