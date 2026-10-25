// Host stand-in for ESP-IDF's esp_err.h, used only by the Mac simulator.
#ifndef SIMULATOR_ESP_ERR_H_
#define SIMULATOR_ESP_ERR_H_

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

#endif  // SIMULATOR_ESP_ERR_H_
