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
	{ WPAD_CLASSIC_BUTTON_FULL_R, K_R1_BUTTON    },	// +attack
	{ WPAD_CLASSIC_BUTTON_ZR,     K_R2_BUTTON    },	// +attack2
	{ WPAD_CLASSIC_BUTTON_FULL_L, K_L1_BUTTON    },	// +duck
	{ WPAD_CLASSIC_BUTTON_ZL,     K_L2_BUTTON    },	// +speed, walk
	{ WPAD_CLASSIC_BUTTON_A,      K_A_BUTTON     },	// +jump
	{ WPAD_CLASSIC_BUTTON_B,      K_B_BUTTON     },	// +use
	{ WPAD_CLASSIC_BUTTON_X,      K_X_BUTTON     },	// +reload
	{ WPAD_CLASSIC_BUTTON_Y,      K_Y_BUTTON     },	// flashlight
	{ WPAD_CLASSIC_BUTTON_UP,     K_DPAD_UP      },
	{ WPAD_CLASSIC_BUTTON_DOWN,   K_DPAD_DOWN    },	// lastinv
	{ WPAD_CLASSIC_BUTTON_LEFT,   K_DPAD_LEFT    },	// invprev
	{ WPAD_CLASSIC_BUTTON_RIGHT,  K_DPAD_RIGHT   },	// invnext
	{ WPAD_CLASSIC_BUTTON_MINUS,  K_START_BUTTON },
	{ WPAD_CLASSIC_BUTTON_PLUS,   K_BACK_BUTTON  },	// pause
	{ WPAD_CLASSIC_BUTTON_HOME,   K_ESCAPE       },	// menu
};

static const ogc_btn_t ogc_map_gamecube[] =
{
	{ PAD_TRIGGER_R,     K_R1_BUTTON   },	// +attack
	{ PAD_TRIGGER_Z,     K_R2_BUTTON   },	// +attack2
	{ PAD_TRIGGER_L,     K_L1_BUTTON   },	// +duck
	{ PAD_BUTTON_A,      K_A_BUTTON    },	// +jump
	{ PAD_BUTTON_B,      K_B_BUTTON    },	// +use
	{ PAD_BUTTON_X,      K_X_BUTTON    },	// +reload
	{ PAD_BUTTON_Y,      K_Y_BUTTON    },	// flashlight
	{ PAD_BUTTON_UP,     K_DPAD_UP     },
	{ PAD_BUTTON_DOWN,   K_DPAD_DOWN   },	// lastinv
	{ PAD_BUTTON_LEFT,   K_DPAD_LEFT   },	// invprev
	{ PAD_BUTTON_RIGHT,  K_DPAD_RIGHT  },	// invnext
	{ PAD_BUTTON_START,  K_ESCAPE      },	// menu
};

static u32 ogc_buttons_held;
static u32 ogc_pad_held;

// where the player is pointing, as -1..1 from the centre of the screen.
// kept here so the view code can lean the weapon towards it.
static vec2_t ogc_pointer;

static qboolean OGC_GetPointerAngles( float *dyaw, float *dpitch );

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
static void OGC_EmitButtons( const ogc_btn_t *map, int count, u32 held, u32 *prev )
{
	u32 changed = held ^ *prev;
	int i;

	*prev = held;

	if( !changed )
		return;

	for( i = 0; i < count; i++ )
        {
                if( changed & map[i].mask )
                {
                        qboolean down = ( held & map[i].mask ) != 0;

                        // En juego, ESC pasaba por el binding "cancelselect" y dependia del
                        // client.dll para abrir el menu. Abrimos el menu directamente con el
                        // comando del motor. Con el menu abierto, la tecla sigue yendo al
                        // menu (volver atras).
                        if( map[i].key == K_ESCAPE && cls.key_dest == key_game )
                        {
                                if( down )
                                        Cbuf_AddText( "escape\n" );
                                continue;
                        }

                        Key_Event( map[i].key, down );
                }
        }
}
static void OGC_PollNunchuk(void);

void OGC_ButtonsFrame( void )
{
	u32 type = WPAD_EXP_NONE;

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
			WPAD_ButtonsHeld( WPAD_CHAN_0 ), &ogc_buttons_held );
	}
	else
	{
		static int once_wiimote = 0;
		if (!once_wiimote) {
			once_wiimote = 1;
		}
		OGC_EmitButtons( ogc_map_wiimote, ARRAYSIZE( ogc_map_wiimote ),
			WPAD_ButtonsHeld( WPAD_CHAN_0 ), &ogc_buttons_held );
	}

	OGC_EmitButtons( ogc_map_gamecube, ARRAYSIZE( ogc_map_gamecube ),
		PAD_ButtonsHeld( PAD_CHAN0 ), &ogc_pad_held );
}

/*
============
OGC_GetPointer

Pointer position as -1..1 from screen centre, for the view model.
============
*/
void OGC_GetPointer( float *x, float *y )
{
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

	if( !wii_ir.value || refState.width <= 0 || refState.height <= 0 )
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
    float dyaw, dpitch;

    if( !view || !wii_ir.value )
        return;

    // Calcula el offset angular real (mismo que el disparo), asi el arma
    // apunta EXACTAMENTE al crosshair, no solo se inclina un poco.
    if( !OGC_GetPointerAngles( &dyaw, &dpitch ))
        return;

    // Blindaje NaN/Inf
    if( isnan( dyaw ) || isinf( dyaw ) ) dyaw = 0.0f;
    if( isnan( dpitch ) || isinf( dpitch ) ) dpitch = 0.0f;

    // Aplica el offset angular al arma
    view->angles[YAW]   += dyaw;
    view->angles[PITCH] -= dpitch;

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

    if( !wii_ir.value || refState.width <= 0 || refState.height <= 0 )
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
