
// Game Link

//------------------------------------------------------------------------------
// Dependencies
//------------------------------------------------------------------------------

#include "config.h"

#if C_GAMELINK

// External Dependencies
#ifdef WIN32
#include <Windows.h>
#else // WIN32
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>
#endif // WIN32

// SDL Dependencies
#include "SDL_syswm.h"

// Local Dependencies
#include "dosbox.h"
#include "gamelink.h"
#include "../resource.h"

extern bool g_paused;

//==============================================================================

//------------------------------------------------------------------------------
// Local Definitions
//------------------------------------------------------------------------------

#define SYSTEM_NAME		"DOSBox"

#define PROTOCOL_VER		4

#ifdef WIN32
#define GAMELINK_MUTEX_NAME		"DWD_GAMELINK_MUTEX_R4"
#else // WIN32
#define GAMELINK_MUTEX_NAME		"/DWD_GAMELINK_MUTEX_R4"
#endif // WIN32

#ifdef MACOSX
#define GAMELINK_MMAP_NAME		"/DWD_GAMELINK_MMAP_R4"
#else // MACOSX
#define GAMELINK_MMAP_NAME		"DWD_GAMELINK_MMAP_R4"
#endif // MACOSX


//------------------------------------------------------------------------------
// Local Data
//------------------------------------------------------------------------------

#ifdef WIN32

// {14CF6AD7-7359-44ea-BAD7-DC36F3F7738C}
static const GUID g_guid_trayicon =
{ 0x14cf6ad7, 0x7359, 0x44ea, { 0xba, 0xd7, 0xdc, 0x36, 0xf3, 0xf7, 0x73, 0x8c } };
static NOTIFYICONDATAA g_systray_icon;

static HANDLE g_mutex_handle;
static HANDLE g_mmap_handle;

#else // WIN32

static sem_t* g_mutex_handle;
static int g_mmap_handle; // fd!

#endif // WIN32

static bool g_trackonly_mode;

static Bit32u g_membase_size;

static GameLink::sSharedMemoryMap_R4* g_p_shared_memory;

#define MEMORY_MAP_CORE_SIZE sizeof( GameLink::sSharedMemoryMap_R4 )


//------------------------------------------------------------------------------
// Local Functions
//------------------------------------------------------------------------------

static void shared_memory_init()
{
	// Initialise

	g_p_shared_memory->version = PROTOCOL_VER;
	g_p_shared_memory->flags = 0;

	memset( g_p_shared_memory->system, 0, sizeof( g_p_shared_memory->system ) );
	strcpy( g_p_shared_memory->system, SYSTEM_NAME );
	memset( g_p_shared_memory->program, 0, sizeof( g_p_shared_memory->program ) );

	g_p_shared_memory->program_hash[ 0 ] = 0;
	g_p_shared_memory->program_hash[ 1 ] = 0;
	g_p_shared_memory->program_hash[ 2 ] = 0;
	g_p_shared_memory->program_hash[ 3 ] = 0;

	// reset input
	g_p_shared_memory->input.mouse_dx = 0;
	g_p_shared_memory->input.mouse_dy = 0;

	g_p_shared_memory->input.mouse_btn = 0;
	for ( int i = 0; i < 8; ++i ) {
		g_p_shared_memory->input.keyb_state[ i ] = 0;
	}

	// reset peek interface
	g_p_shared_memory->peek.addr_count = 0;
	memset( g_p_shared_memory->peek.addr, 0, GameLink::sSharedMMapPeek_R2::PEEK_LIMIT * sizeof( Bit32u ) );
	memset( g_p_shared_memory->peek.data, 0, GameLink::sSharedMMapPeek_R2::PEEK_LIMIT * sizeof( Bit8u ) );

	// blank frame
	g_p_shared_memory->frame.seq = 0;
	g_p_shared_memory->frame.image_fmt = 0; // = no frame
	g_p_shared_memory->frame.width = 0;
	g_p_shared_memory->frame.height = 0;

	g_p_shared_memory->frame.par_x = 1;
	g_p_shared_memory->frame.par_y = 1;
	memset( g_p_shared_memory->frame.buffer, 0, GameLink::sSharedMMapFrame_R1::MAX_PAYLOAD );

	// audio: 100%
	g_p_shared_memory->audio.master_vol_l = 100;
	g_p_shared_memory->audio.master_vol_r = 100;

	// RAM
	g_p_shared_memory->ram_size = g_membase_size;
}

//
// create_mutex
//
// Create a globally unique mutex.
//
// \returns 1 if we made one, 0 if it failed or the mutex existed already (also a failure).
//
static int create_mutex( const char* p_name )
{

#ifdef WIN32

	// The mutex is already open?
	g_mutex_handle = OpenMutexA( SYNCHRONIZE, FALSE, p_name );
	if ( g_mutex_handle != 0 ) {
		// .. didn't fail - so must already exist.
		CloseHandle( g_mutex_handle );
		g_mutex_handle = 0;
		return 0;
	}

	// Actually create it.
	g_mutex_handle = CreateMutexA( NULL, FALSE, p_name );
	if ( g_mutex_handle ) {
		return 1;
	}

#else // WIN32

	// The mutex is already open?
	g_mutex_handle = sem_open( p_name, 0, 0666, 0 );
	if ( g_mutex_handle != SEM_FAILED )
    {
		// .. didn't fail - so must already exist.
		LOG_MSG( "GAMELINK: ERROR: MUTEX \"%s\" already exists.", p_name );
		LOG_MSG( "GAMELINK: Already running Game Link, or maybe a crash?", p_name );
#ifdef MACOSX
		LOG_MSG( "GAMELINK: Might need to manually reboot the system." );
#else // MACOSX
		LOG_MSG( "GAMELINK: Might need to manually tidy up in /dev/shm (or reboot system)." );
#endif // MACOSX
		sem_close( g_mutex_handle );
		g_mutex_handle = 0;
		return 0;
	}

	// Actually create it.
	g_mutex_handle = sem_open( p_name, O_CREAT, 0666, 1 );
	if ( g_mutex_handle == SEM_FAILED )
    {
		LOG_MSG( "GAMELINK: Creating MUTEX \"%s\" failed. [sem_open=%d, errno=%d]", p_name, (int)(size_t)g_mutex_handle, errno );
#ifdef MACOSX
		LOG_MSG( "GAMELINK: Might need to manually reboot the system." );
#else // MACOSX
		LOG_MSG( "GAMELINK: Might need to manually tidy up in /dev/shm (or reboot system)." );
#endif // MACOSX
		g_mutex_handle = 0;
	}
    else
    {
		return 1;
	}

#endif // WIN32

	return 0;
}

//
// destroy_mutex
//
// Tidy up the mutex.
//
static void destroy_mutex( const char* p_name )
{
#ifdef WIN32

	(void)(p_name);

	if ( g_mutex_handle )
	{
		CloseHandle( g_mutex_handle );
		g_mutex_handle = NULL;
	}

#else // WIN32

	if ( g_mutex_handle )
	{
		sem_close( g_mutex_handle );
		sem_unlink( p_name );
		g_mutex_handle = NULL;
	}

#endif // WIN32
}

//
// create_shared_memory
//
// Create a shared memory area.
//
// \returns 1 if we made one, 0 if it failed.
//
static int create_shared_memory()
{
	const int memory_map_size = MEMORY_MAP_CORE_SIZE + g_membase_size;

#ifdef WIN32

	g_mmap_handle = CreateFileMappingA( INVALID_HANDLE_VALUE, NULL,
			PAGE_READWRITE, 0, memory_map_size,	GAMELINK_MMAP_NAME );

	if ( g_mmap_handle )
	{
		g_p_shared_memory = reinterpret_cast< GameLink::sSharedMemoryMap_R4* >(
			MapViewOfFile( g_mmap_handle, FILE_MAP_ALL_ACCESS, 0, 0, memory_map_size )
			);

		if ( g_p_shared_memory )
		{
			return 1; // Success!
		}
		else
		{
			// tidy up file mapping.
			CloseHandle( g_mmap_handle );
			g_mmap_handle = NULL;
		}
	}

#else // WIN32

	g_mmap_handle = shm_open( GAMELINK_MMAP_NAME, O_CREAT
#ifndef MACOSX
								| O_TRUNC
#endif // !MACOSX
								| O_RDWR, 0666 );
	g_p_shared_memory = NULL; // defensive

	if ( g_mmap_handle < 0 )
	{
		LOG_MSG( "GAMELINK: shm_open( \"" GAMELINK_MMAP_NAME "\" ) failed. errno = %d", errno );
	}
	else
	{
		// set size
		int r;
		r = ftruncate( g_mmap_handle, memory_map_size );
		if ( r < 0 ) {
			LOG_MSG( "GAMELINK: ftruncate failed with %d. errno = %d", r, errno );
			close( g_mmap_handle );
			g_mmap_handle = -1;
			shm_unlink( GAMELINK_MMAP_NAME );
			g_p_shared_memory = NULL;
			return 0;
		}

		// map to a pointer.
		g_p_shared_memory = reinterpret_cast< GameLink::sSharedMemoryMap_R4* >(
			mmap( 0, memory_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_mmap_handle, 0 )
		);

		if ( g_p_shared_memory == MAP_FAILED )
		{
			LOG_MSG( "GAMELINK: mmap failed. errno = %d", errno );
			close( g_mmap_handle );
			g_mmap_handle = -1;
			shm_unlink( GAMELINK_MMAP_NAME );
			g_p_shared_memory = NULL;
			return 0;
		}

		// success!
		return 1;
	}

#endif // WIN32

	// Failure
	return 0;
}

//
// destroy_shared_memory
//
// Destroy the shared memory area.
//
static void destroy_shared_memory()
{
	const int memory_map_size = MEMORY_MAP_CORE_SIZE + g_membase_size;

#ifdef WIN32

	if ( g_p_shared_memory )
	{
		UnmapViewOfFile( g_p_shared_memory );
		g_p_shared_memory = NULL;
	}

	if ( g_mmap_handle )
	{
		CloseHandle( g_mmap_handle );
		g_mmap_handle = NULL;
	}

#else // WIN32

	if ( g_p_shared_memory )
	{
		munmap( g_p_shared_memory, memory_map_size );
		g_p_shared_memory = NULL;
	}

	if ( g_mmap_handle >= 0 )
	{
		close( g_mmap_handle );
		g_mmap_handle = -1;
	}

	shm_unlink( GAMELINK_MMAP_NAME );

#endif // WIN32

}

//==============================================================================

//------------------------------------------------------------------------------
// GameLink::Init
//------------------------------------------------------------------------------
int GameLink::Init( const bool trackonly_mode )
{
	int iresult;

	// Already initialised?
	if ( g_mutex_handle )
	{
		LOG_MSG( "GAMELINK: Ignoring re-initialisation." );

		// success
		return 1;
	}

	// Store the mode we're in.
	g_trackonly_mode = trackonly_mode;

	// Create a fresh mutex.
	iresult = create_mutex( GAMELINK_MUTEX_NAME );
	if ( iresult != 1 )
	{
		// failed.
		return 0;
	}

	return 1;
}

//------------------------------------------------------------------------------
// GameLink::AllocRAM
//------------------------------------------------------------------------------
Bit8u* GameLink::AllocRAM( const Bit32u size )
{
	int iresult;

	g_membase_size = size;

	// Create a shared memory area.
	iresult = create_shared_memory();
	if ( iresult != 1 )
	{
		destroy_mutex( GAMELINK_MUTEX_NAME );
		// failed.
		return 0;
	}

	// Initialise
	shared_memory_init();

#ifdef WIN32

	// Always clear this.
	memset( &g_systray_icon, 0, sizeof( PNOTIFYICONDATAA ) );

	if ( !g_trackonly_mode )
	{
		// Add tray icon
		g_systray_icon.cbSize = sizeof( PNOTIFYICONDATAA );
		SDL_SysWMinfo wminfo;
		SDL_VERSION(&wminfo.version);
		SDL_GetWMInfo( &wminfo );
		if ( wminfo.window )
		{
			g_systray_icon.hWnd = (HWND)wminfo.window;
			g_systray_icon.guidItem = g_guid_trayicon;
			g_systray_icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
			strcpy( g_systray_icon.szTip, "DOSBox" );
			HINSTANCE hinst;
			hinst = GetModuleHandle( NULL );
			g_systray_icon.hIcon = LoadIconA( hinst, MAKEINTRESOURCEA( IDI_DOSBOX_ICON ) );
			/*if ( g_systray_icon.hIcon == NULL ) {
				DWORD er = GetLastError();
				// printf( "LoadIconA. Error = %d\n", er );
			}*/
			g_systray_icon.uCallbackMessage = WM_USER;
			Shell_NotifyIconA( NIM_ADD, &g_systray_icon );
			// printf( "Shell_NotifyIconA: hwnd = %x, icon = %d\n", wminfo.window, g_systray_icon.hIcon );
		}
	}

#endif // WIN32

	GameLink::InitTerminal();

	const int memory_map_size = MEMORY_MAP_CORE_SIZE + g_membase_size;
	LOG_MSG( "GAMELINK: Initialised. Allocated %d MB of shared memory.", (memory_map_size + (1024*1024) - 1) / (1024*1024) );

	Bit8u* membase = ((Bit8u*)g_p_shared_memory) + MEMORY_MAP_CORE_SIZE;

	// Return RAM base pointer.
	return membase;
}

//------------------------------------------------------------------------------
// GameLink::Term
//------------------------------------------------------------------------------
void GameLink::Term()
{
#ifdef WIN32
	// Remove tray icon.
	if ( g_systray_icon.hWnd )
		Shell_NotifyIcon( NIM_DELETE, &g_systray_icon );
#endif // WIN32

	// SEND ABORT CODE TO CLIENT (don't care if it fails)
	if ( g_p_shared_memory )
		g_p_shared_memory->version = 0;

	destroy_shared_memory();

	destroy_mutex( GAMELINK_MUTEX_NAME );

	g_membase_size = 0;
}

//------------------------------------------------------------------------------
// GameLink::In
//------------------------------------------------------------------------------
int GameLink::In( GameLink::sSharedMMapInput_R2* p_input,
				  GameLink::sSharedMMapAudio_R1* p_audio )
{
	int ready = 0;

	if ( g_p_shared_memory )
	{
		if ( g_trackonly_mode )
		{
			// No input.
			memset( p_input, 0, sizeof( sSharedMMapInput_R2 ) );
		}
		else
		{
			if ( g_p_shared_memory->input.ready )
			{
				// Copy client input out of shared memory
				memcpy( p_input, &( g_p_shared_memory->input ), sizeof( sSharedMMapInput_R2 ) );

				// Clear remote delta, prevent counting more than once.
				g_p_shared_memory->input.mouse_dx = 0;
				g_p_shared_memory->input.mouse_dy = 0;

				g_p_shared_memory->input.ready = 0;

				ready = 1; // Got some input
			}

			// Volume sync, ignore if out of range.
			if ( g_p_shared_memory->audio.master_vol_l <= 100 )
				p_audio->master_vol_l = g_p_shared_memory->audio.master_vol_l;
			if ( g_p_shared_memory->audio.master_vol_r <= 100 )
				p_audio->master_vol_r = g_p_shared_memory->audio.master_vol_r;
		}
	}

	return ready;
}

//------------------------------------------------------------------------------
// GameLink::Out
//------------------------------------------------------------------------------
void GameLink::Out( const Bit16u frame_width,
					const Bit16u frame_height,
					const double source_ratio,
					const bool want_mouse,
					const char* p_program,
					const Bit32u* p_program_hash,
					const Bit8u* p_frame,
					const Bit8u* p_sysmem )
{
	// Not initialised (or disabled) ?
	if ( g_p_shared_memory == NULL ) {
		return; // <=== EARLY OUT
	}

	// Create integer ratio
	Bit16u par_x, par_y;
	if ( source_ratio >= 1.0 )
	{
		par_x = 4096;
		par_y = static_cast< Bit16u >( source_ratio * 4096.0 );
	}
	else
	{
		par_x = static_cast< Bit16u >( 4096.0 / source_ratio );
		par_y = 4096;
	}

	// Build flags
	Bit8u flags;

	if ( g_trackonly_mode )
	{
		// Tracking Only - DOSBox handles video/input as usual.
		flags = sSharedMemoryMap_R4::FLAG_NO_FRAME;
	}
	else
	{
		// External Input Mode
		flags = sSharedMemoryMap_R4::FLAG_WANT_KEYB;
		if ( want_mouse )
			flags |= sSharedMemoryMap_R4::FLAG_WANT_MOUSE;
	}

	// Paused?
	if ( g_paused )
		flags |= sSharedMemoryMap_R4::FLAG_PAUSED;


	//
	// Send data?

	// Message buffer
	sSharedMMapBuffer_R1 proc_mech_buffer;
	proc_mech_buffer.payload = 0;

#ifdef WIN32

	DWORD mutex_result;
	mutex_result = WaitForSingleObject( g_mutex_handle, INFINITE );
	if ( mutex_result == WAIT_OBJECT_0 )

#else // WIN32

	int mutex_result;
	mutex_result = sem_wait( g_mutex_handle );
	if ( mutex_result < 0 )
	{
		LOG_MSG( "GAMELINK: MUTEX lock failed with %d. errno = %d", mutex_result, errno );
	}
	else

#endif // WIN32

	{
//		printf( "GAMELINK: MUTEX lock ok\n" );

		{ // ========================

			// Set version
			g_p_shared_memory->version = PROTOCOL_VER;

			// Set program
			strncpy( g_p_shared_memory->program, p_program, 256 );
			for ( int i = 0; i < 4; ++i )
			{
				g_p_shared_memory->program_hash[ i ] = p_program_hash[ i ];
			}

			// Store flags
			g_p_shared_memory->flags = flags;

			if ( g_trackonly_mode == false )
			{
				// Update the frame sequence
				++g_p_shared_memory->frame.seq;

				// Copy frame properties
				g_p_shared_memory->frame.image_fmt = 1; // = 32-bit RGBA
				g_p_shared_memory->frame.width = frame_width;
				g_p_shared_memory->frame.height = frame_height;
				g_p_shared_memory->frame.par_x = par_x;
				g_p_shared_memory->frame.par_y = par_y;

				// Frame Buffer
				Bit32u payload;
				payload = frame_width * frame_height * 4;
				if ( frame_width <= sSharedMMapFrame_R1::MAX_WIDTH && frame_height <= sSharedMMapFrame_R1::MAX_HEIGHT )
				{
					memcpy( g_p_shared_memory->frame.buffer, p_frame, payload );
				}
			}

			// Peek
			for ( Bit32u pindex = 0;
					pindex < g_p_shared_memory->peek.addr_count &&
					pindex < sSharedMMapPeek_R2::PEEK_LIMIT;
				++pindex )
			{
				// read address
				Bit32u address;
				address = g_p_shared_memory->peek.addr[ pindex ];

				Bit8u data;
				// valid?
				if ( address < g_membase_size )
				{
					data = p_sysmem[ address ]; // read data
				}
				else
				{
					data = 0; // <-- safe
				}

				// write data
				g_p_shared_memory->peek.data[ pindex ] = data;
			}

			// Message Processing.
			ExecTerminal( &(g_p_shared_memory->buf_recv),
						  &(g_p_shared_memory->buf_tohost),
						  &(proc_mech_buffer) );

		} // ========================

#ifdef WIN32

		ReleaseMutex( g_mutex_handle );

#else // WIN32

		mutex_result = sem_post( g_mutex_handle );
		if ( mutex_result < 0 ) {
			LOG_MSG( "GAMELINK: MUTEX unlock failed with %d. errno = %d", mutex_result, errno );
		} else {
//			printf( "GAMELINK: MUTEX unlock ok.\n" );
		}

#endif // WIN32

		// Mechanical Message Processing, out of mutex.
		if ( proc_mech_buffer.payload )
			ExecTerminalMech( &proc_mech_buffer );
	}

}

//==============================================================================
#endif // C_GAMELINK

