/*
sys_wii.c - misc wii stubs
Copyright (C) 2026 mintferret

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#include "platform/platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <fat.h>
#include <SDL.h>
#include <gccore.h>
#include <ogcsys.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

void Platform_ShellExecute( const char *path, const char *parms )
{
	Con_Reportf( S_WARN "Tried to shell execute ;%s; -- not supported\n", path );
}

void OGC_Init( void )
{
	SYS_STDIO_Report(true);
	WPAD_Init();
	KEYBOARD_Init(NULL);
	MOUSE_Init(NULL);
	printf( "%s\n", __func__ );
}

void OGC_Shutdown( void )
{
	printf( "%s\n", __func__ );
}

//const struct in6_addr in6addr_any = {{ 0 }};
