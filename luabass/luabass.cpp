/**
* \file luabass.c
* \brief LUA extension to drive Midi-out
* \author Franck Revolle
* \version 1.1
* \date 05/10/2015
// update : 01/12/2016 23:00
* Objective :
*  LUA script language can use these functions to create easily any logic over MIDI-out.
*  List of function is available at the end of this source code file.

* platform :  MAC PC 
*/


#ifdef _WIN32
#define V_PC 1
#ifdef _WIN64
#define V_PC 1
#endif
#elif __APPLE__
#include "TargetConditionals.h"
#if TARGET_IPHONE_SIMULATOR
// iOS Simulator
#elif TARGET_OS_IPHONE
// iOS device
#elif TARGET_OS_MAC
#define V_MAC 1
#else
// Unsupported platform
#endif
#elif __linux
// linux
#elif __unix // all unices not caught above
// Unix
#elif __posix
// POSIX
#endif

#ifdef V_PC
#include "stdafx.h"
#endif


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#ifdef V_PC
#include <ctgmath>
#endif
#ifdef V_MAC
#include <math.h>
#endif

#include <lua.hpp>
#include <lauxlib.h>
#include <lualib.h>
#include <bass.h>
#include <bassmidi.h>
#include <bassmix.h>
#include "aeffect.h"
#include "aeffectx.h"
#include "vstfxstore.h"


#ifdef V_PC
#include <bassasio.h>
#include <bass_vst.h>
#include <mmsystem.h>
#include <assert.h>
#endif
#ifdef V_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/MIDIServices.h>
#include <CoreMIDI/MIDISetup.h>
#include <CoreMIDI/MIDIThruConnection.h>
#include <pthread.h>
#endif

#include "luabass.h"
#include "global.h"

#ifdef V_PC
// disable warning for mismatch lngth between LUA int and C int
#pragma warning( disable : 4244 )
// disable warning for deprecated ( e.g. sprintf )
#pragma warning( disable : 4996 )
#endif

#ifdef V_MAC
#define strtok_s strtok_r
#define strnlen_s strnlen
#endif

int g_transposition = 0;
int g_audio_buffer_length = 0; // length if audio buffer ( large by default, latency, but no crack .. )
int g_default_audio_device = 0; // default audio device 

#define MAX_AUDIO_DEVICE 10

/**
* \union T_midimsg
* \brief store a short Midi Messages ( noteOn, ...)
*
* The data manipulated is a 3 bytes structure.
* It is used for short MIDI messages , like Note-On, Note-Off, Control ...
*/
typedef union t_midimsg
{
	DWORD dwData; /*!< The block of data. */
	BYTE bData[4]; /*!< The data, byte per byte. */
} T_midimsg;

#define MAX_VSTI_PENDING_MIDIMSG 256

/**
* \struct T_vi_opened
* \brief Queue of VI already opened.
*
*/
typedef struct t_vi_opened
{
	char filename[512];// file used by this Virtual Instrument
	int nr_device_audio; // audio-device used by this Virtual Instrument
	HSTREAM mstream; // BASS stream connected to the mixer of the audio-device

	HSOUNDFONT sf2_midifont; // handler Soundfont SF2

	AEffect *vsti_plugins; // handler of the VSTi
#ifdef V_PC
	HMODULE vsti_modulePtr; // library dll loaded for the VSTi
#else
    CFBundleRef vsti_modulePtr;// library dll loaded for the VSTi
#endif
	int vsti_nb_outputs; // number of audio outputs of the vsti
	bool vsti_midi_prog; // vst-prog will be sent 
	int vsti_last_prog; // last vst-prog sent to the VSTi on next update
	bool vsti_todo_prog;// pending vst-prog to send to the VSTi on next update
	T_midimsg vsti_pending_midimsg[MAX_VSTI_PENDING_MIDIMSG]; // pending midi msg to send to the VSTi on next update
	int vsti_nb_pending_midimsg; // nb pending midi msg to send to the VS on next update
	float **vsti_outputs; // buffer for vsti output
} T_vi_opened;

/**
* \struct T_midioutmsg
* \brief store a short Midi Messages, with additional information for the output
*
* The information is used to track the output of the short MIDI message.
*/
typedef struct t_midioutmsg
{
	long id; /*!< unique id of the msg used for noteon noteoff control */
	int track; /*!< midiout track for this message. */
	long dt; /*!< delay in ms before to send this message */
	BYTE nbbyte; /*!< number of bytes of the midi message*/
	T_midimsg midimsg; /*!<  short midi msg itself */
} T_midioutmsg;

/**
* \struct T_channel
* \brief properties of a MIDI Channel on a device : extention
*
* The information is used to produce the MIDI out message, just before to send it physically on the phisical MIDI channel.
*/
typedef struct t_channel
{
	int extended; /*!< specify which logical MIDI channel drives this MIDI physical channel */
} T_channel;

#define MAXCURVE 10
#define MAXPOINT 10
typedef struct t_curve
{
	int x[MAXPOINT], y[MAXPOINT]; /*!< list of points for the volume curve of the channel */
} T_curve;
#define MAXTRACK 32
/**
* \struct T_track
* \brief properties of a track : volume and curve
*
* A track is logical group of MIDI flow, which is attaced to a device/channel, through T_channel
* The information is used to control volume on a track..
*/
typedef struct t_track
{
	int device; /*!< MIDI device used to send the MIDI messages */
	int channel; /*!< logical MIDI channel used to send the MIDI messages */
	int volume; /*!< MIDI volume of the channel ( not the CTRL-7 MIDI Volume ) 0..127 */
	bool mute;
	int nrCurve;
} T_track;
/**
* \struct T_chord
* \brief track information to play a chord
*
* The information is used to start ans stop a chord.
* The chord-on can be used many times on the same chord-id.
* The chord-off is used one time on the same chord-id. It off all the previous chord-on.
*/
#define CHORDMAXPITCH 24
#define CHORDMAX 40
typedef struct t_chord
{
	long id; /*!< unique id of the chord */
	int dt; /*!< delay between notes in the chord, in ms */
	int dv; /*!< ratio, in % , between velocity notes in the chord */
	int pitch[CHORDMAXPITCH]; /*!< pitches of the chord */
	int nbPitch; /*!< number of pitches in the chord */

	int nbOff; // number of noteoff when chord-off
	T_midioutmsg msg_off[CHORDMAXPITCH]; // noteon to off
} T_chord;
/**
* \struct T_queue_msg
* \brief Queue of pend MIDI-out messages.
*
* MIDI messages which must be sent later are stored in this queue.
*/
typedef struct t_queue_msg
{
	T_midioutmsg midioutmsg; /*!< The midi message delayed */
	long t; /*!< the time to send the message */
	bool free; /*!< slot free */
} T_queue_msg;
#define OUT_QUEUE_MAX_MSG 1024

#define MAXBUFERROR 64

#define timer_dt 20 // ms between two timer interrupts on output

#define OUT_QUEUE_FLUSH 0
#define OUT_QUEUE_NOTEOFF 1

#define smidiToOpen "midiinOpen" // global LUA table which contains the MIDI-in to open

#define sinit "init" // luabass function called to init the bass midi out 
#define sonStart "onStart" // LUA function called just after the creation of the LUA stack gin_LUAstate ( e.g. to initialise the MIDI settings)
#define sonStop "onStop" // LUA function called before to close the LUA stack gin_LUAstate
#define sfree "free" // luabass function called to free the bass midi out 

// LUA-functions called by this DLL 
#define LUAFunctionNoteOn "onNoteon" // LUA funtion to call on midi msg noteon
#define LUAFunctionNoteOff "onNoteoff" // LUA funtion to call on midi msg noteoff
#define LUAFunctionKeyPressure "onKeypressure" // LUA funtion to call on midi msg keypressure
#define LUAFunctionControl "onControl" // LUA funtion to call on midi msg control
#define LUAFunctionProgram "onProgram" // LUA funtion to call on midi msg program
#define LUAFunctionChannelPressure "onChannelpressure" // LUA funtion to call on midi msg channelpressure
#define LUAFunctionPitchBend "onPitchbend" // LUA funtion to call on midi msg pitchbend
#define LUAFunctionSystemCommon "onSystemecommon" // LUA funtion to call on midi msg systemcommon
#define LUAFunctionSysex "onSysex" // LUA funtion to call on midi msg sysex
#define LUAFunctionActive "onActive" // LUA funtion to call on midi msg active sense
#define LUAFunctionClock "onClock" // LUA funtion to call when midiin clock

#define onTimer "onTimer" // LUA funtion to call when timer is triggered 
#define onSelector "onSelector" // LUA functions called with noteon noteoff event,a dn add info 

#define MIDI_NOTEONOFF		7  // non MIDI message : for selectors only
#define MIDI_NOTEOFF			8  // 0x8
#define MIDI_NOTEON			9  // 0x9
#define MIDI_KEYPRESSURE		10 // 0xA
#define MIDI_CONTROL			11 // 0xB
#define MIDI_PROGRAM			12 // 0xC
#define MIDI_CHANNELPRESSURE 13 // 0xD
#define MIDI_PITCHBEND		14 // 0xE
#define MIDI_SYSTEMCOMMON	15 // 0xF

#define MIDI_SYSEX 0xF0
#define MIDI_ACTIVESENSING 0xFE
#define MIDI_CLOCK 0xF8



//////////////////////////////
//  static statefull variables
//////////////////////////////
#ifdef V_PC
static HMIDIOUT g_midiopened[MIDIOUT_MAX];// list of midiout. 0 if midout is free.
#endif
#ifdef V_MAC
static MIDIEndpointRef g_midiopened[MIDIOUT_MAX]; // list of midiout. 0 if midiout is free.
static MIDIPortRef g_midiOutPortRef = 0;
static MIDIClientRef g_midiClientRef = 0;
#endif

static bool g_audio_open[MAX_AUDIO_DEVICE];
static T_vi_opened g_vi_opened[VI_MAX];
static int g_vi_opened_nb = 0;
static HSTREAM g_mixer_stream[MAX_AUDIO_DEVICE];

#define VSTI_BUFSIZE 4096

VstEvents *g_vsti_events;
int g_vsti_bufsize = 1024;



static int g_midimax_nr_device;

static T_channel g_channels[OUT_MAX_DEVICE][MAXCHANNEL];

static unsigned long g_midistatuspitch[OUT_MAX_DEVICE][MAXCHANNEL][MAXPITCH]; // the status of a output pitch
static unsigned long g_midistatuscontrol[OUT_MAX_DEVICE][MAXCHANNEL][MAXPITCH]; // the status of a output control
static unsigned long g_miditimepitch[OUT_MAX_DEVICE][MAXCHANNEL][MAXPITCH]; // the time of a output pitch
static unsigned long g_miditimecontrol[OUT_MAX_DEVICE][MAXCHANNEL][MAXPITCH]; // the time of a output control

static int g_chordCompensation = 0; // compensation of velocity for each note in a chord
static int g_randomDelay = 0; // random delay in seconds for each note of a chord
static int g_randomVelocity = 0; // random velocity for each note of a chord

static bool g_collectLog = false;
static int nrOutBufLog = 0;
static int nrInBufLog = 0;
#define MAXBUFLOGOUT 512
#define MAXNBLOGOUT 64
static char bufLog[MAXNBLOGOUT][MAXBUFLOGOUT];

static long g_unique_id = 128;

static T_track g_tracks[MAXTRACK]; 
static int g_volume = 64;
static T_curve g_curves[MAXCURVE];

static T_chord g_chords[CHORDMAX];

static long g_current_t = 0 ; // relative time in ms for output

#ifdef V_PC
// timer to flush the midiout queud messages
static HANDLE g_timer = NULL;
// mutex to protect the output ( from LUA and timer, to outputs )
static HANDLE g_mutex_out = NULL;
#endif
#ifdef V_MAC
// mutex to protect the access od the midiout queud messages
static pthread_mutex_t g_mutex_out ;
#endif

static T_queue_msg g_queue_msg[OUT_QUEUE_MAX_MSG];
static int g_end_queue_msg = 0; // end of potential waitin slot
static int g_max_queue_msg = 0; // max of waiting slot

static 	lua_State *g_LUAoutState = 0 ; // LUA state for the process of midiout messages
static bool g_process_NoteOn, g_process_NoteOff;
static bool g_process_Control, g_process_Program;
static bool g_process_PitchBend, g_process_KeyPressure, g_process_ChannelPressure;
static bool g_process_SystemCommon, g_process_Clock;

char g_path_out_error_txt[MAXBUFCHAR] = "luabass_log_out.txt";

static int cap(int vin, int min, int max, int offset)
{
	// -offset is applied to vin
	// return vin inside range [min..max[ . 
	int v = vin - offset;
	if (v < min) return min;
	if (v >= max) return ( max - 1);
	return v;
}
static int pitchbend_value(T_midimsg u)
{
	return((int)(u.bData[2]) * (int)(0x80) + (int)(u.bData[1]) - (int)(0x2000));
}
static void log_init(const char *fname)
{
	if ((fname != NULL) && (strlen(fname) > 0))
	{
		strcpy(g_path_out_error_txt, fname);
		strcat(g_path_out_error_txt, "_out.txt");
	}
	FILE * pFile = fopen(g_path_out_error_txt, "w");;
	if (pFile == NULL) return;
	fprintf(pFile, "log luabass out\n");
	fclose(pFile);
}
int mlog(const char * format, ...)
{
	char msg[MAXBUFLOGOUT];
	va_list args;
	va_start(args, format);
	vsprintf(msg, format, args);
	va_end(args);
	FILE * pFile = fopen(g_path_out_error_txt, "a");;
	if (pFile != NULL)
	{
		fprintf(pFile, "%s\n",msg);
		fclose(pFile);
	}
	if (g_collectLog)
	{
		strcpy(bufLog[nrInBufLog], msg);
		nrInBufLog++;
		if (nrInBufLog >= MAXNBLOGOUT)
			nrInBufLog = 0;
	}
	return(-1);
}
void lock_mutex_out()
{
#ifdef V_PC
	WaitForSingleObject(g_mutex_out, INFINITE);
#endif
#ifdef V_MAC
	pthread_mutex_lock(&g_mutex_out);
#endif
}
void unlock_mutex_out()
{
#ifdef V_PC
	ReleaseMutex(g_mutex_out);
#endif
#ifdef V_MAC
	pthread_mutex_unlock(&g_mutex_out);
#endif
}

static int apply_volume(int nrTrack, int v)
{
	if ((nrTrack < 0) || (nrTrack >= MAXTRACK))
		return v;

	T_track *t;
	int mainvolume;
	t = &(g_tracks[nrTrack]);
	mainvolume = g_volume;

	if ((t->volume == 64) && (mainvolume == 64) && ((t->nrCurve < 0) || (g_curves[t->nrCurve].x[0] == -1)))
		return cap(v, 0, 128, 0);
	if (t->mute)
		return 0;

	int vout = v;
	int x0, y0, x1, y1;
	if (t->nrCurve > 0)
	{
		T_curve *curve = &(g_curves[t->nrCurve]);
		int n = 0;
		while ((n < MAXPOINT) && (curve->x[n] >= 0) && (vout > curve->x[n]))
			n++;
		if ((n >= (MAXPOINT)) || (curve->x[n] < 0))
		{
			x0 = curve->x[n - 1];
			y0 = curve->y[n - 1];
			x1 = 127;
			y1 = 127;
		}
		else
		{
			if (n == 0)
			{
				x0 = 1;
				y0 = 1;
				x1 = curve->x[0];
				y1 = curve->y[0];
			}
			else
			{
				x0 = curve->x[n - 1];
				y0 = curve->y[n - 1];
				x1 = curve->x[n];
				y1 = curve->y[n];
			}
		}
		if (x1 == x0)
			x1 = x0 + 1;
		vout = y0 + ((vout - x0) * (y1 - y0)) / (x1 - x0);
	}

	if (t->volume != 64)
	{
		if (t->volume < 64)
		{
			x0 = 1; y0 = 1;
			x1 = 127; y1 = 2 * t->volume;
		}
		else
		{
			x0 = 1; y0 = (t->volume - 64) * 2;
			x1 = 127; y1 = 127;
		}
		vout = cap(y0 + ((vout - x0) * (y1 - y0)) / (x1 - x0), 1, 128, 0);
	}

	if (mainvolume != 64)
	{
		if (mainvolume < 64)
		{
			x0 = 1; y0 = 1;
			x1 = 127; y1 = 2 * mainvolume;
		}
		else
		{
			x0 = 1; y0 = (mainvolume - 64) * 2;
			x1 = 127; y1 = 127;
		}
		vout = cap(y0 + ((vout - x0) * (y1 - y0)) / (x1 - x0), 1, 128, 0);
	}

	return cap(vout, 1, 128, 0);
}
static bool audio_name(int nr_device, char *name)
{
	*name = '\0';
#ifdef V_PC
	BASS_ASIO_DEVICEINFO info;
	if (BASS_ASIO_GetDeviceInfo(nr_device, &info) == TRUE)
	{
		strcpy(name, info.name);
		return true;
	}
#endif
#ifdef V_MAC
	BASS_DEVICEINFO info;
	nr_device = 1; // 0 = nosound ...
	if (BASS_GetDeviceInfo(nr_device, &info) == TRUE)
	{
		if (info.flags & BASS_DEVICE_ENABLED)
		{
			strcpy(name, info.name);
			return true;
		}
	}
#endif
	return false;

}
static int BASS_MIDI_OutInit(int nr_device)
{
#ifdef V_PC
	UINT device_id = nr_device;
	HRESULT result;
	result = midiOutOpen(&(g_midiopened[nr_device]), device_id, (DWORD)(NULL), (DWORD)(NULL), CALLBACK_NULL);
	if (result != MMSYSERR_NOERROR)
	{
		g_midiopened[nr_device] = 0;
		mlog("BASS_MIDI_OutInit device = %d , error %d", nr_device ,result);
		return(-1);
	}
#endif
#ifdef V_MAC
	ItemCount device_id = nr_device;
	OSStatus result;
	if (g_midiClientRef == 0)
	{
		result = MIDIClientCreate(CFSTR("luabass"), NULL, NULL, &g_midiClientRef);
		if (result != 0)
		{
			mlog("Client Midi Create : error %ld", (SInt32)result);
			g_midiClientRef = 0;
			return (-1);
		}
	}
	if (g_midiOutPortRef == 0)
	{
		result = MIDIOutputPortCreate(g_midiClientRef, CFSTR("Output port"), &g_midiOutPortRef);
		if (result != 0)
		{
			mlog("Client Midi output port create : error %ld", (SInt32)result);
			g_midiOutPortRef = 0;
			return (-1);
		}
	}
	g_midiopened[nr_device] = MIDIGetDestination(device_id);
	if (g_midiopened[nr_device] == 0)
	{
		mlog("MidiGetDestination : error");
		return(-1);
	}
#endif
	if (nr_device >= g_midimax_nr_device)
		g_midimax_nr_device = nr_device + 1;

    T_midimsg midimsg1;
    midimsg1.bData[0] = (MIDI_CONTROL << 4);
    midimsg1.bData[1] = 123;// all note off
    midimsg1.bData[2] = 0;
    T_midimsg midimsg2;
    midimsg2.bData[0] = (MIDI_CONTROL << 4);
    midimsg2.bData[1] = 120;// all sound off
    midimsg2.bData[2] = 0;
    T_midimsg midimsg3;
    midimsg3.bData[0] = (MIDI_CONTROL << 4);
    midimsg3.bData[1] = 121;// reset al controller
    midimsg3.bData[2] = 0;
    
#ifdef V_PC
	midiOutShortMsg(g_midiopened[nr_device], midimsg1.dwData);
	midimsg.bData[1] = 120;// all sound off
	midiOutShortMsg(g_midiopened[nr_device], midimsg2.dwData);
	midimsg.bData[1] = 121;// reset controller
	midiOutShortMsg(g_midiopened[nr_device], midimsg3.dwData);
#endif
#ifdef V_MAC
    // envoi du message Midi version Mac dans un packet
    Byte buffer[1024];
    MIDIPacketList *pktlist = (MIDIPacketList *)buffer;
    MIDIPacket *curPacket = MIDIPacketListInit(pktlist);
    curPacket = MIDIPacketListAdd(pktlist, sizeof(buffer), curPacket, 0, 3, & (midimsg1.bData[0] ));
    curPacket = MIDIPacketListAdd(pktlist, sizeof(buffer), curPacket, 0, 3, & (midimsg2.bData[0] ));
    curPacket = MIDIPacketListAdd(pktlist, sizeof(buffer), curPacket, 0, 3, & (midimsg3.bData[0] ));
    MIDISend(g_midiOutPortRef, g_midiopened[nr_device], pktlist);
#endif

    
	return(nr_device);
}
static void BASS_MIDI_OutFree(int nr_device)
{
#ifdef V_PC
	if (g_midiopened[nr_device])
		midiOutClose(g_midiopened[nr_device]);
	g_midiopened[nr_device] = 0;
#endif
#ifdef V_MAC
	if (g_midiopened[nr_device])
		MIDIClientDispose(g_midiClientRef);
	g_midiopened[nr_device] = 0;
#endif
}
static bool midi_in_name(int nr_device, char *name_device)
{
	BASS_MIDI_DEVICEINFO info;
	name_device[0] = '\0';
	BOOL retCode = BASS_MIDI_InGetDeviceInfo(nr_device, &info);
	bool boolretCode;
	if (retCode)
	{
		boolretCode = true;
		strcpy(name_device, info.name);
	}
	else
	{
		boolretCode = false;
	}
	return boolretCode;
}
static bool midi_out_name(int nr_device, char *name_device)
{
	name_device[0] = '\0';
#ifdef V_PC
	int nb_device;
	size_t i;
	MIDIOUTCAPS midi_caps;
	nb_device = midiOutGetNumDevs();
	if (nr_device >= nb_device)
		return false;
	midiOutGetDevCaps(nr_device, &midi_caps, sizeof(midi_caps));
	wcstombs_s(&i, name_device, (size_t)(MAXBUFCHAR - 1), midi_caps.szPname, (size_t)(MAXBUFCHAR - 1));
	return true;
#endif
#ifdef V_MAC
	unsigned long nb_device = MIDIGetNumberOfDestinations();
	if (nr_device >= nb_device)
		return false;
	char buf[1024];
	MIDIEndpointRef dest = MIDIGetDestination(nr_device);
    CFStringRef result ;
    MIDIObjectGetStringProperty(dest,kMIDIPropertyName, &result);
	CFStringGetCString(result, buf, 1024, kCFStringEncodingASCII);
	CFRelease(result);
	return (true);
#endif
}
int countMidiOut()
{
#ifdef V_PC
	return midiOutGetNumDevs();
#endif
#ifdef V_MAC
	return (int)(MIDIGetNumberOfDevices());
#endif
}
int countMidiIn()
{
#ifdef V_PC
	return midiInGetNumDevs();
#endif
#ifdef V_MAC
    return (int)(MIDIGetNumberOfDevices());
#endif
}
/*
static void inspect_channel()
{
	mlog("=============================================== outInspect extended channel");
	char buf[1024];
	int nb = 0;
	for (int d = 0; d < MIDIOUT_MAX; d++)
	{
		if (g_midiopened[d])
		{
			for (int c = 0; c < MAXCHANNEL; c++)
			{
				if ((g_channels[d][c].extended != c) && (g_channels[d][c].extended != -1))
				{
					sprintf(buf, "device#%d : channel#%d is extension of channel %d", d + 1, c + 1, g_channels[d][c].extended + 1);
					mlog(buf);
					nb++;
				}
			}
		}
	}
	mlog("=============================================== end extended channel");
}
static void inspect_queue()
{
	char buf[1024];
	int nbFree;
	mlog("=============================================== outInspect queue ");
	nbFree = 0;
	for (int n = 0; n < OUT_QUEUE_MAX_MSG; n++)
	{
		if (g_queue_msg[n].free)
			nbFree++;
		else
		{
			sprintf(buf, "waiting %d : t=%ld msg=%02X %02X %02X", n,
				g_queue_msg[n].t,
				g_queue_msg[n].midioutmsg.midimsg.bData[0],
				g_queue_msg[n].midioutmsg.midimsg.bData[1],
				g_queue_msg[n].midioutmsg.midimsg.bData[2]);
			mlog(buf);
		}
	}
	mlog("");
	mlog("nb queue free : %d / max used : %d", nbFree, g_max_queue_msg);

	mlog("=============================================== end queue");
}
static void inspect_note()
{
	mlog("=============================================== outInspect note");
	char buf[1024];
	int nb = 0;
	for (int d = 0; d < OUT_MAX_DEVICE; d++)
	{
		for (int c = 0; c < MAXCHANNEL; c++)
		{
			for (int p = 0; p < MAXPITCH; p++)
			{
				if (g_midistatuspitch[d][c][p] != -1)
				{
					sprintf(buf, "note-ON : device #%d , channel #%d , pitch #%d = %lu", d + 1, c + 1, p, g_midistatuspitch[d][c][p]);
					mlog(buf);
					nb++;
				}
			}
		}
	}
	mlog("total of note-ON : %d", nb);
	mlog("===============================================  end note");
}
static void inspect_device()
{
	mlog("=============================================== outInspect device");
	char buf[MAXBUFCHAR];
	for (int m = 0; m < MIDIOUT_MAX; m++)
	{
		midi_out_name(m, buf);
		if (g_midiopened[m])
			mlog("Midiout open : %s (#%d)", buf, m + 1);
	}
	for (int d = 0; d < MAX_AUDIO_DEVICE; d++)
	{
		if (g_mixer_stream[d])
		{
			sprintf(buf, "vi : mixer_stream[device #%d] %d", d + 1, g_mixer_stream[d]);
			mlog(buf);
		}
}
	for (int s = 0; s < VI_MAX; s++)
	{
		if (g_vi_opened[s].mstream)
			mlog("vi : midi[stream #%d] , %d", s, g_vi_opened[s].mstream + 1);
		if (g_vi_opened[s].sf2_midifont)
			mlog("vi : midifont[stream #%d] %d", s, g_vi_opened[s].sf2_midifont);
	}
	mlog("=============================================== end device");
}
static void inspect()
{
	inspect_device();
	inspect_channel();
	inspect_note();
	inspect_queue();
}
*/
#ifdef V_PC
DWORD CALLBACK asioProc(BOOL input, DWORD channel, void *buffer, DWORD length, void *user)
{
	DWORD c = BASS_ChannelGetData((DWORD)user, buffer, length);
	if (c == -1) c = 0; // an mlog, no data
	return c;
}
#endif

// VSTi : C callbacks  Main host callback
extern "C" { VstIntPtr VSTCALLBACK hostCallback(AEffect *effect, VstInt32 opcode, VstInt32 index, VstInt32 value, void *ptr, float opt);}
typedef AEffect *(*vstPluginFuncPtr)(audioMasterCallback host);// Plugin's entry point
typedef VstIntPtr(*dispatcherFuncPtr)(AEffect *effect, VstInt32 opCode,	VstInt32 index, VstInt32 value, void *ptr, float opt);// Plugin's dispatcher function
typedef float(*getParameterFuncPtr)(AEffect *effect, VstInt32 index);// Plugin's getParameter() method
typedef void(*setParameterFuncPtr)(AEffect *effect, VstInt32 index, float value);// Plugin's setParameter() method
typedef VstInt32(*processEventsFuncPtr)(VstEvents *events);// Plugin's processEvents() method
typedef void(*processFuncPtr)(AEffect *effect, float **inputs,	float **outputs, VstInt32 sampleFrames);// Plugin's process() method
extern "C"
{
	VstIntPtr VSTCALLBACK hostCallback(AEffect *effect, VstInt32 opcode, VstInt32 index, VstInt32 value, void *ptr, float opt)
	{
		switch (opcode)
		{
		case audioMasterVersion:				// VST Version supported (for example 2200 for VST 2.2) --
			return kVstVersion;					// 2 for VST 2.00, 2100 for VST 2.1, 2200 for VST 2.2 etc.
		case audioMasterGetSampleRate:
			return M_SAMPLE_RATE;
		case audioMasterGetVendorString:		// fills <ptr> with a string identifying the vendor (max 64 char)
			strcpy((char*)ptr, "Expresseur" /*max 64 char!*/);
			return(true);
		case audioMasterGetProductString:		// fills <ptr> with a string with product name (max 64 char)
			strcpy((char*)ptr, "ExpresseurV3");
			return(true);
		case audioMasterGetVendorVersion:		// returns vendor-specific version
			return(3);
			break;
		case audioMasterCanDo:					// string in ptr, see below
			if ((strcmp((char*)ptr, "supplyidle") == 0
				|| strcmp((char*)ptr, "sendvstmidievent") == 0	// ... esp. MIDI event for VSTi
				|| strcmp((char*)ptr, "startstopprocess") == 0))	// we calls effStartProcess  and effStopProcess
				return 1;
			else
				return 0;
			break;
		case audioMasterCurrentId:				// Returns the unique id of a plug that's currently loading
		case audioMasterIdle:
		case audioMasterUpdateDisplay: // the plug-in reported an update (e.g. after a program load/rename or any other param change)
		case audioMasterGetTime:
		case audioMasterSizeWindow:				// index: width, value: height
		case audioMasterGetLanguage:			// see enum
		case audioMasterOpenFileSelector:		// open a fileselector window with VstFileSelect* in <ptr>
		case audioMasterCloseFileSelector:
			return 0;
		}
		return 0;
	}
}
static void vsti_send_shortmsg(char vsti_nr, T_midimsg midimsg)
{
	T_vi_opened *vi = &(g_vi_opened[vsti_nr]);
	if (((midimsg.bData[0] >> 4) == MIDI_CONTROL) &&  (midimsg.bData[1] != 0) && (midimsg.bData[2] != 99)) // bank 99
		vi->vsti_midi_prog == false ;
	if (((midimsg.bData[0] >> 4) == MIDI_PROGRAM) && (vi->vsti_midi_prog == false) && (midimsg.bData[1] != vi->vsti_last_prog))
	{
		// send the VST program
		vi->vsti_last_prog = midimsg.bData[1];
		vi->vsti_todo_prog = true;
		vi->vsti_midi_prog == true ;
		return;
	}
	if (vi->vsti_nb_pending_midimsg >= MAX_VSTI_PENDING_MIDIMSG)
		return;
	vi->vsti_pending_midimsg[vi->vsti_nb_pending_midimsg].dwData = midimsg.dwData;
	(g_vi_opened[vsti_nr].vsti_nb_pending_midimsg)++;
}
static void vsti_init()
{
	// malloc for an data-structure in VST-DSK, to send block of midi-msg :-/
	g_vsti_events = (VstEvents *)malloc(sizeof(VstEvents)+MAX_VSTI_PENDING_MIDIMSG*(sizeof(VstEvent *)));;
	g_vsti_events->numEvents = 0;
	g_vsti_events->reserved = 0;
	for (int nrEvent = 0; nrEvent < MAX_VSTI_PENDING_MIDIMSG; nrEvent++)
	{
		VstEvent *mvstevent = (VstEvent *)malloc(sizeof(VstMidiEvent));;
		g_vsti_events->events[nrEvent] = mvstevent;
		VstMidiEvent *midiEvent = (VstMidiEvent *)mvstevent;
		midiEvent->type = kVstMidiType;
		midiEvent->byteSize = sizeof(VstMidiEvent);
		midiEvent->midiData[0] = 0;
		midiEvent->midiData[1] = 0;
		midiEvent->midiData[2] = 0;
		midiEvent->midiData[3] = 0;
		midiEvent->deltaFrames = 0;	///< sample frames related to the current block start sample position
		midiEvent->flags = kVstMidiEventIsRealtime;			///< @see VstMidiEventFlags
		midiEvent->noteLength = 0;	 ///< (in sample frames) of entire note, if available, else 0
		midiEvent->noteOffset = 0;	 ///< offset (in sample frames) into note from note start if available, else 0
		midiEvent->detune = 0;			///< -64 to +63 cents; for scales other than 'well-tempered' ('microtuning')
		midiEvent->noteOffVelocity = 0;	///< Note Off Velocity [0, 127]
		midiEvent->reserved1 = 0;			///< zero (Reserved for future use)
		midiEvent->reserved2 = 0;			///< zero (Reserved for future use)
	}
}
static void vsti_free()
{
	for (int nrEvent = 0; nrEvent < MAX_VSTI_PENDING_MIDIMSG; nrEvent++)
	{
		VstEvent *mvstevent = g_vsti_events->events[nrEvent];
		free(mvstevent);
	}
	free(g_vsti_events);
}
#ifdef V_PC
static bool closeVSTi(T_vi_opened *vi)
{
    if ( vi->vsti_modulePtr == NULL)
        return false ;
    if (vi->vsti_plugins != NULL)
        vi->vsti_plugins->dispatcher(vi->vsti_plugins, effClose, 0, 0, NULL, 0.0f);
    vi->vsti_plugins = NULL;
    FreeLibrary(vi->vsti_modulePtr);
    vi->vsti_modulePtr = NULL ;
    return true ;
}
static bool openVSTi(char *fname , T_vi_opened *vi)
{
    vi->vsti_plugins = NULL ;
    vi->vsti_modulePtr = NULL ;
    
    wchar_t wtext[MAXBUFCHAR];
    mbstowcs(wtext, fname, strlen(fname) + 1);//Plus null
    LPWSTR sdll = wtext;
    vi->vsti_modulePtr = LoadLibrary(sdll);
    if (vi->vsti_modulePtr == NULL)
    {
        mlog("Failed trying to load VST from <%s>, error %d",	fname, GetLastError());
        return false;
    }
    
    LPCSTR spoint = "VSTPluginMain";
    vstPluginFuncPtr mainEntryPoint = (vstPluginFuncPtr)GetProcAddress(vi->vsti_modulePtr, spoint);
    if (mainEntryPoint == NULL)
    {
        spoint = "main";
        mainEntryPoint = (vstPluginFuncPtr)GetProcAddress(vi->vsti_modulePtr, spoint);
        if ( mainEntryPoint == NULL )
        {
            spoint = "main_macho";
            mainEntryPoint = (vstPluginFuncPtr)GetProcAddress(vi->vsti_modulePtr, spoint);
            if ( mainEntryPoint == NULL )
            {
                Mlog("Failed VSTPluginMain VST from <%s>, error %d", fname, GetLastError());
                closeVSTi(vi) ;
                return false;
            }
        }
    }
    // Instantiate the plugin
    vi->vsti_plugins = mainEntryPoint(hostCallback);
    if(vi->vsti_plugins == NULL)
    {
        mlog("Plugin's main() returns null for VSTi %s",fname);
        closeVSTi(vi);
        return false;
    }

    if (vi->vsti_plugins->magic != kEffectMagic) {
        mlog("Plugin magic number is bad <%s>",fname );
        closeVSTi(vi);
        return FALSE;
    }
    
    //dispatcher = (dispatcherFuncPtr)(plugin->dispatcher);
    //plugin->getParameter = (getParameterFuncPtr)plugin->getParameter;
    //plugin->processReplacing = (processFuncPtr)plugin->processReplacing;
    //plugin->setParameter = (setParameterFuncPtr)plugin->setParameter;
    
    
    int numOutputs = vi->vsti_plugins->numOutputs;
    if (numOutputs < 1)
    {
        mlog("Error : VST does not have stereo output <%s>", fname);
        closeVSTi(vi);
        return FALSE;
    }
    vi->vsti_nb_outputs = numOutputs;
    
    
    return true;
}
#endif
#ifdef V_MAC
static bool closeVSTi(T_vi_opened *vi)
{
    if ( vi->vsti_modulePtr == NULL)
        return false ;
    if (vi->vsti_plugins != NULL)
        vi->vsti_plugins->dispatcher(vi->vsti_plugins, effClose, 0, 0, NULL, 0.0f);
    vi->vsti_plugins = NULL;
    CFBundleUnloadExecutable(vi->vsti_modulePtr);
    CFRelease(vi->vsti_modulePtr);
    vi->vsti_modulePtr = NULL ;
    return true ;
}
static bool openVSTi(const char *fname , T_vi_opened *vi)
{
    vi->vsti_plugins = NULL ;
    vi->vsti_modulePtr = NULL ;
    
    // Create a path to the bundle
    CFStringRef pluginPathStringRef = CFStringCreateWithCString(NULL,fname, kCFStringEncodingASCII);
    CFURLRef bundleUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,pluginPathStringRef, kCFURLPOSIXPathStyle, true);
    if(bundleUrl == NULL)
    {
        mlog("Failed trying to load VST from <%s>",	fname);
        return false;
    }
    
    // Open the bundle
    vi->vsti_modulePtr = CFBundleCreate(kCFAllocatorDefault, bundleUrl);
    if(vi->vsti_modulePtr == NULL)
    {
        mlog("Couldn't create bundle reference for VSTi %s" , fname);
        CFRelease(pluginPathStringRef);
        vi->vsti_modulePtr = NULL ;
        return false;
    }
    
    // Clean up
    CFRelease(pluginPathStringRef);
    CFRelease(bundleUrl);
    
    vstPluginFuncPtr mainEntryPoint = NULL;
    mainEntryPoint = (vstPluginFuncPtr)CFBundleGetFunctionPointerForName(vi->vsti_modulePtr,CFSTR("VSTPluginMain"));
    // VST plugins previous to the 2.4 SDK used main_macho for the entry point name
    if(mainEntryPoint == NULL)
    {
        mainEntryPoint = (vstPluginFuncPtr)CFBundleGetFunctionPointerForName(vi->vsti_modulePtr, CFSTR("main"));
        if(mainEntryPoint == NULL)
        {
            mainEntryPoint = (vstPluginFuncPtr)CFBundleGetFunctionPointerForName(vi->vsti_modulePtr, CFSTR("main_macho"));
        }
    }
    
    if(mainEntryPoint == NULL)
    {
        mlog("Couldn't get a pointer to plugin's main() for VSTi %s",fname);
        closeVSTi(vi);
       return false;
    }
    
    audioMasterCallback hostCallbackFuncPtr ;
    hostCallbackFuncPtr = (audioMasterCallback)hostCallback;
    vi->vsti_plugins = mainEntryPoint(hostCallbackFuncPtr);
    if(vi->vsti_plugins == NULL)
    {
        mlog("Plugin's main() returns null for VSTi %s",fname);
        closeVSTi(vi);
        return false;
    }

    if (vi->vsti_plugins->magic != kEffectMagic) {
        mlog("Plugin magic number is bad <%s>",fname );
        closeVSTi(vi);
        return FALSE;
    }

    //dispatcher = (dispatcherFuncPtr)(plugin->dispatcher);
    //plugin->getParameter = (getParameterFuncPtr)plugin->getParameter;
    //plugin->processReplacing = (processFuncPtr)plugin->processReplacing;
    //plugin->setParameter = (setParameterFuncPtr)plugin->setParameter;
    
    
    int numOutputs = vi->vsti_plugins->numOutputs;
    if (numOutputs < 1)
    {
        mlog("Error : VST does not have stereo output <%s>", fname);
        closeVSTi(vi);
       return FALSE;
    }
    vi->vsti_nb_outputs = numOutputs;
    
    
    return true;
}
#endif
static bool vsti_start(const char *fname, char vsti_nr)
{
	T_vi_opened *vi = &(g_vi_opened[vsti_nr]);
    bool ret_code ;
    ret_code = openVSTi(fname , vi);
    if ( ! ret_code )
        return false ;

	vi->vsti_outputs = (float**)malloc(sizeof(float*)* vi->vsti_nb_outputs);
	for (int channel = 0; channel < vi->vsti_nb_outputs; channel++)
		vi->vsti_outputs[channel] = (float*)malloc(sizeof(float)* VSTI_BUFSIZE);
	for (int channel = 0; channel < vi->vsti_nb_outputs; ++channel)
	for (long frame = 0; frame < VSTI_BUFSIZE; ++frame)
		vi->vsti_outputs[channel][frame] = 0.0f;


	MidiProgramName mProgram;
	mProgram.thisProgramIndex = 0;
    VstIntPtr nbProgram ;
    nbProgram = vi->vsti_plugins->dispatcher(vi->vsti_plugins, effGetMidiProgramName, 0, 0, &mProgram, 0.0f);

	vi->vsti_plugins->dispatcher(vi->vsti_plugins, effOpen, 0, 0, NULL, 0.0f);

	// Set some default properties
	float sampleRate = (float)(M_SAMPLE_RATE);
	vi->vsti_plugins->dispatcher(vi->vsti_plugins, effSetSampleRate, 0, 0, NULL, sampleRate);
	vi->vsti_plugins->dispatcher(vi->vsti_plugins, effSetBlockSize, 0, VSTI_BUFSIZE, NULL, 0.0f);

	vi->vsti_plugins->dispatcher(vi->vsti_plugins, effMainsChanged, 0, 1, NULL, 0.0f);

	return true;
}
static void vsti_stop(char vsti_nr)
{
	T_vi_opened *vi = &(g_vi_opened[vsti_nr]);
	if (vi->mstream != 0)
		BASS_StreamFree(vi->mstream);
	vi->mstream = 0;
    closeVSTi(vi);
	if (vi->vsti_outputs != NULL)
	{
		for (int channel = 0; channel < vi->vsti_nb_outputs; channel++)
		{
			if (vi->vsti_outputs[channel] != NULL)
				free(vi->vsti_outputs[channel]);
			vi->vsti_outputs[channel] = NULL;
		}
		free(vi->vsti_outputs);
		vi->vsti_outputs = NULL;
	}
}
DWORD CALLBACK vsti_streamProc(HSTREAM handle, void *buffer, DWORD length, void * pvsti_nr)
{
    VstIntPtr intptr = (VstIntPtr)pvsti_nr ;
    int vsti_nr = (int)intptr;
	T_vi_opened *vi = &(g_vi_opened[vsti_nr]);

	lock_mutex_out();
	// send pending vst-program
	if (vi->vsti_todo_prog)
	{
		g_vi_opened[vsti_nr].vsti_plugins->dispatcher(g_vi_opened[vsti_nr].vsti_plugins, effSetProgram, 0, vi->vsti_last_prog, NULL, 0.0f);
		vi->vsti_todo_prog = false;
	}
	// send pending midi messages
	if (vi->vsti_nb_pending_midimsg > 0)
	{
		g_vsti_events->numEvents = vi->vsti_nb_pending_midimsg;
		for (int nrEvent = 0; nrEvent < vi->vsti_nb_pending_midimsg; nrEvent++)
		{
			VstEvent *mvstevent = g_vsti_events->events[nrEvent] ;
			VstMidiEvent *midiEvent = (VstMidiEvent *)mvstevent;
			midiEvent->midiData[0] = vi->vsti_pending_midimsg[nrEvent].bData[0];
			midiEvent->midiData[1] = vi->vsti_pending_midimsg[nrEvent].bData[1];
			midiEvent->midiData[2] = vi->vsti_pending_midimsg[nrEvent].bData[2];
		}
		vi->vsti_plugins->dispatcher(vi->vsti_plugins, effProcessEvents, 0, 0, g_vsti_events, 0.0f);
		vi->vsti_nb_pending_midimsg = 0;
	}
	unlock_mutex_out();


	float *fbuf = (float *)buffer;
	int nbfloat = length / (sizeof(float) * vi->vsti_nb_outputs);
	float ** ouput = vi->vsti_outputs;
	vi->vsti_plugins->processReplacing(vi->vsti_plugins, NULL, ouput, nbfloat);
	float *pt[10];
	for (int nr_channel = 0; nr_channel < vi->vsti_nb_outputs; nr_channel++)
		pt[nr_channel] = ouput[0];
	for (long frame = 0; frame < nbfloat; ++frame)
	{
		for (int nr_channel = 0; nr_channel < vi->vsti_nb_outputs; nr_channel++)
		{
			*fbuf = *(pt[nr_channel]);
			(pt[nr_channel]) ++;
			fbuf++;
		}
	}
	return length;
}
static void sf2_send_shortmsg(int nr_device, T_midimsg msg)
{
	BYTE channel = msg.bData[0] & 0x0F;
	HSTREAM mstream = g_vi_opened[nr_device].mstream;
	// mlog("vi_send nr_device = %d , channel =%d", nr_device, channel);
	static int vi_rpn_msb = 0;
	static int vi_rpn_lsb = 0;
	switch (msg.bData[0] >> 4)
	{
	case MIDI_NOTEON: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_NOTE, MAKEWORD(msg.bData[1], msg.bData[2])); break;
	case MIDI_NOTEOFF: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_NOTE, MAKEWORD(msg.bData[1], 0)); break;
	case MIDI_PROGRAM: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_PROGRAM, msg.bData[1]); break;
	case MIDI_PITCHBEND:	BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_PITCH, pitchbend_value(msg) + 0x2000); break;
	case MIDI_CHANNELPRESSURE: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_CHANPRES, msg.bData[1]); break;
	case MIDI_CONTROL:
		switch (msg.bData[1])
		{
		case 0: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_BANK, msg.bData[2]); break;
		case 1: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_MODULATION, msg.bData[2]); break;
		case 5: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_PORTATIME, msg.bData[2]); break;
		case 7:	BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_VOLUME, msg.bData[2]); break;
		case 10: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_PAN, msg.bData[2]); break;
		case 11: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_EXPRESSION, msg.bData[2]); break;
		case 64: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_SUSTAIN, msg.bData[2]); break;
		case 65: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_PORTAMENTO, msg.bData[2]); break;
		case 71: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_RESONANCE, msg.bData[2]); break;
		case 72: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_RELEASE, msg.bData[2]); break;
		case 73: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_ATTACK, msg.bData[2]); break;
		case 74: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_CUTOFF, msg.bData[2]); break;
		case 84: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_PORTANOTE, msg.bData[2]); break;
		case 91: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_REVERB, msg.bData[2]); break;
		case 93: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_CHORUS, msg.bData[2]); break;
		case 120: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_SOUNDOFF, 0); break;
		case 121: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_RESET, 0); break;
		case 123: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_NOTESOFF, 0); break;
		case 126: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_MODE, msg.bData[2]); break;
		case 127: BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_MODE, msg.bData[2]); break;

		case 100: vi_rpn_msb = msg.bData[2]; break;
		case 101: vi_rpn_lsb = msg.bData[2]; break;
		case 6:
			if (vi_rpn_msb == 0)
			{
				switch (vi_rpn_lsb)
				{
				case 0:BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_PITCHRANGE, msg.bData[2]); break;
				case 1:BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_FINETUNE, msg.bData[2]); break;
				case 2:BASS_MIDI_StreamEvent(mstream, channel, MIDI_EVENT_COARSETUNE, msg.bData[2]); break;
				default: break;
				}
			}
			break;

		default: break; // other CTRL CHG ignored
		}
	default: break;
	}
}
static bool sf2_create_list_prog(const char *fname)
{
	HSOUNDFONT hvi = BASS_MIDI_FontInit((void*)fname, 0);
	if (hvi == 0)
	{
		mlog("Error BASS_MIDI_FontInit %s, err#%d",fname, BASS_ErrorGetCode());
		return(false);
	}
	const char *name_preset;
	FILE *ftxt;
	char fnameext[MAXBUFCHAR];
	if ((strlen(fname) > 5) && ((strncmp(fname + strlen(fname) - 4, ".sf2", 4) == 0) || (strncmp(fname + strlen(fname) - 4, ".SF2", 4) == 0)))
	{
		char fnamessext[MAXBUFCHAR];
		strcpy(fnamessext, fname);
		fnamessext[strlen(fname) - 4] = '\0';
		sprintf(fnameext, "%s.txt", fnamessext);
	}
	else
		sprintf(fnameext, "%s.txt", fname);
	if ((ftxt = fopen(fnameext, "r")) != NULL)
	{
		fclose(ftxt);
		return(true);
	}
	if ((ftxt = fopen(fnameext, "w")) == NULL)
	{
		mlog("mlog opening vi text list %s err=%d\n", fnameext, errno);
		return false;
	}
	for (int bank = 0; bank < 127; bank++)
	{
		for (int program = 0; program < 127; program++)
		{
			name_preset = BASS_MIDI_FontGetPreset(hvi, program, bank);
			if (name_preset != 0)
				fprintf(ftxt, "%s(P%d/%d)\n", name_preset, bank, program);
		}
	}
	fclose(ftxt);
	return true;
}
static bool vst_create_list_prog(const char *fname)
{
    T_vi_opened vi ;
    bool ret_code ;
    ret_code = openVSTi(fname , &vi);
    if ( ! ret_code )
        return false ;
    
	FILE *ftxt;
	char fnameext[MAXBUFCHAR];
	if ((strlen(fname) > 5) && ((strncmp(fname + strlen(fname) - 4, ".dll", 4) == 0) || (strncmp(fname + strlen(fname) - 4, ".DLL", 4) == 0)))
	{
		char fnamessext[MAXBUFCHAR];
		strcpy(fnamessext, fname);
		fnamessext[strlen(fname) - 4] = '\0';
		sprintf(fnameext, "%s.txt", fnamessext);
	}
	else
		sprintf(fnameext, "%s.txt", fname);
	if ((ftxt = fopen(fnameext, "r")) != NULL)
	{
		fclose(ftxt);
		return(true);
	}
	if ((ftxt = fopen(fnameext, "w")) == NULL)
	{
		mlog("mlog opening vi text list %s err=%d\n", fnameext, errno);
		closeVSTi(&vi);
		return false;
	}

	MidiProgramName mProgram;
	mProgram.thisProgramIndex = 0;
    VstIntPtr nbProgram ;
    nbProgram = vi.vsti_plugins->dispatcher(vi.vsti_plugins, effGetMidiProgramName, 0, 0, &mProgram, 0.0f);
	if (nbProgram > 0)
	{
		for (int nrProgram = 0; nrProgram < nbProgram; nrProgram++)
		{
			mProgram.thisProgramIndex = nrProgram;
			vi.vsti_plugins->dispatcher(vi.vsti_plugins, effGetMidiProgramName, 0, 0, &mProgram, 0.0f);
			if ((mProgram.midiProgram >= 0) && (mProgram.midiBankLsb < 0) && (mProgram.midiBankMsb < 0))
				fprintf(ftxt, "%s(P%d)\n", mProgram.name, mProgram.midiProgram);
			if ((mProgram.midiProgram >= 0) && (mProgram.midiBankLsb >= 0) && (mProgram.midiBankMsb < 0))
				fprintf(ftxt, "%s(P%d/%d)\n", mProgram.name, mProgram.midiBankLsb, mProgram.midiProgram);
			if ((mProgram.midiProgram >= 0) && (mProgram.midiBankLsb >= 0) && (mProgram.midiBankMsb >= 0))
				fprintf(ftxt, "%s(P%d/%d/%d)\n", mProgram.name, mProgram.midiBankMsb, mProgram.midiBankLsb, mProgram.midiProgram);

		}
	}
	else
	{
		int numPrograms = vi.vsti_plugins->numPrograms;
		char nameProgram[kVstMaxProgNameLen];
		for (int nrProgram = 0; nrProgram < numPrograms; nrProgram++)
		{
			bool retCode = vi.vsti_plugins->dispatcher(vi.vsti_plugins, effGetProgramNameIndexed, nrProgram, 0, nameProgram, 0.0f);
			if (retCode)
			{
				fprintf(ftxt, "%s_vst(P99/%d)\n", nameProgram, nrProgram);
			}
		}
	}

	fclose(ftxt);
    closeVSTi(&vi);
	return true;
}
static void sf2_stop(int vsti_nr)
{
	T_vi_opened *vi = &(g_vi_opened[vsti_nr]);
	BASS_MIDI_FontFree(vi->sf2_midifont);
	vi->sf2_midifont = 0;
	BASS_StreamFree(vi->mstream);
	vi->mstream = 0;
}
static int mixer_create(int nr_deviceaudio)
{
#ifdef V_PC
	if (g_audio_open[nr_deviceaudio])
		return nr_deviceaudio;
	// not playing anything via BASS, so don't need an update thread
	BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 0);
	// setup BASS - "no sound" device
	BASS_Init(0, 48000, 0, 0, NULL);
	g_audio_open[nr_deviceaudio] = true;
	if (BASS_ASIO_Init(nr_deviceaudio) == FALSE)
	{
		mlog("Error BASS_ASIO_Init device#%d , err=%d\n", nr_deviceaudio + 1, BASS_ASIO_ErrorGetCode());
		return(-1);
	}
	BASS_ASIO_SetDevice(nr_deviceaudio);
	BASS_ASIO_SetRate(M_SAMPLE_RATE);
	g_mixer_stream[nr_deviceaudio] = BASS_Mixer_StreamCreate(M_SAMPLE_RATE, 2, BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
	if (!g_mixer_stream[nr_deviceaudio])
	{
		mlog("Error asio BASS_Mixer_StreamCreate, err=%d\n", BASS_ErrorGetCode());
		return(-1);
	}
	// setup ASIO stuff
	BASS_CHANNELINFO i;
	BASS_ChannelGetInfo(g_mixer_stream[nr_deviceaudio], &i);
	BASS_ASIO_ChannelEnable(0, 0, &asioProc, (void*)g_mixer_stream[nr_deviceaudio]); // enable 1st output channel...
	for (unsigned int a = 1; a<i.chans; a++)
		BASS_ASIO_ChannelJoin(0, a, 0); // and join the next channels to it
	if (i.chans == 1) BASS_ASIO_ChannelEnableMirror(1, 0, 0); // mirror mono channel to form stereo output
	BASS_ASIO_ChannelSetFormat(0, 0, BASS_ASIO_FORMAT_FLOAT); // set the source format (float)
	BASS_ASIO_ChannelSetRate(0, 0, i.freq); // set the source rate
	BASS_ASIO_SetRate(i.freq); // try to set the device rate too (saves resampling)
	if (!BASS_ASIO_Start(g_audio_buffer_length)) // start output using default buffer/latency
	{
		mlog("Error BASS_ASIO_start device#%d , err=%d\n", nr_deviceaudio + 1, BASS_ASIO_ErrorGetCode());
		return -1;
	}
	else
		mlog("Information : ASIO start #device %d OK", nr_deviceaudio + 1);
#else
	if (!g_mixer_stream[nr_deviceaudio])
	{
		if (BASS_Init(nr_deviceaudio, M_SAMPLE_RATE, 0, 0, NULL) == FALSE)
		{
			mlog("Error BASS_Init device#%d, err=%d\n", nr_deviceaudio + 1, BASS_ErrorGetCode());
			return(-1);
		}
		nr_deviceaudio = BASS_GetDevice(); // Normally , return the device set on the thread opened previously
		if (nr_deviceaudio == -1)
		{
			mlog("Error BASS_Init device#%d , err=%d\n", nr_deviceaudio + 1, BASS_ErrorGetCode());
			return(-1);
		}
		g_mixer_stream[nr_deviceaudio] = BASS_Mixer_StreamCreate(M_SAMPLE_RATE, 2, 0);
		if (g_mixer_stream[nr_deviceaudio] == 0)
		{
            mlog("Error BASS_Mixer_StreamCreate , err=%d\n", BASS_ErrorGetCode());
            return(-1);
		}
	}
#endif
	mlog("Information : audio mixer device#%d create : OK", nr_deviceaudio + 1);
	return(nr_deviceaudio);
}
static void mixer_init()
{
	for (int n = 0; n < MAX_AUDIO_DEVICE; n++)
	{
		g_mixer_stream[n] = 0;
		g_audio_open[n] = false;
	}
	int nr_device = 0;
	char name_audio[MAXBUFCHAR];
	while (audio_name(nr_device, name_audio))
	{
		mlog("Information : Audio asio interface <%s> #%d", name_audio, nr_device + 1);
		nr_device++;
	}
}
static void mixer_free()
{
	bool found = false;
	for (int n = 0; n < MAX_AUDIO_DEVICE; n++)
	{
		if (g_mixer_stream[n] > 0)
		{
			found = true;
			if (BASS_StreamFree(g_mixer_stream[n]) == FALSE)
				mlog("Error free mixer on device audio #%d Err=%d", n + 1, BASS_ErrorGetCode());
			else
				mlog("Information : free mixer on device audio #%d OK", n + 1);
#ifdef V_PC
			BASS_ASIO_SetDevice(n);
			if (BASS_ASIO_Stop() == FALSE)
				mlog("Error stop ASIO device audio #%d Err=%d", n + 1, BASS_ErrorGetCode());
			else
				mlog("Information : stop ASIO device#%d OK", n + 1);
			if (BASS_ASIO_Free() == FALSE)
				mlog("Error free ASIO device#%d Err=%d", n + 1, BASS_ErrorGetCode());
			else
				mlog("Information : free ASIO device#%d  OK", n + 1);
#endif
		}
		g_mixer_stream[n] = 0;
	}
#ifdef V_PC
	if (found)
	{
		BASS_Free();
		Sleep(2000);
	}

#else
	if (found)
		BASS_Free();
#endif
}
static int vi_open(const char *fname, int nr_deviceaudio, bool sf2)
{
	T_vi_opened *vi;
	// fname is the full name ( *.dll for a vsti or *.sf2 for an sf2 )
	for (int nr_vi = 0; nr_vi < VI_MAX; nr_vi++)
	{
		vi = &(g_vi_opened[nr_vi]);
		if ((strcmp(vi->filename, fname) == 0) && (vi->nr_device_audio == nr_deviceaudio))
		{
			// VI allready opened in the same audio channel
			mlog("Information : open vi<%s> audio-device#%d : already open", fname, nr_deviceaudio + 1);
			return (nr_vi);
		}
	}
	int nr_vi = g_vi_opened_nb;
	g_vi_opened_nb++;
	vi = &(g_vi_opened[nr_vi]);
	// new VI stream to create open vi
	strcpy(vi->filename, fname);
	vi->nr_device_audio = nr_deviceaudio;

	if (mixer_create(nr_deviceaudio) == -1) return(-1);
	if (sf2)
	{
		// connect a midi-channel on the mixer-device
		vi->mstream = BASS_MIDI_StreamCreate(MAXCHANNEL, BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT, M_SAMPLE_RATE);
		if (vi->mstream == 0)
		{
			mlog("Error BASS_MIDI_StreamCreate VI, err=%d", BASS_ErrorGetCode());
			return(-1);
		}
		if (BASS_Mixer_StreamAddChannel(g_mixer_stream[nr_deviceaudio], vi->mstream, 0) == FALSE)
		{
			mlog("Error BASS_Mixer_StreamAddChannel VI , err=%d", BASS_ErrorGetCode());
			return -1;
		}
		vi->sf2_midifont = BASS_MIDI_FontInit((void*)fname, 0);
		if (vi->sf2_midifont == 0)
		{
			mlog("Error BASS_MIDI_FontInit <%s> , err=%d", fname, BASS_ErrorGetCode());
			return -1;
		}
		// connect a font in the midi-channel
		if (BASS_MIDI_FontLoad(vi->sf2_midifont, -1, -1) == FALSE)
		{
			mlog("Error BASS_MIDI_FontLoad <%s>, err=%d", fname, BASS_ErrorGetCode());
			return -1;
		}
		BASS_MIDI_FONT mfont;
		mfont.font = vi->sf2_midifont;
		mfont.preset = -1;
		mfont.bank = 0;
		if (BASS_MIDI_StreamSetFonts(vi->mstream, &mfont, 1) == FALSE)
		{
			mlog("Error BASS_MIDI_StreamSetFonts <%s> , err=%d", fname, BASS_ErrorGetCode());
			return -1;
		}
	}
	else
	{
		// load the vsti
		if (vsti_start(fname, nr_vi) == false)
		{
			mlog("Error vsti_start vi <%s>", fname);
			return(-1);
		}
		// connect the vsti on a new stream, via a callback vsti_streamProc
        VstIntPtr intptr = nr_vi ;
        void *voidptr = (void*)intptr ;
		vi->mstream = BASS_StreamCreate(M_SAMPLE_RATE, vi->vsti_nb_outputs, BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT, &vsti_streamProc, voidptr);
		if (vi->mstream == 0)
		{
			mlog("Error BASS_MIDI_StreamCreate vi<%s>, err=%d", fname , BASS_ErrorGetCode());
			return(-1);
		}
		// connect the stream-channel on the mixer-device
		if (BASS_Mixer_StreamAddChannel(g_mixer_stream[nr_deviceaudio], vi->mstream, BASS_MIXER_DOWNMIX | BASS_MIXER_NORAMPIN ) == FALSE)
		{
			mlog("Error BASS_Mixer_StreamAddChannel vi<%s> , err=%d", fname, BASS_ErrorGetCode());
			return(-1);
		}
	}
	mlog("Information : open vi<%s> audio-device#%d : OK", fname, nr_deviceaudio + 1);
	return nr_vi;
}
static void vi_init()
{
	mixer_init();
	g_vi_opened_nb = 0;
	for (int n = 0; n < VI_MAX; n++)
	{
		g_vi_opened[n].mstream = 0;
		g_vi_opened[n].sf2_midifont = 0;
		g_vi_opened[n].filename[0] = '\0';
		g_vi_opened[n].nr_device_audio = -1;
		g_vi_opened[n].vsti_plugins = NULL;
		g_vi_opened[n].vsti_modulePtr = NULL;
		g_vi_opened[n].vsti_outputs = NULL;
		g_vi_opened[n].vsti_last_prog = -1;
		g_vi_opened[n].vsti_todo_prog = false;
		g_vi_opened[n].vsti_nb_pending_midimsg = 0;
		g_vi_opened[n].vsti_midi_prog = true;
		g_vi_opened[n].vsti_nb_outputs = 2;
	}
	vsti_init();
}
static void vi_free()
{
	for (int nr_vi = 0; nr_vi < g_vi_opened_nb; nr_vi++)
	{
		if (g_vi_opened[nr_vi].sf2_midifont != 0)
			sf2_stop(nr_vi);
		if (g_vi_opened[nr_vi].vsti_plugins != NULL)
			vsti_stop(nr_vi);
	}
	g_vi_opened_nb = 0;
	vsti_free();
}
static int sound_play(const char*fname, int volume, int pan, int nr_deviceaudio)
{
	if (mixer_create(nr_deviceaudio) == -1) return(-1);
	HSTREAM hsound = BASS_StreamCreateFile(FALSE, fname, 0, 0, BASS_STREAM_DECODE);
	if (!hsound)
		return(mlog("Error BASS_StreamCreateFile mixer %s, err=%d\n", fname, BASS_ErrorGetCode()));
	BASS_ChannelSetAttribute(hsound, BASS_ATTRIB_VOL, (float)(volume) / 64.0);
	BASS_ChannelSetAttribute(hsound, BASS_ATTRIB_PAN, (float)(pan - 64) / 64.0);
	if (BASS_Mixer_StreamAddChannel(g_mixer_stream[nr_deviceaudio], hsound, BASS_STREAM_AUTOFREE) == FALSE)
		return(mlog("Error BASS_Mixer_StreamAddChannel, err=%d\n", BASS_ErrorGetCode()));
	return(hsound);
}
static int sound_control(HSTREAM hsound, int volume, int pan, int ctrl)
{
	int return_code = BASS_ChannelSetAttribute(hsound, BASS_ATTRIB_VOL, (float)(volume) / 64.0);
	if (return_code)
	{
		BASS_ChannelSetAttribute(hsound, BASS_ATTRIB_PAN, (float)(pan - 64) / 64.0);
		switch (ctrl)
		{
		case 0: BASS_ChannelPause(hsound); break;
		case 1: BASS_ChannelPlay(hsound, FALSE); break;
		case 2:BASS_ChannelStop(hsound); break;
		default: break;
		}
	}
	return(return_code);
}
static void picth_init()
{
	for (int n = 0; n < OUT_MAX_DEVICE; n++)
	{
		for (int c = 0; c < MAXCHANNEL; c++)
		{
			for (int p = 0; p < MAXPITCH; p++)
			{
				g_midistatuspitch[n][c][p] = -1;
				g_miditimepitch[n][c][p] = 0;
				g_midistatuscontrol[n][c][p] = -1;
				g_miditimecontrol[n][c][p] = 0;
			}
		}
	}
}
static int send_sysex(int nrTrack, const char *sysex)
{
	if ((nrTrack < 0) || (nrTrack >= MAXTRACK))
		return -1;
	int nr_device = g_tracks[nrTrack].device;
	if ((nr_device < 0) || (nr_device >= MIDIOUT_MAX) || (g_midiopened[nr_device] == 0))
		return -1;

	// translate ASCII Hexa format to binary byffer
	// e.g. GM-Modesysex = "F0 7E 7F 09 01 F7";

	char *bufstr;
	bufstr = (char*)malloc(strlen(sysex) + 2);
	strcpy(bufstr, sysex);
	bufstr[strlen(sysex)] = '\0';
	int nbByte = 0;
	char * pch;
	char *next_token = NULL;
    pch = strtok_s(bufstr, " ,;.-", &next_token);
	while (pch != NULL)
	{
		nbByte++;
        pch = strtok_s(NULL, " ,.-", &next_token);
	}
	BYTE *buf;
	buf = (BYTE*)malloc(nbByte + 1);
	strcpy(bufstr, sysex);
	bufstr[strlen(sysex)] = '\0';
    pch = strtok_s(bufstr, " ,;.-", &next_token);
	nbByte = 0;
	while (pch != NULL)
	{
		buf[nbByte] = (BYTE)strtol(pch, NULL, 16);
		nbByte++;
        pch = strtok_s(NULL, " ,.-", &next_token);
	}
	free(bufstr);
	if (nbByte < 4)
	{
		free(buf);
		return -1;
	}
	// send the sysex in binary
#ifdef V_PC
	MIDIHDR midiHdr;
	midiHdr.lpData = (LPSTR)(malloc(nbByte + 1));
	midiHdr.dwBufferLength = (DWORD)(nbByte);
	midiHdr.dwFlags = 0;
	memcpy(midiHdr.lpData, buf, nbByte);
	if (midiOutPrepareHeader(g_midiopened[nr_device], &midiHdr, sizeof(MIDIHDR)) == MMSYSERR_NOERROR)
	{
		int antiloop = 0;
		while ((midiHdr.dwFlags & MHDR_PREPARED) != MHDR_PREPARED)
		{
			if (antiloop > 500)
			{
				midiOutUnprepareHeader(g_midiopened[nr_device], &midiHdr, sizeof(MIDIHDR));
				free(midiHdr.lpData);
				free(buf);
				// mlog("sysex : mlog timeout midiOutPrepareHeader flags=%d\n", midiHdr.dwFlags);
				return(-1);
			}
			antiloop++;
		}
		if (midiOutLongMsg(g_midiopened[nr_device], &midiHdr, sizeof(MIDIHDR)) != MMSYSERR_NOERROR)
		{
			midiOutUnprepareHeader(g_midiopened[nr_device], &midiHdr, sizeof(MIDIHDR));
			free(midiHdr.lpData);
			free(buf);
			// mlog("sysex : mlog creation midiOutLongMsg\n");
			return(-1);
		}
		antiloop = 0;
		while ((midiHdr.dwFlags & MHDR_DONE) != MHDR_DONE)
		{
			if (antiloop > 500)
			{
				midiOutUnprepareHeader(g_midiopened[nr_device], &midiHdr, sizeof(MIDIHDR));
				free(midiHdr.lpData);
				free(buf);
				return(-1);
			}
			antiloop++;
		}
		if (midiOutUnprepareHeader(g_midiopened[nr_device], &midiHdr, sizeof(MIDIHDR)) != MMSYSERR_NOERROR)
		{
			free(midiHdr.lpData);
			free(buf);
			return(-1);
		}
		free(midiHdr.lpData);
		free(buf);
		return(0);
	}
	else
	{
		free(midiHdr.lpData);
		free(buf);
		return(-1);
	}
	free(buf);
	return(-1);
#endif
#ifdef V_MAC
    // envoi du message version Mac
    BYTE  *bufferData;
    bufferData = (BYTE*)(malloc(nbByte + 1)); // sera libere dans le callback completionSysex
    memcpy(bufferData, buf, nbByte);
    MIDISysexSendRequest *request = (MIDISysexSendRequest *)(malloc(sizeof(MIDISysexSendRequest)+1)); // sera libere dans le callback completionSysex
    request->destination = g_midiopened[nr_device];
    request->data = bufferData;
    request->bytesToSend = nbByte;
    request->complete = false;
    request->completionProc = NULL;
    request->completionRefCon = NULL;
    MIDISendSysex(request);
    free(buf);
    return(0);
#endif
}
static void queue_insert(const T_midioutmsg midioutmsg)
{
	T_queue_msg* pt = g_queue_msg;
	bool found = false;
	for (int n = 0; n < OUT_QUEUE_MAX_MSG; n++ , pt ++)
	{
		if (pt->free)
		{
			if (n >= g_end_queue_msg)
				g_end_queue_msg = n + 1;
			found = true;
			break;
		}
	}
	if (! found)
	{
		return;
	}
	pt->free = false;
	pt->midioutmsg = midioutmsg;
	pt->t = g_current_t + midioutmsg.dt;
	if (g_end_queue_msg > g_max_queue_msg)
		g_max_queue_msg = g_end_queue_msg;
}
static bool sendmidimsg(T_midioutmsg midioutmsg, bool first);
static bool processPostMidiOut(T_midioutmsg midioutmsg)
{
	int type_msg = (midioutmsg.midimsg.bData[0] & 0xF0) >> 4;
	switch (type_msg)
	{
	case MIDI_NOTEON:
		if (midioutmsg.midimsg.bData[2] > 0)
		{
			if (!g_process_NoteOn) return(false);
			lua_getglobal(g_LUAoutState, LUAFunctionNoteOn);
		}
		else
		{
			if (!g_process_NoteOff) return(false);
			lua_getglobal(g_LUAoutState, LUAFunctionNoteOff);
		}
		break;
	case MIDI_NOTEOFF:
		if (!g_process_NoteOff) return(false);
		lua_getglobal(g_LUAoutState, LUAFunctionNoteOff);
		break;
	case MIDI_PROGRAM:
		if (!g_process_Program) return(false);
		lua_getglobal(g_LUAoutState, LUAFunctionProgram);
		break;
	case MIDI_CONTROL:
		if (!g_process_Control) return(false);
		lua_getglobal(g_LUAoutState, LUAFunctionControl);
		break;
	case MIDI_KEYPRESSURE:
		if (!g_process_KeyPressure) return(false);
		lua_getglobal(g_LUAoutState, LUAFunctionKeyPressure);
		break;
	case MIDI_CHANNELPRESSURE:
		if (!g_process_ChannelPressure) return(false);
		lua_getglobal(g_LUAoutState, LUAFunctionChannelPressure);
		break;
	case MIDI_CLOCK:
		if (!g_process_Clock) return(false);
		lua_getglobal(g_LUAoutState, LUAFunctionClock);
		break;
	case MIDI_SYSTEMCOMMON:
		if (!g_process_SystemCommon) return(false);
		lua_getglobal(g_LUAoutState, LUAFunctionSystemCommon);
		break;
	default:
		return(false);
	}
	lua_pushinteger(g_LUAoutState, midioutmsg.track + 1);
	int nbParam = 1;
	switch (type_msg)
	{
	case MIDI_CLOCK:
		break;
	case MIDI_CHANNELPRESSURE:
	case MIDI_PROGRAM:
		lua_pushinteger(g_LUAoutState, midioutmsg.midimsg.bData[1]); // program#
		nbParam += 1;
		break;
	case MIDI_PITCHBEND:
		lua_pushinteger(g_LUAoutState, pitchbend_value(midioutmsg.midimsg)); // pitchend value
		nbParam += 1;
		break;
	default:
		lua_pushinteger(g_LUAoutState, midioutmsg.midimsg.bData[1]); // pitch#
		lua_pushinteger(g_LUAoutState, midioutmsg.midimsg.bData[2]); // velocity for note, value for control , or msb pitchbend
		nbParam += 2;
		break;
	}
	if (lua_pcall(g_LUAoutState, nbParam, LUA_MULTRET, 0) != LUA_OK)
	{
		mlog("erreur onMidiOut calling LUA , err: %s", lua_tostring(g_LUAoutState, -1));
		lua_pop(g_LUAoutState, 1);
		return(false);
	}
	else
	{
		int dtIdMidioutPost = 10000;
		// pop midi-msgs from the returned values, to send it imediatly 
		while (lua_gettop(g_LUAoutState) >= 4)
		{
			// param 1 : integer track 1..MAXTRACK
			// param 2 : string type msg ( MIDI_NOTEON, MIDI_NOTEOFF, MIDI_CONTROL, MIDI_PROGRAM, MIDI_CHANNELPRESSURE, MIDI_KEYPRESSURE, MIDI_PITCHBEND )
			// param 3 : integer byte 1 ( e.g. pitch )
			// param 4 : integer byte 2 ( e.g. velocity )
			T_midioutmsg midioutpostmsg;
			midioutpostmsg.id = midioutmsg.id + (dtIdMidioutPost++);
			midioutpostmsg.dt = 0;
			midioutpostmsg.track = cap(lua_tonumber(g_LUAoutState, -4), 0, MAXTRACK, 1);
			const char *stype = lua_tostring(g_LUAoutState, -3);
			int min = 0;
			bool typeOk = true;
			midioutpostmsg.nbbyte = 3;
			midioutpostmsg.midimsg.bData[1] = cap(lua_tonumber(g_LUAoutState, -2), 0, 128, 0);
			midioutpostmsg.midimsg.bData[2] = cap(lua_tonumber(g_LUAoutState, -1), min, 128, 0);
			switch (stype[0])
			{
			case 'P':
			case 'p':
				if (strlen(stype) > 7)
					midioutpostmsg.midimsg.bData[0] = (MIDI_PITCHBEND << 4);
				else
				{
					midioutpostmsg.midimsg.bData[0] = (MIDI_PROGRAM << 4);
					midioutpostmsg.nbbyte = 2;
				}
				break;
			case 'N':
			case 'n':
				if (strlen(stype) > 6)
				{
					midioutpostmsg.midimsg.bData[0] = (MIDI_NOTEOFF << 4);
				}
				else
				{
					midioutpostmsg.midimsg.bData[0] = (MIDI_NOTEON << 4);
					min = 1;
				}
				break;
			case 'C':
			case 'c':
				if (strlen(stype) > 6)
				{
					midioutpostmsg.midimsg.bData[0] = (MIDI_CHANNELPRESSURE << 4);
				}
				else
					midioutpostmsg.midimsg.bData[0] = (MIDI_CONTROL << 4);
				break;
			case 'K':
			case 'k':
				midioutpostmsg.midimsg.bData[0] = (MIDI_KEYPRESSURE << 4);
				break;
			default:
				typeOk = false;
				break;
			}
			if (typeOk)
				sendmidimsg(midioutpostmsg, false);
			lua_pop(g_LUAoutState, 4);
		}
		lua_pop(g_LUAoutState, lua_gettop(g_LUAoutState));
	}
	return(true);
}
static bool sendshortmsg(T_midioutmsg midioutmsg, bool first)
{
	if (g_collectLog)
		mlog("sendshortmsg device=%d msg=%d ch=%d p=%d v=%d", g_tracks[midioutmsg.track].device, midioutmsg.midimsg.bData[0] >> 4, midioutmsg.midimsg.bData[0] & 0xF, midioutmsg.midimsg.bData[1], midioutmsg.midimsg.bData[2]);
	int nr_device = g_tracks[midioutmsg.track].device;
	if (nr_device >= VI_ZERO) 
    {
		int nrvi = nr_device - VI_ZERO;
		if (g_vi_opened[nrvi].sf2_midifont != 0)
			sf2_send_shortmsg(nrvi, midioutmsg.midimsg);
		if (g_vi_opened[nrvi].vsti_plugins != 0)
			vsti_send_shortmsg(nrvi, midioutmsg.midimsg);
		return true;
    }
	else
	{
#ifdef V_PC
		if (midiOutShortMsg(g_midiopened[nr_device], midioutmsg.midimsg.dwData) != MMSYSERR_NOERROR)
			return false;
#endif
#ifdef V_MAC
		OSStatus err;
		// envoi du message Midi version Mac dans un packet
		Byte buffer[1024];
		MIDIPacketList *pktlist = (MIDIPacketList *)buffer;
		MIDIPacket *curPacket = MIDIPacketListInit(pktlist);
		curPacket = MIDIPacketListAdd(pktlist, sizeof(buffer), curPacket, 0, midioutmsg.nbbyte, & (midioutmsg.midimsg.bData[0] ));
		err = MIDISend(g_midiOutPortRef, g_midiopened[nr_device], pktlist);
		if (err != 0)
			return false;
#endif
	}
	return true;
}
static bool sendmidimsg(T_midioutmsg midioutmsg , bool first)
{
	// process midiout messages
	if (first && (g_LUAoutState) && processPostMidiOut(midioutmsg))
		return true;

	// check the non replication of Note-on on a same channel, and repair it if neccesary

	int type_msg = (midioutmsg.midimsg.bData[0] & 0xF0) >> 4;
	int nr_device = g_tracks[midioutmsg.track].device;
	int nr_channel = g_tracks[midioutmsg.track].channel;
	int pitch = midioutmsg.midimsg.bData[1];
	bool retCode = true;
	switch (type_msg)
    {
    case MIDI_NOTEON:
		if (g_transposition != 0)
		{
			int p = midioutmsg.midimsg.bData[1] + g_transposition;
			while (p < 0)
				p += 12;
			while (p > 127)
				p -= 12;
			midioutmsg.midimsg.bData[1] = p;
		}
		midioutmsg.midimsg.bData[2] = apply_volume(midioutmsg.track, midioutmsg.midimsg.bData[2]);
		for (int c = 0; c < MAXCHANNEL; c++)
		{
			// search a noteon slot free in the extended channels
			if (g_channels[nr_device][c].extended == nr_channel)
			{
				if (g_midistatuspitch[nr_device][c][pitch] == -1 )
				{
					midioutmsg.midimsg.bData[0] = (MIDI_NOTEON << 4) + (BYTE)c;
					sendshortmsg(midioutmsg,first);
					g_midistatuspitch[nr_device][c][pitch] = midioutmsg.id;
					g_miditimepitch[nr_device][nr_channel][pitch] = g_current_t;
					// mlog("note p=%d sent on device#%d channel#%d", pitch, nr_device,c);
					return(true);
				}
			}
		}
		// filter flooding of same [pitch] in short period
		if ((g_midistatuspitch[nr_device][nr_channel][pitch] != midioutmsg.id) && (g_miditimepitch[nr_device][nr_channel][pitch] < (g_current_t - 200)))
		{
			// no slot for this new note : note-off and note-on
			midioutmsg.midimsg.bData[0] = (MIDI_NOTEOFF << 4) + (BYTE)nr_channel;
			if (sendshortmsg(midioutmsg, first) == false)
				return(false);
			midioutmsg.midimsg.bData[0] = (MIDI_NOTEON << 4) + (BYTE)nr_channel;
			sendshortmsg(midioutmsg, first);
			g_midistatuspitch[nr_device][nr_channel][pitch] = midioutmsg.id;
			g_miditimepitch[nr_device][nr_channel][pitch] = g_current_t;
		}
		return(true);
    case MIDI_NOTEOFF:
		if (g_transposition != 0)
		{
			int p = midioutmsg.midimsg.bData[1] + g_transposition;
			while (p < 0)
				p += 12;
			while (p > 127)
				p -= 12;
			midioutmsg.midimsg.bData[1] = p;
		}
		for (int c = 0; c < MAXCHANNEL; c++)
		{
			// search a noteon slot occupied in the extended channels
			if (g_channels[nr_device][c].extended == nr_channel)
			{
				if (g_midistatuspitch[nr_device][c][pitch] == midioutmsg.id)
				{
					g_midistatuspitch[nr_device][c][pitch] = -1 ;
					midioutmsg.midimsg.bData[0] = (MIDI_NOTEOFF << 4) + (BYTE)c;
					sendshortmsg(midioutmsg, first);
					return(true);
				}
			}
		}
		return(false); // no slot for this note : mlog !?
	case MIDI_SYSTEMCOMMON:
		return(sendshortmsg(midioutmsg, first));
    default:
		for (int c = 0; c < MAXCHANNEL; c++)
		{
			// replication of the messages on all extended channels
			if (g_channels[nr_device][c].extended == nr_channel)
			{
				midioutmsg.midimsg.bData[0] = (midioutmsg.midimsg.bData[0] & 0xF0) + (BYTE)c;
				// filter flooding of same [MIDI_CONTROL/value] in short period
				if ((type_msg != MIDI_CONTROL ) || (g_miditimecontrol[nr_device][c][midioutmsg.midimsg.bData[1]] < (g_current_t - 200)) || (g_midistatuscontrol[nr_device][c][midioutmsg.midimsg.bData[1]] != midioutmsg.midimsg.bData[2]))
				{
					if (sendshortmsg(midioutmsg, first) == false)
						retCode = false;
				}
				g_miditimecontrol[nr_device][c][midioutmsg.midimsg.bData[1]] = g_current_t;
				g_midistatuscontrol[nr_device][c][midioutmsg.midimsg.bData[1]] = midioutmsg.midimsg.bData[2];
			}
		}
        return(retCode);
    }
}
static bool sendmsg(T_midioutmsg midioutmsg)
{
    // send immediatly the short midioutmsg on the nr_device
	bool return_code = false;
	int nr_device = g_tracks[midioutmsg.track].device;
	//mlog("sendmsg track=%d nrdevice=%d", midioutmsg.track, nr_device);
	if ((nr_device >= 0) && (nr_device < OUT_MAX_DEVICE))
    {
        // send to one device
		if (nr_device < MIDIOUT_MAX)
        {
			if (g_midiopened[nr_device])
			{
				//mlog("Sent on midi opened");
				return_code = sendmidimsg(midioutmsg, true);
			}
        }
        else
        {
			if (nr_device < (VI_ZERO + g_vi_opened_nb))
                return_code = sendmidimsg(midioutmsg,true);
        }
    }
    return(return_code);
}
static int unqueue(int critere, T_midioutmsg midioutmsg)
{
	if (critere == OUT_QUEUE_FLUSH)
		g_current_t += timer_dt;
	long tmsg = g_current_t + midioutmsg.dt;
	int nb_waiting = 0;
	int retCode = 0;
	T_queue_msg *pt = g_queue_msg;
	for (int n = 0; n < g_end_queue_msg; n++, pt++)
	{
		if (!(pt->free))
		{
			nb_waiting++;
			switch (critere)
			{
			case OUT_QUEUE_FLUSH:
			{	// flush all messages
									if (pt->t <= g_current_t) // which are in the past
									{
										// remove the message from the waiting queue
										pt->free = true;
										sendmsg(pt->midioutmsg);
									}
									break;
			}
			case OUT_QUEUE_NOTEOFF:
			{
									  if ((((pt->midioutmsg.midimsg.bData[0]) & 0xF0) == (MIDI_NOTEON << 4)) // search a note-on 
										  && (((pt->midioutmsg.midimsg.bData[0]) & 0xF) == ((midioutmsg.midimsg.bData[0]) & 0xF)) // on same channel
										  && ((pt->midioutmsg.midimsg.bData[1] == midioutmsg.midimsg.bData[1]) || (midioutmsg.midimsg.bData[1] == 0)) //  and same pitch
										  && (pt->midioutmsg.track == midioutmsg.track) // on same track
										  && (pt->midioutmsg.id == midioutmsg.id) // with same id
										  && (pt->t >= tmsg) // after the required note-off
										  )
									  {
										  pt->free = true;
										  retCode = 1;
									  }
									  break;
			}
			default: break;
			}
		}
	}
	if (nb_waiting == 0)
		g_end_queue_msg = 0; // no more waintig slot

	return retCode;
}
static bool sendmsgdt(const T_midioutmsg midioutmsg)
{
    // send a midid_msg on nr_device immedialtly, or queud it for later process

	bool retCode = true;
    if (midioutmsg.dt == 0)
		retCode = sendmsg(midioutmsg);
    else
        queue_insert(midioutmsg);

	return(retCode);
}
static int mvi_open(const char *fname, int nr_deviceaudio, int volume,bool sf2)
{
	// fname is the full name ( *.dll for a vsti or *.sf2 for an sf2 )
	int nr_vi = vi_open(fname, nr_deviceaudio, sf2);
	if (nr_vi == -1)
		return -1;
	if (sf2)
	{
		if (BASS_MIDI_FontSetVolume(g_vi_opened[nr_vi].sf2_midifont, (float)(volume) / 64.0) == FALSE)
		{
			mlog("Error setting volume SF2<%s> , err=%d", fname, BASS_ErrorGetCode());
		}
		// send a dummy note to load vi
		T_midioutmsg u;
		u.midimsg.bData[0] = (MIDI_NOTEON << 4);
		u.midimsg.bData[1] = 30;
		u.midimsg.bData[2] = 1;
		u.midimsg.bData[3] = 0;
		u.track = VI_ZERO + nr_vi;
		u.dt = 50;
		u.nbbyte = 3;
		sendmsgdt(u);
		u.dt = 200;
		u.midimsg.bData[0] = (MIDI_NOTEOFF << 4);
		sendmsgdt(u);
	}
	else
	{
		if (BASS_ChannelSetAttribute(g_vi_opened[nr_vi].mstream, BASS_ATTRIB_VOL, (float)(volume) / 64.0) == FALSE)
		{
			mlog("Error setting volume VST<%s> , err=%d", fname, BASS_ErrorGetCode());
		}
	}
	return(VI_ZERO + nr_vi);
}
static void send_control(int nrTrack, int nrControl, int v, unsigned int dt)
{
	T_midioutmsg m;
	m.midimsg.bData[1] = nrControl;
	m.midimsg.bData[2] = v;
	m.track = nrTrack;
	m.dt = dt;
	m.nbbyte = 3;
	m.id = 0;
	m.midimsg.bData[0] = (MIDI_CONTROL << 4 ) ;
	sendmsgdt(m);
}
static void send_program(int nrTrack, int nrProgram, unsigned int dt)
{
	T_midioutmsg m;
	m.midimsg.bData[1] = nrProgram;
	m.midimsg.bData[2] = 0;
	m.track = nrTrack;
	m.dt = dt;
	m.nbbyte = 2;
	m.id = 0;
	m.midimsg.bData[0] = ( MIDI_PROGRAM << 4 ) ;
	sendmsgdt(m);
}
static void send_tune(int nrTrack, float freq)
{
    int coarse, coarsemsb , finemsb ;
    float fine;
    float cents = 1200.0 * log2f(freq / 440.0);
    if (cents >= 0)
    {
        coarse = (int)((cents + 50.0)/100.0);
        fine = cents - 100.0 * (float)(coarse);
    }
    else
    {
        cents = (-1.0)*cents;
        coarse = (int)((cents + 50.0)/100.0);
        fine = cents - 100.0 * (float)(coarse);
        coarse *= -1;
        fine *= -1.0;
        cents = (-1.0)*cents;
    }
    finemsb = (int)((float)(0x20) * fine / 50.0 ) + 0x40;
    
    coarsemsb = coarse + 0x40;
    
	send_control(nrTrack, 101, 0, 0);
	send_control(nrTrack, 100, 2, 0);
	send_control(nrTrack, 6, coarsemsb, 0);
    
	send_control(nrTrack, 101, 0, 0);
	send_control(nrTrack, 100, 1, 0);
	send_control(nrTrack, 6, finemsb, 0);
    // 	send_control(nrTrack,38, finelsb, 0);
}
static void send_bendrange(int nrTrack, int semitone)
{
	send_control(nrTrack, 101, 0, 0);
	send_control(nrTrack, 100, 0, 0);
	send_control(nrTrack, 6, 0, semitone);
}
static void chord_init()
{
	for (int n = 0; n < CHORDMAX; n++)
	{
		g_chords[n].id = -1;
		g_chords[n].nbPitch = 0;
		g_chords[n].nbOff = 0;
	}
}
static T_chord* chord_new(unsigned long id)
{
	for (int n = 0; n < CHORDMAX; n++)
	{
		if ((g_chords[n].id == -1) || (g_chords[n].id == id))
		{
			if (id == -1)
				g_chords[n].id = n;
			else
				g_chords[n].id = id;
			return(&(g_chords[n]));
		}
	}
	return (NULL);
}
static T_chord* chord_get(int id)
{
	if (id == -1)
		return NULL;
	for (int n = 0; n < CHORDMAX; n++)
	{
		if (g_chords[n].id == id)
			return(&(g_chords[n]));
	}
	return (NULL);
}
static void channel_extended_init()
{
	// all channels are unused
	for (int nr_device = 0; nr_device < OUT_MAX_DEVICE; nr_device++)
	{
		for (int channel = 0; channel < MAXCHANNEL; channel++)
		{
			g_channels[nr_device][channel].extended = -1 ; // not used
		}
	}
}
static int channel_extended_set(int nr_device , int nr_channel, int nb_additional_channel, bool except_channel10)
{
	if (g_channels[nr_device][nr_channel].extended == nr_channel)
	{
		// already set
		int nb = 0;
		for (int n = 0; n < MAXCHANNEL; n++)
		{
			if (g_channels[nr_device][n].extended == nr_channel)
				nb++;
		}
		if (nb == nb_additional_channel)
		{
			// already done 
			return(1);
		}
		else
		{
			// free the previous extension to redo it
			for (int n = 0; n < MAXCHANNEL; n++)
			{
				if (g_channels[nr_device][n].extended == nr_channel)
					g_channels[nr_device][n].extended = -1;
			}

		}
	}
	g_channels[nr_device][nr_channel].extended = nr_channel;
	// search a channel not yet "used", starting from 16, 15, 14...
	int m = MAXCHANNEL - 1;
	while (g_channels[nr_device][m].extended != -1)
	{
		m--;
		if ((except_channel10) && (m == 9))
			m--;
		if (m <= nr_channel)
			return(-1);
	}
	// m contains a channel not yet used
	// extend the channel on the last ones : m, m-1, m-2, ...
	for (int n = 0; n < nb_additional_channel; n++)
	{
		if ((except_channel10) && (m == 9))
			m--;
		if (m <= nr_channel)
			return(-1);
		if (g_channels[nr_device][m].extended == -1) // channel#v is not free
			g_channels[nr_device][m].extended = nr_channel; //  channel#v receives order from <channel>
		m--;
	}
	return(0);
}
static void string_to_control(int nrTrack, const char *param)
{
	// read the string and send control on the track
	// syntaxe of the string  :
	// name(P[[Bank_MSB/]Bank_LSB/]<Program>],[C<Control-nr>=<Value>,]*)
	// example :
	//    change progam to value 30 : myProgram(P30)
	//    change volume to value 40 : myVolume(C7/40)
	//    change MSB=10/LSB=11/program=30, volume=40 and pan=5 : myControl(P10/11/30,C7/40,C10/5)

	char bufstr[1024];
	strcpy(bufstr, param);
	bufstr[strlen(param)] = '\0';

	char *next_token_name = NULL;
	char * name;
	name = strtok_s(bufstr, "()", &next_token_name); // name
	char *progcontrol;
	progcontrol = strtok_s(NULL, "()", &next_token_name); // prog,control
	if (progcontrol == NULL)
		return;

	char *next_token_param = NULL;
	char * ptprogram;
	char *control[65];
	int nbControl = 0;
	ptprogram = strtok_s(progcontrol, ",", &next_token_param); // program ?
	if ((ptprogram == NULL) || (strlen(ptprogram) < 2))
		return;
	if (ptprogram[0] == 'P')
	{
		//good program
		control[0] = strtok_s(NULL, ",", &next_token_param); // next is a control
	}
	else
	{
		control[0] = ptprogram; // not a good program. Give it to the control
		ptprogram = NULL;
	}
	while ((control[nbControl] != NULL) && (nbControl < 64))
	{
		nbControl++;
		control[nbControl] = strtok_s(NULL, ",", &next_token_param);
	}

	// send program 
	if (ptprogram != NULL)
	{
		char *next_tokenProgram = NULL;
		char *bankprogram[5];
 		int nb_bankprogram = 0;
        bankprogram[nb_bankprogram] = strtok_s(ptprogram + 1 , "/" , &next_tokenProgram);
        while ((bankprogram[nb_bankprogram] != NULL) && (nb_bankprogram < 4))
		{
			nb_bankprogram++;
            bankprogram[nb_bankprogram] = strtok_s(NULL, "/" , &next_tokenProgram);
        }
		switch (nb_bankprogram)
		{
		case 1:
			send_program(nrTrack, cap(atoi(bankprogram[0]), 0, 128, 0), 0);
			break;
		case 2:
			send_control(nrTrack, 0, cap(atoi(bankprogram[0]), 0, 128, 0), 0);
			send_program(nrTrack, cap(atoi(bankprogram[1]), 0, 128, 0), 0);
			break;
		case 3:
			send_control(nrTrack, 0, cap(atoi(bankprogram[0]), 0, 128, 0), 0);
			send_control(nrTrack, 0x20, cap(atoi(bankprogram[1]), 0, 128, 0), 0);
			send_program(nrTrack, cap(atoi(bankprogram[2]), 0, 128, 0), 0);
			break;
		default:
			break;
		}
	}

	// send controls
	for (int nrControl = 0; nrControl < nbControl; nrControl++)
	{
		char *next_tokenControl = NULL;
		char *controlnumber, *controlvalue;
		controlnumber = strtok_s(control[nrControl] + 1, "/", &next_tokenControl);
		if (controlnumber != NULL)
		{
			controlvalue = strtok_s(NULL, "/", &next_tokenControl);
			if (controlvalue != NULL)
			{
				send_control(nrTrack, cap(atoi(controlnumber), 0, 127, 0), cap(atoi(controlvalue), 0, 128, 0), 0);
			}
		}
	}
}
static void midiclose_device(int nr_device)
{
	if (g_midiopened[nr_device])
	{
        T_midimsg midimsg1;
        midimsg1.bData[0] = (MIDI_CONTROL << 4);
        midimsg1.bData[1] = 123;// all note off
        midimsg1.bData[2] = 0;
        T_midimsg midimsg2;
        midimsg2.bData[0] = (MIDI_CONTROL << 4);
        midimsg2.bData[1] = 120;// all sound off
        midimsg2.bData[2] = 0;
        T_midimsg midimsg3;
        midimsg3.bData[0] = (MIDI_CONTROL << 4);
        midimsg3.bData[1] = 121;// reset al controller
        midimsg3.bData[2] = 0;
        
#ifdef V_PC
        midiOutShortMsg(g_midiopened[nr_device], midimsg1.dwData);
        midimsg.bData[1] = 120;// all sound off
        midiOutShortMsg(g_midiopened[nr_device], midimsg2.dwData);
        midimsg.bData[1] = 121;// reset controller
        midiOutShortMsg(g_midiopened[nr_device], midimsg3.dwData);
#endif
#ifdef V_MAC
        OSStatus err;
        // envoi du message Midi version Mac dans un packet
        Byte buffer[1024];
        MIDIPacketList *pktlist = (MIDIPacketList *)buffer;
        MIDIPacket *curPacket = MIDIPacketListInit(pktlist);
        curPacket = MIDIPacketListAdd(pktlist, sizeof(buffer), curPacket, 0, 3, & (midimsg1.bData[0] ));
        curPacket = MIDIPacketListAdd(pktlist, sizeof(buffer), curPacket, 0, 3, & (midimsg2.bData[0] ));
        curPacket = MIDIPacketListAdd(pktlist, sizeof(buffer), curPacket, 0, 3, & (midimsg3.bData[0] ));
        err = MIDISend(g_midiOutPortRef, g_midiopened[nr_device], pktlist);
#endif
        BASS_MIDI_OutFree(nr_device);
		g_midiopened[nr_device] = 0;
	}
}
static void midiclose_devices()
{
	for (int n = 0; n < MIDIOUT_MAX; n++)
		midiclose_device(n);
	g_midimax_nr_device = 0;
}
static int midiopen(int nr_devicemidi)
{
	if (g_midiopened[nr_devicemidi] != 0)
	{
		return(nr_devicemidi); // already opened
	}
	int retCode =  BASS_MIDI_OutInit(nr_devicemidi);
	return retCode;
}
void all_note_off(const char *soption, int nrTrack)
{
	int track_min = nrTrack;
	int track_max = nrTrack + 1;
	if (nrTrack < 0)
	{
		track_min = 0;
		track_max = MAXTRACK;
	}
	for (int nrTrack = track_min; nrTrack < track_max; nrTrack++)
	{
		for (unsigned int p = 0; p < strnlen_s(soption, 5); p++)
		{
			switch (soption[p])
			{
			case 's': send_control(nrTrack, 120, 0, 0); break; // all sound off
			case 'c': send_control(nrTrack, 121, 0, 0); break; // reset controller
			case 'n': send_control(nrTrack, 123, 0, 0); break; // all note off
			case 'a': // all
				send_control(nrTrack, 120, 0, 0);
				send_control(nrTrack, 121, 0, 0);
				send_control(nrTrack, 123, 0, 0);
				break;
			default: break;
			}
		}
	}
	for (unsigned int p = 0; p < strnlen_s(soption, 5); p++)
	{
		switch (soption[p])
		{
		case 'n' :
		case 'a': // all
			picth_init();
			break;
		default: break;
		}
	}
}
static void onMidiOut_filter_set()
{
	// validate the LUA functions available to process the MIDI messages
	//g_process_Sysex = (lua_getglobal(g_LUAoutState, LUAFunctionSysex) == LUA_TFUNCTION);
	//lua_pop(g_LUAoutState, 1);
	//if (g_process_Sysex) mlog("Information : onMidiOut function %s registered", LUAFunctionSysex);

	//g_process_Activesensing = (lua_getglobal(g_LUAoutState, LUAFunctionActive) == LUA_TFUNCTION);
	//lua_pop(g_LUAoutState, 1);
	//if (g_process_Activesensing) mlog("Information : onMidiOut function %s registered", LUAFunctionActive);

	g_process_Clock = (lua_getglobal(g_LUAoutState, LUAFunctionClock) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_Clock) mlog("Information : onMidiOut function %s registered", LUAFunctionClock);

	g_process_ChannelPressure = (lua_getglobal(g_LUAoutState, LUAFunctionChannelPressure) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_ChannelPressure) mlog("Information : onMidiOut function %s registered", LUAFunctionChannelPressure);

	g_process_KeyPressure = (lua_getglobal(g_LUAoutState, LUAFunctionKeyPressure) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_KeyPressure) mlog("Information : onMidiOut function %s registered", LUAFunctionKeyPressure);

	g_process_Control = (lua_getglobal(g_LUAoutState, LUAFunctionControl) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_Control) mlog("Information : onMidiOut function %s registered", LUAFunctionControl);

	g_process_SystemCommon = (lua_getglobal(g_LUAoutState, LUAFunctionSystemCommon) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_SystemCommon) mlog("Information : onMidiOut function %s registered", LUAFunctionSystemCommon);

	g_process_Program = (lua_getglobal(g_LUAoutState, LUAFunctionProgram) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_Program) mlog("Information : onMidiOut function %s registered", LUAFunctionProgram);

	g_process_NoteOff = (lua_getglobal(g_LUAoutState, LUAFunctionNoteOff) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_NoteOff) mlog("Information : onMidiOut function %s registered", LUAFunctionNoteOff);

	g_process_NoteOn = (lua_getglobal(g_LUAoutState, LUAFunctionNoteOn) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_NoteOn) mlog("Information : onMidiOut function %s registered", LUAFunctionNoteOn);

	g_process_PitchBend = (lua_getglobal(g_LUAoutState, LUAFunctionPitchBend) == LUA_TFUNCTION);
	lua_pop(g_LUAoutState, 1);
	if (g_process_PitchBend) mlog("Information : onMidiOut function %s registered", LUAFunctionPitchBend);
}
static bool onMidiout_open(const char* fname)
{
	if (g_LUAoutState)
		lua_close(g_LUAoutState);
	g_LUAoutState = 0;

	// open the dedicated midiin-LUA-thread to process midiout msg
	g_LUAoutState = luaL_newstate(); // newthread 
	luaL_openlibs(g_LUAoutState);

	if (luaL_loadfile(g_LUAoutState, fname) != LUA_OK)
	{
		mlog("onMIdiOut mlog lua_loadfile <%s>", lua_tostring(g_LUAoutState, -1));
		lua_close(g_LUAoutState);
		g_LUAoutState = NULL;
		return false;
	}
	// run the script
	if (lua_pcall(g_LUAoutState, 0, 0, 0) != LUA_OK)
	{
		mlog("onMIdiOut mlog lua_pcall <%s>", lua_tostring(g_LUAoutState, -1));
		lua_pop(g_LUAoutState, 1);
		lua_close(g_LUAoutState);
		g_LUAoutState = NULL;
		return false;
	}
	mlog("Information : onMidiOutOpen(%s) OK", fname);
	onMidiOut_filter_set();
	return true;
}
static bool getTypeFile(char *vinamedevice, int *nr_deviceaudio, char *viname , char *extension)
{
	// return true if extension(vinamedevice) is sf2 or dll or wav
	// extension is completed
	// nr_deviceaudio is set to the device audio if suffixe @name_audio or @nr_audio
	// viname contains the name , with extension and without the section @audio
	*extension = '\0';
	bool extension_ok = false;
	*nr_deviceaudio = -1;
	if ((strlen(vinamedevice) < 5) || (vinamedevice[strlen(vinamedevice) - 4] != '.'))
		return false;
	if ((strncmp(vinamedevice + strlen(vinamedevice) - 4, ".sf2", 4) == 0) || (strncmp(vinamedevice + strlen(vinamedevice) - 4, ".SF2", 4) == 0))
	{
		extension_ok = true;
		strcpy(extension,"sf2");
	}
	if ((strncmp(vinamedevice + strlen(vinamedevice) - 4, ".dll", 4) == 0) || (strncmp(vinamedevice + strlen(vinamedevice) - 4, ".DLL", 4) == 0))
	{
		extension_ok = true;
		strcpy(extension, "dll");
	}
	if ((strncmp(vinamedevice + strlen(vinamedevice) - 4, ".wav", 4) == 0) || (strncmp(vinamedevice + strlen(vinamedevice) - 4, ".WAV", 4) == 0))
	{
		extension_ok = true;
		strcpy(extension, "wav");
	}
	if (!extension_ok)
		return false;
	vinamedevice[strlen(vinamedevice) - 4] = '\0';
	char *ptdevice = strchr(vinamedevice, '@');
	if (ptdevice)
	{
		// audio-device is specified after the @
		*ptdevice = '\0';
		*nr_deviceaudio = 0;
		char name_device[MAXBUFCHAR];
		bool found = false;
		while (true)
		{
			// search an audio_device with the same name
			audio_name(*nr_deviceaudio, name_device);
			if (*name_device == '\0')
				break;
			if (strcmp(name_device, ptdevice + 1) == 0)
			{
				found = true;
				break;
			}
			(*nr_deviceaudio)++;
		}
		if (!found)
			// select the audio device #
			*nr_deviceaudio = atoi(ptdevice + 1);
	}
	strcpy(viname, vinamedevice); 
	if (strcmp(extension, "sf2") == 0)
		strcat(viname, ".sf2");
	if (strcmp(extension, "dll") == 0)
		strcat(viname, ".dll");
	if (strcmp(extension, "wav") == 0)
		strcat(viname, ".wav");
	return true;
}
static void curve_init()
{
	for (int c = 0; c < MAXCURVE; c++)
	{
		for (int n = 0; n < MAXPOINT; n++)
		{
			g_curves[c].x[n] = -1;
			g_curves[c].y[n] = -1;
			g_curves[c].x[n] = -1;
			g_curves[c].y[n] = -1;
		}
	}
}
static void track_init()
{
	bool channelUsed[OUT_MAX_DEVICE][MAXCHANNEL];
	for (int nr_device = 0; nr_device < OUT_MAX_DEVICE; nr_device++)
	{
		for (int nr_channel = 0; nr_channel < MAXCHANNEL; nr_channel++)
			channelUsed[nr_device][nr_channel] = false;
	}
	for (int nrTrack = 0; nrTrack < MAXTRACK; nrTrack++)
	{
		if ((g_tracks[nrTrack].device >= 0) && (g_tracks[nrTrack].channel> 0) && (g_tracks[nrTrack].channel < MAXCHANNEL))
		{
			if (channelUsed[g_tracks[nrTrack].device][g_tracks[nrTrack].channel] == false)
			{
				channelUsed[g_tracks[nrTrack].device][g_tracks[nrTrack].channel] = true;
				send_control(nrTrack, 123, 0, 0); // all note off
				send_control(nrTrack, 120, 0, 0); // all sound off
				send_control(nrTrack, 121, 0, 0); // reset controller
			}
		}
		g_tracks[nrTrack].volume = 64; // neutral volume
		g_tracks[nrTrack].mute = false;
		g_tracks[nrTrack].device = -2; // no device  attached to this track
		g_tracks[nrTrack].channel = -2; // no channel  attached to this track
		g_tracks[nrTrack].nrCurve = 0; // no curve
		channel_extended_init();
		g_volume = 64;
	}
}
static void midi_init()
{
	for (int n = 0; n < MIDIOUT_MAX; n++)
	{
		g_midiopened[n] = 0;
	}
	int nrDevice = 0;
	char name_device[MAXBUFCHAR];
	while (midi_in_name(nrDevice, name_device))
	{
		mlog("Information : midiin <%s> #%d", name_device, nrDevice + 1);
		nrDevice++;
	}
	nrDevice = 0;
	while (midi_out_name(nrDevice, name_device))
	{
		mlog("Information : midiout <%s> #%d", name_device, nrDevice + 1);
		nrDevice++;
	}
}
static void fifo_init()
{
	for (int n = 0; n < OUT_QUEUE_MAX_MSG; n++)
		g_queue_msg[n].free = true;
	g_end_queue_msg = 0;
}
static void init_mutex()
{
#ifdef V_PC
	// create a mutex to manipulate safely the outputs ( timer , lua ) up to ( queus, midiout )
	g_mutex_out = CreateMutex(NULL, FALSE, NULL);
	if (g_mutex_out == NULL)
		mlog("mlog init_mutex");
#endif
#ifdef V_MAC
	// create a mutex to manipulae safely the queued midiout msg
	if (pthread_mutex_init(&g_mutex_out, NULL) != 0)
		mlog("mlog init_mutex");
#endif
}
static void free_mutex()
{
#ifdef V_PC
	CloseHandle(g_mutex_out);
#endif
#ifdef V_MAC
	pthread_mutex_destroy(&g_mutex_out);
#endif
}
#ifdef V_PC
VOID CALLBACK timer(PVOID lpParam, BOOLEAN TimerOrWaitFired)
{
	lock_mutex_out();
	T_midioutmsg msg;
	msg.midimsg.dwData = 0;
	unqueue(OUT_QUEUE_FLUSH, msg);
	unlock_mutex_out();
}
#endif
#ifdef V_MAC
// void (*CFRunLoopTimerCallBack) ( CFRunLoopTimerRef timer, void *info );
void timer(CFRunLoopTimerRef timer, void *info)
{
	lock_mutex_out();
	T_midioutmsg msg;
	msg.midimsg.dwData = 0;
	unqueue(OUT_QUEUE_FLUSH, msg);
	unlock_mutex_out();
}
#endif
static void timer_init()
{
	// create a periodic timer to flush-out the queud midiout msg
#ifdef V_PC
	if (!CreateTimerQueueTimer(&g_timer, NULL, timer, 0, timer_dt, timer_dt, 0))
		mlog("mlog timer_init");
#endif
#ifdef V_MAC
	CFRunLoopTimerCreate(NULL, (float)(timer_dt) / 1000.0, (float)(timer_dt) / 1000.0, 0, 0, timer, NULL);
#endif
}
static void free_timer()
{
#ifdef V_PC
    
	DeleteTimerQueueTimer(NULL, g_timer, NULL);
#endif
}
static void init(const char *fname)
{
	log_init(fname);
	picth_init();
	midi_init();
	fifo_init();
	vi_init();
	chord_init();
	channel_extended_init();
	track_init();
	curve_init();
	init_mutex();
	timer_init();
	mixer_init();
	g_LUAoutState = NULL;
}
static void free()
{
	free_timer();
	free_mutex();
	vi_free();
	midiclose_devices();
	mixer_free();
	if (g_LUAoutState)
	{
		lua_close(g_LUAoutState);
	}
	g_LUAoutState = 0;
}

static int LoutTrackMute(lua_State *L)
{
	// set MIDI noteon volume for a channel 
	// It's different than an audio volume on the channel using note ctrl7 !!
	// parameter #1 : mute = 0, unmute = 1, toggle = 2
	// parameter #2 : optional nrTrack ( default 1 ) 
	lock_mutex_out();

	int mute = (int)lua_tointeger(L, 1);
	int nrTrack = cap((int)luaL_optinteger(L, 2, 1), 0, MAXTRACK, 1);
	switch (mute)
	{
	case 0: g_tracks[nrTrack].mute = true; break;
	case 1: g_tracks[nrTrack].mute = false; break;
	case 2: g_tracks[nrTrack].mute = !(g_tracks[nrTrack].mute ) ; break;
	}
	unlock_mutex_out();
	return (0);
}
static int LoutSetTrackVolume(lua_State *L)
{
	// set MIDI noteon volume for a channel 
	// It's different than an audio volume on the channel using note ctrl-7 !!
	// parameter #1 : volume 0..64(neutral)..127
	// parameter #2 : optional nrTrack ( default 1 ) 
	lock_mutex_out();

	int volume = (int)lua_tointeger(L, 1);
	int nrTrack = cap((int)luaL_optinteger(L, 2, 1), 0, MAXTRACK, 1);
	g_tracks[nrTrack].volume = volume;
	unlock_mutex_out();
	return (0);
}
static int LoutGetTrackVolume(lua_State *L)
{
	// get MIDI noteon volume for a track 
	// It's different than an audio volume on the channel using note ctrl7 !!
	// parameter #1 : optional nrTrack ( default 1 ) 
	// return : volume 0..64(neutral)..127
	lock_mutex_out();

	int nrTrack = cap((int)luaL_optinteger(L, 1, 1), 0, MAXTRACK, 1);

	int v = g_tracks[nrTrack].volume * ((g_tracks[nrTrack].mute) ? 0 : 1);
	lua_pushinteger(L, v );
	
	unlock_mutex_out();
	return (1);
}
static int LoutSetCurve(lua_State *L)
{
	// set MIDI noteon curve for a channel 
	// parameter #1 : nrCurve 
	// parameetr #2 : table of points x1,y1 ; x2,y2, ...
	// e.g to reverse the velocity : 0,1 1,0
	// e.g. to have more p and mp : 0,0 0.8,0.5 1,0.8
	lock_mutex_out();

	int nrCurve = cap((int)lua_tointeger(L, 1), 0, MAXCURVE, 0);
	int nrArg = 2;
	int x, y, nbp;
	nbp = 0;
	while (nrArg <= lua_gettop(L))
	{
		x = cap((int)lua_tointeger(L, nrArg - 1), 0, 128, 0);
		y = cap((int)lua_tointeger(L, nrArg), 0, 128, 0);
		g_curves[nrCurve].x[nbp] = x;
		g_curves[nrCurve].y[nbp] = y;
		if ((nbp + 1) < MAXPOINT)
		{
			g_curves[nrCurve].x[nbp + 1] = -1;
		}
		nrArg += 2;
		nbp++;
		if (nbp >= MAXPOINT)
			break;
	}

	unlock_mutex_out();
	return (0);
}
static int LoutTranspose(lua_State *L)
{
	// transpose midiout ( make also a allnoteoff, else : mismatch between previous noteon and netx noteoff on midiout )
	// parameter #1 : g_transposition value 
	lock_mutex_out();
	all_note_off("n", -1);
	g_transposition = (-1) * cap((int)lua_tointeger(L, 1), -24, 24, 0);
	unlock_mutex_out();
	return (0);
}

static int LoutSetTrackCurve(lua_State *L)
{
	// set MIDI noteon curve for a track 
	// parameter #1 : curve to use
	// parameter #2 : optional nrTrack ( default 1 ) 
	lock_mutex_out();

	int nrCurve = cap((int)lua_tointeger(L, 1), 0, MAXCURVE, 0);
	int nrTrack = cap((int)luaL_optinteger(L, 2, 1), 0, MAXTRACK, 1);
	g_tracks[nrTrack].nrCurve = nrCurve;
	
	unlock_mutex_out();
	return(0);
}
static int LoutSetTrackInstrument(lua_State *L)
{
	// set instrument for a track 
	// parameter #1 : string which describe the instrument required
	// parameter #2 : optional nrTrack ( default 1 ) 
	lock_mutex_out();

	const char *tuning = lua_tostring(L, 1);
	int nrTrack = cap((int)luaL_optinteger(L, 2, 1), 0, MAXTRACK , 1);
	string_to_control(nrTrack, tuning);

	unlock_mutex_out();
	return(0);
}
static int LoutSetVolume(lua_State *L)
{
	// set MIDI noteon volume for all outputs 
	// parameter #1 : volume 0..64(neutral)..127
	lock_mutex_out();

	g_volume = (int)lua_tointeger(L, 1);
	
	unlock_mutex_out();
	return (0);
}
static int LoutGetVolume(lua_State *L)
{
	// get MIDI-out noteon volume for all outputs 
	// return : volume 0..64(neutral)..127
	lock_mutex_out();

	lua_pushinteger(L, g_volume);

	unlock_mutex_out();
	return (1);
}
/**
* \fn void LinMidiList()
* \brief List the Midi in devices.
* LUA function inMidiList().
* \return table of midi in devices, with device number, and name.
**/
static int LinGetMidiList(lua_State *L)
{
    // return the midi In device in an array
    // no parameter
	lock_mutex_out();

	DWORD nrDevice;
	char name_device[MAXBUFCHAR];
	lua_newtable(L);
	nrDevice = 0;
	while (midi_in_name(nrDevice, name_device))
	{
		lua_pushinteger(L, nrDevice + 1);
		lua_pushstring(L, name_device);
		lua_settable(L, -3);
		nrDevice++;
	}
	
	unlock_mutex_out();
	return(1);
}
static int LinGetMidiName(lua_State *L)
{
	// return the name of the midi In device 
	// parameter #1 : device nr
	lock_mutex_out();

	int nrDevice = cap((int)lua_tointeger(L, 1), 0, OUT_MAX_DEVICE, 1);
	char name_device[MAXBUFCHAR] = "";
	midi_in_name(nrDevice, name_device);
	lua_pushstring(L, name_device);

	unlock_mutex_out();
	return(1);
}
/**
* \fn void LoutMidiList()
* \brief List the Midi out devices.
* LUA function outMidiList().
* \return table of midi out devices, with device number, and name.
**/
static int LoutGetMidiList(lua_State *L)
{
	// return the midi Out devices in an array
	// no parameter
	lock_mutex_out();

	DWORD nrDevice;
	char name_device[MAXBUFCHAR];
	lua_newtable(L);
	nrDevice = 0;
	while (midi_out_name(nrDevice, name_device))
	{
		lua_pushinteger(L, nrDevice + 1);
		lua_pushstring(L, name_device);
		lua_settable(L, -3);
		nrDevice++;
	}

	unlock_mutex_out();
	return(1);
}
static int LoutGetMidiName(lua_State *L)
{
	// return the midi Out device name
	// parameter #1 : device nr
	lock_mutex_out();

	int nrDevice = cap((int)lua_tointeger(L, 1), 0, OUT_MAX_DEVICE, 1);
	char name_device[MAXBUFCHAR] = "";
	midi_out_name(nrDevice, name_device);
	lua_pushstring(L, name_device);
	
	unlock_mutex_out();
	return(1);
}

static int LoutSetChordCompensation(lua_State *L)
{
	// set the Chord Compensation for the chords
	// parameter #1 : [0..32]  Compensation
	lock_mutex_out();
	g_chordCompensation = (int)lua_tointeger(L, 1);
	unlock_mutex_out();
	return(0);
}
static int LoutSetRandomDelay(lua_State *L)
{
	// set the  random delay for chords
	// parameter #1 : [0..127] random delay in ms
	lock_mutex_out();
	g_randomDelay = (int)lua_tointeger(L, 1);
	unlock_mutex_out();
	return(0);
}
static int LoutSetRandomVelocity(lua_State *L)
{
	// set the random velocity for the g_chords
	// parameter #1 : [0..100] random velocity in %
	lock_mutex_out();
	g_randomVelocity = (int)lua_tointeger(L, 1);
	unlock_mutex_out();
	return(0);
}
static int LoutChordSet(lua_State *L)
{
	// set the chord to play with chordon
	// parameter #1 : unique_id
	//     with -1 : return a unique-id
	// parameter #2 : transpose
	// parameter #3 : [0..] delay in ms between notes
	// parameter #4 : ([0..50..100]) % dvelocity between notes ( 50 all equal, 25 divide by two for next note )
	// parameter #5 & #6 : start# and end# of pitches ( 1 & -1 : all pitches , -1 & 1 : all in reverse order )
	// parameter #7.. : list of pitch
	// return the unique_id
	lock_mutex_out();

	long retCode = 0;
	int erropt;
	unsigned long id = (int)lua_tointegerx(L, 1, &erropt);
	if (! erropt)
	{
		mlog("Error chordset,  id is not valid");
		retCode = -3;
	}
	else
	{
		if (id == -1)
			id = g_unique_id++;
		T_chord* chord = chord_new(id);
		if (chord == NULL)
		{
			mlog("mlog outChordSet %d. No more chord-slot available\n", id);
			retCode = -1;
		}
		else
		{
			int transpose = (int)lua_tointeger(L, 2);
			chord->dt = (int)lua_tointeger(L, 3);
			chord->dv = (int)lua_tointeger(L, 4);
			int start = (int)lua_tointeger(L, 5);
			int end = (int)lua_tointeger(L, 6);
			int endArg, startArg;
			if (start < 0)
				startArg = lua_gettop(L) - (start * (-1) - 1);
			else
				startArg = start - 1 + 7;
			if (end < 0)
				endArg = lua_gettop(L) - (end * (-1) - 1);
			else
				endArg = end - 1 + 7;
			chord->nbPitch = 0;
			for (int nrArg = startArg; (startArg <= endArg) ? (nrArg <= endArg) : (nrArg >= endArg); nrArg += (startArg <= endArg) ? 1 : -1)
			{
				int p = (int)lua_tointeger(L, nrArg) + transpose;
				while (p < 0)
					p += 12;
				while (p > 127)
					p -= 12;
				chord->pitch[chord->nbPitch] = p;
				chord->nbPitch++;
				if (chord->nbPitch >= CHORDMAXPITCH)
					break;
			}
			retCode = chord->id;
		}
	}
	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutChordOn(lua_State *L)
{
	// play a chord prepared by chordset
	// parameter #1 : unique_id ( set in outChordSet )
	// parameter #2 : velocity
	// parameter #3 : optional integer delay dt in ms before to send the msg
	// parameter #4 : optional nrTrack ( default 0 )
	lock_mutex_out();

	T_chord* chord = NULL ;
	long retCode = -3;
	int erropt;
	int id = (int)lua_tointegerx(L, 1 , &erropt);
	if (! erropt)
	{
		mlog("Error chordon,  id is not valid");
		retCode = -3;
	}
	else
	{
		chord = chord_get(id);
		if (chord == NULL)
		{
			mlog("Error chordon,  chord %d does not exist", id);
			retCode = -2;
		}
		else
		{
			retCode = chord->id;
			T_midioutmsg u;
			//mlog("chordon,  chord %d \n", id);
			int v = (int)lua_tointeger(L, 2);
			if (g_chordCompensation != 0)
				v = ((200 - (g_chordCompensation * (chord->nbPitch - 1))) * v) / 200;
			u.dt = (int)luaL_optinteger(L, 3, 0);
			u.track = cap((int)luaL_optinteger(L, 4, 1), 0, MAXTRACK, 1);
			//mlog("chordon,  u.track %d \n", u.track);
			if (u.track >= 0)
			{
				retCode = 0;
				u.midimsg.bData[0] = (MIDI_NOTEON << 4);
				u.midimsg.bData[3] = 0;
				u.nbbyte = 3;

				for (int c = 0; c < chord->nbPitch; c++)
				{
					int p = chord->pitch[c];
					while (p < 0)
						p += 12;
					while (p>127)
						p -= 12;
					u.midimsg.bData[1] = p;
					int rv = v;
					if (g_randomVelocity != 0)
						rv += (g_randomVelocity * rand()) / RAND_MAX - g_randomVelocity / 2;
					u.midimsg.bData[2] = cap(rv, 1, 128, 0);
					// mlog("chordon p=%d,v=%d", u.midimsg.bData[1], u.midimsg.bData[2]);
					u.id = g_unique_id++;
					if (sendmsgdt(u) == false)
						retCode = -1;
					else
					{
						chord->msg_off[chord->nbOff] = u; // to remember that this msg is played
						(chord->nbOff)++;
					}
					if (chord->dv == 0)
						break;
					u.dt = c * chord->dt;
					if (g_randomDelay != 0)
						u.dt += (g_randomDelay * rand()) / RAND_MAX;
					if (chord->dv != 64)
						v = ((127 + (chord->dv - 64)) * v) / 127;
					if (v < 1)
						break;
				}
			}
		}
	}
	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutChordOff(lua_State *L)
{
	// stop a chord played with chordon
	// parameter #1 : unique_id ( set in outChordSet )
	// parameter #2 : optional velocity, default 0
	// parameter #3 : optional integer delay dt in ms before to send the msg
	lock_mutex_out();
	int erropt;
	T_chord* chord = NULL;
	int retCode = -1;
	int id = (int)lua_tointegerx(L, 1, &erropt);
	if (! erropt)
	{
		mlog("Error chordoff,  id is not valid");
		retCode = -3;
	}
	else
	{
		chord = chord_get(id);
		if (chord == NULL)
		{
			mlog("Error chordoff,  chord %d does not exist", id);
			retCode = -2;
		}
		else
		{
			T_midioutmsg u;
			int velo = cap((int)luaL_optinteger(L, 2, 0), 0, 128, 0);
			int dt = (int)luaL_optinteger(L, 3, 0);
			retCode = 0;
			for (int c = 0; c < chord->nbOff; c++)
			{
				u = chord->msg_off[c];
				u.midimsg.bData[2] = velo;
				if (dt != -1000)
					u.dt = dt;
				u.midimsg.bData[0] = (MIDI_NOTEOFF << 4) + ((u.midimsg.bData[0]) & 0xF);
				if (unqueue(OUT_QUEUE_NOTEOFF, u) == 0)
				{
					if (sendmsgdt(u) == false)
						retCode = -1;
				}
				else
					retCode = -1;
			}
			chord->id = -1;
			chord->nbOff = 0;
		}
	}
	lua_pushinteger(L, retCode);
	
	unlock_mutex_out();
	return (1);
}

static int LoutNoteOn(lua_State *L)
{
	// parameter #1 : pitch 1..127
	// parameter #2 : optional velocity ( default 64 )
	// parameter #3 : optional unique id.  ( to manage concurrential note-on note-off )
			// default 0
			// with -1 : a unique_id is returned
	// parameter #4 : optional integer delay dt in ms before to send the msg
	// parameter #5 : optional nrTrack ( default 1 )
	// return the unique id
	lock_mutex_out();

	unsigned long retCode = -1;
	T_midioutmsg u;
	int p = (int)lua_tointeger(L, 1);
	while (p > 127)
		p -= 12;
	while (p < 0)
		p += 12;
	u.midimsg.bData[1] = p;
	u.midimsg.bData[2] = cap((int)luaL_optinteger(L, 2, 64), 1, 128, 0);
	u.id = (int)luaL_optinteger(L, 3, 0);
	if (u.id == -1)
		u.id = g_unique_id++;
	u.dt = (int)luaL_optinteger(L, 4, 0);
	u.track = cap((int)luaL_optinteger(L, 5, 1), 0, MAXTRACK, 1);
	if (u.track >= 0)
	{
		u.midimsg.bData[0] = (MIDI_NOTEON << 4);
		u.midimsg.bData[3] = 0;
		u.nbbyte = 3;
		//mlog("noteon %X %d %d", u.midimsg.bData[0], u.midimsg.bData[1], u.midimsg.bData[2]);
		if (sendmsgdt(u) == false)
			retCode = -1;
		else
			retCode = u.id;
	}
	lua_pushinteger(L, retCode);	
	unlock_mutex_out();
	return (1);
}
static int LoutNoteOff(lua_State *L)
{
	// parameter #1 : pitch  1..127 ( if unique id is defined : optional = 0 )
	// parameter #2 : optional velocity, default 0
	// parameter #3 : optional unique id ( to manage concurrential note-on note-off )
	// parameter #4 : optional integer delay dt in ms before to send the msg
	// parameter #5 : optional nrTrack ( default 1 )
	lock_mutex_out();

	int retCode = -1;
	T_midioutmsg u;
	int p = (int)lua_tointeger(L, 1);
	while (p > 127)
		p -= 12;
	while (p < 0)
		p += 12;
	u.midimsg.bData[1] = p;
	u.midimsg.bData[2] = cap((int)luaL_optinteger(L, 2, 0), 0, 128, 0);
	u.id = (int)luaL_optinteger(L, 3, 0);
	u.dt = (int)luaL_optinteger(L, 4, 0);
	u.track = cap((int)luaL_optinteger(L, 5, 1), 0, MAXTRACK, 1);
	if (u.track >= 0)
	{
		u.midimsg.bData[0] = (MIDI_NOTEOFF << 4);
		u.midimsg.bData[3] = 0;
		u.nbbyte = 3;
		if (unqueue(OUT_QUEUE_NOTEOFF, u) == 0)
			retCode = sendmsgdt(u);
		else
			retCode =  0;
	}
	else
		retCode = -1;

	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutPressure(lua_State *L)
{
    // parameter #1 : pitch
    // parameter #2 : pressure
    // parameter #3 : optional integer delay dt in ms before to send the msg
 	// parameter #4 : optional nrTrack ( default 1 )
	lock_mutex_out();

	int retCode = -1;
	T_midioutmsg u;
	int p = (int)lua_tointeger(L, 1);
	while (p > 127)
		p -= 12;
	while (p < 0)
		p += 12;
	u.midimsg.bData[1] = p;
	u.midimsg.bData[2] = cap((int)lua_tointeger(L, 2), 0, 128, 0);
	u.dt = (int)luaL_optinteger(L, 3, 0);
	u.track = cap((int)luaL_optinteger(L, 4, 1), 0, MAXTRACK, 1);
	if (u.track >= 0)
	{
		u.nbbyte = 3;
		u.id = 0;
		u.midimsg.bData[0] = (MIDI_KEYPRESSURE << 4);
		u.midimsg.bData[3] = 0;
		retCode = sendmsgdt(u);
	}
	else
		retCode = -1;

	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutControl(lua_State *L)
{
    // parameter #1 : nr control
    // parameter #2 : data
    // parameter #3 : optional integer delay dt in ms before to send the msg
	// parameter #4 : optional nrTrack ( default 1 )
	lock_mutex_out();

	int retCode = -1;
	T_midioutmsg u;
	u.midimsg.bData[1] = cap((int)lua_tointeger(L, 1), 0, 128, 0);
	u.midimsg.bData[2] = cap((int)lua_tointeger(L, 2), 0, 128, 0);
	u.dt = (int)luaL_optinteger(L, 3, 0);
	u.track = cap((int)luaL_optinteger(L, 4, 1), 0, MAXTRACK, 1);
	if (u.track >= 0)
	{
		u.midimsg.bData[0] = (MIDI_CONTROL << 4);
		u.midimsg.bData[3] = 0;
		u.nbbyte = 3;
		u.id = 0;
		retCode =sendmsgdt(u);
	}
	else
		retCode = -1;

	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutProgram(lua_State *L)
{
    // parameter #1 : nr program
    // parameter #2 : optional integer delay dt in ms before to send the msg
	// parameter #3 : optional nrTrack ( default 1 )
    // parameter #4 : optional bank msb
    // parameter #5 : optional bank lsb
	lock_mutex_out();
	
	T_midioutmsg u;
	int retCode = -1;
	u.midimsg.bData[1] = cap((int)lua_tointeger(L, 1), 0, 128, 0);
	u.midimsg.bData[2] = 0;
	u.dt = (int)luaL_optinteger(L, 2, 0);
	u.track = cap((int)luaL_optinteger(L, 3, 1), 0, MAXTRACK, 1);
	if (u.track >= 0)
	{
		u.midimsg.bData[0] = (MIDI_PROGRAM << 4);
		u.midimsg.bData[3] = 0;
		u.nbbyte = 2;
		u.id = 0;
		int bank_msb = cap((int)luaL_optinteger(L, 4, -1), -1, 128, 0);
		int bank_lsb = cap((int)luaL_optinteger(L, 5, -1), -1, 128, 0);
		if (bank_msb != -1)
		{
			T_midioutmsg u1;
			u1.midimsg.bData[0] = (MIDI_CONTROL << 4);
			u1.midimsg.bData[1] = 0;
			u1.midimsg.bData[2] = bank_msb;
			u1.midimsg.bData[3] = 0;
			u1.track = u.track;
			u1.dt = u.dt;
			u1.id = 0;
			u1.nbbyte = 3;
			sendmsgdt(u1);
		}
		if (bank_lsb != -1)
		{
			T_midioutmsg u1;
			u1.midimsg.bData[0] = (MIDI_CONTROL << 4);
			u1.midimsg.bData[1] = 0x20;
			u1.midimsg.bData[2] = bank_lsb;
			u1.midimsg.bData[3] = 0;
			u1.track = u.track;
			u1.dt = u.dt;
			u1.id = 0;
			u1.nbbyte = 3;
			sendmsgdt(u1);
		}
		retCode = sendmsgdt(u);
	}
	else
		retCode = -1;

	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutPitchbend(lua_State *L)
{
    // parameter #1 : value -8192 ..0..8192
    // parameter #2 : optional integer delay dt in ms before to send the msg
	// parameter #3 : optional nrTrack ( default 1 )
	lock_mutex_out();

	int retCode = -1;
	T_midioutmsg u;
	int v = cap((int)lua_tointeger(L, 1), -8192, 8192, 0) + (int)(0x40)*(int)(0x80);
	u.dt = (int)luaL_optinteger(L, 2, 0);
	u.track = cap((int)luaL_optinteger(L, 3, 1), 0, MAXTRACK, 1);
	if (u.track >= 0)
	{
		u.midimsg.bData[0] = (MIDI_PITCHBEND << 4) ;
		u.midimsg.bData[2] = (int)(v / (int)(0x80));
		u.midimsg.bData[1] = (int)(v - (int)(u.midimsg.bData[2]) * (int)(0x80));
		u.midimsg.bData[3] = 0;
		u.nbbyte = 3;
		u.id = 0;
		retCode =  sendmsgdt(u);
	}
	else
		retCode = -1;

	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutChannelPressure(lua_State *L)
{
    // parameter #1 : value
    // parameter #2 : optional integer delay dt in ms before to send the msg
    // parameter #3 : optional nrTrack ( default 1 )
	lock_mutex_out();
	
	int retCode = -1;
	T_midioutmsg u;
	u.midimsg.bData[1] = cap((int)lua_tointeger(L, 1), 0, 128, 0);
	u.midimsg.bData[2] = 0;
	u.dt = (int)luaL_optinteger(L, 2, 0);
	u.track = cap((int)luaL_optinteger(L, 3, 1), 0, MAXTRACK, 1);
	if (u.track >= 0)
	{
		u.midimsg.bData[0] = (MIDI_CHANNELPRESSURE << 4);
		u.midimsg.bData[3] = 0;
		u.nbbyte = 2;
		u.id = 0;
		retCode = sendmsgdt(u);
	}
	else
		retCode = -1;

	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutTune(lua_State *L)
{
    // parameter #1 : optional, float, A4 Frequency Hz ( default 440 Hz )
	// parameter #2 : optional nrTrack ( default 1 )
	lock_mutex_out();
	
	float freq = luaL_optnumber(L, 1, 440.0);
	int nrTrack = cap((int)luaL_optinteger(L, 2, 1), 0, MAXTRACK, 1);
	send_tune(nrTrack, freq);
	unlock_mutex_out();
	return (0);
}
static int LoutBendRange(lua_State *L)
{
    // parameter #1 : semitone , integer , ( default 1 )
	// parameter #2 : optional nrTrack ( default 1 )
	lock_mutex_out();
	
	int semitone = (int)luaL_optinteger(L, 1, 1);
	int nrTrack = cap((int)luaL_optinteger(L, 2, 1), 0, MAXTRACK, 1);
	send_bendrange(nrTrack, semitone);
	unlock_mutex_out();
	return (0);
}
static int LoutAllNoteOff(lua_State *L)
{
	// parameter #1 : optional, string options with a n s or c ( a:all n:noteoff s:soundoff , c:controler )
	// parameter #2 : optional integer track ( default all )
	lock_mutex_out();

	const char* soption = luaL_optstring(L, 1, "a");
	int nrTrack = cap((int)luaL_optinteger(L, 2, 0), -1, MAXTRACK, 1);
	all_note_off(soption, nrTrack);
	unlock_mutex_out();
	return (0);
}

static int LoutClock(lua_State *L)
{
    // parameter #1 : optional integer track ( default 1  )
	lock_mutex_out();
	
	T_midioutmsg u;
	u.track = cap((int)luaL_optinteger(L, 2, 1), 0, MAXTRACK, 1);
	u.midimsg.bData[0] = MIDI_CLOCK;
    u.midimsg.bData[1] = 0;
    u.midimsg.bData[2] = 0;
    u.midimsg.bData[3] = 0;
	u.nbbyte = 1;
    lua_pushinteger(L, sendmsgdt(u));  
	unlock_mutex_out();
	return (1);
}
static int LoutSystem(lua_State *L)
{
    // parameter #1 : byte 1
    // parameter #2 : byte 2
    // parameter #3 : byte 3
   // parameter #5 : optional integer track ( default 1  )
	lock_mutex_out();
	
	T_midioutmsg u;
	u.midimsg.bData[0] = cap((int)lua_tointeger(L, 1), 0, 128, 0);
	u.midimsg.bData[1] = cap((int)lua_tointeger(L, 2), 0, 128, 0);
	u.midimsg.bData[2] = cap((int)lua_tointeger(L, 3), 0, 128, 0);
	u.nbbyte = 3;
	u.track = cap((int)luaL_optinteger(L, 4, 1), 0, MAXTRACK, 1);
	lua_pushinteger(L, sendmsgdt(u));
    
	unlock_mutex_out();
	return (1);
}
static int LoutSysex(lua_State *L)
{
    // parameter #1 : message sysex
	// parameter #2 : optional integer track ( default 0  )
	lock_mutex_out();

	const char *sysex = lua_tostring(L, 1);
	int nrTrack = cap((int)luaL_optinteger(L, 2, 1), 0, MAXTRACK, 1);
	int retCode = send_sysex(nrTrack, sysex);
	lua_pushinteger(L, retCode);

	unlock_mutex_out();
	return (1);
}
static int LaudioClose(lua_State *L)
{
	// close audio
	lock_mutex_out();
	mixer_free();
	vi_free();
	vi_init();
	mixer_init();
	unlock_mutex_out();
	return(0);
}
static int LaudioList(lua_State *L)
{
	// return list of audio devices available
    // no parameter
	// return : LUA table strings
	lock_mutex_out();
	
	DWORD nr_device;
    lua_newtable(L);
	char name_audio[MAXBUFCHAR];
    nr_device = 0;
	while (audio_name(nr_device, name_audio))
	{
		lua_pushinteger(L, nr_device + 1);
		lua_pushstring(L, name_audio);
		lua_settable(L, -3);
	}
	unlock_mutex_out();
	return(1);
}
static int LaudioName(lua_State *L)
{
	// return name of audio device
	// parameter #1 : audio_device 1...
	// return : string ( empty if no  device )
	lock_mutex_out();

	char name_audio[MAXBUFCHAR];
	*name_audio = '\0';
	int nr_deviceaudio = cap((int)lua_tointeger(L, 1), 0, MAX_AUDIO_DEVICE, 1);
	audio_name(nr_deviceaudio, name_audio);
	lua_pushstring(L, name_audio);
	unlock_mutex_out();
	return(1);
}

static int LaudioAsioBuflenSet(lua_State *L)
{
	// set audio buffer settings ( to be called before any use of audio ( vi, wav ... )
	// parameter #1 : Buffer length in samples ( 0 :use the prefereed one )
	lock_mutex_out();
	g_audio_buffer_length = (int)lua_tointeger(L, 1);
	unlock_mutex_out();
	return (0);
}
static int LaudioDefaultDevice(lua_State *L)
{
	// set audio default device ( to be called before any use of audio ( vi, wav ... )
	// parameter #1 : audio_device 1...
	lock_mutex_out();
	g_default_audio_device = cap((int)lua_tointeger(L, 1), 0, MAX_AUDIO_DEVICE, 1);
	unlock_mutex_out();
	return (0);
}
static int LaudioAsioSet(lua_State *L)
{
	// set audio settings ( to be called after audio opening )
	// parameter #1 : audio device 1...
	// return : buffer_length
	lock_mutex_out();
	int buflen = 0;
#ifdef V_PC
	buflen = 1024;
	int nr_deviceaudio = cap(lua_tointeger(L, 1), 0, MAX_AUDIO_DEVICE, 1);
	bool toBeFree = true ;
	bool err = false;
	if (BASS_ASIO_Init(nr_deviceaudio) == FALSE)
	{
		if (BASS_ASIO_ErrorGetCode() == BASS_ERROR_ALREADY )
			toBeFree = false;
		else
		{
			err = true;
			mlog("audioSet : Error BASS_ASIO_Init err:%d", BASS_ASIO_ErrorGetCode());
		}
	}
	if ((!err) && (BASS_ASIO_SetDevice(nr_deviceaudio) == FALSE))
	{
		mlog("audioSet :  Error BASS_ASIO_SetDevice err:%d", BASS_ASIO_ErrorGetCode());
		err = true;
	}
	if ((!err) && (BASS_ASIO_ControlPanel() == FALSE))
	{
		mlog("audioSet :  Error BASS_ASIO_ControlPanel err:%d", BASS_ASIO_ErrorGetCode());
	}
	BASS_ASIO_INFO info;
	if (!err)
	{
		if (BASS_ASIO_GetInfo(&info) == FALSE)
		{
			mlog("audioSet :  Error BASS_ASIO_GetInfo err:%d", BASS_ASIO_ErrorGetCode());
		}
		else
		{
			buflen = info.bufpref;
		}
	}
	if ((! err ) && (toBeFree))
	{
		BASS_ASIO_Free();
	}
#endif
	lua_pushinteger(L, buflen);
	unlock_mutex_out();
	return (1);
}
static int LviVolume(lua_State *L)
{
	// set the volume of a midi VI
	// parameter #1 : VI track ( returned by vi_open )
	// parameter #2 : volume 0..64..127
	lock_mutex_out();

	int return_code = 0;
	int vi_nr = (int)lua_tointeger(L, 1) - VI_ZERO;
	int volume = (int)lua_tointeger(L, 2);
	if ((vi_nr >= 0) && (vi_nr < g_vi_opened_nb))
	{
		if (g_vi_opened[vi_nr].sf2_midifont != 0)
		{
			if (BASS_MIDI_FontSetVolume(g_vi_opened[vi_nr].sf2_midifont, (float)(volume) / 64.0) == FALSE)
			{
				mlog("Error setting volume VI , err=%d\n", BASS_ErrorGetCode());
				return_code = -1;
			}
		}
		else
		{
			if (BASS_ChannelSetAttribute(g_vi_opened[vi_nr].mstream, BASS_ATTRIB_VOL, (float)(volume) / 64.0) == FALSE)
			{
				mlog("Error setting volume VI , err=%d\n", BASS_ErrorGetCode());
				return_code = -1;
			}
		}
	}
	else
	{
		mlog("Error volume VI, incorrect nrVI %d\n", vi_nr);
		return_code = -1;
	}
	lua_pushinteger(L, return_code);

	unlock_mutex_out();
	return (1);
}

static int LsoundPlay(lua_State *L)
{
    // parameter #1 : audio file name, with its extension
    // parameter #2 : optional  volume 0..64..127
    // parameter #3 : optional  pan 0..64..127
    // parameter #4 : optional  integer, audio ( ASIO for PC ) device nr, default g_default_audio_device 
    // returned : ref for further manipulation
	lock_mutex_out();
	
	int return_code = 0;
	const char *fname = lua_tostring(L, 1);
    int volume = (int)luaL_optinteger(L, 2, 64);
    int pan = (int)luaL_optinteger(L, 3, 64);
	int nr_deviceaudio = cap((int)luaL_optinteger(L, 4, g_default_audio_device + 1), 0, MAX_AUDIO_DEVICE, 1);
	char vinamedevice[MAXBUFCHAR];
	char viname[MAXBUFCHAR];
	char extension[6];
	strcpy(vinamedevice, fname);
	int forced_device_audio = -1;
	if (getTypeFile(vinamedevice, &forced_device_audio, viname, extension) && ( strcmp(extension,"wav") == 0))
	{
		if (forced_device_audio != -1)
			nr_deviceaudio = forced_device_audio;
		return_code = sound_play(viname, volume, pan, nr_deviceaudio);
	}
    lua_pushinteger(L, return_code);

	unlock_mutex_out();
	return (1);
}
static int LsoundControl(lua_State *L)
{
	// parameter #1 : reference to audio ( returned by sound_play )
	// parameter #2 : optional  volume 0..64..127
	// parameter #3 : optional  pan 0..64..127
	// parameter #4 : control the sound : 0=pause, 1=restart, 2=stop
	lock_mutex_out();

	int return_code = 0;
	int hsound = (int)lua_tointeger(L, 1);
	int volume = luaL_optnumber(L, 2, 64);
	int pan = luaL_optnumber(L, 3, 64);
	int ctrl = (int)luaL_optinteger(L, 4, -1);
	return_code = sound_control(hsound, volume, pan , ctrl);
	lua_pushinteger(L, return_code);

	unlock_mutex_out();
	return (1);
}

static int LoutListProgramVi(lua_State *L)
{
	// create files with the programs in a VI
	// parameter #1 : VI file name, with its .sf2 or .dll extension
	lock_mutex_out();

	const char *fname = lua_tostring(L, 1);
	char vinamedevice[MAXBUFCHAR];
	int nr_deviceaudio ;
	char viname[MAXBUFCHAR];
	strcpy(vinamedevice, fname);
	char extension[6];
	int retCode = true;;
	if (getTypeFile(vinamedevice, &nr_deviceaudio, viname, extension))
	{
		if ( strcmp(extension,"sf2") == 0 )
			retCode = sf2_create_list_prog(viname);
		if (strcmp(extension, "dll") == 0)
			retCode = vst_create_list_prog(viname);
	}
	lua_pushinteger(L, retCode);

	unlock_mutex_out();
	return (1);
}
static int LoutTrackOpenVi(lua_State *L)
{
	// open the track on a Virtual-Instrument ( midi-SF2 or VSTI )
	// parameter #1 : integer track nr
	// parameter #2 : integer midi channel
	// parameter #3 : string initial
	// parameter #4 : SF2/VSTI filename, with its .sf2/.dll extension. If file-name ends with @x : x is the parameter#7
	// parameter #5 : optional , integer, number of additional physical MIDI channels attached to this MIDI channel ( default 0 )
	// parameter #6 : optional  integer, Vi font volume 0..64..127
	// parameter #7 : optional  integer, audio ( ASIO for PC ) device nr. Default g_default_audio_device, or as defined in parameter #4
	lock_mutex_out();
	bool retCode = false;
	int nrTrack = cap((int)lua_tointeger(L, 1), 0, MAXTRACK, 1);
	int nr_channelmidi = cap((int)lua_tointeger(L, 2), 0, MAXTRACK, 1);
	const char *tuning = lua_tostring(L, 3);
	const char *fname = lua_tostring(L, 4);
	int nb_extended_midichannel = cap((int)luaL_optinteger(L, 5, 0), 0, 10, 0);
	int volume = (int)luaL_optinteger(L, 6, 64);
	int nr_deviceaudio = cap((int)luaL_optinteger(L, 7, g_default_audio_device + 1), 0, MAX_AUDIO_DEVICE, 1);
	char vinamedevice[MAXBUFCHAR];
	char viname[MAXBUFCHAR];
	char extension[6];
	strcpy(vinamedevice, fname);
	int forced_device_audio = -1;
	if (getTypeFile(vinamedevice, &forced_device_audio, viname, extension) && ((strcmp(extension, "dll") == 0) || (strcmp(extension, "sf2") == 0)))
	{
		int nr_device;
		if (forced_device_audio != -1)
			nr_deviceaudio = forced_device_audio;
		nr_device = mvi_open(viname, nr_deviceaudio, volume, (strcmp(extension, "sf2") == 0));
		if (nr_device != -1)
		{
			g_tracks[nrTrack].device = nr_device;
			g_tracks[nrTrack].channel = nr_channelmidi;
			channel_extended_set(nr_device, nr_channelmidi, nb_extended_midichannel, true);
			g_tracks[nrTrack].volume = 64;
			string_to_control(nrTrack, tuning);
			retCode = true;
			mlog("Information : vi open file %s for track#%d : OK", fname, nrTrack + 1);
		}
		else
			mlog("Error : midi vi open file %s for track#%d", fname, nrTrack + 1);
	}
	lua_pushinteger(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LoutTrackOpenMidi(lua_State *L)
{
	// open the track on a midi out
	// parameter #1 : integer track nr
	// parameter #2 : integer midi channel
	// parameter #3 : string initial
	// parameter #4 : midi out device
	// parameter #5 : optional number of additional physical MIDI channels attached to this MIDI channel ( default 0 )
	// parameter #6 : optional track name for information
	// parameter #7 : optional localoff. Default true
	lock_mutex_out();

	int nrTrack = cap((int)lua_tointeger(L, 1), 0, MAXTRACK, 1);
	int nr_channelmidi = cap((int)lua_tointeger(L, 2), 0, MAXCHANNEL, 1);
	const char *tuning = lua_tostring(L, 3);
	int nr_devicemidi = cap((int)lua_tointeger(L, 4), 0, MIDIOUT_MAX, 1);
	int nb_extended_midichannel = cap((int)luaL_optinteger(L, 5, 0), 0, 10, 0);
	const char *trackName = luaL_optstring(L, 6, "");
	int localoff = (int)luaL_optinteger(L, 7, (int)true);
	
	int nr_device = midiopen(nr_devicemidi);
	if (nr_device != -1)
	{
		g_tracks[nrTrack].device = nr_device;
		g_tracks[nrTrack].channel = nr_channelmidi;
		channel_extended_set(nr_device, nr_channelmidi, nb_extended_midichannel, true);
		g_tracks[nrTrack].volume = 64;
		if ( localoff ) 
			string_to_control(nrTrack, "C122/0");
		string_to_control(nrTrack, tuning);
		char name_device[MAXBUFCHAR] = "";
		midi_out_name(nr_device, name_device);
		mlog("Information : midiOut open device#%d<%s> for track#%d<%s> %s : OK", nr_device + 1, name_device, nrTrack + 1, trackName,localoff?"with localoff":"");
	}
	else
		mlog("Error : midiOut open device#%d for track#%d<%s>", nr_device + 1, nrTrack + 1, trackName);
	lua_pushinteger(L, (nr_device + 1));
	
	unlock_mutex_out();
	return (1);
}
static int LoutTracksClose(lua_State *L)
{
//	mlog("tracksclose");
//	inspect_device();
//	inspect_channel();
	// close all output tracks
	lock_mutex_out();
	track_init();
	unlock_mutex_out();

	return(0);
}

static int Linit(lua_State *L)
{
	// init the luabass module.
	// parameter #1 : fullpath of log file
	lock_mutex_out();
	const char *fname = lua_tostring(L, 1);
	init(fname);
	unlock_mutex_out();
	return (0);
}
static int Lfree(lua_State *L)
{
	lock_mutex_out();
	free();
	unlock_mutex_out();
	return (0);
}

static int LonMidiOut(lua_State *L)
{
	// set a LUA script for each midiout msg
	// this script contains optional functions :
	//	onNoteOn(track,pitch,velocity) : process midiout noteon
	//	onNoteOff(track,pitch,velocity) : process midiout noteoff
	//	onProgram(track,program) : process midiout program
	//	onControl(track,control,value) : process midiout control
	//	onPitchbend(track,value) : process midiout pitchbend
	//	onChannelPressure(track,value) : process midiout channel pressure
	//	onKeyPressure(track,pitch, value) : process midiout key pressure
	//	onClock(track) : process midiout clock
	// The LUA function returns midi-messages to play. 
	// These midi-messages are played immediatly.
	// Each midi-messages is define by these 5 parameters :
	//	track-nr : integer ( must already exists and opened )
	//	type message : string ( MIDI_NOTEON MIDI_NOTEOFF MIDI_CONTROL MIDI_PROGRAM MIDI_CHANNELPRESSURE MIDI_KEYPRESSURE MIDI_PITCHBEND )
	//  value 1 : integer 0..127 ( e.g pitch for noteon, program, ... )
	//  value 2 : integer 0..127 ( e.g. velocity for noteon, no meaning fro program , .. )

	// parameter #1 : LUA file name, with its extension
	lock_mutex_out();
	const char *fname = lua_tostring(L, 1);
	bool retCode = onMidiout_open(fname);
	lua_pushboolean(L, retCode);
	unlock_mutex_out();
	return (1);
}
static int LsetVarMidiOut(lua_State *L)
{
	// set a global variable in LUA script which processes the midiout  
	// parameter #1 : variable name
	// parameter #2 : its value
	if (g_LUAoutState)
	{
		lock_mutex_out();
		const char *name = lua_tostring(L, 1);
		lua_xmove(L, g_LUAoutState,1);
		lua_setglobal(g_LUAoutState, name);
		unlock_mutex_out();
	}
	return (0);
}

static int Llog(lua_State *L)
{
	const char* s = lua_tostring(L, 1);
	lock_mutex_out();
	mlog(s);
	unlock_mutex_out();
	return(0);
}
static int LoutGetLog(lua_State *L)
{
	lock_mutex_out();
	g_collectLog = lua_tointeger(L, 1) ? true : false;
	if ((g_collectLog) && (nrOutBufLog != nrInBufLog))
	{
		lua_pushboolean(L, true);
		//lua_pushinteger(L, 3);
		lua_pushstring(L, bufLog[nrOutBufLog]);
		nrOutBufLog++;
		if (nrOutBufLog >= MAXNBLOGOUT)
			nrOutBufLog = 0;
	}
	else
	{
		lua_pushboolean(L, false);
		lua_pushstring(L, "");
	}
	unlock_mutex_out();

	return(2);
}

// publication of functions visible from LUA script
//////////////////////////////////////////////////

static const struct luaL_Reg luabass[] =
{
	// list of functions available in the module { "name_in_LUA", C_name }

	{ sinit, Linit }, // init MIDI and audio
	{ sfree , Lfree }, // free MIDI and audio ressources

	{ "onMidiOut", LonMidiOut }, // set a LUA script for each MIDI-out
	{ "setVarMidiOut", LsetVarMidiOut }, // set a global variable in the LUA script for MIDI-out

	{ "logmsg", Llog }, // log a string in mlog file
	{ soutGetLog, LoutGetLog }, // get log


	////// in ///////

	{ "inGetMidiList", LinGetMidiList }, // list the midiin ports 
	{ sinGetMidiName, LinGetMidiName }, // name of midiin port 

	/////// out ////////
	{ "outSetCurve", LoutSetCurve }, // set the curves
	{ "outTranspose", LoutTranspose }, // transpose

	{ "outGetMidiList", LoutGetMidiList }, // list the midiout ports 
	{ soutGetMidiName, LoutGetMidiName }, // return name of midiout port 

	{ soutListProgramVi, LoutListProgramVi }, // create list of programs available in a Virtual Instrument ( SF2 or VST )
	{ soutTrackOpenVi, LoutTrackOpenVi }, // open a track on Virtual Instrument ( SF2 or VST )
	{ soutTrackOpenMidi, LoutTrackOpenMidi }, // open a track on an MIDI-out/channel

	{ soutTracksClose, LoutTracksClose }, // close all outputs tracks
	{ soutTrackMute, LoutTrackMute }, // mute this track
	{ soutSetTrackVolume, LoutSetTrackVolume }, // set the volume of the noteOn on this track
	{ soutGetTrackVolume, LoutGetTrackVolume }, // get the volume of the noteOn on this track
	{ soutSetTrackCurve, LoutSetTrackCurve }, // set the curve of the noteOn on this track
	{ soutSetTrackInstrument, LoutSetTrackInstrument }, // set the instrument on this track
	{ soutSetVolume, LoutSetVolume }, // set the volume of the outputs
	{ soutGetVolume, LoutGetVolume }, // get the volume of the outputs

	{ soutSetChordCompensation, LoutSetChordCompensation }, // set chord compensation 
	{ soutSetRandomDelay, LoutSetRandomDelay }, // set random delay 
	{ soutSetRandomVelocity, LoutSetRandomVelocity }, // set random velocity 

	{ "outChordSet", LoutChordSet }, // set a chord 
	{ "outChordOn", LoutChordOn }, // send a chord-on a track
	{ "outChordOff", LoutChordOff }, // send a chord-off on a track

	{ "outNoteOn", LoutNoteOn }, // send a note-on a track
	{ "outNoteOff", LoutNoteOff }, // send a note-off on a track
	{ "outProgram", LoutProgram }, // send a program on a track
	{ "outControl", LoutControl }, // send a control on a track
	{ "outPitchbend", LoutPitchbend }, // send a pitchbend on a track
	{ "outChannelPressure", LoutChannelPressure }, // send a channel pressure on a track
	{ "outPressure", LoutPressure }, // send a key pressure on a track
	{ "outTune", LoutTune }, // tune the A4 frequency on a track
	{ "outBendrange", LoutBendRange }, // set the bend range in semiton on a track

	{ soutAllNoteOff, LoutAllNoteOff }, // switch off all current notes on a track
	
	{ "outSysex", LoutSysex }, // send a sysex message on a track ( midi only )
	{ "outClock", LoutClock }, // send a timing-clock message on a track ( midi only )
	{ "outSystem", LoutSystem }, // send a free-format short midi message on a track ( midi only )

	{ "audioList", LaudioList }, // list audio device
	{ "audioName", LaudioName }, // name audio device
	{ "audioClose", LaudioClose }, // close audio device
	{ "audioAsioSet", LaudioAsioSet }, // open audio asio device settings
	{ "audioAsioBuflenSet", LaudioAsioBuflenSet }, // set audio asio device buffer length 
	{ "audioDefaultDevice", LaudioDefaultDevice }, // set audio default device 
	{ "viVolume", LviVolume }, // volume of the vi

	{ "outSoundPlay", LsoundPlay }, // play a sound file
	{ "outSoundControl", LsoundControl }, // control a sound playing

	{ NULL, NULL }
};
int luaopen_luabass(lua_State *L)
{
    // used by LUA funtion : luabass = require "luabass"
    // luabass.dll must be in a path "visible" from the LUA interpreter
    // after "require", function can be called from LUA, using luabass.open, ...
    // this function is the only one which must be visible in the DLL from LUA script
    // it is declared in the luabass.def
    // the def file is linked with the optopn set in the properties/link
    luaL_newlib(L, luabass);
    return(1);
}

