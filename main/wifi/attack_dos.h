/**
 * @file attack_dos.h
 * @author risinek (risinek@gmail.com), SameerAlSahab (sameeralsahab54@gmail.com)
 * @date 2021-04-07
 * @copyright Copyright (c) 2021
 * 
 * @brief Provides interface for DoS attack using deauthentication methods
 */

#ifndef ATTACK_DOS_H
#define ATTACK_DOS_H

#include "attack.h"

/**
 * @brief Available methods that can be chosen for the DoS attack.
 * 
 */
typedef enum{
    ATTACK_DOS_METHOD_ROGUE_AP    = 0,
    ATTACK_DOS_METHOD_BROADCAST   = 1,
    ATTACK_DOS_METHOD_SUPER_CLONE = 3,   ///< "Multi-Clone Deauth" in the web UI
    ATTACK_DOS_METHOD_TARGETED    = 4    ///< sniff associated clients and deauth each one directly
} attack_dos_methods_t;

/**
 * @brief Starts DoS attack against target AP and using provided method.
 * 
 * To stop DoS attack, call attack_dos_stop().
 * 
 * @param attack_config attack config with valid ap_record and attack method chosen 
 */
void attack_dos_start(attack_config_t *attack_config);

/**
 * @brief Stops DoS attack.
 * 
 * This function stops everything that attack_dos_start() started and resets all values to default state.
 */
void attack_dos_stop();

#endif
