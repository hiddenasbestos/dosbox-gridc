
// Game Link - Console

//------------------------------------------------------------------------------
// Dependencies
//------------------------------------------------------------------------------

#include "config.h"

#if C_GAMELINK

// External Dependencies
#ifdef WIN32
#else // WIN32
#include <string.h>
#include <stdlib.h>
#include <memory.h>
#endif // WIN32

// Local Dependencies
#include "dosbox.h"
#include "gamelink.h"
#include "../resource.h"
#include <stdio.h>
#include "mem.h"

extern Bit32u MemBaseSize;

typedef GameLink::sSharedMMapBuffer_R1	Buffer;

//==============================================================================

//------------------------------------------------------------------------------
// Local Data
//------------------------------------------------------------------------------

static Buffer* g_p_outbuf;

static Bit32u g_range_start;
static Bit32u g_range_end;

static Bit8u* g_ram_copy_private; // Use get_ram_snapshot
static Bit8u* g_ram_ignore_private; // Use get_ram_ignore
static Bit8u g_cmp_value;
static Bit8u g_snap_first;
static Bit8u g_snap_mode;
static Bit32u g_snap_leads;
static Bit32u g_monlist_index;


//------------------------------------------------------------------------------
// Local Functions
//------------------------------------------------------------------------------

//
// get_ram_snapshot
//
// Access the ram copy buffer. Auto-allocates on first use.
//
static Bit8u* get_ram_snapshot()
{
	// Allocate!
	if ( g_ram_copy_private == 0 ) {
		g_ram_copy_private = (Bit8u*)malloc( MemBaseSize );
		memset( g_ram_copy_private, 0, MemBaseSize );
	}		
		
	return g_ram_copy_private;
}

//
// get_ram_ignore
//
// Access the ram ignore buffer. Auto-allocates on first use.
//
static Bit8u* get_ram_ignore()
{
	// Allocate!
	if ( g_ram_ignore_private == 0 ) {
		g_ram_ignore_private = (Bit8u*)malloc( MemBaseSize );
		memset( g_ram_ignore_private, 0, MemBaseSize );

		// Blank outside of search range
		for ( Bit32u i = 0; i < g_range_start; ++i ) {
			g_ram_ignore_private[ i ] = 1;
		}
		for ( Bit32u i = g_range_end; i < MemBaseSize; ++i ) {
			g_ram_ignore_private[ i ] = 1;
		}
	}		
		
	return g_ram_ignore_private;
}

//
// out_char
//
// Copy a string into the output buffer.
//
static void out_char( const char ch )
{
	// Overflow?
	if ( g_p_outbuf->payload >= Buffer::BUFFER_SIZE - 1 ) {
		return;
	}

	// Copy character
	g_p_outbuf->data[ g_p_outbuf->payload++ ] = ch;

	// terminate
	g_p_outbuf->data[ g_p_outbuf->payload ] = 0;
}

//
// out_strcpy
//
// Copy a string into the output buffer.
//
static void out_strcpy( const char* p )
{
	while ( *p )
	{
		// Overflow?
		if ( g_p_outbuf->payload >= Buffer::BUFFER_SIZE - 1 ) {
			break;
		}

		// Copy character
		g_p_outbuf->data[ g_p_outbuf->payload++ ] = *p++;
	}

	// terminate
	g_p_outbuf->data[ g_p_outbuf->payload ] = 0;
}

//
// out_strint
//
// Copy an integer as plain text into the output buffer.
//
static void out_strint( const int v )
{
	char buf[ 32 ];
	sprintf( buf, "%d", v );
	out_strcpy( buf );
}

//
// out_strhex
//
// Copy a hex value as plain text into the output buffer.
// Zero prefixed.
//
static void out_strhex( const int v, const int wide )
{
	char buf[ 32 ], fmt[ 32 ] = "%08X";

	if ( wide >= 1 && wide <= 9 ) {
		fmt[ 2 ] = '0' + wide;
		sprintf( buf, fmt, v );
		out_strcpy( buf );
	}
}

//
// out_strhexspc
//
// Copy a hex value as plain text into the output buffer.
//
static void out_strhexspc( const int v, const int wide )
{
	char buf[ 32 ], fmt[ 32 ] = "%8X";

	if ( wide >= 1 && wide <= 9 ) {
		fmt[ 1 ] = '0' + wide;
		sprintf( buf, fmt, v );
		out_strcpy( buf );
	}
}

//
// out_memcpy
//
// Copy a block of memory into the output buffer.
//
static void out_memcpy( const void* p, const Bit16u len )
{
	const Bit8u* src = (const Bit8u*)p;

	for ( Bit16u i = 0; i < len; ++i )
	{
		// Overflow?
		if ( g_p_outbuf->payload >= Buffer::BUFFER_SIZE - 1 ) {
			break;
		}

		// Copy data
		g_p_outbuf->data[ g_p_outbuf->payload++ ] = *src++;
	}
}

//==============================================================================

//
// proc_mech
//
// Process a mechanical command - encoded form for computer-computer communication. Minimal feedback.
//
static void proc_mech( Buffer* cmd, Bit16u payload )
{
	// Ignore NULL commands.
	if ( payload <= 1 || payload > 128 )
		return;

	cmd->payload = 0;
	char* com = (char*)(cmd->data);
	com[ payload ] = 0;

//	printf( "command = %s; payload = %d\n", com, payload );

	//
	// Decode

	if ( strcmp( com, ":reset" ) == 0 )
	{
//		printf( "do reset\n" );

		extern void Restart(bool pressed);
		Restart( true );
	}
	else if ( strcmp( com, ":pause" ) == 0 )
	{
//		printf( "do pause\n" );

		extern void PauseDOSBox(bool pressed);
		PauseDOSBox( true );
	}
	else if ( strcmp( com, ":shutdown" ) == 0 )
	{
//		printf( "do shutdown\n" );

		extern void KillSwitch(bool pressed);
		KillSwitch( true );
	}
}

//==============================================================================

static void proc_human_findb( const char* arg )
{
	if ( strlen( arg ) == 0 )
	{
		out_strcpy( "findb <value> [<value> ...]\n" );
	}
	else
	{
		// Build needle
		Bit8u needle[ 256 ];
		Bit32u needle_size = 0;

		char* next;
		Bit32u val;
		for ( const char* p = arg; *p ; p = next )
		{
			// skip white space
			while ( *p == ' ' ) {
				++p;
			}

			// actual zero?
			const bool okay = ( *p == '0' );

			// get value (in hex) and advancement pointer
			val = strtoul( p, &next, 16 );
				
			// failed?
			if ( ( val == 0 && !okay ) || ( val >= 256 ) ) {
				break;
			} else {
				needle[ needle_size++ ] = static_cast< Bit8u >( val );
				if ( needle_size > 255 ) {
					break;
				}
			}
		}

		bool found = false;
		int count = 0;

		if ( needle_size > 0 )
		{
			for ( Bit32u i = g_range_start; i <= g_range_end - needle_size; ++i )
			{
				const Bit8u* q = (Bit8u*)( MemBase + i );
				
				// Matches the needle?
				bool match = true;
				for ( Bit32u j = 0; j < needle_size; ++j ) {
					if ( q[ j ] != needle[ j ] ) {
						match = false;
						break;
					}
				}
				
				if ( match )
				{
					if ( found == false )
					{
						/*out_strcpy( "Found" );
					
						for ( Bit32u j = 0; j < needle_size; ++j ) {
							out_strcpy( " 0x" ); out_strhex( needle[ j ], 2 );
						}

						out_strcpy( " ...\n" );*/

						found = true;
					}

					out_strcpy( " found at 0x" ); out_strhex( i, 5 ); out_strcpy( "\n" );

					found = true;

					++count;
					if ( count >= 100 ) {
						out_strcpy( " ... stopping at 100 results.\n" );
						break;
					}
				}
			}
		
		}; // have a needle?

		if ( found == false ) {
			out_strcpy( "Not found\n" );
		}
	}
}

static void proc_human_findw( const char* arg )
{
	if ( strlen( arg ) == 0 )
	{
		out_strcpy( "findw <value>\n" );
	}
	else
	{
		char* end;
		Bit16u val;
		val = static_cast< Bit16u >( strtoul( arg, &end, 16 ) );
			
		bool found = false;
		int count = 0;

		for ( Bit32u i = g_range_start; i <= g_range_end - 2; ++i )
		{
			const Bit16u* q = (Bit16u*)( MemBase + i );
			if ( val == *q )
			{
				if ( found == false )
				{
					out_strcpy( "Found 0x" ); out_strhex( val, 4 );
					found = true;
				}

				out_strcpy( " at 0x" ); out_strhex( i, 5 ); out_strcpy( "\n" );

				++count;
				if ( count >= 100 ) {
					out_strcpy( " ... stopping at 100 results.\n" );
					break;
				}
			}
		}

		if ( found == false ) {
			out_strcpy( "Not found\n" );
		}
	}
}

static void proc_human_findd( const char* arg )
{
	if ( strlen( arg ) == 0 )
	{
		out_strcpy( "findd <value>\n" );
	}
	else
	{
		char* end;
		Bit32u val;
		val = static_cast< Bit32u >( strtoul( arg, &end, 16 ) );
			
		bool found = false;
		int count = 0;

		for ( Bit32u i = g_range_start; i <= g_range_end - 4; ++i )
		{
			const Bit32u* q = (Bit32u*)( MemBase + i );
			if ( val == *q )
			{
				out_strcpy( "Found 0x" ); out_strhex( val, 8 ); out_strcpy( " at 0x" ); out_strhex( i, 5 ); out_strcpy( "\n" );
				found = true;

				++count;
				if ( count >= 100 ) {
					out_strcpy( " ... stopping at 100 results.\n" );
					break;
				}
			}
		}

		if ( found == false ) {
			out_strcpy( "Not found\n" );
		}
	}
}

static void proc_human_finds( const char* arg )
{
	Bit32u len = ( Bit32u )strlen( arg );

	if ( len == 0 )
	{
		out_strcpy( "finds <string>\n" );
	}
	else
	{
		bool found = false;
		int count = 0;

		for ( Bit32u i = g_range_start; i < g_range_end - len; ++i )
		{
			bool match = true;
			for ( Bit32u j = 0; j < len; ++j )
			{
				if ( MemBase[ i + j ] != arg[ j ] ) {
					match = false;
					break;
				}
			}

			if ( match )
			{
				out_strcpy( "Found \"" ); out_strcpy( arg ); out_strcpy( "\" at 0x" ); out_strhex( i, 5 ); out_strcpy( "\n" );
				found = true;

				++count;
				if ( count >= 100 ) {
					out_strcpy( " ... stopping at 100 results.\n" );
					break;
				}
			}
		}

		if ( found == false ) {
			out_strcpy( "Not found\n" );
		}
	}
}

static void proc_human_ramdump( const char* arg )
{
	// skip white space
	while ( *arg == ' ' ) {
		++arg;
	}

	if ( strlen( arg ) == 0 )
	{
		out_strcpy( "ramdump <filename>\n" );
	}
	else
	{
		FILE* fp = fopen( arg, "wb" );
		if ( fp )
		{
			fwrite( MemBase, 1, MemBaseSize, fp );

			fclose( fp );

			const int one_meg = 1024 * 1024;
			int mb = ( ( MemBaseSize + ( one_meg - 1 ) ) / one_meg );

			out_strcpy( "Wrote \"" ); out_strcpy( arg ); out_strcpy( "\" (" ); out_strint( mb ); out_strcpy( "Mb)\n" );
		}
		else
		{
			out_strcpy( "Error: Write failed.\n" );
		}
	}
}

static void proc_human_range( const char* arg )
{
	Bit32u count = 0;
	Bit32u vals[ 3 ];

	char* next;
	Bit32u addr;
	for ( const char* p = arg ; *p && ( count < 3 ) ; p = next )
	{
		// get value (in hex) and advancement pointer
		addr = strtoul( p, &next, 16 );
				
		vals[ count++ ] = addr;
	}

	if ( count != 2 )
	{
		out_strcpy( "range <addr-lo> <addr-hi>\n" );
		out_strcpy( "Current: 0x" ); 
		out_strhex( g_range_start, 5 ); 
		out_strcpy( " - 0x" ); 
		out_strhex( g_range_end, 5 ); 
		out_strcpy( "\n" );
	}
	else
	{
		g_range_start = vals[ 0 ];
		g_range_end = vals[ 1 ];
		if ( g_range_end < g_range_start ) {
			g_range_start = g_range_end;
		}
		if ( g_range_end > MemBaseSize ) {
			g_range_end = MemBaseSize;
		}
		out_strcpy( "Changed find address range to 0x" ); out_strhex( g_range_start, 5 ); 
		out_strcpy( " - 0x" ); out_strhex( g_range_end, 5 ); out_strcpy( "\n" );
	}
}

static void proc_human_rangelo( const char* arg )
{
	if ( strlen( arg ) == 0 )
	{
		out_strcpy( "rangelo <addr>\n" );
	}
	else
	{
		char* end;
		Bit32u val;
		val = static_cast< Bit32u >( strtoul( arg, &end, 16 ) );
		
		out_strcpy( "Changed find start address to 0x" ); out_strhex( val, 5 ); out_strcpy( "\n" );
		g_range_start = val;
	}
}

static void proc_human_rangehi( const char* arg )
{
	if ( strlen( arg ) == 0 )
	{
		out_strcpy( "rangehi <addr>\n" );
	}
	else
	{
		char* end;
		Bit32u val;
		val = static_cast< Bit32u >( strtoul( arg, &end, 16 ) );

		if ( val < g_range_start ) {
			val = g_range_start;
		}
		if ( val > MemBaseSize ) {
			val = MemBaseSize;
		}
		
		out_strcpy( "Changed find end address to 0x" ); out_strhex( val, 5 ); out_strcpy( "\n" );
		g_range_end = val;
	}
}

static void proc_human_mem( const char* arg )
{
	// Parse addresses in argument
	char* next;
	Bit32u addr;
	for ( const char* p = arg; *p; p = next )
	{
		// skip white space
		while ( *p == ' ' ) {
			++p;
		}

		// actual zero?
		const bool okay = ( *p == '0' );

		// get value (in hex) and advancement pointer
		addr = strtoul( p, &next, 16 );
				
		// failed?
		if ( ( addr == 0 && !okay ) || ( addr >= MemBaseSize ) )
		{
			break;
		}
		else
		{
			Bit32u true_addr = addr;
			bool restore_video = false;

			// 16 byte align
			addr &= 0xFFFFFFF0;

			{
				//
				// - 16 byte block

				out_strcpy( " " ); out_strhexspc( addr, 8 ); out_strcpy( " |" );

				for ( int i = 0; i < 16; ++i ) {
					if ( restore_video ) {
						out_strcpy( "\033[0m " ); // NORMAL
						restore_video = false;
					}
					else if ( ( addr + i ) == true_addr ) {
						out_strcpy( " \033[7m" ); // REVERSE
						restore_video = true;

					}
					else {
						out_strcpy( " " );
					}
					out_strhex( MemBase[ addr + i ], 2 );
				}

				if ( restore_video ) {
					out_strcpy( "\27q" );
				}

				out_strcpy( " " );

				for ( int i = 0; i < 16; ++i ) {
					Bit8u ch = MemBase[ addr + i ];
					if ( ( addr + i ) == true_addr ) {
						out_strcpy( "\033[7m" ); // REVERSE
					}
					if ( ch >= 32 && ch < 127 ) {
						out_char( ch );
					}
					else {
						out_char( '.' );
					}
					if ( ( addr + i ) == true_addr ) {
						out_strcpy( "\033[0m" ); // NORMAL
					}
				}

				out_strcpy( "\n" );
			}
		}
	}
}

static void proc_human_page( char* arg )
{
	// Parse addresses in argument
	char* next;
	Bit32u addr;
	for ( const char* p = arg; *p; p = next )
	{
		// skip white space
		while ( *p == ' ' ) {
			++p;
		}

		// actual zero?
		const bool okay = ( *p == '0' );

		// get value (in hex) and advancement pointer
		addr = strtoul( p, &next, 16 );
				
		// failed?
		if ( ( addr == 0 && !okay ) || ( addr >= MemBaseSize ) )
		{
			break;
		}
		else
		{
			Bit32u true_addr = addr;
			bool restore_video = false;

			// 16 byte align
			addr &= 0xFFFFFFF0;

			for ( Bit32u r = 0; r < 16; ++r )
			{
				{
					//
					// - 16 byte block

					out_strcpy( " " ); out_strhexspc( addr, 8 ); out_strcpy( " |" );

					for ( int i = 0; i < 16; ++i ) {
						if ( restore_video ) {
							out_strcpy( "\033[0m " ); // NORMAL
							restore_video = false;
						}
						else if ( ( addr + i ) == true_addr ) {
							out_strcpy( " \033[7m" ); // REVERSE
							restore_video = true;

						}
						else {
							out_strcpy( " " );
						}
						out_strhex( MemBase[ addr + i ], 2 );
					}

					if ( restore_video ) {
						out_strcpy( "\27q" );
					}

					out_strcpy( " " );

					for ( int i = 0; i < 16; ++i ) {
						Bit8u ch = MemBase[ addr + i ];
						if ( ( addr + i ) == true_addr ) {
							out_strcpy( "\033[7m" ); // REVERSE
						}
						if ( ch >= 32 && ch < 127 ) {
							out_char( ch );
						}
						else {
							out_char( '.' );
						}
						if ( ( addr + i ) == true_addr ) {
							out_strcpy( "\033[0m" ); // NORMAL
						}
					}

					out_strcpy( "\n" );
				}

				addr += 16;
			}
		}
	}
}

static void proc_human_list() 
{
	Bit8u* copy = get_ram_snapshot();
	Bit8u* ignore = get_ram_ignore();

	if ( g_snap_leads == 0 )
	{
		out_strcpy( "No results. Use snap command.\n" );
	}
	else
	{
		Bit32u count = 0;

		for ( Bit32u addr = 0; addr < MemBaseSize; ++addr )
		{
			if ( ignore[ addr ] ) {
				continue;
			}

			char buffer[ 256 ];
			out_strcpy( " " ); out_strhexspc( addr, 8 ); out_strcpy( " | " );
			sprintf( buffer, "mem = %3d (0x%02X), snap = %3d (0x%02X) \n", MemBase[ addr ], MemBase[ addr ], copy[ addr ], copy[ addr ] );
			out_strcpy( buffer );

			++count;
			if ( count == 100 ) {
				out_strcpy( "  ... stopping at 100 results.\n" );
				return;
			}
		}

		out_strcpy( "  ... " );
		out_strint( g_snap_leads );
		out_strcpy( " results.\n" );
	}
}

static void proc_human_meminfo()
{
	out_strcpy( "&MemBase = 0x" ); out_strhex( ( Bit32u )( size_t )MemBase, 8 );
	out_strcpy( "; MemBaseSize = " ); out_strint( MemBaseSize );
	out_strcpy( " (" ); out_strint( ( MemBaseSize + 1048575 ) / 1048576 ); out_strcpy( "MB)\n" );
}

static void proc_human_cls()
{
	out_strcpy( "\033[2J" );
}

static void proc_human_peek( char* arg )
{
	Bit8u* copy = get_ram_snapshot();

	char* next;
	Bit32u addr;
	for ( const char* p = arg ; *p ; p = next )
	{
		// skip white space
		while ( *p == ' ' ) {
			++p;
		}

		// actual zero?
		const bool okay = ( *p == '0' );

		// get value (in hex) and advancement pointer
		addr = strtoul( p, &next, 16 );
				
		// failed?
		if ( ( addr == 0 && !okay ) || ( addr >= MemBaseSize ) ) {
			break;
		} else {
			char buffer[ 256 ];
			out_strcpy( " " ); out_strhexspc( addr, 8 ); out_strcpy( " | " );
			sprintf( buffer, "mem = %3d (0x%02X), snap = %3d (0x%02X)\n", MemBase[ addr ], MemBase[ addr ], copy[ addr ], copy[ addr ] );
			out_strcpy( buffer );
		}
	}
}

static void proc_human_peekw( char* arg )
{
	Bit8u* copy = get_ram_snapshot();

	char* next;
	Bit32u addr;
	for ( const char* p = arg ; *p ; p = next )
	{
		// skip white space
		while ( *p == ' ' ) {
			++p;
		}

		// actual zero?
		const bool okay = ( *p == '0' );

		// get value (in hex) and advancement pointer
		addr = strtoul( p, &next, 16 );
				
		// failed?
		if ( ( addr == 0 && !okay ) || ( addr >= MemBaseSize ) ) {
			break;
		} else {
			char buffer[ 256 ];
			const Bit16u oldpeekw = *(Bit16u*)( copy + addr );
			const Bit16u peekw = *(Bit16u*)( MemBase + addr );
			sprintf( buffer, " addr:%05x | mem = %5d (0x%04x), snap = %5d (0x%04x) \n", addr,
				peekw, peekw, oldpeekw, oldpeekw );
			out_strcpy( buffer );
		}
	}
}

static void proc_human_reset()
{
	Bit8u* copy = get_ram_snapshot();
	memset( copy, 0, MemBaseSize );
	Bit8u* ignore = get_ram_ignore();
	memset( ignore, 0, MemBaseSize );

	// Blank outside of search range
	for ( Bit32u i = 0; i < g_range_start; ++i ) {
		ignore[ i ] = 1;
	}
	for ( Bit32u i = g_range_end; i < MemBaseSize; ++i ) {
		ignore[ i ] = 1;
	}

	g_snap_first = 0;
	g_snap_mode = 0;

 	g_snap_leads = MemBaseSize;
 	g_monlist_index = 0;
	
	out_strcpy( "Reset snapshot. Using range: 0x" ); out_strhex( g_range_start, 5 ); out_strcpy( " - 0x" ); out_strhex( g_range_end - 1, 5 ); out_strcpy( "\n" );
}

static void proc_human_setd( char* arg )
{
	g_cmp_value = atoi( arg );
	out_strcpy( "Snap compare register = 0x" );
	out_strhex( g_cmp_value, 2 );
	out_strcpy( "\n" );
}

static void proc_human_setx( char* arg )
{
	char* end;
	g_cmp_value = static_cast< Bit8u >( strtoul( arg, &end, 16 ) );
	out_strcpy( "Snap compare register = 0x" );
	out_strhex( g_cmp_value, 2 );
	out_strcpy( "\n" );
}

static void proc_human_snap_mode( char* arg )
{
	int cmd = atoi( arg );
	Bit8u* copy = get_ram_snapshot();
	Bit8u* ignore = get_ram_ignore();

	g_snap_leads = 0;

	switch ( cmd )
	{

	case 0:
	default:
		{
			// recount leads.
			for ( Bit32u i = 0; i < MemBaseSize; ++i )
			{
				// Ignore?
				if ( ignore[ i ] ) {
					continue;
				}

				++g_snap_leads;
			}
		}
		break;

	case 1:
		{
			for ( Bit32u i = 0; i < MemBaseSize; ++i )
			{
				// Ignore?
				if ( ignore[ i ] ) {
					continue;
				}

				if ( MemBase[ i ] == copy[ i ] )
				{
					++g_snap_leads;
				}
				else
				{
					ignore[ i ] = true;
				}
			}
		}
		break;

	case 2:
		{
			for ( Bit32u i = 0; i < MemBaseSize; ++i )
			{
				// Ignore?
				if ( ignore[ i ] ) {
					continue;
				}

				if ( MemBase[ i ] != copy[ i ] )
				{
					++g_snap_leads;
				}
				else
				{
					ignore[ i ] = true;
				}
			}
		}
		break;

	case 3:
		{
			for ( Bit32u i = 0; i < MemBaseSize; ++i )
			{
				// Ignore?
				if ( ignore[ i ] ) {
					continue;
				}

				if ( MemBase[ i ] < copy[ i ] )
				{
					++g_snap_leads;
				}
				else
				{
					ignore[ i ] = true;
				}
			}
		}
		break;

	case 4:
		{
			for ( Bit32u i = 0; i < MemBaseSize; ++i )
			{
				// Ignore?
				if ( ignore[ i ] ) {
					continue;
				}

				if ( MemBase[ i ] > copy[ i ] )
				{
					++g_snap_leads;
				}
				else
				{
					ignore[ i ] = true;
				}
			}
		}
		break;

	case 5:
		{
			for ( Bit32u i = 0; i < MemBaseSize; ++i )
			{
				// Ignore?
				if ( ignore[ i ] ) {
					continue;
				}

				if ( MemBase[ i ] == g_cmp_value )
				{
					++g_snap_leads;
				}
				else
				{
					ignore[ i ] = true;
				}
			}
		}
		break;

	case 6:
		{
			for ( Bit32u i = 0; i < MemBaseSize; ++i )
			{
				// Ignore?
				if ( ignore[ i ] ) {
					continue;
				}

				if ( MemBase[ i ] != g_cmp_value )
				{
					++g_snap_leads;
				}
				else
				{
					ignore[ i ] = true;
				}
			}
		}
		break;

	}; // switch ( cmd )

	out_strcpy( "  ... " );
	out_strint( g_snap_leads );
	out_strcpy( " results.\n" );

	// Copy DOS memory into snap buffer
	memcpy( copy, MemBase, MemBaseSize );

	g_snap_mode = 0;
}

static void proc_human_snap()
{
	Bit8u* copy = get_ram_snapshot();

	if ( g_snap_first )
	{
		out_strcpy( "Initial memory snapshot taken. Snap again to compare.\n" );
		g_snap_first = 0;

		// Copy DOS memory into snap buffer
		memcpy( copy, MemBase, MemBaseSize );
	}
	else
	{
		// Compare
		char buffer[ 256 ];
		sprintf( buffer, "\nSelect Filter: 1.EQ 2.NE 3.LT 4.GT 5.EQ:%02x 6.NE:%02x (0.CANCEL) ?",
			g_cmp_value, g_cmp_value );
		out_strcpy( buffer );

		g_snap_mode = 1;
	}
}

//==============================================================================

//
// proc_human
//
// Process a human command - slightly more verbose, string based. Printable feedback for interactive terminal use.
//
static void proc_human( char* cmd )
{
	if ( g_snap_mode == 1 ) 
	{
		proc_human_snap_mode( cmd );
		return;
	}

	char verb[ 2048 ];

	// Get verb
	char* p = cmd;

	// skip whitespace
	while ( *p && *p == ' ' ) {
		++p;
	}

	char* arg = p;

	// find next null or whitespace
	while ( !( *arg == 0 || *arg == ' ' ) ) {
		++arg;
	}
	// ... copy arg to verb buffer.
	memcpy( verb, p, arg - p );
	verb[ arg - p ] = 0;
	
	// ... prepare arg pointer (if valid)
	while ( *arg == ' ' ) {
		++arg;
	}

	//
	// Decode

	if ( ( *verb == 0 ) )
	{
		//
	}
	else if ( strcmp( verb, "help" ) == 0 || strcmp( verb, "?" ) == 0 )
	{
		out_strcpy( "\n" );
		out_strcpy( "  help     ... This help.\n" );
		out_strcpy( "  cls      ... Clear screen.\n" );
 		out_strcpy( "  finds    ... Find a given string in the set range.\n" );
 		out_strcpy( "  findb    ... Find a pattern of hexadecimal byte value(s).\n" );
 		out_strcpy( "  findw    ... Find the given 16-bit hexadecimal value.\n" );
 		out_strcpy( "  findd    ... Find the given 32-bit hexadecimal value.\n" );
  		out_strcpy( "  list     ... List relevant snapshot values.\n" );
		out_strcpy( "  mem      ... Print 16 bytes around the given addr(s).\n" );
		out_strcpy( "  meminfo  ... Print base address and size of emulated RAM.\n" );
 		out_strcpy( "  page     ... Print 256 bytes around the given addr(s).\n" );
  		out_strcpy( "  peek     ... Peek byte(s) from specific addresses (space delimited).\n" );
  		out_strcpy( "  peekw    ... Peek 16-bit words(s) from specific addresses (space delimited).\n" );
		out_strcpy( "  ramdump  ... Write the current contents of all RAM to a file.\n" );
		out_strcpy( "  range    ... Set address range for searches [0x" ); out_strhex( g_range_start, 5 ); out_strcpy( " - 0x" ); out_strhex( g_range_end, 5 ); out_strcpy( "]\n" );
		out_strcpy( "  rangelo  ... Set search start address.\n" );
		out_strcpy( "  rangehi  ... Set search end address.\n" );
		out_strcpy( "  reset    ... Reset snapshot. Start over and update for new range.\n" );
		out_strcpy( "  setd     ... Set the snap compare register (as decimal byte).\n" );
		out_strcpy( "  setx     ... Set the snap compare register (as hex byte).\n" );
		out_strcpy( "  snap     ... Memory snapshot and compare functions.\n" );
	}
	else if ( strcmp( verb, "cls" ) == 0 )
	{
		proc_human_cls();
	}
	else if ( strcmp( verb, "findb" ) == 0 )
	{
		proc_human_findb( arg );
	}
	else if ( strcmp( verb, "findw" ) == 0 )
	{
		proc_human_findw( arg );
	}
	else if ( strcmp( verb, "findd" ) == 0 )
	{
		proc_human_findd( arg );
	}
	else if ( strcmp( verb, "finds" ) == 0 )
	{
		proc_human_finds( arg );
	}
	else if ( strcmp( verb, "list" ) == 0 )
	{
		proc_human_list();
	}
	else if ( strcmp( verb, "meminfo" ) == 0 )
	{
		proc_human_meminfo();
	}
	else if ( strcmp( verb, "mem" ) == 0 )
	{
		proc_human_mem( arg );
	}
	else if ( strcmp( verb, "page" ) == 0 )
	{
		proc_human_page( arg );
	}
	else if ( strcmp( verb, "peek" ) == 0 )
	{
		proc_human_peek( arg );
	}
	else if ( strcmp( verb, "ramdump" ) == 0 )
	{
		proc_human_ramdump( arg );
	}
	else if ( strcmp( verb, "range" ) == 0 )
	{
		proc_human_range( arg );
	}
	else if ( strcmp( verb, "rangelo" ) == 0 )
	{
		proc_human_rangelo( arg );
	}
	else if ( strcmp( verb, "rangehi" ) == 0 )
	{
		proc_human_rangehi( arg );
	}
	else if ( strcmp( verb, "reset" ) == 0 )
	{
		proc_human_reset();
	}
	else if ( strcmp( verb, "setd" ) == 0 )
	{
		proc_human_setd( arg );
	}
	else if ( strcmp( verb, "setx" ) == 0 )
	{
		proc_human_setx( arg );
	}
	else if ( strcmp( verb, "snap" ) == 0 )
	{
		proc_human_snap();
	}
	else
	{
		out_strcpy( "Bad command\n" );
	}
}

//------------------------------------------------------------------------------
// Global Functions
//------------------------------------------------------------------------------

void GameLink::InitTerminal()
{
	g_p_outbuf = 0;

	g_range_start = 0;
	g_range_end = 640 * 1024;

	g_ram_copy_private = 0;
	g_ram_ignore_private = 0;
	g_cmp_value = 0;
	g_snap_first = 1;
	g_snap_leads = MemBaseSize;
}

void GameLink::TermTerminal()
{
	if ( g_ram_copy_private )
	{
		free( g_ram_copy_private );
		g_ram_copy_private = 0;
	}
	if ( g_ram_ignore_private )
	{
		free( g_ram_ignore_private );
		g_ram_ignore_private = 0;
	}
}

void GameLink::ExecTerminalMech( Buffer* p_procbuf )
{
	proc_mech( p_procbuf, p_procbuf->payload );
}

void GameLink::ExecTerminal( Buffer* p_inbuf, 
							 Buffer* p_outbuf,
							 Buffer* p_procbuf )
{
	// Nothing from the host, or host hasn't acknowledged our last message.
	if ( p_inbuf->payload == 0 ) {
		return;
	}
	if ( p_outbuf->payload > 0 ) {
		return;
	}

	// Store output pointer
	g_p_outbuf = p_outbuf;

	// Process mode select ...
	if ( p_inbuf->data[ 0 ] == ':' )
	{
		// Acknowledge now, to avoid loops.
		Bit16u payload = p_inbuf->payload;
		p_inbuf->payload = 0;

		// Copy out.
		memcpy( p_procbuf->data, p_inbuf->data, payload );
		p_procbuf->payload = payload;
	}
	else
	{
		// Human command
		char buf[ Buffer::BUFFER_SIZE + 1 ], *b = buf;

		// Convert into printable ASCII
		for ( Bit32u i = 0; i < p_inbuf->payload; ++i )
		{
			Bit8u u8 = p_inbuf->data[ i ];
			if ( u8 < 32 || u8 > 127 ) {
				*b++ = '?';
			} else {
				*b++ = (char)u8;
			}
		}

		// ... terminate
		*b++ = 0;

		// Acknowledge
		p_inbuf->payload = 0;

		proc_human( buf );
	}
}

//==============================================================================
#endif // C_GAMELINK

