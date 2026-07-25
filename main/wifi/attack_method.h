/**
 * @file attack_method.h
 * @author risinek (risinek@gmail.com)
 * @date 2021-04-07
 * @copyright Copyright (c) 2021
 *
 * @brief Provides interface for common methods used in various attacks
 */
#ifndef ATTACK_METHOD_H
#define ATTACK_METHOD_H

#include "esp_wifi_types.h"
#include "attack.h"

/** Frames-per-burst ceiling for attack_method_set_intensity() (broadcast
 *  deauth). Bursts fire every 100ms, so 50 is 500 frames/sec — per target,
 *  since each selected AP gets its own independent 100ms timer. That's the
 *  practical ceiling before esp_wifi_80211_tx() back-to-back calls start
 *  eating into the next tick's own 100ms budget (see the TX-failure
 *  handling in timer_send_deauth_frame(), which is the actual backstop if
 *  this or TARGETED_INTENSITY_MAX still turns out too high for a given
 *  situation). */
#define DEAUTH_INTENSITY_MAX 50

/** Separate, lower ceiling for attack_method_targeted_start(). Unlike
 *  broadcast deauth (one target per independent timer), targeted mode runs
 *  off a SINGLE shared timer that, every tick, sends a frame per
 *  intensity-unit to EVERY discovered client (up to TGT_MAX_CLIENTS) plus a
 *  broadcast fallback per target AP — a multiplier the user doesn't
 *  directly control (client count grows on its own as the attack sniffs
 *  more of them out), unlike ap_count in broadcast mode which they pick by
 *  hand. The same nominal intensity is therefore far more frames/tick here
 *  than in broadcast mode; capped lower to keep that multiplication from
 *  running away on a busy network. */
#define TARGETED_INTENSITY_MAX 10

/**
 * @brief Starts periodic deauthentication frame broadcast
 *
 * @param ap_record target AP record which BSSID will be used in deauthentication frame
 * @param period_sec period of broadcast in seconds
 */
void attack_method_broadcast(const wifi_ap_record_t *ap_record, unsigned period_sec);

/**
 * @brief Stop periodic deauthentication frame broadcast
 */
void attack_method_broadcast_stop();

/**
 * @brief Sets how many deauthentication frames are sent per burst (test intensity)
 *        for broadcast-mode deauth. Higher values send more frames per 100 ms tick.
 *
 * @param intensity frames per burst, clamped to <1,DEAUTH_INTENSITY_MAX>. 0 is treated as 1.
 */
void attack_method_set_intensity(uint8_t intensity);

/**
 * @brief Starts targeted client deauthentication.
 *
 * Puts the radio into promiscuous mode, learns the MAC addresses of clients
 * associated with the given target AP(s), and repeatedly sends a targeted
 * deauth frame to each discovered client (plus a broadcast deauth
 * fallback). Hops across the distinct target channels when needed.
 *
 * @param records     array of target AP records (BSSID + channel)
 * @param count       number of targets in @p records
 * @param intensity   frames per burst per client, clamped to <1,TARGETED_INTENSITY_MAX>
 */
void attack_method_targeted_start(const wifi_ap_record_t **records, uint8_t count, uint8_t intensity);

/**
 * @brief Stops targeted client deauthentication and disables promiscuous mode.
 */
void attack_method_targeted_stop(void);

/**
 * @brief Starts duplicated AP with same BSSID as genuine AP from ap_record
 *
 * This will execute deauthentication attack for given AP.
 * @param ap_record target AP that will be cloned/duplicated
 */
void attack_method_rogueap(const wifi_ap_record_t *ap_record);


void attack_method_evil_twin(const wifi_ap_record_t *ap_record);


void attack_method_super_clone(const wifi_ap_record_t *ap_record);
void attack_method_super_clone_stop(void);

bool is_super_clone_running(void);


/**
 * @brief Stops the deauth jammer attack.
 */
void attack_method_deauth_all_stop();

#endif
