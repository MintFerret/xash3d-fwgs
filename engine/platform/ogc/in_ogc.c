/*
in_ogc.c - Wii remote pointer aiming
Copyright (C) 2026 twixerisss

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

#if XASH_OGC
#include "common.h"
#include "client.h"
#include "input.h"
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <ogc/usbmouse.h>
#include <wiikeyboard/keyboard.h>

/*
The remote's IR sensor arrives from SDL as an absolute pointer, not as
relative motion, so the engine's usual mouse look does not work with it: push
the pointer to the edge of the screen and the deltas simply stop, leaving you
unable to keep turning.

What console shooters do instead is treat the screen as two regions. Near the
middle the pointer only moves the aim - the view stays put, which is what
makes fine aiming feel steady. Past that box the view starts turning, faster
the further out you point, so you swing the camera by pushing towards the
edge of the screen and stop by coming back to the middle.
*/

static CVAR_DEFINE_AUTO( wii_ir, "1", FCVAR_ARCHIVE, "aim with the Wii remote pointer" );
static CVAR_DEFINE_AUTO( wii_ir_deadzone, "0.35", FCVAR_ARCHIVE, "fraction of the screen where pointing does not turn the view" );
static CVAR_DEFINE_AUTO( wii_ir_yawspeed, "220", FCVAR_ARCHIVE, "degrees per second of turn at the screen edge" );
static CVAR_DEFINE_AUTO( wii_ir_pitchspeed, "160", FCVAR_ARCHIVE, "degrees per second of pitch at the screen edge" );
static CVAR_DEFINE_AUTO( wii_ir_gunsway, "7", FCVAR_ARCHIVE, "degrees the weapon leans towards the pointer" );
static CVAR_DEFINE_AUTO( wii_ir_cursor, "1", FCVAR_ARCHIVE, "show the pointer in game as the aiming reticle" );
static CVAR_DEFINE_AUTO( wii_nunchuk_sens, "320", FCVAR_ARCHIVE, "Nunchuk stick sensitivity (128=lento, 320=normal, 512=rapido)" );
static CVAR_DEFINE_AUTO( wii_buttons, "1", FCVAR_ARCHIVE, "read the remote and nunchuk buttons straight from WPAD" );
static CVAR_DEFINE_AUTO( wii_rumble, "1", FCVAR_ARCHIVE, "controller vibration strength (0 = off, 1 = full)" );
static CVAR_DEFINE_AUTO( wii_gun_tilt, "1", FCVAR_ARCHIVE, "weapon rolls with the Wii remote (0 = off, negative = inverted)" );
static CVAR_DEFINE_AUTO( wii_gun_tilt_max, "55", FCVAR_ARCHIVE, "maximum weapon roll in degrees" );
static CVAR_DEFINE_AUTO( wii_gun_pitch, "1", FCVAR_ARCHIVE, "weapon follows the remote pitch when the pointer is lost (0 = off, negative = inverted)" );
static CVAR_DEFINE_AUTO( wii_mouse_sens, "2", FCVAR_ARCHIVE, "USB mouse look speed" );
static CVAR_DEFINE_AUTO( wii_mouse_invert, "0", FCVAR_ARCHIVE, "invert the USB mouse vertical look" );
static CVAR_DEFINE_AUTO( wii_mouse_zoom, "1", FCVAR_ARCHIVE, "slow the USB mouse down when the field of view is zoomed in" );
static CVAR_DEFINE_AUTO( wii_mouse_menu_sens, "1", FCVAR_ARCHIVE, "USB mouse cursor speed in the menus" );
static CVAR_DEFINE_AUTO( wii_stick_smooth, "0.012", FCVAR_ARCHIVE, "Classic/GameCube look smoothing in seconds (0 = off)" );
static CVAR_DEFINE_AUTO( wii_stick_curve, "1.7", FCVAR_ARCHIVE, "Classic/GameCube look response curve (1 = linear, higher = finer aim near the centre)" );
static CVAR_DEFINE_AUTO( wii_stick_yawspeed, "190", FCVAR_ARCHIVE, "Classic/GameCube turn speed at full stick, degrees per second" );
static CVAR_DEFINE_AUTO( wii_stick_pitchspeed, "120", FCVAR_ARCHIVE, "Classic/GameCube pitch speed at full stick, degrees per second" );
static CVAR_DEFINE_AUTO( wii_aimassist, "1", FCVAR_ARCHIVE, "aim assist for every controller (0 = off)" );
static CVAR_DEFINE_AUTO( wii_input_tune, "1", FCVAR_ARCHIVE, "input tuning revision (internal)" );
static CVAR_DEFINE_AUTO( wii_aimassist_strength, "0.35", FCVAR_ARCHIVE, "aim assist strength (0..1)" );

static CVAR_DEFINE_AUTO( wii_aimassist_cone, "6", FCVAR_ARCHIVE, "aim assist capture angle in degrees" );
static CVAR_DEFINE_AUTO( wii_aimassist_range, "2500", FCVAR_ARCHIVE, "aim assist maximum distance in map units" );
static CVAR_DEFINE_AUTO( wii_aimassist_idle, "0.3", FCVAR_ARCHIVE, "stick aim assist strength while the stick is not moving, as a fraction of wii_aimassist_strength (0 = only while moving)" );
static CVAR_DEFINE_AUTO( wii_aimassist_ir, "1", FCVAR_ARCHIVE, "aim assist also steers the camera towards enemies near the Wii remote pointer" );
#ifdef XASH_OGC_INPUTTEST
#define WII_SHOWINPUT_DEFAULT "1"	// input test build: on with nothing to configure
#else
#define WII_SHOWINPUT_DEFAULT "0"
#endif
static CVAR_DEFINE_AUTO( wii_showinput, WII_SHOWINPUT_DEFAULT, 0, "print raw controller state whenever it changes" );

/*
SDL synthesises a gamepad mapping for whatever it finds on the WPAD channel,
and which physical button ends up as "A" or "leftshoulder" is its guess, not
ours. The trigger and the A button already arrive as mouse buttons and work,
so those are left alone; everything else is read here, where the WPAD masks
say exactly which button was pressed.

The engine keys below are chosen so the stock bindings already land on the
right actions, which keeps the whole scheme visible and rebindable from
Options -> Controls rather than hidden in a config file.
*/
typedef struct { u32 mask; int key; } ogc_btn_t;

/*
Un "key" mayor que OGC_ACT_BASE no es una tecla del motor sino una accion que
se ejecuta directamente. Asi el marcador, el autoguardado y el chat no
dependen de que haya un bind en la configuracion del jugador (TAB venia sin
bind y no abria nada), y el teclado en pantalla puede usar los mismos botones.
*/
#define OGC_ACT_BASE 0x10000
enum
{
	OGC_ACT_SCORES = OGC_ACT_BASE,	// +showscores / -showscores
	OGC_ACT_QUICKSAVE,		// savequick
	OGC_ACT_CHAT,			// messagemode
	OGC_ACT_HOME,			// toque corto: ESC, mantenido: consola
	OGC_OSK_BASE,			// + OSK_ACT_*
};

#define OGC_OSK( act ) ( OGC_OSK_BASE + ( act ))


/*
Three layouts, one set of engine keys. The keys are picked so the stock
bindings already land on the right actions, which keeps every scheme visible
and rebindable from Options -> Controls instead of hidden in a config file.
*/
static const ogc_btn_t ogc_map_wiimote[] =
{
    { WPAD_BUTTON_LEFT,      K_DPAD_LEFT   }, // invprev
    { WPAD_BUTTON_RIGHT,     K_DPAD_RIGHT  }, // invnext
    { WPAD_BUTTON_DOWN,      K_R2_BUTTON   }, // +attack2
    { WPAD_BUTTON_UP,        K_Y_BUTTON    }, // flashlight

    { WPAD_NUNCHUK_BUTTON_C, K_B_BUTTON    }, // +use
    { WPAD_NUNCHUK_BUTTON_Z, K_R1_BUTTON   }, // +attack

    { WPAD_BUTTON_1,         OGC_ACT_SCORES    }, // marcador (TAB)
    { WPAD_BUTTON_2,         OGC_ACT_QUICKSAVE }, // savequick

    { WPAD_BUTTON_PLUS,      OGC_ACT_CHAT  }, // chat
    { WPAD_BUTTON_MINUS,     K_X_BUTTON    }, // +reload

    { WPAD_BUTTON_A,         K_A_BUTTON    }, // +jump
    { WPAD_BUTTON_B,         K_L1_BUTTON   }, // +duck

    { WPAD_BUTTON_HOME,      OGC_ACT_HOME  }, // ESC (mantenido: consola)
};

// con el teclado en pantalla visible: A y B son clic, el puntero IR elige tecla
static const ogc_btn_t ogc_osk_wiimote[] =
{
    { WPAD_BUTTON_A,         OGC_OSK( OSK_ACT_SELECT )    },
    { WPAD_BUTTON_B,         OGC_OSK( OSK_ACT_SELECT )    },
    { WPAD_BUTTON_2,         OGC_OSK( OSK_ACT_BACKSPACE ) },
    { WPAD_BUTTON_1,         OGC_OSK( OSK_ACT_SHIFT )     },
    { WPAD_BUTTON_MINUS,     OGC_OSK( OSK_ACT_SPACE )     },
    { WPAD_BUTTON_PLUS,      OGC_OSK( OSK_ACT_ENTER )     },
    { WPAD_NUNCHUK_BUTTON_Z, OGC_OSK( OSK_ACT_ENTER )     },
    { WPAD_BUTTON_UP,        OGC_OSK( OSK_ACT_UP )        },
    { WPAD_BUTTON_DOWN,      OGC_OSK( OSK_ACT_DOWN )      },
    { WPAD_BUTTON_LEFT,      OGC_OSK( OSK_ACT_LEFT )      },
    { WPAD_BUTTON_RIGHT,     OGC_OSK( OSK_ACT_RIGHT )     },
    { WPAD_BUTTON_HOME,      OGC_ACT_HOME                 },
};

/*
The classic controller reports through the same word as the nunchuk and the
two overlap bit for bit, so which table applies is decided by the expansion
WPAD_Probe reports rather than by ORing them together.
*/
static const ogc_btn_t ogc_map_classic[] =
{
        // L y R (gatillos grandes) disparan: R primario, L secundario.
        // ZL y ZR (los pequenos) hacen de agacharse y linterna.
        { WPAD_CLASSIC_BUTTON_FULL_R, K_R1_BUTTON    }, // +attack
        { WPAD_CLASSIC_BUTTON_FULL_L, K_R2_BUTTON    }, // +attack2
        { WPAD_CLASSIC_BUTTON_ZR,     K_Y_BUTTON     }, // linterna (impulse 100)
        { WPAD_CLASSIC_BUTTON_ZL,     K_L1_BUTTON    }, // +duck
        { WPAD_CLASSIC_BUTTON_A,      K_A_BUTTON     }, // +jump
        { WPAD_CLASSIC_BUTTON_X,      K_B_BUTTON     }, // +use
        { WPAD_CLASSIC_BUTTON_Y,      K_X_BUTTON     }, // +reload
        { WPAD_CLASSIC_BUTTON_B,      OGC_ACT_QUICKSAVE }, // autoguardado
        { WPAD_CLASSIC_BUTTON_PLUS,   OGC_ACT_CHAT   }, // chat
        { WPAD_CLASSIC_BUTTON_MINUS,  OGC_ACT_SCORES }, // marcador multijugador
        { WPAD_CLASSIC_BUTTON_UP,     K_DPAD_UP      }, // spray
        { WPAD_CLASSIC_BUTTON_DOWN,   K_DPAD_DOWN    }, // lastinv
        { WPAD_CLASSIC_BUTTON_LEFT,   K_DPAD_LEFT    }, // invprev
        { WPAD_CLASSIC_BUTTON_RIGHT,  K_DPAD_RIGHT   }, // invnext
        { WPAD_CLASSIC_BUTTON_HOME,   OGC_ACT_HOME   }, // ESC (mantenido: consola)
};

static const ogc_btn_t ogc_osk_classic[] =
{
        { WPAD_CLASSIC_BUTTON_A,      OGC_OSK( OSK_ACT_SELECT )    },
        { WPAD_CLASSIC_BUTTON_B,      OGC_OSK( OSK_ACT_BACKSPACE ) },
        { WPAD_CLASSIC_BUTTON_X,      OGC_OSK( OSK_ACT_SPACE )     },
        { WPAD_CLASSIC_BUTTON_Y,      OGC_OSK( OSK_ACT_SHIFT )     },
        { WPAD_CLASSIC_BUTTON_PLUS,   OGC_OSK( OSK_ACT_ENTER )     },
        { WPAD_CLASSIC_BUTTON_UP,     OGC_OSK( OSK_ACT_UP )        },
        { WPAD_CLASSIC_BUTTON_DOWN,   OGC_OSK( OSK_ACT_DOWN )      },
        { WPAD_CLASSIC_BUTTON_LEFT,   OGC_OSK( OSK_ACT_LEFT )      },
        { WPAD_CLASSIC_BUTTON_RIGHT,  OGC_OSK( OSK_ACT_RIGHT )     },
        { WPAD_CLASSIC_BUTTON_HOME,   OGC_ACT_HOME                 },
};

static const ogc_btn_t ogc_map_gamecube[] =
{
        { PAD_TRIGGER_R,     K_R1_BUTTON   },   // +attack
        { PAD_TRIGGER_L,     K_R2_BUTTON   },   // +attack2
        { PAD_TRIGGER_Z,     K_L1_BUTTON   },   // +duck
        { PAD_BUTTON_A,      K_A_BUTTON    },   // +jump
        { PAD_BUTTON_X,      K_B_BUTTON    },   // +use
        { PAD_BUTTON_Y,      K_X_BUTTON    },   // +reload
        { PAD_BUTTON_B,      OGC_ACT_QUICKSAVE }, // autoguardado
        { PAD_BUTTON_UP,     K_Y_BUTTON    },   // linterna (impulse 100)
        { PAD_BUTTON_DOWN,   OGC_ACT_SCORES },  // marcador multijugador
        { PAD_BUTTON_LEFT,   K_DPAD_LEFT   },   // invprev
        { PAD_BUTTON_RIGHT,  K_DPAD_RIGHT  },   // invnext
        { PAD_BUTTON_START,  OGC_ACT_HOME  },   // ESC (mantenido: consola)
};

static const ogc_btn_t ogc_osk_gamecube[] =
{
        { PAD_BUTTON_A,      OGC_OSK( OSK_ACT_SELECT )    },
        { PAD_BUTTON_B,      OGC_OSK( OSK_ACT_BACKSPACE ) },
        { PAD_BUTTON_X,      OGC_OSK( OSK_ACT_SPACE )     },
        { PAD_BUTTON_Y,      OGC_OSK( OSK_ACT_SHIFT )     },
        { PAD_TRIGGER_Z,     OGC_OSK( OSK_ACT_ENTER )     },
        { PAD_BUTTON_UP,     OGC_OSK( OSK_ACT_UP )        },
        { PAD_BUTTON_DOWN,   OGC_OSK( OSK_ACT_DOWN )      },
        { PAD_BUTTON_LEFT,   OGC_OSK( OSK_ACT_LEFT )      },
        { PAD_BUTTON_RIGHT,  OGC_OSK( OSK_ACT_RIGHT )     },
        { PAD_BUTTON_START,  OGC_ACT_HOME                 },
};

// estado por mando: lo que estaba apretado, lo que se mando como "pulsado" y la tabla en uso
typedef struct { u32 prev; u32 active; qboolean osk; } ogc_dev_t;
static ogc_dev_t ogc_dev_wii, ogc_dev_classic, ogc_dev_gc;

/*
Which device the player touched last. It decides where the rumble goes and
whether the Wii remote pointer is allowed to aim: with a USB mouse in use the
remote is usually lying on a table, and its IR sensor would keep dragging the
aim around.
*/
enum { OGC_SRC_WII, OGC_SRC_GC, OGC_SRC_CLASSIC, OGC_SRC_MOUSE };
static int ogc_src = OGC_SRC_WII;

/* Forward declarations para evitar warnings de "implicit declaration" */
static qboolean OGC_AssistActive( void );
static qboolean OGC_FindAssistTarget( const vec3_t aim_angles, float *dyaw, float *dpitch, float *angle );
static float OGC_AssistBlend( float offset, float dt );
// mirada de la palanca derecha (-1..1, x derecha, y arriba) ya con zona muerta y curva
static float ogc_look_raw[2];
static float ogc_look_filt[2];

// set by OGC_Init: reading the queues of a device that failed to start would
// walk an uninitialised list
int ogc_usb_kbd_ok;
int ogc_usb_mouse_ok;

// where the player is pointing, as -1..1 from the centre of the screen.
// kept here so the view code can lean the weapon towards it.
static vec2_t ogc_pointer;

/*
Cambia el dispositivo activo. Mientras no sea el Wiimote, la mira deja de ser la
del puntero IR (ver OGC_GetPointerAngles y OGC_PointerMove) y vuelve al centro,
como en los shooters de Wii: con Classic, GameCube o raton el arma y el disparo
salen rectos de la camara. El cursor de los menus conserva su ultima posicion.
*/
static void OGC_SetSource( int src )
{
	ogc_src = src;
}

static qboolean OGC_GetPointerAngles( float *dyaw, float *dpitch );

// USB mouse: motion waiting to be turned into view angles, and a virtual
// cursor in screen pixels for the menus
static float ogc_mouse_dx, ogc_mouse_dy;
static float ogc_mouse_x, ogc_mouse_y;
static void OGC_RumbleFrame( void );
/*
Diagnostico de disparos sin imprimir nada mientras se juega: cada comando con
+attack deja una entrada en memoria (cliente: instante y ms; servidor: tiempo
del jugador, instante real y ms). "wii_firestats" las muestra y las borra.
*/
#define OGC_FIRELOG_MAX 96
static struct { byte server; int ms; double t, t2; } ogc_firelog[OGC_FIRELOG_MAX];
static int ogc_firelog_n;

__attribute__((visibility("default"))) void OGC_FireLog( int server, double t, double t2, int ms )
{
	if( ogc_firelog_n >= OGC_FIRELOG_MAX )
		return;

	ogc_firelog[ogc_firelog_n].server = server ? 1 : 0;
	ogc_firelog[ogc_firelog_n].t = t;
	ogc_firelog[ogc_firelog_n].t2 = t2;
	ogc_firelog[ogc_firelog_n].ms = ms;
	ogc_firelog_n++;
}

/*
Perfil silencioso (no imprime durante el juego): tiempo medio y maximo de cada parte
del cuadro y de la carga de nivel. "wii_prof" lo muestra y lo reinicia.
  0 cuadro completo  1 servidor  2 cliente (todo)  3 pantalla (render + vsync)
  4 sonido           5 entrada
*/
#define OGC_PROF_N 24
static const char *ogc_prof_name[OGC_PROF_N] = { "cuadro", "servidor", "cliente", "cl.previo", "dibujo", "entrada", "snd.update", "S_LoadSnd", "LoadFile", "FS_Read", "Mod_ForNm", "LoadImage", "FS_Open", "FS_Seek", "escena", "mundo", "entidad", "gpu.esp", "vsync", "sv.fisica", "sv.envio", "est.huesos", "est.luz", "est.malla" };
static double ogc_prof_sum[OGC_PROF_N], ogc_prof_max[OGC_PROF_N];
static int ogc_prof_cnt[OGC_PROF_N];
static double ogc_prof_bytes[OGC_PROF_N];
static double ogc_load_t0, ogc_load_server, ogc_load_total;

/*
Tirones: histograma del tiempo de cada cuadro y registro de los ultimos cuadros lentos
(mas de 30 ms) con el desglose de lo que paso en ese cuadro. Se ve con "wii_prof".
*/
#define OGC_HITCH_MS 30.0
#define OGC_HITCH_N 40
static double ogc_frame_acc[OGC_PROF_N];
static int ogc_hist[6];	// <=18, <=34, <=50, <=100, <=250, >250 ms
static struct { double ms, v[OGC_PROF_N]; int frame; } ogc_hitch[OGC_HITCH_N];
static int ogc_hitch_total, ogc_frames_seen;

__attribute__((visibility("default"))) __attribute__((visibility("default"))) void OGC_ProfBytes( int bucket, double bytes )
{
	if( bucket >= 0 && bucket < OGC_PROF_N )
		ogc_prof_bytes[bucket] += bytes;
}

__attribute__((visibility("default"))) void OGC_ProfAdd( int bucket, double dt )
{
	if( bucket < 0 || bucket >= OGC_PROF_N )
		return;

	ogc_prof_sum[bucket] += dt;
	ogc_prof_cnt[bucket]++;
	if( dt > ogc_prof_max[bucket] )
		ogc_prof_max[bucket] = dt;

	if( bucket != 0 )
	{
		ogc_frame_acc[bucket] += dt;
	}
	else
	{
		// fin de cuadro: clasificar y, si fue lento, guardar el desglose
		double ms = dt * 1000.0;
		int h = ms <= 18.0 ? 0 : ms <= 34.0 ? 1 : ms <= 50.0 ? 2 : ms <= 100.0 ? 3 : ms <= 250.0 ? 4 : 5;

		ogc_hist[h]++;
		ogc_frames_seen++;
		if( ms > OGC_HITCH_MS )
		{
			int k = ogc_hitch_total % OGC_HITCH_N, i;

			ogc_hitch[k].ms = ms;
			ogc_hitch[k].frame = ogc_frames_seen;
			for( i = 1; i < OGC_PROF_N; i++ )
				ogc_hitch[k].v[i] = ogc_frame_acc[i] * 1000.0;
			ogc_hitch_total++;
		}
		memset( ogc_frame_acc, 0, sizeof( ogc_frame_acc ));
	}
}

// 0 = empieza la carga, 1 = servidor listo, 2 = el cliente ya esta jugando
__attribute__((visibility("default"))) void OGC_LoadMark( int what )
{
	double now = Platform_DoubleTime();

	if( what == 0 )
	{
		ogc_load_t0 = now;
		ogc_load_server = ogc_load_total = 0.0;
	}
	else if( what == 1 && ogc_load_t0 > 0.0 )
		ogc_load_server = now - ogc_load_t0;
	else if( what == 2 && ogc_load_t0 > 0.0 && ogc_load_total == 0.0 )
		ogc_load_total = now - ogc_load_t0;
}

static void OGC_Prof_f( void )
{
	int i;

	Con_Printf( "wii_prof (ms: media / maximo / cuadros)\n" );
	for( i = 0; i < OGC_PROF_N; i++ )
	{
		if( !ogc_prof_cnt[i] )
			continue;

		Con_Printf( "  %-8s %6.2f / %6.1f / %d", ogc_prof_name[i],
			ogc_prof_sum[i] * 1000.0 / ogc_prof_cnt[i], ogc_prof_max[i] * 1000.0, ogc_prof_cnt[i] );
		if( i >= 6 )
		{
			// tiempo total de la categoria desde el ultimo wii_prof, y datos leidos
			Con_Printf( "  total %.2f s", ogc_prof_sum[i] );
			if( ogc_prof_bytes[i] > 0.0 )
				Con_Printf( ", %.1f MB (%.2f MB/s)", ogc_prof_bytes[i] / 1048576.0,
					ogc_prof_sum[i] > 0.0 ? ogc_prof_bytes[i] / 1048576.0 / ogc_prof_sum[i] : 0.0 );
		}
		Con_Printf( "\n" );
		ogc_prof_sum[i] = ogc_prof_max[i] = ogc_prof_bytes[i] = 0.0;
		ogc_prof_cnt[i] = 0;
	}

	if( ogc_frames_seen )
	{
		Con_Printf( "  cuadros (ms): <=18 %d, <=34 %d, <=50 %d, <=100 %d, <=250 %d, >250 %d\n",
			ogc_hist[0], ogc_hist[1], ogc_hist[2], ogc_hist[3], ogc_hist[4], ogc_hist[5] );
		if( ogc_hitch_total )
		{
			int n = ogc_hitch_total < OGC_HITCH_N ? ogc_hitch_total : OGC_HITCH_N;
			int first = ogc_hitch_total - n;

			Con_Printf( "  cuadros lentos (>%d ms): %d, ultimos %d\n", (int)OGC_HITCH_MS, ogc_hitch_total, n );
			Con_Printf( "  cuadro total | srv prev dibujo snd | sonido lect modelo imagen | escena mundo entid gpu vsync | fis env (ms)\n" );
			for( i = 0; i < n; i++ )
			{
				int k = ( first + i ) % OGC_HITCH_N;

				Con_Printf( "  #%d %6.1f | %4.1f %4.1f %5.1f %4.1f | %4.1f %4.1f %4.1f %4.1f | %5.1f %5.1f %5.1f %5.1f %5.1f | %4.1f %4.1f\n",
					ogc_hitch[k].frame, ogc_hitch[k].ms, ogc_hitch[k].v[1], ogc_hitch[k].v[3],
					ogc_hitch[k].v[4], ogc_hitch[k].v[6], ogc_hitch[k].v[7], ogc_hitch[k].v[9],
					ogc_hitch[k].v[10], ogc_hitch[k].v[11], ogc_hitch[k].v[14], ogc_hitch[k].v[15],
					ogc_hitch[k].v[16], ogc_hitch[k].v[17], ogc_hitch[k].v[18],
					ogc_hitch[k].v[19], ogc_hitch[k].v[20] );
			}
		}
		memset( ogc_hist, 0, sizeof( ogc_hist ));
		ogc_hitch_total = ogc_frames_seen = 0;
	}

	if( ogc_load_total > 0.0 )
		Con_Printf( "  carga de nivel: servidor %.2f s, hasta jugar %.2f s\n", ogc_load_server, ogc_load_total );
}

static void OGC_FireStats_f( void )
{
	double base_real = -1.0, base_time = -1.0;
	int i, ncl = 0, nsv = 0;

	for( i = 0; i < ogc_firelog_n; i++ )
	{
		if( ogc_firelog[i].server ) nsv++; else ncl++;

		if( base_real < 0.0 )
			base_real = ogc_firelog[i].server ? ogc_firelog[i].t2 : ogc_firelog[i].t;
		if( base_time < 0.0 && ogc_firelog[i].server )
			base_time = ogc_firelog[i].t;
	}

	Con_Printf( "firestats: %d cmds cliente, %d servidor\n", ncl, nsv );
	Con_Printf( "  C = cliente (real, ms)   S = servidor (tiempo jugador, real, ms)\n" );

	for( i = 0; i < ogc_firelog_n; i++ )
	{
		if( ogc_firelog[i].server )
			Con_Printf( "S %.3f %.3f %d\n", ogc_firelog[i].t - base_time,
				ogc_firelog[i].t2 - base_real, ogc_firelog[i].ms );
		else
			Con_Printf( "C %.3f %d\n", ogc_firelog[i].t - base_real, ogc_firelog[i].ms );
	}

	ogc_firelog_n = 0;
}

static void OGC_GetRemoteTilt( float *roll, float *pitch );
static void OGC_KeyboardFrame( void );
static void OGC_MouseFrame( void );

void OGC_InputInit( void )
{
	// The IR coordinates come back in whatever space we ask for, and SDL
	// reports them straight through as absolute mouse position, so this has
	// to match the screen the engine thinks it is drawing to.
	if( refState.width > 0 && refState.height > 0 )
		WPAD_SetVRes( WPAD_CHAN_ALL, refState.width, refState.height );

	Cvar_RegisterVariable( &wii_ir );
	Cvar_RegisterVariable( &wii_ir_deadzone );
	Cvar_RegisterVariable( &wii_ir_yawspeed );
	Cvar_RegisterVariable( &wii_ir_pitchspeed );
	Cvar_RegisterVariable( &wii_ir_gunsway );
	Cvar_RegisterVariable( &wii_ir_cursor );
Cvar_RegisterVariable( &wii_nunchuk_sens );
	Cvar_RegisterVariable( &wii_buttons );
	Cvar_RegisterVariable( &wii_showinput );
	Cmd_AddCommand( "wii_firestats", OGC_FireStats_f, "show (and clear) the +attack usercmd log" );
	Cmd_AddCommand( "wii_prof", OGC_Prof_f, "show (and reset) the frame / level load timing profile" );
	Cvar_RegisterVariable( &wii_rumble );
	Cvar_RegisterVariable( &wii_gun_tilt );
	Cvar_RegisterVariable( &wii_gun_tilt_max );
	Cvar_RegisterVariable( &wii_gun_pitch );
	Cvar_RegisterVariable( &wii_mouse_sens );
	Cvar_RegisterVariable( &wii_mouse_invert );
	Cvar_RegisterVariable( &wii_mouse_zoom );
	Cvar_RegisterVariable( &wii_mouse_menu_sens );
	Cvar_RegisterVariable( &wii_stick_smooth );
	Cvar_RegisterVariable( &wii_stick_curve );
	Cvar_RegisterVariable( &wii_stick_yawspeed );
	Cvar_RegisterVariable( &wii_stick_pitchspeed );
    Cvar_RegisterVariable( &wii_input_tune );
	Cvar_RegisterVariable( &wii_aimassist );
	Cvar_RegisterVariable( &wii_aimassist_strength );
	Cvar_RegisterVariable( &wii_aimassist_cone );
	Cvar_RegisterVariable( &wii_aimassist_range );
	Cvar_RegisterVariable( &wii_aimassist_idle );
	Cvar_RegisterVariable( &wii_aimassist_ir );


#if XASH_OGC_AIMTEST
	// The aim maths cannot be exercised without a real pointer, so check it
	// against known positions at startup instead. At a 90 degree horizontal
	// field of view the screen edge is 45 degrees off centre.
	{
		static const float cases[][2] = { {0,0}, {1,0}, {-1,0}, {0,1}, {0.5f,-0.5f} };
		vec2_t saved;
		int i;

		Vector2Copy( ogc_pointer, saved );
		for( i = 0; i < 5; i++ )
		{
			float dy = 0, dp = 0;

			ogc_pointer[0] = cases[i][0];
			ogc_pointer[1] = cases[i][1];
			OGC_GetPointerAngles( &dy, &dp );
			printf( "[AIMTEST] pointer=%+.2f,%+.2f -> dyaw=%+.2f dpitch=%+.2f (fov=%.0f %dx%d)\n",
				cases[i][0], cases[i][1], dy, dp, cl.local.scr_fov, refState.width, refState.height );
		}
		Vector2Copy( saved, ogc_pointer );
	}
#endif
}

/*
============
OGC_ButtonsFrame

Turns the WPAD button state into engine key events. Edge triggered, so only
changes are reported. WPAD is not scanned here - SDL already does that every
frame to produce the pointer motion, and scanning twice would race it.
============
*/
/*
En juego, ESC pasaba por el binding "cancelselect" y dependia del client.dll
para abrir el menu. Abrimos el menu directamente con el comando del motor. Con
el menu abierto, la tecla sigue yendo al menu (volver atras). Lo usan tanto
los botones de los mandos como el teclado USB.
*/
static void OGC_SendKey( int key, qboolean down );

// toque corto de Inicio/Start = ESC; mantenido ~1s = consola
#define OGC_HOME_LONG 0.8
static struct { qboolean down, used; double time; } ogc_home;

static void OGC_SendAction( int act, qboolean down )
{
	static qboolean scores_down;

	switch( act )
	{
	case OGC_ACT_SCORES:
		// la tecla se suelta aunque en medio se haya abierto un menu
		if( !down && scores_down )
		{
			scores_down = false;
		Cbuf_AddText( "-showscores\n" );
		}
		else if( down && cls.key_dest == key_game && Cmd_Exists( "+showscores" ))
		{
			scores_down = true;
		Cbuf_AddText( "+showscores\n" );
		}
		else
			Key_Event( K_TAB, down );	// autocompletar en la consola, o el bind de TAB
		break;

	case OGC_ACT_QUICKSAVE:
		if( down && cls.key_dest == key_game && Cmd_Exists( "savequick" ))
		Cbuf_AddText( "savequick\n" );
		break;

	case OGC_ACT_CHAT:
		if( down && cls.key_dest == key_game && Cmd_Exists( "messagemode" ))
		Cbuf_AddText( "messagemode\n" );
		break;

	case OGC_ACT_HOME:
		if( down )
		{
			ogc_home.down = true;
			ogc_home.used = false;
			ogc_home.time = host.realtime;
		}
		else if( ogc_home.down )
		{
			ogc_home.down = false;

			if( !ogc_home.used )
			{
				OGC_SendKey( K_ESCAPE, true );
				OGC_SendKey( K_ESCAPE, false );
			}
		}
		break;

	default:
		if( act >= OGC_OSK_BASE )
			OSK_Action( act - OGC_OSK_BASE, down );
		break;
	}
}

static void OGC_SendKey( int key, qboolean down )
{
	if( key >= OGC_ACT_BASE )
	{
		OGC_SendAction( key, down );
		return;
	}

	if( key == K_ESCAPE && cls.key_dest == key_game )
	{
		if( down )
		Cbuf_AddText( "escape\n" );
		return;
	}

	Key_Event( key, down );
}

// mantener Inicio abre la consola (para escribir comandos con el teclado en pantalla)
static void OGC_HomeFrame( void )
{
	if( ogc_home.down && !ogc_home.used && host.realtime - ogc_home.time >= OGC_HOME_LONG )
	{
		ogc_home.used = true;
		Cbuf_AddText( "toggleconsole\n" );
	}
}

static void OGC_EmitButtons( const ogc_btn_t *map, int count, const ogc_btn_t *oskmap, int oskcount,
	u32 held, ogc_dev_t *dev, int src )
{
	qboolean osk = OSK_IsVisible();
	const ogc_btn_t *cur = dev->osk ? oskmap : map;
	int ncur = dev->osk ? oskcount : count;
	u32 changed;
	int i;

	// el teclado aparecio o se cerro: soltar lo que se habia pulsado con la
	// tabla anterior; lo que sigue apretado no cuenta hasta soltarlo y volver a pulsar
	if( osk != dev->osk )
	{
		for( i = 0; i < ncur; i++ )
		{
			if( dev->active & cur[i].mask )
				OGC_SendKey( cur[i].key, false );
		}

		dev->active = 0;
		dev->osk = osk;
		cur = osk ? oskmap : map;
		ncur = osk ? oskcount : count;
	}

	changed = held ^ dev->prev;
	dev->prev = held;

	if( !changed )
		return;

	// tocar un boton pasa el control a ese dispositivo (y deja el teclado USB)
	OGC_SetSource( src );
	OSK_SetSuspended( false );

	for( i = 0; i < ncur; i++ )
	{
		if( !( changed & cur[i].mask ))
			continue;

		if( held & cur[i].mask )
		{
			dev->active |= cur[i].mask;
			OGC_SendKey( cur[i].key, true );
		}
		else if( dev->active & cur[i].mask )
		{
			dev->active &= ~cur[i].mask;
			OGC_SendKey( cur[i].key, false );
		}
	}
}

/*
Teclado en pantalla: el Wiimote elige tecla con el puntero IR, el raton USB con
su cursor; Classic y GameCube con la cruceta. Tambien dice que boton hace que.
*/
static void OGC_OskFrame( void )
{
	static const char *hint_wii = "A/B: escribir  2: borrar  1: mayus  -: espacio  +: enviar  Home: salir";
	static const char *hint_classic = "A: escribir  B: borrar  Y: mayus  X: espacio  +: enviar  Home: salir";
	static const char *hint_gc = "A: escribir  B: borrar  Y: mayus  X: espacio  Z: enviar  Start: salir";
	float x = -1.0f, y = -1.0f;

	if( !OSK_IsVisible( ))
		return;

	if( ogc_src == OGC_SRC_CLASSIC )
		OSK_SetHint( hint_classic );
	else if( ogc_src == OGC_SRC_GC )
		OSK_SetHint( hint_gc );
	else
		OSK_SetHint( hint_wii );

	if( refState.width <= 0 || refState.height <= 0 )
		return;

	if( ogc_src == OGC_SRC_WII )
	{
		static float last_x, last_y;
		WPADData *data = WPAD_Data( WPAD_CHAN_0 );

		if( data && data->ir.valid )
		{
			x = data->ir.x / (float)refState.width;
			y = data->ir.y / (float)refState.height;
		}

		// la cruceta mueve la seleccion: el puntero solo la mueve si se mueve el
		OSK_Hover( x, y, fabsf( x - last_x ) + fabsf( y - last_y ) > 0.004f );
		last_x = x;
		last_y = y;
	}
	else if( ogc_src == OGC_SRC_MOUSE )
	{
		x = ogc_mouse_x / (float)refState.width;
		y = ogc_mouse_y / (float)refState.height;
		OSK_Hover( x, y, true );
	}
	else
		OSK_Hover( -1.0f, -1.0f, false );
}
static void OGC_PollNunchuk(void);
/*
============
OGC_StickLookMove

Mirada con la palanca derecha del Classic Controller y del mando de GameCube:
filtro de paso bajo (los cambios bruscos de la palanca no sacuden la camara),
velocidad maxima propia por eje y ayuda de punteria (friccion y atraccion
suave) cuando hay un enemigo cerca de la mira.
============
*/
void OGC_StickLookMove( float *pitch, float *yaw )
{
    float dt = bound( 0.0f, (float)host.realframetime, 0.1f );
    float tau = bound( 0.0f, wii_stick_smooth.value, 0.5f );
    float k = ( tau > 0.0f ) ? 1.0f - expf( -dt / tau ) : 1.0f;
    float speed = 1.0f, dy = 0.0f, dp = 0.0f, ay, ap, ang;
    qboolean moving;
    static double last_time = -1.0;
    int i;

    // una sola vez por cuadro aunque input.c tambien la llame
    if( host.realtime == last_time )
        return;
    last_time = host.realtime;

	if( ogc_src != OGC_SRC_CLASSIC && ogc_src != OGC_SRC_GC )
	{
		ogc_look_filt[0] = ogc_look_filt[1] = 0.0f;
		return;
	}

	// en los menus las palancas no giran la camara
	if( cls.key_dest != key_game )
	{
		ogc_look_filt[0] = ogc_look_filt[1] = 0.0f;
		return;
	}

	for( i = 0; i < 2; i++ )
	{
		ogc_look_filt[i] += ( ogc_look_raw[i] - ogc_look_filt[i] ) * k;
		if( fabsf( ogc_look_filt[i] ) < 0.002f && ogc_look_raw[i] == 0.0f )
			ogc_look_filt[i] = 0.0f;
	}

	moving = fabsf( ogc_look_filt[0] ) > 0.02f || fabsf( ogc_look_filt[1] ) > 0.02f;

	// ayuda de punteria: mas lento cerca del enemigo y un empujon hacia el;
	// con la palanca quieta tambien actua (wii_aimassist_idle)
	if( OGC_AssistActive( ) && ( moving || wii_aimassist_idle.value > 0.0f ))
	{
		vec3_t aim = { cl.viewangles[PITCH], cl.viewangles[YAW], 0.0f };

		if( OGC_FindAssistTarget( aim, &ay, &ap, &ang ))
		{
			float str = bound( 0.0f, wii_aimassist_strength.value, 1.0f );
			float f = bound( 0.0f, 1.0f - ang / bound( 0.5f, wii_aimassist_cone.value, 20.0f ), 1.0f );
            float blend = OGC_AssistBlend( ang, dt );

            if( moving )
                speed = 1.0f - str * 0.5f * f;
            else
                blend *= bound( 0.0f, wii_aimassist_idle.value, 1.0f );

            dy = ay * blend;
            dp = ap * blend;
		}
	}

	*yaw   += -ogc_look_filt[0] * wii_stick_yawspeed.value * dt * speed + dy;
	*pitch += -ogc_look_filt[1] * wii_stick_pitchspeed.value * dt * speed + dp;
}

/*
Zona muerta radial (no por eje, para que apuntar en diagonal no se vuelva
escalonado) y curva de respuesta: el principio del recorrido es fino, el final
rapido.
*/
static void OGC_ShapeLook( float x, float y, float *ox, float *oy )
{
        const float dz = 0.16f;
        float m = sqrtf( x * x + y * y );
        float k;

        if( m < dz )
        {
                *ox = *oy = 0.0f;
                return;
        }

        m = bound( 0.0f, m, 1.0f );
        k = powf(( m - dz ) / ( 1.0f - dz ), bound( 1.0f, wii_stick_curve.value, 3.0f ));

        // x / |v| es la direccion; sqrtf( x*x+y*y ) >= dz > 0 aqui
        {
                float len = sqrtf( x * x + y * y );
                *ox = x / len * k;
                *oy = y / len * k;
        }
}

static void OGC_PollSticks( u32 wpad_type );

void OGC_ButtonsFrame( void )
{
	u32 type = WPAD_EXP_NONE;

    static qboolean tuned;

    // una sola vez, con la configuracion ya cargada: los valores guardados de
    // versiones anteriores pisaban los nuevos por defecto de suavizado y autoapuntado
    if( !tuned )
    {
        tuned = true;
        if( wii_input_tune.value < 2.0f )
        {
            Cvar_DirectSet( &wii_stick_smooth, "0.012" );
            Cvar_DirectSet( &wii_aimassist_strength, "0.7" );
            Cvar_DirectSet( &wii_aimassist_cone, "10" );
            Cvar_DirectSet( &wii_aimassist_range, "2500" );
            Cvar_DirectSet( &wii_input_tune, "2" );
        }

        if( wii_input_tune.value < 3.0f )
        {
            // el contador de FPS quedaba guardado en "3" en configs viejas
            Cvar_Set( "cl_showfps", "0" );
            Cvar_DirectSet( &wii_input_tune, "3" );
        }

        if( wii_input_tune.value < 4.0f )
        {
            // el suavizado de 0.035 s daba sensacion de retardo en Classic y GameCube
            Cvar_DirectSet( &wii_stick_smooth, "0.012" );
            Cvar_DirectSet( &wii_input_tune, "4" );
        }

        if( wii_input_tune.value < 5.0f )
        {
            // el autoapuntado era demasiado duro: menos fuerza, cono mas cerrado
            Cvar_DirectSet( &wii_aimassist_strength, "0.35" );
            Cvar_DirectSet( &wii_aimassist_cone, "6" );
            Cvar_DirectSet( &wii_aimassist_idle, "0.3" );
            Cvar_DirectSet( &wii_input_tune, "5" );
        }
    }

	// teclado y raton USB, y la vibracion, no dependen de wii_buttons
	OGC_KeyboardFrame();
	OGC_MouseFrame();
	OGC_RumbleFrame();

	if( !wii_buttons.value )
		return;

	// WPAD_ButtonsHeld reads the buffer WPAD_ScanPads fills, and SDL never
	// calls it: it takes the remote through WPAD_ReadPending and WPAD_Data
	// instead, which leaves that buffer untouched. Without this the reads
	// below always come back empty and no remote button does anything.
	//
	// PAD is the other way round - SDL does call PAD_ScanPads every frame, so
	// scanning it again here would only race it.
	WPAD_ScanPads();
OGC_PollNunchuk();

	if( wii_showinput.value )
	{
		// Print on change AND on a timer. Change-only was useless: if the
		// reads are dead the value never changes, so it spoke once during the
		// loading screen and then never again, which looks exactly like the
		// diagnostic not running at all.
		static u32 last_w = 0xdeadbeef, last_p = 0xdeadbeef;
		static int tick;
		u32 w = WPAD_ButtonsHeld( WPAD_CHAN_0 );
		u32 pd = (u32)PAD_ButtonsHeld( PAD_CHAN0 );

		if( w != last_w || pd != last_p || ( tick % 90 ) == 0 )
		{
			u32 t = 0xffffffff;
			s32 probe = WPAD_Probe( WPAD_CHAN_0, &t );

			last_w = w; last_p = pd;
			Con_Printf( "^3[INPUT]^7 wpad=%08x pad=%04x probe=%d exp=%u\n",
				(unsigned)w, (unsigned)pd, (int)probe, (unsigned)t );
		}
		tick++;
	}

	if( WPAD_Probe( WPAD_CHAN_0, &type ) == WPAD_ERR_NONE && type == WPAD_EXP_CLASSIC )
	{
		OGC_EmitButtons( ogc_map_classic, ARRAYSIZE( ogc_map_classic ),
			ogc_osk_classic, ARRAYSIZE( ogc_osk_classic ),
			WPAD_ButtonsHeld( WPAD_CHAN_0 ), &ogc_dev_classic, OGC_SRC_CLASSIC );
	}
	else
	{
		OGC_EmitButtons( ogc_map_wiimote, ARRAYSIZE( ogc_map_wiimote ),
			ogc_osk_wiimote, ARRAYSIZE( ogc_osk_wiimote ),
			WPAD_ButtonsHeld( WPAD_CHAN_0 ), &ogc_dev_wii, OGC_SRC_WII );
	}

	OGC_EmitButtons( ogc_map_gamecube, ARRAYSIZE( ogc_map_gamecube ),
		ogc_osk_gamecube, ARRAYSIZE( ogc_osk_gamecube ),
		PAD_ButtonsHeld( PAD_CHAN0 ), &ogc_dev_gc, OGC_SRC_GC );

	OGC_HomeFrame();
	OGC_OskFrame();

        // Sticks del Classic Controller y del GameCube (el del Nunchuk va aparte)
        OGC_PollSticks( type );
}

/*
============
OGC_AimIsPointer

true solo si el Wiimote es el dispositivo activo: unicamente entonces el
crosshair sigue al IR. Con Classic/GameCube/raton se queda en el centro.
============
*/
qboolean OGC_AimIsPointer( void )
{
	return ogc_src == OGC_SRC_WII;
}

/*
============
OGC_GetPointer

Pointer position as -1..1 from screen centre, for the view model.
============
*/
void OGC_GetPointer( float *x, float *y )
{
	if( ogc_src == OGC_SRC_MOUSE && refState.width > 0 && refState.height > 0 )
	{
		// cursor del raton USB, para los menus
		if( x ) *x = ogc_mouse_x / (float)refState.width * 2.0f - 1.0f;
		if( y ) *y = ogc_mouse_y / (float)refState.height * 2.0f - 1.0f;
		return;
	}

	if( x ) *x = ogc_pointer[0];
	if( y ) *y = ogc_pointer[1];
}

/*
============
OGC_WantVisiblePointer

The engine hides the cursor in game. With pointer aiming that would leave the
player with no indication of where the shot is going, since it no longer
leaves from the centre of the screen.
============
*/
qboolean OGC_WantVisiblePointer( void )
{
    if( cls.key_dest == key_menu || cls.key_dest == key_console ) return true;
    return false;
}

/*
============
OGC_GetPointerAngles

The angular offset from the centre of the screen to where the player is
pointing. Derived from the field of view rather than being a fixed number,
so a shot fired along these angles lands exactly under the pointer.
============
*/
static qboolean OGC_GetPointerAngles( float *dyaw, float *dpitch )
{
	float halffov, tanhalf, aspect;

	// con el raton USB en uso el puntero IR no apunta (el mando suele estar en la mesa)
	if( !wii_ir.value || ogc_src != OGC_SRC_WII || refState.width <= 0 || refState.height <= 0 )
		return false;

	halffov = bound( 10.0f, cl.local.scr_fov, 150.0f ) * 0.5f;
	tanhalf = tan( DEG2RAD( halffov ));
	aspect  = (float)refState.height / (float)refState.width;

	// screen x grows right and yaw decreases right; screen y grows down and
	// pitch increases down
	*dyaw   = -RAD2DEG( atan( ogc_pointer[0] * tanhalf ));
	*dpitch =  RAD2DEG( atan( ogc_pointer[1] * tanhalf * aspect ));

	return true;
}

/*
============
OGC_ApplyPointerToAim

Aims the shot where the player is pointing rather than where the camera is
looking. GoldSrc has no notion of an aim direction separate from the view:
the client puts its view angles in the usercmd and the server fires along
them. So the split is made here, after the client dll has filled the command
in - the command carries the pointer angles while cl.viewangles, which the
camera renders from, is left alone.
============
*/
/*
============
Aim assist

Looks for the enemy closest to where the player is aiming and reports how far
off it is. The sticks use it to slow down and drift towards the target, the
Wii remote pointer to bend the shot a little. Only live, visible, solid
characters count; line of sight is checked against the world with a cache so
it costs at most a few traces per second.
============
*/
#ifndef PM_WORLD_ONLY
#define PM_WORLD_ONLY 0x00000008
#endif

static qboolean OGC_AssistActive( void )
{
	return wii_aimassist.value > 0.0f && cls.state == ca_active && cls.signon == SIGNONS
		&& cls.key_dest == key_game && !cl.paused && !cls.demoplayback;
}

static float OGC_AngleDiff( float a, float b )
{
	float d = a - b;

	while( d >  180.0f ) d -= 360.0f;
	while( d < -180.0f ) d += 360.0f;
	return d;
}

// the client cannot tell friendly NPCs from enemies, so go by model name
static qboolean OGC_IsFriendlyModel( const model_t *mod )
{
	static const char *friends[] = { "scientist", "barney", "otis", "construction", "cleansuit" };
	size_t i;

	if( !mod || !mod->name[0] )
		return false;

	for( i = 0; i < ARRAYSIZE( friends ); i++ )
		if( Q_stristr( mod->name, friends[i] ))
			return true;

	return false;
}

/*
Finds the best target within the capture cone around aim_angles.
Returns the angular offset to it (dyaw, dpitch) and how far off centre it is.
*/
static qboolean OGC_FindAssistTarget( const vec3_t aim_angles, float *dyaw, float *dpitch, float *angle )
{
	static int los_ent = -1;
	static double los_time = 0.0;
	static qboolean los_ok = false;

    const float cone = bound( 0.5f, wii_aimassist_cone.value, 30.0f );
	const float range = bound( 100.0f, wii_aimassist_range.value, 8192.0f );
	cl_entity_t *local;
	vec3_t eye, fwd, best_pt, to;
    float best_eff = cone, best_ang = 0.0f;
	int best = -1, i;

	if( !clgame.entities || !clgame.pmove || clgame.pmove->numphysent <= 0 )
		return false;

	local = CL_GetEntityByIndex( cl.playernum + 1 );

	VectorAdd( cl.simorg, cl.viewheight, eye );
	AngleVectors( aim_angles, fwd, NULL, NULL );

	for( i = 1; i < clgame.maxEntities; i++ )
	{
		cl_entity_t *ent = clgame.entities + i;
		vec3_t mins, maxs, pt;
		float dist, dot;

		if( ent == local || ent->curstate.messagenum != cl.parsecount )
			continue;
		if( ent->curstate.solid != SOLID_SLIDEBOX || FBitSet( ent->curstate.effects, EF_NODRAW ))
			continue;
		if( !ent->model || ent->model->type != mod_studio )
			continue;

		// other players: only in the opposite team when teams are in use
		if( CL_IsPlayerIndex( i ))
		{
			if( local && ent->curstate.team != 0 && ent->curstate.team == local->curstate.team )
				continue;
		}
		else if( OGC_IsFriendlyModel( ent->model ))
			continue;

		if( ent->curstate.maxs[2] > ent->curstate.mins[2] )
		{
			VectorCopy( ent->curstate.mins, mins );
			VectorCopy( ent->curstate.maxs, maxs );
		}
		else
		{
			VectorCopy( ent->model->mins, mins );
			VectorCopy( ent->model->maxs, maxs );
		}

		if( maxs[2] <= mins[2] )
			continue;

		// cadaveres: el juego les deja una caja de ~1 unidad de alto al terminar de
		// morir; durante la animacion de muerte se detecta por la salud (solo si el
		// servidor la envia: antes se vio con salud > 0 la misma entidad)
		if( maxs[2] - mins[2] < 8.0f )
			continue;

		{
			static const model_t *alive_model[2048];

			if( i < (int)ARRAYSIZE( alive_model ))
			{
				if( ent->curstate.health > 0 )
					alive_model[i] = ent->model;
				else if( alive_model[i] == ent->model )
					continue;
			}
		}

		// chest height rather than the middle of the box
		pt[0] = ent->origin[0] + ( mins[0] + maxs[0] ) * 0.5f;
		pt[1] = ent->origin[1] + ( mins[1] + maxs[1] ) * 0.5f;
		pt[2] = ent->origin[2] + ( mins[2] + maxs[2] ) * 0.5f + ( maxs[2] - mins[2] ) * 0.2f;

		VectorSubtract( pt, eye, to );
		dist = VectorLength( to );
		if( dist < 24.0f || dist > range )
			continue;

            dot = DotProduct( fwd, to ) / dist;
            if( dot <= 0.0f )
                continue;

            {
                // el enemigo cuenta con su tamano: si la mira esta sobre su cuerpo
                // la distancia angular es cero, de cerca o de lejos
                float hw = ( ( maxs[0] - mins[0] ) + ( maxs[1] - mins[1] )) * 0.25f;
                float a = RAD2DEG( acosf( bound( -1.0f, dot, 1.0f )));
                float eff = Q_max( 0.0f, a - RAD2DEG( atan2f( hw, dist )));

                if( eff > best_eff || ( eff == best_eff && best >= 0 && a >= best_ang ))
                    continue;

                best_eff = eff;
                best_ang = a;
            }

            best = i;
            VectorCopy( pt, best_pt );
	}

	if( best < 0 )
	{
		los_ent = -1;
		return false;
	}

	if( best != los_ent || host.realtime - los_time > 0.1 )
	{
		pmtrace_t tr = CL_TraceLine( eye, best_pt, PM_WORLD_ONLY );

		los_ok = !tr.allsolid && tr.fraction >= 0.99f;
		los_ent = best;
		los_time = host.realtime;
	}

	if( !los_ok )
		return false;

	{
		float len, yaw, pitch;

		VectorSubtract( best_pt, eye, to );
		len = VectorLength( to );
		yaw = RAD2DEG( atan2f( to[1], to[0] ));
		pitch = -RAD2DEG( asinf( bound( -1.0f, to[2] / len, 1.0f )));

		*dyaw = OGC_AngleDiff( yaw, aim_angles[YAW] );
		*dpitch = pitch - aim_angles[PITCH];
        *angle = best_eff;
	}

	return true;
}

/*
Acercamiento suave hacia el objetivo: cada cuadro cubre una fraccion del error
(aproximacion exponencial), asi corrige poco si ya casi apunta y mucho si esta
lejos del centro, sin saltos. offset = distancia angular al objetivo.
*/
static float OGC_AssistBlend( float offset, float dt )
{
    float str = bound( 0.0f, wii_aimassist_strength.value, 1.0f );
    float f = bound( 0.0f, 1.0f - offset / bound( 0.5f, wii_aimassist_cone.value, 30.0f ), 1.0f );

    return bound( 0.0f, str * f * 8.0f * dt, 0.25f );
}

static float ogc_aim_delta[2];

/*
============
OGC_GetAimDelta

Offset (yaw, pitch) between the camera and the angles sent in the last usercmd.
Client-predicted events (tau cannon, gluon...) are built by the client dll from
the camera angles, so without this they draw the beam along the camera while the
server traces along the pointer.
============
*/
void OGC_GetAimDelta( float *dyaw, float *dpitch )
{
	*dyaw = ogc_aim_delta[0];
	*dpitch = ogc_aim_delta[1];
}

/*
============
OGC_RestoreAim

The client dll reads the view angles through pfnGetViewAngles, which on the
Wiimote returns camera + aim offset (so its weapon prediction, egon beam, etc.
follow the pointer like the server does). The usercmd it builds therefore
carries the offset too: take it back out so the camera angles are clean again.
============
*/
void OGC_RestoreAim( vec3_t viewangles )
{
	viewangles[YAW]   -= ogc_aim_delta[0];
	viewangles[PITCH] -= ogc_aim_delta[1];
	// la camara nunca pasa de +-89: sin esto el DLL (suma camara+desvio) la dejaba
	// acumular y se podia girar hacia arriba/abajo sin limite
	viewangles[PITCH] = bound( -89.0f, viewangles[PITCH], 89.0f );
	VectorCopy( viewangles, cl.viewangles );
	ogc_aim_delta[0] = ogc_aim_delta[1] = 0.0f;
}

void OGC_ApplyPointerToAim( vec3_t viewangles, float *forwardmove, float *sidemove )
{
	float dyaw, dpitch;
	float xt, yt, halffov, aspect;
	vec3_t fwd, right, up, dir, cam;

#ifdef XASH_OGC_AIMPROOF
	// force a fixed pointer offset so the two ends can be compared
	ogc_pointer[0] = 0.5f; ogc_pointer[1] = 0.0f;
#endif
    // el cliente leyo camara + desvio anterior (pfnGetViewAngles): quitarlo antes de aplicar el nuevo
    OGC_RestoreAim( viewangles );
	if( !OGC_GetPointerAngles( &dyaw, &dpitch ))
		return;

	// Sumar dyaw/dpitch a los angulos de la camara solo es correcto con la camara
	// a nivel: un desplazamiento horizontal en pantalla gira alrededor del eje
	// "arriba de la camara", no del eje vertical del mundo. Con la camara
	// inclinada (mirando arriba/abajo) el disparo quedaba mucho mas cerca del
	// centro que el reticulo. Se calcula la direccion real (misma proyeccion que
	// el reticulo) y de ahi los angulos del comando.
	halffov = bound( 10.0f, cl.local.scr_fov, 150.0f ) * 0.5f;
	xt = ogc_pointer[0] * tan( DEG2RAD( halffov ));
	aspect = (float)refState.height / (float)refState.width;
	yt = ogc_pointer[1] * tan( DEG2RAD( halffov )) * aspect;

	VectorCopy( viewangles, cam );
	AngleVectors( cam, fwd, right, up );
	VectorMA( fwd, xt, right, dir );
	VectorMA( dir, -yt, up, dir );
	VectorNormalize( dir );

	{
		float flat = sqrt( dir[0] * dir[0] + dir[1] * dir[1] );
		float newyaw = RAD2DEG( atan2( dir[1], dir[0] ));
		float newpitch = -RAD2DEG( atan2( dir[2], flat ));

		dyaw = newyaw - cam[YAW];
		while( dyaw > 180.0f ) dyaw -= 360.0f;
		while( dyaw < -180.0f ) dyaw += 360.0f;
		dpitch = newpitch - cam[PITCH];
		// near the poles yaw is ill-defined; keep the offset sane
		if( flat < 0.02f ) dyaw = 0.0f;
	}

#ifdef XASH_OGC_AIMPROOF
	{
		static int n;
		if(( n % 4 ) == 0 && n < 160 )
			Con_Printf( "[AIMPROOF] cl_n=%d camera yaw=%.3f  sent yaw=%.3f\n",
				n, cl.viewangles[YAW], viewangles[YAW] + dyaw );
		n++;
	}
#endif
	{
		float oldpitch = viewangles[PITCH];

		viewangles[YAW]   += dyaw;
		viewangles[PITCH] += dpitch;
		viewangles[PITCH] = bound( -89.0f, viewangles[PITCH], 89.0f );
		ogc_aim_delta[0] = dyaw;
		ogc_aim_delta[1] = viewangles[PITCH] - oldpitch;
	}

	// Movement is derived from these same angles, so turning the aim would
	// also turn "forward" - walk while pointing at the edge of the screen and
	// you would drift sideways. Counter-rotate the move vector by the same
	// angle so it stays relative to the camera.
	if( forwardmove && sidemove )
	{
		float rad = DEG2RAD( dyaw );
		float c = cos( rad ), sn = sin( rad );
		float fm = *forwardmove, sm = *sidemove;

		*forwardmove = fm * c - sm * sn;
		*sidemove    = fm * sn + sm * c;
	}
}

/*
============
OGC_ApplyPointerToViewModel

Leans the weapon towards where the player is pointing. Purely cosmetic - the
shot still leaves along the view axis - but without it the gun sits dead
centre while the pointer moves independently, which looks wrong.
============
*/
void OGC_ApplyPointerToViewModel( cl_entity_t *view )
{
    float dyaw = 0.0f, dpitch = 0.0f, droll = 0.0f, tpitch = 0.0f;

    if( !view )
        return;

    // Calcula el offset angular real (mismo que el disparo), asi el arma
    // apunta EXACTAMENTE al crosshair, no solo se inclina un poco.
    if( !OGC_GetPointerAngles( &dyaw, &dpitch ))
        dyaw = dpitch = 0.0f;

    // Giro del propio mando (acelerometro): al girar el Wiimote el arma gira igual
    OGC_GetRemoteTilt( &droll, &tpitch );

    if( dyaw == 0.0f && dpitch == 0.0f && droll == 0.0f && tpitch == 0.0f )
        return;

    // Blindaje NaN/Inf
    if( isnan( dyaw ) || isinf( dyaw ) ) dyaw = 0.0f;
    if( isnan( dpitch ) || isinf( dpitch ) ) dpitch = 0.0f;

    // Aplica el offset angular al arma
    view->angles[YAW]   += dyaw;
    view->angles[PITCH] -= dpitch;
    view->angles[PITCH] += tpitch;
    view->angles[ROLL]  += droll;
    view->angles[ROLL]   = bound( -90.0f, view->angles[ROLL], 90.0f );

    // Clamp duro para Wii
    if( view->angles[PITCH] > 89.0f ) view->angles[PITCH] = 89.0f;
    if( view->angles[PITCH] < -89.0f ) view->angles[PITCH] = -89.0f;
    while( view->angles[YAW] > 180.0f ) view->angles[YAW] -= 360.0f;
    while( view->angles[YAW] < -180.0f ) view->angles[YAW] += 360.0f;

    VectorCopy( view->angles, view->curstate.angles );
    VectorCopy( view->angles, view->latched.prevangles );
}

/*
============
OGC_PointerMove

Adds the pointer's contribution to the view angles.
============
*/
void OGC_PointerMove( float *pitch, float *yaw )
{
    WPADData *data;
    u32 type = WPAD_EXP_NONE;
    float nx, ny, deadzone, scale;

    // Tolerancia de perdida de IR: no congelar por micro-parpadeos
    static int ir_lost_frames = 0;

    #define IR_TOLERANCE_FRAMES 5

    // con el raton USB en uso el puntero IR no apunta (el mando suele estar en la mesa)
	if( !wii_ir.value || ogc_src != OGC_SRC_WII || refState.width <= 0 || refState.height <= 0 )
        return;

    if( WPAD_Probe( WPAD_CHAN_0, &type ) != WPAD_ERR_NONE )
        return;

    // Classic Controller: no usar su estado como puntero IR.
    if( type == WPAD_EXP_CLASSIC )
        return;

    data = WPAD_Data( WPAD_CHAN_0 );

    // Si se pierde el IR, contar frames antes de congelar
    if( !data || !data->ir.valid )
    {
        ir_lost_frames++;
        if( wii_showinput.value && ( ir_lost_frames % 30 ) == 1 )
            Con_Printf( "^1[IR]^7 PERDIDO frame=%d\n", ir_lost_frames );
        if( ir_lost_frames > IR_TOLERANCE_FRAMES )
            return;
        // Dentro de tolerancia: no actualizar pero no congelar
        return;
    }

    ir_lost_frames = 0;

    // Log de datos crudos del IR (cada ~1 segundo)
    if( wii_showinput.value )
    {
        static int log_tick = 0;
        if( ( log_tick % 60 ) == 0 )
            Con_Printf( "[IR] valid=%d x=%d y=%d sx=%d sy=%d", 
                (int)data->ir.valid, (int)data->ir.x, (int)data->ir.y, 
                (int)data->ir.sx, (int)data->ir.sy );
        log_tick++;
    }


    nx = ( data->ir.x / (float)refState.width ) * 2.0f - 1.0f;
    ny = ( data->ir.y / (float)refState.height ) * 2.0f - 1.0f;

    nx = bound( -1.0f, nx, 1.0f );
    ny = bound( -1.0f, ny, 1.0f );

    ogc_pointer[0] = nx;
    ogc_pointer[1] = ny;

    // ayuda de punteria con el puntero: gira la CAMARA hacia el enemigo (el disparo
    // sigue saliendo exactamente donde apunta el puntero)
    if( wii_aimassist_ir.value && OGC_AssistActive( ))
    {
        vec3_t aim = { cl.viewangles[PITCH] + ogc_aim_delta[1], cl.viewangles[YAW] + ogc_aim_delta[0], 0.0f };
        float ay, ap, ang, dt = bound( 0.0f, (float)host.realframetime, 0.1f );

        if( OGC_FindAssistTarget( aim, &ay, &ap, &ang ))
        {
            float blend = OGC_AssistBlend( ang, dt );

            *yaw   += ay * blend;
            *pitch += ap * blend;
        }
    }

    deadzone = bound( 0.0f, wii_ir_deadzone.value, 0.95f );
    scale = 1.0f - deadzone;

    if( scale <= 0.0f )
        return;

    // Blindaje NaN/Inf antes de aplicar
    if( isnan( nx ) || isinf( nx ) ) nx = 0.0f;
    if( isnan( ny ) || isinf( ny ) ) ny = 0.0f;

    if( fabs( nx ) > deadzone )
    {
        float over = ( fabs( nx ) - deadzone ) / scale;
        if( nx < 0.0f ) over = -over;

        *yaw -= over * wii_ir_yawspeed.value * (float)host.realframetime;

        if( isnan( *yaw ) || isinf( *yaw ) ) *yaw = 0.0f;
    }

    if( fabs( ny ) > deadzone )
    {
        float over = ( fabs( ny ) - deadzone ) / scale;
        if( ny < 0.0f ) over = -over;

        *pitch += over * wii_ir_pitchspeed.value * (float)host.realframetime;

        if( isnan( *pitch ) || isinf( *pitch ) ) *pitch = 0.0f;
    }
}

/*
============
OGC_GetRemoteTilt

How far the player has twisted the Wii remote, for the weapon. The roll comes
from the accelerometer (gravity), so it is only meaningful while the remote is
held still-ish and not pointing straight up or down; it is filtered so the gun
does not shake with every wrist tremor. The pitch is only used when the IR
sensor has lost the bar - otherwise the pointer already leans the weapon.
============
*/
static void OGC_GetRemoteTilt( float *roll, float *pitch )
{
	static float f_roll, f_pitch;
	float t_roll = 0.0f, t_pitch = 0.0f, k;
	u32 type = WPAD_EXP_NONE;

	if( wii_gun_tilt.value != 0.0f && ogc_src == OGC_SRC_WII
		&& WPAD_Probe( WPAD_CHAN_0, &type ) == WPAD_ERR_NONE && type != WPAD_EXP_CLASSIC )
	{
		WPADData *data = WPAD_Data( WPAD_CHAN_0 );

		if( data )
		{
			float r = data->orient.roll, p = data->orient.pitch;
			float lim = bound( 5.0f, wii_gun_tilt_max.value, 90.0f );

			if( isnan( r ) || isinf( r )) r = 0.0f;
			if( isnan( p ) || isinf( p )) p = 0.0f;

			// zona muerta de 3 grados, el resto se reescala
			if( fabsf( r ) < 3.0f ) r = 0.0f; else r -= r > 0.0f ? 3.0f : -3.0f;
			t_roll = bound( -lim, r * wii_gun_tilt.value, lim );

			if( !data->ir.valid && wii_gun_pitch.value != 0.0f )
			{
				if( fabsf( p ) < 3.0f ) p = 0.0f; else p -= p > 0.0f ? 3.0f : -3.0f;
				t_pitch = bound( -lim, p * wii_gun_pitch.value, lim );
			}

			if( wii_showinput.value )
			{
				static int tick;
				if(( tick++ % 60 ) == 0 )
					Con_Printf( "^3[TILT]^7 roll=%.1f pitch=%.1f ir=%d\n",
					data->orient.roll, data->orient.pitch, (int)data->ir.valid );
			}
		}
	}

	// filtro exponencial (~70 ms): sigue al mando sin temblar
	k = 1.0f - expf( -(float)host.realframetime * 14.0f );
	k = bound( 0.0f, k, 1.0f );
	f_roll  += ( t_roll  - f_roll  ) * k;
	f_pitch += ( t_pitch - f_pitch ) * k;

	if( fabsf( f_roll )  < 0.05f ) f_roll  = 0.0f;
	if( fabsf( f_pitch ) < 0.05f ) f_pitch = 0.0f;

	*roll  = f_roll;
	*pitch = f_pitch;
}

/*
============
Rumble

The motors only know on and off, so the strength is the share of time they
spend on: a cycle of six frames, on for as many of them as the strength asks.

The remote is the one that shakes, also when a Classic Controller is plugged
into it. With a GameCube pad in use the pad's own motor is driven instead, and
with a USB mouse nothing moves at all.
============
*/
static double ogc_rumble_end;
static float ogc_rumble_power;
static qboolean ogc_rumble_wii_on;
static qboolean ogc_rumble_gc_on;

void OGC_Rumble( float power, float seconds )
{
	double now = host.realtime;

	power = bound( 0.0f, power * wii_rumble.value, 1.0f );
	if( power <= 0.0f || seconds <= 0.0f )
		return;

	if( now < ogc_rumble_end )
	{
		// ya vibrando: se queda lo mas fuerte y lo mas largo
		if( power > ogc_rumble_power )
			ogc_rumble_power = power;
		if( now + seconds > ogc_rumble_end )
			ogc_rumble_end = now + seconds;
	}
	else
	{
		ogc_rumble_power = power;
		ogc_rumble_end = now + seconds;
	}
}

static void OGC_RumbleFrame( void )
{
	static unsigned tick;
	static int prev_health;
	qboolean on = false, want_wii, want_gc;

	// dano recibido: la vida baja mientras se juega
	if( cls.state == ca_active && !cl.paused )
	{
		int hp = cl.local.health;

		if( prev_health > 0 && hp < prev_health )
		{
			int lost = bound( 1, prev_health - hp, 50 );

			if( hp <= 0 )
				OGC_Rumble( 1.0f, 0.8f );
			else
				OGC_Rumble( 0.55f + lost * 0.009f, 0.15f + lost * 0.008f );
		}
		prev_health = hp;
	}
	else prev_health = 0;

	if( wii_rumble.value > 0.0f && host.realtime < ogc_rumble_end )
	{
		int duty = (int)( ogc_rumble_power * 6.0f + 0.5f );
		on = ( tick++ % 6 ) < (unsigned)duty;
	}
	else tick = 0;

	want_wii = on && ( ogc_src == OGC_SRC_WII || ogc_src == OGC_SRC_CLASSIC );
	want_gc  = on && ogc_src == OGC_SRC_GC;

	if( want_wii != ogc_rumble_wii_on )
	{
		WPAD_Rumble( WPAD_CHAN_0, want_wii ? 1 : 0 );
		ogc_rumble_wii_on = want_wii;
	}

	if( want_gc != ogc_rumble_gc_on )
	{
		PAD_ControlMotor( PAD_CHAN0, want_gc ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP_HARD );
		ogc_rumble_gc_on = want_gc;
	}
}

/*
============
USB keyboard

libogc reports USB HID usage codes, which are laid out in the same order as
the keys on the board, so a small switch gets from there to the engine's key
numbers. Characters for the console, chat and menu take the symbol libogc has
already shifted, the same split the SDL build makes between key events and
text input.

The keyboard sends one event per press and one per release; holding a key
down does not repeat, which would make the console unusable (backspace!), so
the repeat is done here.
============
*/
static int OGC_HIDToKey( int u )
{
	if( u >= 4 && u <= 29 )   return 'a' + ( u - 4 );
	if( u >= 30 && u <= 38 )  return '1' + ( u - 30 );
	if( u == 39 )             return '0';
	if( u >= 58 && u <= 69 )  return K_F1 + ( u - 58 );

	switch( u )
	{
	case 40:  return K_ENTER;
	case 41:  return K_ESCAPE;
	case 42:  return K_BACKSPACE;
	case 43:  return K_TAB;
	case 44:  return K_SPACE;
	case 45:  return '-';
	case 46:  return '=';
	case 47:  return '[';
	case 48:  return ']';
	case 49:  return '\\';
	case 51:  return ';';
	case 52:  return '\'';
	case 53:  return '`';
	case 54:  return ',';
	case 55:  return '.';
	case 56:  return '/';
	case 57:  return K_CAPSLOCK;
	case 71:  return K_SCROLLLOCK;
	case 72:  return K_PAUSE;
	case 73:  return K_INS;
	case 74:  return K_HOME;
	case 75:  return K_PGUP;
	case 76:  return K_DEL;
	case 77:  return K_END;
	case 78:  return K_PGDN;
	case 79:  return K_RIGHTARROW;
	case 80:  return K_LEFTARROW;
	case 81:  return K_DOWNARROW;
	case 82:  return K_UPARROW;
	case 83:  return K_KP_NUMLOCK;
	case 84:  return K_KP_SLASH;
	case 85:  return K_KP_MUL;
	case 86:  return K_KP_MINUS;
	case 87:  return K_KP_PLUS;
	case 88:  return K_KP_ENTER;
	case 89:  return K_KP_END;
	case 90:  return K_KP_DOWNARROW;
	case 91:  return K_KP_PGDN;
	case 92:  return K_KP_LEFTARROW;
	case 93:  return K_KP_5;
	case 94:  return K_KP_RIGHTARROW;
	case 95:  return K_KP_HOME;
	case 96:  return K_KP_UPARROW;
	case 97:  return K_KP_PGUP;
	case 98:  return K_KP_INS;
	case 99:  return K_KP_DEL;
	case 224: case 228: return K_CTRL;
	case 225: case 229: return K_SHIFT;
	case 226: case 230: return K_ALT;
	case 227: case 231: return K_WIN;
	}

	return 0;
}

// only text and editing keys repeat; repeating ESC or ENTER would reopen the
// menu or fire the same console line over and over
static qboolean OGC_RepeatableKey( int key )
{
	switch( key )
	{
	case K_BACKSPACE: case K_DEL:
	case K_UPARROW: case K_DOWNARROW: case K_LEFTARROW: case K_RIGHTARROW:
	case K_PGUP: case K_PGDN:
		return true;
	}
	return false;
}

#define OGC_REPEAT_DELAY 0.40
#define OGC_REPEAT_RATE  0.04

static struct
{
	int keycode;	// 0 = nothing held
	int key;
	int ch;		// character to repeat, 0 if none
	double next;
} ogc_repeat;

static void OGC_KeyboardFrame( void )
{
	keyboard_event ev;
	qboolean look;
	int guard = 0;

	if( !ogc_usb_kbd_ok )
		return;

	while( guard++ < 64 && KEYBOARD_GetEvent( &ev ) > 0 )
	{
		qboolean down;
		qboolean ctrl;
		int key, ch = 0;

		if( ev.type == KEYBOARD_CONNECTED || ev.type == KEYBOARD_DISCONNECTED )
		{
			Con_Printf( ev.type == KEYBOARD_CONNECTED ? "^2[KBD]^7 teclado USB detectado\n" : "^1[KBD]^7 teclado USB desconectado\n" );
			continue;
		}

		if( ev.type != KEYBOARD_PRESSED && ev.type != KEYBOARD_RELEASED )
			continue;

		down = ev.type == KEYBOARD_PRESSED;
		if( down )
			OSK_SetSuspended( true );	// con teclado fisico no hace falta el de pantalla
		ctrl = MOD_ONESET( ev.modifiers, MOD_ANYCONTROL );
		key = OGC_HIDToKey( ev.keycode );

		if( !down )
		{
			if( ogc_repeat.keycode == ev.keycode )
				ogc_repeat.keycode = 0;
		}
		else if( cls.key_dest != key_game )
		{
			// texto escrito: consola, chat y menus. Ctrl+A..Z manda 1..26
			// (copiar, pegar...), como hace la version de SDL.
			if( ctrl )
			{
				if( ev.keycode >= 4 && ev.keycode <= 29 )
				{
					CL_CharEvent( ev.keycode - 4 + 1 );
					continue;
				}
			}
			else if( ev.symbol >= 32 && ev.symbol < 127 )
			{
				ch = ev.symbol;
				CL_CharEvent( ch );
			}
		}

		if( key )
		{
			// las letras y numeros escritos ya llegaron como texto
			if( down && cls.key_dest != key_game && ev.keycode >= 4 && ev.keycode <= 39 )
			{
				// nada
			}
			else OGC_SendKey( key, down );
		}

		if( down && cls.key_dest != key_game && !ctrl && ( ch || OGC_RepeatableKey( key )))
		{
			ogc_repeat.keycode = ev.keycode;
			ogc_repeat.key = key;
			ogc_repeat.ch = ch;
			ogc_repeat.next = host.realtime + OGC_REPEAT_DELAY;
		}
	}

	// repeticion: solo fuera de la partida (consola, chat, menus)
	if( ogc_repeat.keycode )
	{
		if( cls.key_dest == key_game )
			ogc_repeat.keycode = 0;
		else if( host.realtime >= ogc_repeat.next )
		{
			ogc_repeat.next = host.realtime + OGC_REPEAT_RATE;

			if( ogc_repeat.ch )
				CL_CharEvent( ogc_repeat.ch );
			else if( ogc_repeat.key )
				OGC_SendKey( ogc_repeat.key, true );
		}
	}
}

/*
============
USB mouse

Motion is collected here and handed to the view code by OGC_MouseMove, the
same way the pointer does it; buttons and the wheel go through the engine's
normal mouse entry points so the bindings (MOUSE1 attack and so on) and the
menus behave as on a PC. The menus get a virtual cursor, since there is no
absolute position to read from a mouse.
============
*/
static void OGC_MouseFrame( void )
{
	static u8 held;
	static qboolean placed;
	static int was_connected, retries, total_events;
	static double next_retry;
	qboolean look;
	mouse_event ev;
	int guard = 0;

	if( !ogc_usb_mouse_ok )
	{
		// si el USB no estaba listo al arrancar, se reintenta unas veces
		if( retries < 5 && host.realtime >= next_retry )
		{
			next_retry = host.realtime + 3.0;
			retries++;
			ogc_usb_mouse_ok = ( MOUSE_Init() == 0 );
			if( ogc_usb_mouse_ok )
				Con_Printf( "^2[MOUSE]^7 USB iniciado (intento %d)\n", retries );
		}

		if( !ogc_usb_mouse_ok )
			return;
	}

	// avisa cuando libogc encuentra o pierde el raton (necesita un raton HID
	// estandar, "boot protocol"; algunos raton gaming o receptores no lo son)
	if(( MOUSE_IsConnected() ? 1 : 0 ) != was_connected )
	{
		was_connected = MOUSE_IsConnected() ? 1 : 0;
		Con_Printf( was_connected ? "^2[MOUSE]^7 raton USB detectado\n" : "^1[MOUSE]^7 raton USB desconectado\n" );
	}

	// el cursor del menu arranca en el centro de la pantalla
	if( !placed && refState.width > 0 && refState.height > 0 )
	{
		ogc_mouse_x = refState.width * 0.5f;
		ogc_mouse_y = refState.height * 0.5f;
		placed = true;
	}

	// el movimiento solo cuenta para la mirada durante la partida: en menus,
	// pausa, pantalla de carga o con un cuadro muy largo se descarta, o al
	// volver la camara daria un giro de golpe con todo lo acumulado
	look = ( cls.state == ca_active && cls.key_dest == key_game && !cl.paused
		&& !cl.intermission && host.frametime < 0.5 );

	// se vacia toda la cola (libogc guarda un evento por cada informe USB):
	// si se queda atras el raton sigue moviendose un rato despues de pararlo
	while( guard++ < 4096 && MOUSE_GetEvent( &ev ) > 0 )
	{
		u8 changed = ev.button ^ held;
		int i;

		total_events++;

		if( ev.rx || ev.ry || ev.rz || changed )
			OGC_SetSource( OGC_SRC_MOUSE );

		if( look )
		{
			ogc_mouse_dx += ev.rx;
			ogc_mouse_dy += ev.ry;
		}

		if( refState.width > 0 && refState.height > 0 )
		{
			float cs = wii_mouse_menu_sens.value;

			if( cs <= 0.0f )
				cs = 1.0f;

			ogc_mouse_x = bound( 0.0f, ogc_mouse_x + ev.rx * cs, (float)( refState.width - 1 ));
			ogc_mouse_y = bound( 0.0f, ogc_mouse_y + ev.ry * cs, (float)( refState.height - 1 ));
		}

		for( i = 0; i < 5; i++ )
		{
			if( changed & ( 1 << i ))
				IN_MouseEvent( i, ( ev.button & ( 1 << i )) != 0 );
		}
		held = ev.button;

		if( ev.rz )
			IN_MWheelEvent( ev.rz > 0 ? 1 : -1 );
	}

	if( !look )
		ogc_mouse_dx = ogc_mouse_dy = 0.0f;

	if( wii_showinput.value )
	{
		static int tick;
		if(( tick++ % 120 ) == 0 )
			Con_Printf( "^3[MOUSE]^7 detectado=%d eventos=%d botones=%02x cursor=%d,%d\n",
				(int)was_connected, total_events, (unsigned)held, (int)ogc_mouse_x, (int)ogc_mouse_y );
	}
}

/*
============
OGC_MouseMove

Adds the USB mouse's motion since the last call to the view angles.
============
*/
void OGC_MouseMove( float *pitch, float *yaw )
{
	float s;

   // palanca derecha (Classic / GameCube): se aplica aqui para no depender de input.c
   OGC_StickLookMove( pitch, yaw );

	if( ogc_mouse_dx == 0.0f && ogc_mouse_dy == 0.0f )
		return;

	// fuera de la partida el raton solo mueve el cursor del menu
	if( cls.key_dest != key_game )
	{
		ogc_mouse_dx = ogc_mouse_dy = 0.0f;
		return;
	}

	s = wii_mouse_sens.value;

	// con zoom (francotirador, ballesta) el campo de vision es menor y la misma
	// distancia del raton movia demasiado la mira
	if( wii_mouse_zoom.value && cl.local.scr_fov > 1.0f && cl.local.scr_fov < 90.0f )
		s *= cl.local.scr_fov / 90.0f;

	*yaw   -= ogc_mouse_dx * Cvar_VariableValue( "m_yaw" ) * s;
	*pitch += ogc_mouse_dy * Cvar_VariableValue( "m_pitch" ) * s * ( wii_mouse_invert.value ? -1.0f : 1.0f );

	ogc_mouse_dx = ogc_mouse_dy = 0.0f;
}

/*
============
OGC_StickToAxis

Convierte una palanca (-1..1) al rango de eje del motor, con zona muerta
propia; lo que queda fuera se reescala a todo el rango.
- movimiento: el motor ya aplica su zona muerta
- mirada: el motor anula todo lo menor a 16384, asi que se reescala a 16500..30000
============
*/
static short OGC_StickToAxis( float n, qboolean look )
{
        const float dz = 0.18f;
        float a = fabsf( n );
        float out;

        if( a < dz )
                return 0;
        if( a > 1.0f )
                a = 1.0f;

        a = ( a - dz ) / ( 1.0f - dz );

        if( look )
                out = 16500.0f + a * ( 30000.0f - 16500.0f );
        else
                out = a * 32767.0f;

        return (short)( n < 0.0f ? -out : out );
}

// posicion de una palanca de WPAD como -1..1 (x derecha, y arriba)
static float OGC_WpadAxis( int pos, int center, int min, int max )
{
        float range = ( pos >= center ) ? (float)( max - center ) : (float)( center - min );

        if( range < 1.0f )
                return 0.0f;

        return bound( -1.0f, (float)( pos - center ) / range, 1.0f );
}

static void OGC_PollSticks( u32 wpad_type )
{
        static short last[4] = { 0, 0, 0, 0 };  // ultimo valor enviado
        static const engineAxis_t axes[4] = { JOY_AXIS_SIDE, JOY_AXIS_FWD, JOY_AXIS_YAW, JOY_AXIS_PITCH };
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
        short out[4];
        qboolean gc_moved = false, moved = false;
        int i;

        if( wpad_type == WPAD_EXP_CLASSIC )
        {
                WPADData *data = WPAD_Data( WPAD_CHAN_0 );

                if( data )
                {
                        const joystick_t *l = &data->exp.classic.ljs;
                        const joystick_t *r = &data->exp.classic.rjs;

                        lx = OGC_WpadAxis( l->pos.x, l->center.x, l->min.x, l->max.x );
                        ly = OGC_WpadAxis( l->pos.y, l->center.y, l->min.y, l->max.y );
                        rx = OGC_WpadAxis( r->pos.x, r->center.x, r->min.x, r->max.x );
                        ry = OGC_WpadAxis( r->pos.y, r->center.y, r->min.y, r->max.y );
                }
        }

        // GameCube: si las palancas del mando de GC se mueven, mandan ellas
        {
                float gx = bound( -1.0f, PAD_StickX( PAD_CHAN0 ) / 80.0f, 1.0f );
                float gy = bound( -1.0f, PAD_StickY( PAD_CHAN0 ) / 80.0f, 1.0f );
                float cx = bound( -1.0f, PAD_SubStickX( PAD_CHAN0 ) / 80.0f, 1.0f );
                float cy = bound( -1.0f, PAD_SubStickY( PAD_CHAN0 ) / 80.0f, 1.0f );

                if( fabsf( gx ) > 0.18f || fabsf( gy ) > 0.18f ) { lx = gx; ly = gy; gc_moved = true; }
                if( fabsf( cx ) > 0.18f || fabsf( cy ) > 0.18f ) { rx = cx; ry = cy; gc_moved = true; }
        }

        // convencion de SDL que usa el motor: arriba es negativo en Y
        out[0] = OGC_StickToAxis( lx, false );
        out[1] = OGC_StickToAxis( -ly, false );
        // la mirada no pasa por el motor: se suaviza en OGC_StickLookMove
        OGC_ShapeLook( rx, ry, &ogc_look_raw[0], &ogc_look_raw[1] );
        out[2] = ( ogc_look_raw[0] != 0.0f ) ? 1 : 0;
        out[3] = ( ogc_look_raw[1] != 0.0f ) ? 1 : 0;

        // mover una palanca tambien pasa el control a ese mando
        for( i = 0; i < 4; i++ )
                if( out[i] ) moved = true;

        if( moved )
        {
                if( gc_moved )
                        OGC_SetSource( OGC_SRC_GC );
                else if( wpad_type == WPAD_EXP_CLASSIC )
                        OGC_SetSource( OGC_SRC_CLASSIC );
        }

        for( i = 0; i < 4; i++ )
        {
                // los ejes de mirada se quedan en 0 para que el motor no los aplique otra vez
                out[2] = out[3] = 0;

                if( out[i] != last[i] )
                {
                        Joy_AxisMotionEvent( axes[i], out[i] );
                        last[i] = out[i];
                }
        }
}

static void OGC_PollNunchuk(void)
{
    WPADData *data;
    u32 type;
    s16 nx, ny;
    static s16 last_x = 0, last_y = 0;

    if (!wii_buttons.value)
        return;

    if (WPAD_Probe(WPAD_CHAN_0, &type) != WPAD_ERR_NONE)
        return;

    if (type != WPAD_EXP_NUNCHUK)
        return;

    data = WPAD_Data(WPAD_CHAN_0);
    if (!data)
        return;

    {
        int tmp;
        tmp = (data->exp.nunchuk.js.pos.x - data->exp.nunchuk.js.center.x) * (int)wii_nunchuk_sens.value;
        if (tmp >  32767) tmp =  32767;
        if (tmp < -32768) tmp = -32768;
        nx = (s16)tmp;

        tmp = -((data->exp.nunchuk.js.pos.y - data->exp.nunchuk.js.center.y) * (int)wii_nunchuk_sens.value);
        if (tmp >  32767) tmp =  32767;
        if (tmp < -32768) tmp = -32768;
        ny = (s16)tmp;
    }

    if (nx > -8000 && nx < 8000) nx = 0;
    if (ny > -8000 && ny < 8000) ny = 0;
    // ny = -ny; (movido arriba para evitar overflow s16)

    if (nx != last_x)
    {
        Joy_AxisMotionEvent(JOY_AXIS_SIDE, nx);
        last_x = nx;
    }
    if (ny != last_y)
    {
        Joy_AxisMotionEvent(JOY_AXIS_FWD, ny);
        last_y = ny;
    }

    if (wii_showinput.value)
    {
        static int tick = 0;
        if ((tick % 30) == 0)
        {
            Con_Printf("[NUNCHUK] nx=%d ny=%d",
                (int)nx, (int)ny);
        }
        tick++;
    }
}

#endif // XASH_OGC
