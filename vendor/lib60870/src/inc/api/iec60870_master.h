/*
 *  Copyright 2016-2025 Michael Zillgith
 *
 *  This file is part of lib60870-C
 *
 *  lib60870-C is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  lib60870-C is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with lib60870-C.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  See COPYING file for the complete license text.
 */

#ifndef SRC_IEC60870_MASTER_H_
#define SRC_IEC60870_MASTER_H_

#include <stdint.h>
#include <stdbool.h>

#include "iec60870_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \file iec60870_master.h
 * \brief Common master side definitions for IEC 60870-5-101/104
 * These types are used by CS101/CS104 master
 */

/**
 * \brief Callback handler for received ASDUs
 *
 * This callback handler will be called for each received ASDU.
 * The CS101_ASDU object that is passed is only valid in the context
 * of the callback function.
 *
 * \param parameter user provided parameter
 * \param address address of the sender (slave/other station) - undefined for CS 104
 * \param asdu object representing the received ASDU
 *
 * \return true if the ASDU has been handled by the callback, false otherwise
 */
typedef bool (*CS101_ASDUReceivedHandler) (void* parameter, int address, CS101_ASDU asdu);

/**
 * \brief Plugin interface for CS101 or CS104 masters
 */
typedef struct sCS101_MasterPlugin* CS101_MasterPlugin;

typedef struct sIPeerConnection* IPeerConnection;

typedef enum
{
    CS101_MASTER_PLUGIN_RESULT_NOT_HANDLED = 0,
    CS101_MASTER_PLUGIN_RESULT_HANDLED = 1,
    CS101_MASTER_PLUGIN_RESULT_INVALID_ASDU = 2
} CS101_MasterPlugin_Result;

typedef void (*CS101_MasterPluginForwardAsduFunc)(CS101_MasterPlugin plugin, void* ctx, CS101_ASDU asdu);

struct sCS101_MasterPlugin
{
    CS101_MasterPlugin_Result (*handleAsdu) (void* parameter, IPeerConnection connection, CS101_ASDU asdu);
    CS101_MasterPlugin_Result (*sendAsdu) (void* parameter, IPeerConnection connection, CS101_ASDU asdu);
    void (*eventHandler)(void* parameter, IPeerConnection connection, int event); /* only be called when CS104 is used */
    void (*runTask) (void* parameter, IPeerConnection connection);

     void (*setForwardAsduFunction) (void* parameter, CS101_MasterPluginForwardAsduFunc func, void* ctx);

    //TODO check if this can be removed from master plugin
    Frame (*getNextAsduToSend) (void* parameter, Frame frame);

    void* parameter; /* parameter is set by the plugin and is used as the first parameter of all the plugin calls */
};

#ifdef __cplusplus
}
#endif

#endif /* SRC_IEC60870_MASTER_H_ */
