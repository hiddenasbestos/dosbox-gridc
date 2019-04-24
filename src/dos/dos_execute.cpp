/*
 *  Copyright (C) 2002-2019  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include <string.h>
#include <ctype.h>
#include "dosbox.h"
#include "mem.h"
#include "dos_inc.h"
#include "regs.h"
#include "callback.h"
#include "debug.h"
#include "cpu.h"

const char * RunningProgram="DOSBOX";
Bit32u RunningProgramHash[4]={0,0,0,0}; // DWD

#ifdef _MSC_VER
#pragma pack(1)
#endif
struct EXE_Header {
	Bit16u signature;					/* EXE Signature MZ or ZM */
	Bit16u extrabytes;					/* Bytes on the last page */
	Bit16u pages;						/* Pages in file */
	Bit16u relocations;					/* Relocations in file */
	Bit16u headersize;					/* Paragraphs in header */
	Bit16u minmemory;					/* Minimum amount of memory */
	Bit16u maxmemory;					/* Maximum amount of memory */
	Bit16u initSS;
	Bit16u initSP;
	Bit16u checksum;
	Bit16u initIP;
	Bit16u initCS;
	Bit16u reloctable;
	Bit16u overlay;
} GCC_ATTRIBUTE(packed);
#ifdef _MSC_VER
#pragma pack()
#endif

#define MAGIC1 0x5a4d
#define MAGIC2 0x4d5a
#define MAXENV 32768u
#define ENV_KEEPFREE 83				 /* keep unallocated by environment variables */
#define LOADNGO 0
#define LOAD    1
#define OVERLAY 3



static void SaveRegisters(void) {
	reg_sp-=18;
	mem_writew(SegPhys(ss)+reg_sp+ 0,reg_ax);
	mem_writew(SegPhys(ss)+reg_sp+ 2,reg_cx);
	mem_writew(SegPhys(ss)+reg_sp+ 4,reg_dx);
	mem_writew(SegPhys(ss)+reg_sp+ 6,reg_bx);
	mem_writew(SegPhys(ss)+reg_sp+ 8,reg_si);
	mem_writew(SegPhys(ss)+reg_sp+10,reg_di);
	mem_writew(SegPhys(ss)+reg_sp+12,reg_bp);
	mem_writew(SegPhys(ss)+reg_sp+14,SegValue(ds));
	mem_writew(SegPhys(ss)+reg_sp+16,SegValue(es));
}

static void RestoreRegisters(void) {
	reg_ax=mem_readw(SegPhys(ss)+reg_sp+ 0);
	reg_cx=mem_readw(SegPhys(ss)+reg_sp+ 2);
	reg_dx=mem_readw(SegPhys(ss)+reg_sp+ 4);
	reg_bx=mem_readw(SegPhys(ss)+reg_sp+ 6);
	reg_si=mem_readw(SegPhys(ss)+reg_sp+ 8);
	reg_di=mem_readw(SegPhys(ss)+reg_sp+10);
	reg_bp=mem_readw(SegPhys(ss)+reg_sp+12);
	SegSet16(ds,mem_readw(SegPhys(ss)+reg_sp+14));
	SegSet16(es,mem_readw(SegPhys(ss)+reg_sp+16));
	reg_sp+=18;
}

extern void GFX_SetTitle(Bit32s cycles,Bits frameskip,bool paused);
void DOS_UpdatePSPName(void) {
	DOS_MCB mcb(dos.psp()-1);
	static char name[9];
	mcb.GetFileName(name);
	name[8] = 0;
	if (!strlen(name)) strcpy(name,"DOSBOX");
	for(Bitu i = 0;i < 8;i++) { //Don't put garbage in the title bar. Mac OS X doesn't like it
		if (name[i] == 0) break;
		if ( !isprint(*reinterpret_cast<unsigned char*>(&name[i])) ) name[i] = '?';
	}
	RunningProgram = name;
	GFX_SetTitle(-1,-1,false);
}

void DOS_Terminate(Bit16u pspseg,bool tsr,Bit8u exitcode) {

	dos.return_code=exitcode;
	dos.return_mode=(tsr)?(Bit8u)RETURN_TSR:(Bit8u)RETURN_EXIT;
	
	DOS_PSP curpsp(pspseg);
	if (pspseg==curpsp.GetParent()) return;
	/* Free Files owned by process */
	if (!tsr) curpsp.CloseFiles();
	
	/* Get the termination address */
	RealPt old22 = curpsp.GetInt22();
	/* Restore vector 22,23,24 */
	curpsp.RestoreVectors();
	/* Set the parent PSP */
	dos.psp(curpsp.GetParent());
	DOS_PSP parentpsp(curpsp.GetParent());

	/* Restore the SS:SP to the previous one */
	SegSet16(ss,RealSeg(parentpsp.GetStack()));
	reg_sp = RealOff(parentpsp.GetStack());		
	/* Restore the old CS:IP from int 22h */
	RestoreRegisters();
	/* Set the CS:IP stored in int 0x22 back on the stack */
	mem_writew(SegPhys(ss)+reg_sp+0,RealOff(old22));
	mem_writew(SegPhys(ss)+reg_sp+2,RealSeg(old22));
	/* set IOPL=3 (Strike Commander), nested task set,
	   interrupts enabled, test flags cleared */
	mem_writew(SegPhys(ss)+reg_sp+4,0x7202);
	// Free memory owned by process
	if (!tsr) DOS_FreeProcessMemory(pspseg);
	DOS_UpdatePSPName();

	if ((!(CPU_AutoDetermineMode>>CPU_AUTODETERMINE_SHIFT)) || (cpu.pmode)) return;

	CPU_AutoDetermineMode>>=CPU_AUTODETERMINE_SHIFT;
	if (CPU_AutoDetermineMode&CPU_AUTODETERMINE_CYCLES) {
		CPU_CycleAutoAdjust=false;
		CPU_CycleLeft=0;
		CPU_Cycles=0;
		CPU_CycleMax=CPU_OldCycleMax;
		GFX_SetTitle(CPU_OldCycleMax,-1,false);
	} else {
		GFX_SetTitle(-1,-1,false);
	}
#if (C_DYNAMIC_X86) || (C_DYNREC)
	if (CPU_AutoDetermineMode&CPU_AUTODETERMINE_CORE) {
		cpudecoder=&CPU_Core_Normal_Run;
		CPU_CycleLeft=0;
		CPU_Cycles=0;
	}
#endif

	return;
}

static bool MakeEnv(char * name,Bit16u * segment) {
	/* If segment to copy environment is 0 copy the caller's environment */
	DOS_PSP psp(dos.psp());
	PhysPt envread,envwrite;
	Bit16u envsize=1;
	bool parentenv=true;

	if (*segment==0) {
		if (!psp.GetEnvironment()) parentenv=false;				//environment seg=0
		envread=PhysMake(psp.GetEnvironment(),0);
	} else {
		if (!*segment) parentenv=false;						//environment seg=0
		envread=PhysMake(*segment,0);
	}

	if (parentenv) {
		for (envsize=0; ;envsize++) {
			if (envsize>=MAXENV - ENV_KEEPFREE) {
				DOS_SetError(DOSERR_ENVIRONMENT_INVALID);
				return false;
			}
			if (mem_readw(envread+envsize)==0) break;
		}
		envsize += 2;									/* account for trailing \0\0 */
	}
	Bit16u size=long2para(envsize+ENV_KEEPFREE);
	if (!DOS_AllocateMemory(segment,&size)) return false;
	envwrite=PhysMake(*segment,0);
	if (parentenv) {
		MEM_BlockCopy(envwrite,envread,envsize);
//		mem_memcpy(envwrite,envread,envsize);
		envwrite+=envsize;
	} else {
		mem_writeb(envwrite++,0);
	}
	mem_writew(envwrite,1);
	envwrite+=2;
	char namebuf[DOS_PATHLENGTH];
	if (DOS_Canonicalize(name,namebuf)) {
		MEM_BlockWrite(envwrite,namebuf,(Bitu)(strlen(namebuf)+1));
		return true;
	} else return false;
}

bool DOS_NewPSP(Bit16u segment, Bit16u size) {
	DOS_PSP psp(segment);
	psp.MakeNew(size);
	Bit16u parent_psp_seg=psp.GetParent();
	DOS_PSP psp_parent(parent_psp_seg);
	psp.CopyFileTable(&psp_parent,false);
	// copy command line as well (Kings Quest AGI -cga switch)
	psp.SetCommandTail(RealMake(parent_psp_seg,0x80));
	return true;
}

bool DOS_ChildPSP(Bit16u segment, Bit16u size) {
	DOS_PSP psp(segment);
	psp.MakeNew(size);
	Bit16u parent_psp_seg = psp.GetParent();
	DOS_PSP psp_parent(parent_psp_seg);
	psp.CopyFileTable(&psp_parent,true);
	psp.SetCommandTail(RealMake(parent_psp_seg,0x80));
	psp.SetFCB1(RealMake(parent_psp_seg,0x5c));
	psp.SetFCB2(RealMake(parent_psp_seg,0x6c));
	psp.SetEnvironment(psp_parent.GetEnvironment());
	psp.SetSize(size);
	// push registers in case child PSP is terminated
	SaveRegisters();
	psp.SetStack(RealMakeSeg(ss,reg_sp));
	reg_sp+=18;
	return true;
}

static void SetupPSP(Bit16u pspseg,Bit16u memsize,Bit16u envseg) {
	/* Fix the PSP for psp and environment MCB's */
	DOS_MCB mcb((Bit16u)(pspseg-1));
	mcb.SetPSPSeg(pspseg);
	mcb.SetPt((Bit16u)(envseg-1));
	mcb.SetPSPSeg(pspseg);

	DOS_PSP psp(pspseg);
	psp.MakeNew(memsize);
	psp.SetEnvironment(envseg);

	/* Copy file handles */
	DOS_PSP oldpsp(dos.psp());
	psp.CopyFileTable(&oldpsp,true);

}

static void SetupCMDLine(Bit16u pspseg,DOS_ParamBlock & block) {
	DOS_PSP psp(pspseg);
	// if cmdtail==0 it will inited as empty in SetCommandTail
	psp.SetCommandTail(block.exec.cmdtail);
}

// DWD BEGIN
/*-
 *  COPYRIGHT (C) 1986 Gary S. Brown.  You may use this program, or
 *  code or tables extracted from it, as desired without restriction.
 */
 
static Bit32u crc32_tab[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static Bit32u crc32( Bit32u crc, Bit8u* buf, Bitu len )
{
	const Bit8u *p;

	p = buf;
	crc = crc ^ ~0U;

	while (len--)
		crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);

	return crc ^ ~0U;
}
// DWD END

bool DOS_Execute(char * name,PhysPt block_pt,Bit8u flags) {
	EXE_Header head;Bitu i;
	Bit16u fhandle;Bit16u len;Bit32u pos;
	Bit16u pspseg,envseg,loadseg,memsize,readsize;
	PhysPt loadaddress;RealPt relocpt;
	Bitu headersize=0,imagesize=0;
	DOS_ParamBlock block(block_pt);
	Bit32u checksum = 0; // DWD
	Bit32u checksum_bytes = 0; // DWD

	block.LoadData();
	//Remove the loadhigh flag for the moment!
	if(flags&0x80) LOG(LOG_EXEC,LOG_ERROR)("using loadhigh flag!!!!!. dropping it");
	flags &= 0x7f;
	if (flags!=LOADNGO && flags!=OVERLAY && flags!=LOAD) {
		DOS_SetError(DOSERR_FORMAT_INVALID);
		return false;
//		E_Exit("DOS:Not supported execute mode %d for file %s",flags,name);
	}
	/* Check for EXE or COM File */
	bool iscom=false;
	if (!DOS_OpenFile(name,OPEN_READ,&fhandle)) {
		DOS_SetError(DOSERR_FILE_NOT_FOUND);
		return false;
	}
	len=sizeof(EXE_Header);
	if (!DOS_ReadFile(fhandle,(Bit8u *)&head,&len)) {
		DOS_CloseFile(fhandle);
		return false;
	}
	if (len<sizeof(EXE_Header)) {
		if (len==0) {
			/* Prevent executing zero byte files */
			DOS_SetError(DOSERR_ACCESS_DENIED);
			DOS_CloseFile(fhandle);
			return false;
		}
		/* Otherwise must be a .com file */
		iscom=true;
	} else {
		/* Convert the header to correct endian, i hope this works */
		HostPt endian=(HostPt)&head;
		for (i=0;i<sizeof(EXE_Header)/2;i++) {
			*((Bit16u *)endian)=host_readw(endian);
			endian+=2;
		}
		if ((head.signature!=MAGIC1) && (head.signature!=MAGIC2)) iscom=true;
		else {
			if(head.pages & ~0x07ff) /* 1 MB dos maximum address limit. Fixes TC3 IDE (kippesoep) */
				LOG(LOG_EXEC,LOG_NORMAL)("Weird header: head.pages > 1 MB");
			head.pages&=0x07ff;
			headersize = head.headersize*16;
			imagesize = head.pages*512-headersize; 
			if (imagesize+headersize<512) imagesize = 512-headersize;
		}
	}
	Bit8u * loadbuf=(Bit8u *)new Bit8u[0x10000];
	if (flags!=OVERLAY) {
		/* Create an environment block */
		envseg=block.exec.envseg;
		if (!MakeEnv(name,&envseg)) {
			DOS_CloseFile(fhandle);
			return false;
		}
		/* Get Memory */		
		Bit16u minsize,maxsize;Bit16u maxfree=0xffff;DOS_AllocateMemory(&pspseg,&maxfree);
		if (iscom) {
			minsize=0x1000;maxsize=0xffff;
			if (machine==MCH_PCJR) {
				/* try to load file into memory below 96k */ 
				pos=0;DOS_SeekFile(fhandle,&pos,DOS_SEEK_SET);	
				Bit16u dataread=0x1800;
				DOS_ReadFile(fhandle,loadbuf,&dataread);
				if (dataread<0x1800) maxsize=dataread;
				if (minsize>maxsize) minsize=maxsize;
			}
		} else {	/* Exe size calculated from header */
			minsize=long2para(imagesize+(head.minmemory<<4)+256);
			if (head.maxmemory!=0) maxsize=long2para(imagesize+(head.maxmemory<<4)+256);
			else maxsize=0xffff;
		}
		if (maxfree<minsize) {
			if (iscom) {
				/* Reduce minimum of needed memory size to filesize */
				pos=0;DOS_SeekFile(fhandle,&pos,DOS_SEEK_SET);	
				Bit16u dataread=0xf800;
				DOS_ReadFile(fhandle,loadbuf,&dataread);
				if (dataread<0xf800) minsize=((dataread+0x10)>>4)+0x20;
			}
			if (maxfree<minsize) {
				DOS_CloseFile(fhandle);
				DOS_SetError(DOSERR_INSUFFICIENT_MEMORY);
				DOS_FreeMemory(envseg);
				return false;
			}
		}
		if (maxfree<maxsize) memsize=maxfree;
		else memsize=maxsize;
		if (!DOS_AllocateMemory(&pspseg,&memsize)) E_Exit("DOS:Exec error in memory");
		if (iscom && (machine==MCH_PCJR) && (pspseg<0x2000)) {
			maxsize=0xffff;
			/* resize to full extent of memory block */
			DOS_ResizeMemory(pspseg,&maxsize);
		}
		loadseg=pspseg+16;
		if (!iscom) {
			/* Check if requested to load program into upper part of allocated memory */
			if ((head.minmemory == 0) && (head.maxmemory == 0))
				loadseg = (Bit16u)(((pspseg+memsize)*0x10-imagesize)/0x10);
		}
	} else loadseg=block.overlay.loadseg;
	/* Load the executable */
	loadaddress=PhysMake(loadseg,0);

	if (iscom) {	/* COM Load 64k - 256 bytes max */
		pos=0;DOS_SeekFile(fhandle,&pos,DOS_SEEK_SET);	
		readsize=0xffff-256;
		DOS_ReadFile(fhandle,loadbuf,&readsize);
		checksum = crc32( checksum, loadbuf, readsize ); // DWD
		checksum_bytes += readsize; // DWD
		MEM_BlockWrite(loadaddress,loadbuf,readsize);
	} else {	/* EXE Load in 32kb blocks and then relocate */
		pos=headersize;DOS_SeekFile(fhandle,&pos,DOS_SEEK_SET);	
		while (imagesize>0x7FFF) {
			readsize=0x8000;DOS_ReadFile(fhandle,loadbuf,&readsize);
			checksum = crc32( checksum, loadbuf, readsize ); // DWD
			checksum_bytes += readsize; // DWD
			MEM_BlockWrite(loadaddress,loadbuf,readsize);
//			if (readsize!=0x8000) LOG(LOG_EXEC,LOG_NORMAL)("Illegal header");
			loadaddress+=0x8000;imagesize-=0x8000;
		}
		if (imagesize>0) {
			readsize=(Bit16u)imagesize;DOS_ReadFile(fhandle,loadbuf,&readsize);
			checksum = crc32( checksum, loadbuf, readsize ); // DWD
			checksum_bytes += readsize; // DWD
			MEM_BlockWrite(loadaddress,loadbuf,readsize);
//			if (readsize!=imagesize) LOG(LOG_EXEC,LOG_NORMAL)("Illegal header");
		}
		/* Relocate the exe image */
		Bit16u relocate;
		if (flags==OVERLAY) relocate=block.overlay.relocation;
		else relocate=loadseg;
		pos=head.reloctable;DOS_SeekFile(fhandle,&pos,0);
		for (i=0;i<head.relocations;i++) {
			readsize=4;DOS_ReadFile(fhandle,(Bit8u *)&relocpt,&readsize);
			relocpt=host_readd((HostPt)&relocpt);		//Endianize
			PhysPt address=PhysMake(RealSeg(relocpt)+loadseg,RealOff(relocpt));
			mem_writew(address,mem_readw(address)+relocate);
		}
	}
	delete[] loadbuf;
	DOS_CloseFile(fhandle);

	/* Setup a psp */
	if (flags!=OVERLAY) {
		// Create psp after closing exe, to avoid dead file handle of exe in copied psp
		SetupPSP(pspseg,memsize,envseg);
		SetupCMDLine(pspseg,block);
	};
	CALLBACK_SCF(false);		/* Carry flag cleared for caller if successfull */
	if (flags==OVERLAY) return true;			/* Everything done for overlays */
	RealPt csip,sssp;
	if (iscom) {
		csip=RealMake(pspseg,0x100);
		if (memsize<0x1000) {
			LOG(LOG_EXEC,LOG_WARN)("COM format with only %X paragraphs available",memsize);
			sssp=RealMake(pspseg,(memsize<<4)-2);
		} else sssp=RealMake(pspseg,0xfffe);
		mem_writew(Real2Phys(sssp),0);
	} else {
		csip=RealMake(loadseg+head.initCS,head.initIP);
		sssp=RealMake(loadseg+head.initSS,head.initSP);
		if (head.initSP<4) LOG(LOG_EXEC,LOG_ERROR)("stack underflow/wrap at EXEC");
	}

	if ((flags==LOAD) || (flags==LOADNGO)) {
		/* Get Caller's program CS:IP of the stack and set termination address to that */
		RealSetVec(0x22,RealMake(mem_readw(SegPhys(ss)+reg_sp+2),mem_readw(SegPhys(ss)+reg_sp)));
		SaveRegisters();
		DOS_PSP callpsp(dos.psp());
		/* Save the SS:SP on the PSP of calling program */
		callpsp.SetStack(RealMakeSeg(ss,reg_sp));
		/* Switch the psp's and set new DTA */
		dos.psp(pspseg);
		DOS_PSP newpsp(dos.psp());
		dos.dta(RealMake(newpsp.GetSegment(),0x80));
		/* save vectors */
		newpsp.SaveVectors();
		/* copy fcbs */
		newpsp.SetFCB1(block.exec.fcb1);
		newpsp.SetFCB2(block.exec.fcb2);
		/* Save the SS:SP on the PSP of new program */
		newpsp.SetStack(RealMakeSeg(ss,reg_sp));

		/* Setup bx, contains a 0xff in bl and bh if the drive in the fcb is not valid */
		DOS_FCB fcb1(RealSeg(block.exec.fcb1),RealOff(block.exec.fcb1));
		DOS_FCB fcb2(RealSeg(block.exec.fcb2),RealOff(block.exec.fcb2));
		Bit8u d1 = fcb1.GetDrive(); //depends on 0 giving the dos.default drive
		if ( (d1>=DOS_DRIVES) || !Drives[d1] ) reg_bl = 0xFF; else reg_bl = 0;
		Bit8u d2 = fcb2.GetDrive();
		if ( (d2>=DOS_DRIVES) || !Drives[d2] ) reg_bh = 0xFF; else reg_bh = 0;

		/* Write filename in new program MCB */
		char stripname[8]= { 0 };Bitu index=0;
		while (char chr=*name++) {
			switch (chr) {
			case ':':case '\\':case '/':index=0;break;
			default:if (index<8) stripname[index++]=(char)toupper(chr);
			}
		}
		index=0;
		while (index<8) {
			if (stripname[index]=='.') break;
			if (!stripname[index]) break;	
			index++;
		}
		memset(&stripname[index],0,8-index);
		DOS_MCB pspmcb(dos.psp()-1);
		pspmcb.SetFileName(stripname);
		DOS_UpdatePSPName();
		// --- DWD BEGIN
		RunningProgramHash[ 0 ] = head.checksum;
		RunningProgramHash[ 1 ] = checksum_bytes;
		RunningProgramHash[ 2 ] = checksum;
		RunningProgramHash[ 3 ] = 0;
		// --- DWD END
	}

	if (flags==LOAD) {
		/* First word on the stack is the value ax should contain on startup */
		real_writew(RealSeg(sssp-2),RealOff(sssp-2),reg_bx);
		/* Write initial CS:IP and SS:SP in param block */
		block.exec.initsssp = sssp-2;
		block.exec.initcsip = csip;
		block.SaveData();
		/* Changed registers */
		reg_sp+=18;
		reg_ax=RealOff(csip);
		reg_bx=memsize;
		reg_dx=0;
		return true;
	}

	if (flags==LOADNGO) {
		if ((reg_sp>0xfffe) || (reg_sp<18)) LOG(LOG_EXEC,LOG_ERROR)("stack underflow/wrap at EXEC");
		/* Set the stack for new program */
		SegSet16(ss,RealSeg(sssp));reg_sp=RealOff(sssp);
		/* Add some flags and CS:IP on the stack for the IRET */
		CPU_Push16(RealSeg(csip));
		CPU_Push16(RealOff(csip));
		/* DOS starts programs with a RETF, so critical flags
		 * should not be modified (IOPL in v86 mode);
		 * interrupt flag is set explicitly, test flags cleared */
		reg_flags=(reg_flags&(~FMASK_TEST))|FLAG_IF;
		//Jump to retf so that we only need to store cs:ip on the stack
		reg_ip++;
		/* Setup the rest of the registers */
		reg_ax=reg_bx;
		reg_cx=0xff;
		reg_dx=pspseg;
		reg_si=RealOff(csip);
		reg_di=RealOff(sssp);
		reg_bp=0x91c;	/* DOS internal stack begin relict */
		SegSet16(ds,pspseg);SegSet16(es,pspseg);
#if C_DEBUG		
		/* Started from debug.com, then set breakpoint at start */
		DEBUG_CheckExecuteBreakpoint(RealSeg(csip),RealOff(csip));
#endif
		return true;
	}
	return false;
}
