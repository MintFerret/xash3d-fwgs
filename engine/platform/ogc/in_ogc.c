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
Three layouts, one set of engine keys. The keys are picked so the stock
bindings already land on the right actions, which keeps every scheme visible
and rebindable from Options -> Controls instead of hidden in a config file.
*/
static const ogc_btn_t ogc_map_wiimote[] =
{
    { WPAD_BUTTON_A,         K_A_BUTTON    }, // +jump
    { WPAD_BUTTON_B,         K_L1_BUTTON   }, // +duck
    { WPAD_NUNCHUK_BUTTON_C, K_B_BUTTON    }, // +use
    { WPAD_NUNCHUK_BUTTON_Z, K_R1_BUTTON   }, // +attack

    { WPAD_BUTTON_1,         K_TAB         }, // Tab
    { WPAD_BUTTON_2,         K_F6          }, // savequick

    { WPAD_BUTTON_PLUS,      K_ESCAPE      }, // menu (esc)
    { WPAD_BUTTON_MINUS,     K_X_BUTTON    }, // +reload

    { WPAD_BUTTON_UP,        K_Y_BUTTON    }, // flashlight
    { WPAD_BUTTON_DOWN,      K_R2_BUTTON   }, // +attack2
    { WPAD_BUTTON_LEFT,      K_DPAD_LEFT   }, // invprev
    { WPAD_BUTTON_RIGHT,     K_DPAD_RIGHT  }, // invnext

    { WPAD_BUTTON_HOME,      K_ESCAPE      }, // menu
};

/*
The classic controller reports through the same word as the nunchuk and the
two overlap bit for bit, so which table applies is decided by the expansion
WPAD_Probe reports rather than by ORing them together.
*/
static const ogc_btn_t ogc_map_classic[] =
{
        // ZL y ZR disparan (cualquiera de los dos; se combinan en una sola entrada)
        { WPAD_CLASSIC_BUTTON_ZL | WPAD_CLASSIC_BUTTON_ZR, K_R1_BUTTON }, // +attack
        { WPAD_CLASSIC_BUTTON_FULL_R, K_Y_BUTTON     }, // linterna (impulse 100)
        { WPAD_CLASSIC_BUTTON_FULL_L, K_L1_BUTTON    }, // +duck
        { WPAD_CLASSIC_BUTTON_A,      K_A_BUTTON     }, // +jump
        { WPAD_CLASSIC_BUTTON_X,      K_B_BUTTON     }, // +use
        { WPAD_CLASSIC_BUTTON_Y,      K_X_BUTTON     }, // +reload
        { WPAD_CLASSIC_BUTTON_B,      K_F6           }, // savequick (autoguardado)
        { WPAD_CLASSIC_BUTTON_PLUS,   K_TAB          }, // marcador multijugador
        { WPAD_CLASSIC_BUTTON_MINUS,  K_ESCAPE       }, // menu
        { WPAD_CLASSIC_BUTTON_UP,     K_DPAD_UP      }, // spray
        { WPAD_CLASSIC_BUTTON_DOWN,   K_DPAD_DOWN    }, // lastinv
        { WPAD_CLASSIC_BUTTON_LEFT,   K_DPAD_LEFT    }, // invprev
        { WPAD_CLASSIC_BUTTON_RIGHT,  K_DPAD_RIGHT   }, // invnext
        { WPAD_CLASSIC_BUTTON_HOME,   K_ESCAPE       }, // menu
};

static const ogc_btn_t ogc_map_gamecube[] =
{
        { PAD_TRIGGER_R,     K_R1_BUTTON   },   // +attack
        { PAD_TRIGGER_L,     K_R2_BUTTON   },   // +attack2
        { PAD_TRIGGER_Z,     K_L1_BUTTON   },   // +duck
        { PAD_BUTTON_A,      K_A_BUTTON    },   // +jump
        { PAD_BUTTON_X,      K_B_BUTTON    },   // +use
        { PAD_BUTTON_Y,      K_X_BUTTON    },   // +reload
        { PAD_BUTTON_B,      K_F6          },   // savequick (autoguardado)
        { PAD_BUTTON_UP,     K_Y_BUTTON    },   // linterna (impulse 100)
        { PAD_BUTTON_DOWN,   K_TAB         },   // marcador multijugador
        { PAD_BUTTON_LEFT,   K_DPAD_LEFT   },   // invprev
        { PAD_BUTTON_RIGHT,  K_DPAD_RIGHT  },   // invnext
        { PAD_BUTTON_START,  K_ESCAPE      },   // menu
};

static u32 ogc_buttons_held;
static u32 ogc_pad_held;

/*
Which device the player touched last. It decides where the rumble goes and
whether the Wii remote pointer is allowed to aim: with a USB mouse in use the
remote is usually lying on a table, and its IR sensor would keep dragging the
aim around.
*/
enum { OGC_SRC_WII, OGC_SRC_GC, OGC_SRC_MOUSE };
static int ogc_src = OGC_SRC_WII;

// set by OGC_Init: reading the queues of a device that failed to start would
// walk an uninitialised list
int ogc_usb_kbd_ok;
int ogc_usb_mouse_ok;

// where the player is pointing, as -1..1 from the centre of the screen.
// kept here so the view code can lean the weapon towards it.
static vec2_t ogc_pointer;

static qboolean OGC_GetPointerAngles( float *dyaw, float *dpitch );

// USB mouse: motion waiting to be turned into view angles, and a virtual
// cursor in screen pixels for the menus
static float ogc_mouse_dx, ogc_mouse_dy;
static float ogc_mouse_x, ogc_mouse_y;
static void OGC_RumbleFrame( void );
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
	Cvar_RegisterVariable( &wii_rumble );
	Cvar_RegisterVariable( &wii_gun_tilt );
	Cvar_RegisterVariable( &wii_gun_tilt_max );
	Cvar_RegisterVariable( &wii_gun_pitch );
	Cvar_RegisterVariable( &wii_mouse_sens );


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
static void OGC_SendKey( int key, qboolean down )
{
	if( key == K_ESCAPE && cls.key_dest == key_game )
	{
		if( down )
			Cbuf_AddText( "escape\n" );
		return;
	}

	Key_Event( key, down );
}

static void OGC_EmitButtons( const ogc_btn_t *map, int count, u32 held, u32 *prev, int src )
{
	u32 changed = held ^ *prev;
	int i;

	*prev = held;

	if( !changed )
		return;


	// tocar un boton devuelve el control al mando (y no al raton)
	ogc_src = src;

	for( i = 0; i < count; i++ )
	{
		if( changed & map[i].mask )
			OGC_SendKey( map[i].key, ( held & map[i].mask ) != 0 );
	}
}
static void OGC_PollNunchuk(void);
static void OGC_PollSticks( u32 wpad_type );

void OGC_ButtonsFrame( void )
{
	u32 type = WPAD_EXP_NONE;

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
		static int once_classic = 0;
		if (!once_classic) {
			once_classic = 1;
		}
		OGC_EmitButtons( ogc_map_classic, ARRAYSIZE( ogc_map_classic ),
			WPAD_ButtonsHeld( WPAD_CHAN_0 ), &ogc_buttons_held, OGC_SRC_WII );
	}
	else
	{
		static int once_wiimote = 0;
		if (!once_wiimote) {
			once_wiimote = 1;
		}
		OGC_EmitButtons( ogc_map_wiimote, ARRAYSIZE( ogc_map_wiimote ),
			WPAD_ButtonsHeld( WPAD_CHAN_0 ), &ogc_buttons_held, OGC_SRC_WII );
	}

	OGC_EmitButtons( ogc_map_gamecube, ARRAYSIZE( ogc_map_gamecube ),
		PAD_ButtonsHeld( PAD_CHAN0 ), &ogc_pad_held, OGC_SRC_GC );

        // Sticks del Classic Controller y del GameCube (el del Nunchuk va aparte)
        OGC_PollSticks( type );
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
	if( !wii_ir.value || ogc_src == OGC_SRC_MOUSE || refState.width <= 0 || refState.height <= 0 )
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
void OGC_ApplyPointerToAim( vec3_t viewangles, float *forwardmove, float *sidemove )
{
	float dyaw, dpitch;

#ifdef XASH_OGC_AIMPROOF
	// force a fixed pointer offset so the two ends can be compared
	ogc_pointer[0] = 0.5f; ogc_pointer[1] = 0.0f;
#endif
	if( !OGC_GetPointerAngles( &dyaw, &dpitch ))
		return;

#ifdef XASH_OGC_AIMPROOF
	{
		static int n;
		if(( n % 4 ) == 0 && n < 160 )
			Con_Printf( "[AIMPROOF] cl_n=%d camera yaw=%.3f  sent yaw=%.3f\n",
				n, cl.viewangles[YAW], viewangles[YAW] + dyaw );
		n++;
	}
#endif
	viewangles[YAW]   += dyaw;
	viewangles[PITCH] += dpitch;
	viewangles[PITCH] = bound( -89.0f, viewangles[PITCH], 89.0f );

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
	if( !wii_ir.value || ogc_src == OGC_SRC_MOUSE || refState.width <= 0 || refState.height <= 0 )
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
WPAD_Probe( WPAD_CHAN_0, &type ) == WPAD_ERR_NONE && type != WPAD_EXP_CLASSIC )
{
*data = WPAD_Data( WPAD_CHAN_0 );

data )
r = data->orient.roll, p = data->orient.pitch;
lim = bound( 5.0f, wii_gun_tilt_max.value, 90.0f );

isnan( r ) || isinf( r )) r = 0.0f;
isnan( p ) || isinf( p )) p = 0.0f;

zona muerta de 3 grados, el resto se reescala
fabsf( r ) < 3.0f ) r = 0.0f; else r -= r > 0.0f ? 3.0f : -3.0f;
= bound( -lim, r * wii_gun_tilt.value, lim );

!data->ir.valid && wii_gun_pitch.value != 0.0f )
fabsf( p ) < 3.0f ) p = 0.0f; else p -= p > 0.0f ? 3.0f : -3.0f;
= bound( -lim, p * wii_gun_pitch.value, lim );
wii_showinput.value )
int tick;
tick++ % 60 ) == 0 )
_Printf( "^3[TILT]^7 roll=%.1f pitch=%.1f ir=%d\n",
t.roll, data->orient.pitch, (int)data->ir.valid );
filtro exponencial (~70 ms): sigue al mando sin temblar
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
;

if( now < ogc_rumble_end )
{
ya vibrando: se queda lo mas fuerte y lo mas largo
power > ogc_rumble_power )
= power;
now + seconds > ogc_rumble_end )
d = now + seconds;
}
else
{
= power;
d = now + seconds;
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
t hp = cl.local.health;

prev_health > 0 && hp < prev_health )
t lost = bound( 1, prev_health - hp, 50 );

hp <= 0 )
1.0f, 0.8f );
0.55f + lost * 0.009f, 0.15f + lost * 0.008f );
= hp;
}
else prev_health = 0;

if( wii_rumble.value > 0.0f && host.realtime < ogc_rumble_end )
{
t duty = (int)( ogc_rumble_power * 6.0f + 0.5f );
 = ( tick++ % 6 ) < (unsigned)duty;
}
else tick = 0;

want_wii = on && ogc_src == OGC_SRC_WII;
want_gc  = on && ogc_src == OGC_SRC_GC;

if( want_wii != ogc_rumble_wii_on )
{
WPAD_CHAN_0, want_wii ? 1 : 0 );
 = want_wii;
}

if( want_gc != ogc_rumble_gc_on )
{
trolMotor( PAD_CHAN0, want_gc ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP_HARD );
 = want_gc;
}
}

/*
============
USB keyboard
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
case 45:  return 0x2D;
case 46:  return 0x3D;
case 47:  return 0x5B;
case 48:  return 0x5D;
case 49:  return 0x5C;
case 51:  return 0x3B;
case 52:  return 0x27;
case 53:  return 0x60;
case 54:  return 0x2C;
case 55:  return 0x2E;
case 56:  return 0x2F;
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
 true;
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
	int guard = 0;

	if( !ogc_usb_kbd_ok )
		return;

	while( guard++ < 64 && KEYBOARD_GetEvent( &ev ) > 0 )
	{
		qboolean down;
		qboolean ctrl;
		int key, ch = 0;

		if( ev.type != KEYBOARD_PRESSED && ev.type != KEYBOARD_RELEASED )
			continue; // conectado / desconectado

		down = ev.type == KEYBOARD_PRESSED;
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
	mouse_event ev;
	int guard = 0;

	if( !ogc_usb_mouse_ok )
		return;

	// el cursor del menu arranca en el centro de la pantalla
	if( !placed && refState.width > 0 && refState.height > 0 )
	{
		ogc_mouse_x = refState.width * 0.5f;
		ogc_mouse_y = refState.height * 0.5f;
		placed = true;
	}

	while( guard++ < 128 && MOUSE_GetEvent( &ev ) > 0 )
	{
		u8 changed = ev.button ^ held;
		int i;

		if( ev.rx || ev.ry || ev.rz || changed )
			ogc_src = OGC_SRC_MOUSE;

		ogc_mouse_dx += ev.rx;
		ogc_mouse_dy += ev.ry;

		if( refState.width > 0 && refState.height > 0 )
		{
			ogc_mouse_x = bound( 0.0f, ogc_mouse_x + ev.rx, (float)( refState.width - 1 ));
			ogc_mouse_y = bound( 0.0f, ogc_mouse_y + ev.ry, (float)( refState.height - 1 ));
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

	if( ogc_mouse_dx == 0.0f && ogc_mouse_dy == 0.0f )
		return;

	// fuera de la partida el raton solo mueve el cursor del menu
	if( cls.key_dest != key_game )
	{
		ogc_mouse_dx = ogc_mouse_dy = 0.0f;
		return;
	}

	s = wii_mouse_sens.value;
	*yaw   -= ogc_mouse_dx * Cvar_VariableValue( "m_yaw" ) * s;
	*pitch += ogc_mouse_dy * Cvar_VariableValue( "m_pitch" ) * s;

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

                if( fabsf( gx ) > 0.18f || fabsf( gy ) > 0.18f ) { lx = gx; ly = gy; }
                if( fabsf( cx ) > 0.18f || fabsf( cy ) > 0.18f ) { rx = cx; ry = cy; }
        }

        // convencion de SDL que usa el motor: arriba es negativo en Y
        out[0] = OGC_StickToAxis( lx, false );
        out[1] = OGC_StickToAxis( -ly, false );
        out[2] = OGC_StickToAxis( rx, true );
        out[3] = OGC_StickToAxis( -ry, true );

        for( i = 0; i < 4; i++ )
        {
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
