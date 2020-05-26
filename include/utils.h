/***************************************************************************
 * This file is part of NUSspli.                                           *
 * Copyright (c) 2019-2020 Pokes303                                        *
 * Copyright (c) 2020 V10lator <v10lator@myway.de>                         *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 2 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.             *
 ***************************************************************************/

#ifndef NUSSPLI_UTILS_H
#define NUSSPLI_UTILS_H

#include <wut-fixups.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NUSSPLI_DEBUG
	#include <whb/log.h>
	#include <whb/log_udp.h>
	#define shutdownDebug WHBLogUdpDeinit
#else
	// We need to initialize the socket API as we don't have network access without it, even if we use the AC network API to make sure we're connected.
	// This is most likely a bug in devkitPPC or WUT.
	#include <nsysnet/socket.h>
	#define debugPrintf(...)
	#define debugInit socket_lib_init
	#define shutdownDebug socket_lib_finish
#endif

#ifdef __cplusplus
	extern "C" {
#endif

extern int mcpHandle;

void enableShutdown();
void disableShutdown();
char* b_tostring(bool b);
char* hex(uint64_t i, int digits); //ex: 000050D1
bool pathExists(char *path);
long getFilesize(FILE *fp);
bool isNumber(char c);
bool isLowercase(char c);
bool isUppercase(char c);
bool isLowercaseHexa(char c);
bool isUppercaseHexa(char c);
bool isHexa(char c);
void toLowercase(char *inOut);
uint32_t getRandom();
void initRandom();
void getSpeedString(float bytePerSecond, char *out);
#ifdef NUSSPLI_DEBUG
void debugInit();
void debugPrintf(const char *str, ...);
#endif

#ifdef __cplusplus
	}
#endif

#endif // ifndef NUSSPLI_UTILS_H
