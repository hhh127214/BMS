/*
 *  Copyright 2016-2024 Michael Zillgith
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

#ifndef SRC_INC_T104_FRAME_H_
#define SRC_INC_T104_FRAME_H_

#include <stdint.h>

#include "frame.h"

#ifndef CONFIG_LIB60870_STATIC_FRAMES
#define CONFIG_LIB60870_STATIC_FRAMES 0
#endif

struct sT104Frame
{
    FrameVFT virtualFunctionTable;

    uint8_t buffer[256];
    int msgSize;

#if (CONFIG_LIB60870_STATIC_FRAMES == 1)
    uint8_t allocated;
#endif
};

typedef struct sT104Frame* T104Frame;

T104Frame
T104Frame_create(void);

T104Frame
T104Frame_createEx(T104Frame self);

void
T104Frame_destroy(Frame self);

void
T104Frame_resetFrame(Frame self);

void
T104Frame_prepareToSend(T104Frame self, int sendCounter, int receiveCounter);

void
T104Frame_setNextByte(Frame self, uint8_t byte);

void
T104Frame_appendBytes(Frame self, const uint8_t* bytes, int numberOfBytes);

int
T104Frame_getMsgSize(Frame self);

uint8_t*
T104Frame_getBuffer(Frame self);

int
T104Frame_getSpaceLeft(Frame self);


#endif /* SRC_INC_T104_FRAME_H_ */
