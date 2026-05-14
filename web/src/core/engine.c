
/*
 * Web/Emscripten adaptation for Emu48 Android.
 *
 * Modified by: Claudio_ymt 
 * Date: 2026-05-14
 *
 * This file is part of a modified version of Emu48 Android.
 * Emulator-derived portions are licensed under the GNU General Public License v2.0.
 * See LICENSE-GPL.TXT for details.
 */




/*
 *   engine.c
 *
 *   This file is part of Emu48
 *
 *   Copyright (C) 1995 Sebastien Carlier
 *
 */
#include "pch.h"
#include "Emu48.h"
#include "Opcodes.h"
#include "io.h"
#include "debugger.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif


#define SAMPLE    16384						// speed adjust sample frequency

BOOL    bInterrupt = FALSE;
UINT    nState = SM_INVALID;
UINT    nNextState = SM_RUN;
BOOL    bEnableSlow = TRUE;					// slow down is enabled
BOOL    bRealSpeed = FALSE;
BOOL    bKeySlow = FALSE;					// slow down for key emulation
BOOL    bSoundSlow = FALSE;					// slow down for sound emulation
UINT    nOpcSlow = 0;						// no. of opcodes to slow down

CHIPSET Chipset;

TCHAR   szSerialWire[16];					// devicename for wire port
TCHAR   szSerialIr[16];						// devicename for IR port

DWORD   dwSXCycles = 82;					// SX cpu cycles in interval
DWORD   dwGXCycles = 123;					// GX cpu cycles in interval
DWORD   dwGPCycles = 123*3;			    	// g+ cpu cycles in interval    // CdB for HP: add apples display management
DWORD   dwG2Cycles = 123*2;					// gII cpu cycles in interval    // CdB for HP: add apples display management

DWORD   dwT2Cycles = 0xFFFFFFFF;			// CPU cycles in 16384 Hz time frame

// variables for debugger engine
HANDLE  hEventDebug;						// event handle to stop cpu thread

BOOL    bDbgAutoStateCtrl = TRUE;			// debugger suspend control by SwitchToState()
INT     nDbgState = DBG_OFF;				// state of debugger

BOOL    bDbgNOP3 = FALSE;					// halt on NOP3 (#420 opcode)
BOOL    bDbgRPL = FALSE;					// halt on RPL entry
BOOL    bDbgCode = FALSE;					// halt on DOCODE entry

BOOL    bDbgSkipInt = FALSE;				// execute interrupt handler

DWORD   dwDbgStopPC = -1;					// stop address for goto cursor
DWORD   dwDbgRplPC = -1;					// stop address for RPL breakpoint

DWORD   dwDbgRstkp;							// stack recursion level of step over end
DWORD   dwDbgRstk;							// possible return address

DWORD   *pdwInstrArray = NULL;				// last instruction array
WORD    wInstrSize = 256;					// size of last instruction array
WORD    wInstrWp;							// write pointer of instruction array
WORD    wInstrRp;							// read pointer of instruction array

VOID (*fnOutTrace)(VOID) = NULL;			// callback function for file trace

static INT   nDbgRplBreak = BN_ASM;			// flag for RPL breakpoint detection
static INT   nDbgOldState = DBG_OFF;		// old state of debugger for suspend/resume

static BOOL  bCpuSlow = FALSE;				// enable/disable real speed

static DWORD dwEDbgT2 = 0;					// debugger timer2 emulation
static DWORD dwEDbgCycles = 0;				// debugger cycle counter

static DWORD dwOldCyc;						// cpu cycles at last event
static DWORD dwSpeedRef;					// timer value at last event
static DWORD dwTickRef;						// sample timer ticks

#include "Ops.h"

// save last instruction in circular instruction buffer
static __inline VOID SaveInstrAddr(DWORD dwAddr)
{
	EnterCriticalSection(&csDbgLock);
	{
		if (pdwInstrArray)					// circular buffer allocated
		{
			pdwInstrArray[wInstrWp] = dwAddr;
			wInstrWp = (wInstrWp + 1) % wInstrSize;
			if (wInstrWp == wInstrRp)
				wInstrRp = (wInstrRp + 1) % wInstrSize;
		}
	}
	LeaveCriticalSection(&csDbgLock);
	return;
}

static __inline VOID Debugger(VOID)			// debugger part
{
	LARGE_INTEGER lDummyInt;				// sample timer ticks
	BOOL bStopEmulation;
	LPBYTE I = FASTPTR(Chipset.pc);			// get opcode stream

	UpdateDbgCycleCounter();				// update 64 bit cpu cycle counter

	SaveInstrAddr(Chipset.pc);				// save pc in last instruction buffer
	if (fnOutTrace != NULL)					// has a trace function
	{
		fnOutTrace();						// write file trace
	}

	nDbgRplBreak = BN_ASM;					// notify ASM breakpoint

	// check for code breakpoints
	bStopEmulation = CheckBreakpoint(Chipset.pc, 1, BP_EXEC);

	// check for memory breakpoints, opcode #14x or #15x
	if (I[0] == 0x1 && (I[1] == 0x4 || I[1] == 0x5))
	{
		DWORD dwData  = (I[2] & 0x1) ? Chipset.d1 : Chipset.d0;
		UINT  nType   = (I[2] & 0x2) ? BP_READ : BP_WRITE;

		DWORD dwRange;
		if (I[1] == 0x4)					// B,A
		{
			dwRange = (I[2] & 0x8) ? 2 : 5;
		}
		else								// number of nibbles, (P,WP,XS,X,S,M,W)
		{
			dwRange = (I[2] & 0x8) ? (I[3]+1) : (F_l[I[3]]);
		}
		#if defined DEBUG_DEBUGGER
		{
			TCHAR buffer[256];
			wsprintf(buffer,_T("Memory breakpoint %.5lx, %u\n",dwData,dwRange));
			OutputDebugString(buffer);
		}
		#endif
		bStopEmulation |= CheckBreakpoint(dwData, dwRange, nType);
	}

	// check for step cursor
	bStopEmulation |= (dwDbgStopPC == Chipset.pc);

	// NOP3, opcode #420 (GOC)
	if (bDbgNOP3 && I[0] == 0x4 && I[1] == 0x2 && I[2] == 0x0)
		bStopEmulation = TRUE;

	// stop on first instruction of DOCODE object
	if (bDbgCode && (Chipset.pc == 0x02DDE || Chipset.pc == 0x02E3C))
	{
		// return address
		DWORD dwAddr = Chipset.rstk[(Chipset.rstkp-1)&7];

		_ASSERT(I[0] == 0 && I[1] == 1);	// stopped at RTN opcode

		if (MapData(dwAddr) != M_ROM)		// address not in ROM
			dwDbgStopPC = dwAddr;			// then stop
	}

	// check for RPL breakpoint
	if (dwDbgRplPC == Chipset.pc)
	{
		dwDbgRplPC = -1;
		nDbgRplBreak = bStopEmulation ? BN_ASM_BT : BN_RPL;
		bStopEmulation = TRUE;
	}

	// RPL breakpoints, PC=(A), opcode #808C or PC=(C), opcode #808E
	if (I[0] == 0x8 && I[1] == 0x0 && I[2] == 0x8 && (I[3] == 0xC || I[3] == 0xE ))
	{
		// get next RPL entry
		DWORD dwAddr = Npack((I[3] == 0xC) ? Chipset.A : Chipset.C,5);

		if (bDbgRPL || CheckBreakpoint(dwAddr, 1, BP_RPL))
		{
			BYTE  byRplPtr[5];

			Npeek(byRplPtr,dwAddr,5);		// get PC address of next opcode
			dwDbgRplPC = Npack(byRplPtr,5);	// set RPL breakpoint
		}
	}

	// step over interrupt execution
	if (bDbgSkipInt && !bStopEmulation && !Chipset.inte)
		return;

	// check for step into
	bStopEmulation |= (nDbgState == DBG_STEPINTO);

	// check for step over
	bStopEmulation |= (nDbgState == DBG_STEPOVER) && dwDbgRstkp == Chipset.rstkp;

	// check for step out, something was popped from hardware stack
	if (nDbgState == DBG_STEPOUT && dwDbgRstkp == Chipset.rstkp)
	{
		_ASSERT(bStopEmulation == FALSE);
		if ((bStopEmulation = (Chipset.pc == dwDbgRstk)) == FALSE)
		{
			// it was C=RSTK, check for next object popped from hardware stack
			dwDbgRstkp = (Chipset.rstkp-1)&7;
			dwDbgRstk  = Chipset.rstk[dwDbgRstkp];
		}
	}

	if (bStopEmulation)						// stop condition
	{
		StopTimers();						// hold timer values when emulator is stopped
		if (Chipset.IORam[TIMER2_CTRL]&RUN)	// check if timer running
		{
			if (dwEDbgT2 == Chipset.t2)
			{
				// cpu cycles for one timer2 tick elapsed
				if ((DWORD) (Chipset.cycles & 0xFFFFFFFF) - dwEDbgCycles
					>= (SAMPLE / 8192) * dwT2Cycles)
				{
					--Chipset.t2;
					// adjust cycles reference
					dwEDbgCycles += (SAMPLE / 8192) * dwT2Cycles;
				}
			}
			else							// new timer2 value
			{
				// new cycle reference
				dwEDbgCycles = (DWORD) (Chipset.cycles & 0xFFFFFFFF);
			}

			// check rising edge of Bit 8 of timer2
			if ((dwEDbgT2 & 0x100) == 0 && (Chipset.t2 & 0x100) != 0)
				Chipset.t1 = (Chipset.t1 - 1) & 0xF;
		}
		dwEDbgT2 = Chipset.t2;				// timer2 check reference value

		// redraw debugger window and stop
		NotifyDebugger(nDbgRplBreak);
		WaitForSingleObject(hEventDebug,INFINITE);

		StartTimers();						// continue timers

		if (nDbgState >= DBG_OFF)			// if debugger is active
		{
			Chipset.Shutdn = FALSE;
			Chipset.bShutdnWake = FALSE;
		}

		// init slow down part
		dwOldCyc = (DWORD) (Chipset.cycles & 0xFFFFFFFF);
		QueryPerformanceCounter(&lDummyInt);
		dwSpeedRef = lDummyInt.LowPart;
	}
	return;
}

VOID SuspendDebugger(VOID)
{
	// auto control enabled, emulation halted by debugger
	if (bDbgAutoStateCtrl && nDbgState > DBG_OFF)
	{
		nDbgOldState = nDbgState;			// save old state
		nDbgState = DBG_SUSPEND;			// suspend state
		SetEvent(hEventDebug);				// exit debugger
	}
	return;
}

VOID ResumeDebugger(VOID)
{
	// auto control enabled, debugger is suspended
	if (bDbgAutoStateCtrl && nDbgState == DBG_SUSPEND)
	{
		// active RPL breakpoint
		if (nDbgRplBreak) dwDbgRplPC = Chipset.pc;
		nDbgState = nDbgOldState;			// set to old debugger state
		if (Chipset.Shutdn)					// inside shutdown
			SetEvent(hEventShutdn);			// leave it to call debugger
	}
	return;
}

static __inline VOID CheckDisp(BOOL bSync)
{
	if (disp == 0) return;					// no display update need

	// update display when drawing top line or display is off
	if (bSync && GetLineCounter() != 0x3F && (Chipset.IORam[0x00]&8))
		return;

	_ASSERT((disp & DISP_POINTER) == 0);	// display pointer already updated
	if (disp & DISP_MAIN)  UpdateMainDisplay();
	if (disp & DISP_MENUE) UpdateMenuDisplay();
	disp = 0;								// display updated
	return;
}

static __inline VOID AdjustSpeed(VOID)		// adjust emulation speed
{
	// emulation slow down
	if (   bEnableSlow
		&& (bCpuSlow || bKeySlow || bSoundSlow || nOpcSlow > 0))
	{
		DWORD dwCycles,dwTicks;

		EnterCriticalSection(&csSlowLock);
		{
			// cycles elapsed for next check
			if ((dwCycles = (DWORD) (Chipset.cycles & 0xFFFFFFFF)-dwOldCyc) >= dwT2Cycles)
			{
				LARGE_INTEGER lAct;
				do
				{
					VERIFY(QueryPerformanceCounter(&lAct));

					// get time difference
					dwTicks = lAct.LowPart - dwSpeedRef;
				}
				// ticks elapsed or negative number (workaround for QueryPerformanceCounter() in Win2k)
				while (dwTicks <= dwTickRef || (dwTicks & 0x80000000) != 0);

				dwOldCyc += dwT2Cycles;		// adjust cycles reference
				dwSpeedRef += dwTickRef;	// adjust reference time
			}

			if (nOpcSlow > 0) --nOpcSlow;	// decr. slow down opcode counter
		}
		LeaveCriticalSection(&csSlowLock);
	}
	return;
}

static __inline VOID SetT2Cycles(VOID)		// set device specific cpu cycles in interval
{
	switch (cCurrentRomType)
	{
	case 'S': // HP48SX
		dwT2Cycles = dwSXCycles;
		break;
	case '6': // HP38G with 64KB RAM
	case 'A': // HP38G
	case 'E': // HP39/40G
	case 'G': // HP48GX
	case 'X': // HP49G
	default:
		dwT2Cycles = dwGXCycles;
		break;
	case 'P': // HP39G+
	case 'Q': // HP49G+
		dwT2Cycles = dwGPCycles;
		break;
	case '2': // HP48GII
		dwT2Cycles = dwG2Cycles;
		break;
	}
	return;
}

VOID CheckSerial(VOID)
{
	// COM port closed and serial on
	if (CommIsOpen() == FALSE && (Chipset.IORam[IOC] & SON) != 0)
	{
		CommOpen(szSerialWire,szSerialIr); // open COM ports
	}

	// COM port opened and serial off
	if (CommIsOpen() == TRUE && (Chipset.IORam[IOC] & SON) == 0)
	{
		CommClose();					// close COM port
	}
	return;
}

VOID InitAdjustSpeed(VOID)
{
	// slow down function not initalized
	if (!bEnableSlow || (!bCpuSlow && !bKeySlow && !bSoundSlow && nOpcSlow == 0))
	{
		LARGE_INTEGER lTime;				// sample timer ticks

		EnterCriticalSection(&csSlowLock);
		{
			// save reference cycles
			dwOldCyc = (DWORD) (Chipset.cycles & 0xFFFFFFFF);
			QueryPerformanceCounter(&lTime);// get timer ticks
			dwSpeedRef = lTime.LowPart;		// save reference time
		}
		LeaveCriticalSection(&csSlowLock);
	}
	return;
}

VOID AdjKeySpeed(VOID)						// slow down key repeat
{
	WORD i;
	BOOL bKey;

	bKey = FALSE;							// search for a pressed key
	for (i = 0;i < ARRAYSIZEOF(Chipset.Keyboard_Row) && !bKey;++i)
		bKey = (Chipset.Keyboard_Row[i] != 0);

	if (bKey)								// key pressed
	{
		InitAdjustSpeed();					// init variables if necessary
	}
	bKeySlow = bKey;						// save new state
	return;
}

VOID SetSpeed(BOOL bAdjust)					// set emulation speed
{
	if (bAdjust)							// switch to real speed
	{
		InitAdjustSpeed();					// init variables if necessary
	}
	bCpuSlow = bAdjust;						// save emulation speed
	return;
}

VOID UpdateKdnBit(VOID)						// update KDN bit
{
	if (   Chipset.intk
		&& (Chipset.IORam[TIMER2_CTRL]&RUN) != 0
		&& (DWORD) (Chipset.cycles & 0xFFFFFFFF) - Chipset.dwKdnCycles > dwT2Cycles * 16)
		IOBit(SRQ2,KDN,Chipset.in != 0);
	return;
}

BOOL WaitForSleepState(VOID)				// wait for cpu SHUTDN then sleep state
{
	DWORD dwRefTime;

	SuspendDebugger();						// suspend debugger

	dwRefTime = timeGetTime();
	// wait for the SHUTDN command with 1.5 sec timeout
	while (timeGetTime() - dwRefTime < 1500L && !Chipset.Shutdn)
		Sleep(0);

	if (Chipset.Shutdn)						// not timeout, cpu is down
		SwitchToState(SM_SLEEP);			// go to sleep state
	else
		ResumeDebugger();					// timeout, resume to debugger

	return SM_SLEEP != nNextState;			// state not changed, emulator was busy
}

UINT SwitchToState(UINT nNewState)
{
	UINT nOldState = nState;

	if (nState == nNewState) return nOldState;
	switch (nState)
	{
	case SM_RUN: // Run
		switch (nNewState)
		{
		case SM_INVALID: // -> Invalid
			nNextState = SM_INVALID;
			if (Chipset.Shutdn)
				SetEvent(hEventShutdn);
			else
				bInterrupt = TRUE;
			SuspendDebugger();				// suspend debugger
			while (nState!=nNextState) Sleep(0);
			break;
		case SM_RETURN: // -> Return
			DisableDebugger();				// disable debugger
			nNextState = SM_INVALID;
			if (Chipset.Shutdn)
				SetEvent(hEventShutdn);
			else
				bInterrupt = TRUE;
			while (nState!=nNextState) Sleep(0);
			nNextState = SM_RETURN;
			SetEvent(hEventShutdn);
			WaitForSingleObject(hThread,INFINITE);
			break;
		case SM_SLEEP: // -> Sleep
			nNextState = SM_SLEEP;
			bInterrupt = TRUE;				// exit main loop
			SuspendDebugger();				// suspend debugger
			SetEvent(hEventShutdn);			// exit shutdown
			while (nState!=nNextState) Sleep(0);
			bInterrupt = FALSE;
			ResetEvent(hEventDebug);
			ResetEvent(hEventShutdn);
			break;
		}
		break;
	case SM_INVALID: // Invalid
		switch (nNewState)
		{
		case SM_RUN: // -> Run
			nNextState = SM_RUN;
			// don't enter opcode loop on interrupt request
			bInterrupt = Chipset.Shutdn || Chipset.SoftInt;
			ResumeDebugger();
			SetEvent(hEventShutdn);
			while (nState!=nNextState) Sleep(0);
			break;
		case SM_RETURN: // -> Return
			DisableDebugger();				// disable debugger
			nNextState = SM_RETURN;
			SetEvent(hEventShutdn);
			WaitForSingleObject(hThread,INFINITE);
			break;
		case SM_SLEEP: // -> Sleep
			nNextState = SM_SLEEP;
			SetEvent(hEventShutdn);
			while (nState!=nNextState) Sleep(0);
			break;
		}
		break;
	case SM_SLEEP: // Sleep
		switch (nNewState)
		{
		case SM_RUN: // -> Run
			nNextState = SM_RUN;
			// don't enter opcode loop on interrupt request
			bInterrupt = (nDbgState == DBG_OFF) && (Chipset.Shutdn || Chipset.SoftInt);
			ResumeDebugger();
			SetEvent(hEventShutdn);			// leave sleep state
			break;
		case SM_INVALID: // -> Invalid
			nNextState = SM_INVALID;
			SetEvent(hEventShutdn);
			while (nState!=nNextState) Sleep(0);
			break;
		case SM_RETURN: // -> Return
			DisableDebugger();				// disable debugger
			nNextState = SM_INVALID;
			SetEvent(hEventShutdn);
			while (nState!=nNextState) Sleep(0);
			nNextState = SM_RETURN;
			SetEvent(hEventShutdn);
			WaitForSingleObject(hThread,INFINITE);
			break;
		}
		break;
	}
	return nOldState;
}

UINT WorkerThread(LPVOID pParam)
{
	LARGE_INTEGER lDummyInt;				// sample timer ticks
	QueryPerformanceFrequency(&lDummyInt);	// init timer ticks
	lDummyInt.QuadPart /= SAMPLE;			// calculate sample ticks
	dwTickRef = lDummyInt.LowPart;			// sample timer ticks
	_ASSERT(dwTickRef);						// tick resolution error

loop:
	while (nNextState == SM_INVALID)		// go into invalid state
	{
		OnToolMacroStop();					// close open keyboard macro handler
		CommClose();						// close COM port
		nState = SM_INVALID;				// in invalid state
		#ifdef __EMSCRIPTEN__
                                Chipset.bShutdnWake = TRUE;
#else
                                WaitForSingleObject(hEventShutdn,INFINITE);
#endif
		if (nNextState == SM_RETURN)		// go into return state
		{
			nState = SM_RETURN;				// in return state
			return 0;						// kill thread
		}
		CheckSerial();						// test if UART on
	}
	while (nNextState == SM_RUN)
{
#ifdef __EMSCRIPTEN__
{
static int s_wt_iter = 0;
if (s_wt_iter++ < 5) {
LOGD(
"WorkerThread iter #%d: nState=%u nNextState=%u (SM_RUN=%u, SM_INVALID=%u)",
s_wt_iter, nState, nNextState, SM_RUN, SM_INVALID);
}
}
#endif
if (nState != SM_RUN)
{
#ifdef __EMSCRIPTEN__
LOGD(
"WorkerThread entering SM_RUN init block, type=0x%02X, pbyRom=%p",
(BYTE)cCurrentRomType, pbyRom);
#endif
			nState = SM_RUN;
			// clear port2 status bits
			Chipset.cards_status &= ~(PORT2_PRESENT | PORT2_WRITE);
			if (pbyPort2 || Port2)			// card plugged in port2
			{
				Chipset.cards_status |= PORT2_PRESENT;

				if (bPort2Writeable)		// is card writeable
					Chipset.cards_status |= PORT2_WRITE;
			}
			// card detection off and timer running
			if ((Chipset.IORam[CARDCTL] & ECDT) == 0 && (Chipset.IORam[TIMER2_CTRL] & RUN) != 0)
			{
				BOOL bNINT2 = Chipset.IORam[SRQ1] == 0 && (Chipset.IORam[SRQ2] & LSRQ) == 0;
				BOOL bNINT  = (Chipset.IORam[CARDCTL] & SMP) == 0;

				// state of CDT2
				bNINT2 = bNINT2 && (Chipset.cards_status & (P2W|P2C)) != P2C;
				// state of CDT1
				bNINT  = bNINT  && (Chipset.cards_status & (P1W|P1C)) != P1C;

				IOBit(SRQ2,NINT2,bNINT2);
				IOBit(SRQ2,NINT,bNINT);
			}
			#ifdef __EMSCRIPTEN__
LOGD(
"engine.c: calling RomSwitch(Bank_FF=%u) NOW", (unsigned)Chipset.Bank_FF);
#endif
RomSwitch(Chipset.Bank_FF);// select HP49G ROM bank and update memory mapping
			#ifdef __EMSCRIPTEN__
// Dump 32 nibbles around the boot loop area (0xB0..0xD0)
{
BYTE *p = FASTPTR(0xB0);
if (p) {
LOGD(
"ROM[0xB0..0xCF] nibbles: %X%X %X%X %X%X %X%X  %X%X %X%X %X%X %X%X  %X%X %X%X %X%X %X%X  %X%X %X%X %X%X %X%X",
p[0]&0xF,p[1]&0xF,  p[2]&0xF,p[3]&0xF,  p[4]&0xF,p[5]&0xF,  p[6]&0xF,p[7]&0xF,
p[8]&0xF,p[9]&0xF,  p[10]&0xF,p[11]&0xF,p[12]&0xF,p[13]&0xF,p[14]&0xF,p[15]&0xF,
p[16]&0xF,p[17]&0xF,p[18]&0xF,p[19]&0xF,p[20]&0xF,p[21]&0xF,p[22]&0xF,p[23]&0xF,
p[24]&0xF,p[25]&0xF,p[26]&0xF,p[27]&0xF,p[28]&0xF,p[29]&0xF,p[30]&0xF,p[31]&0xF);
}
}
#endif
UpdateContrast(Chipset.contrast);
			UpdateDisplayPointers();
			UpdateMainDisplay();
			UpdateMenuDisplay();
			RefreshDisp0();    // CdB for HP: add apples display management
			UpdateAnnunciators(0x3F);
			// init speed reference
			dwOldCyc = (DWORD) (Chipset.cycles & 0xFFFFFFFF);
			QueryPerformanceCounter(&lDummyInt);
			dwSpeedRef = lDummyInt.LowPart;
			SetT2Cycles();					// set device specific cpu cycles in interval
			SetHP48Time();					// update HP48 time & date
			// start display counter/update engine
			StartDisplay((BYTE)(((Chipset.IORam[LINECOUNT+1]<<4)|Chipset.IORam[LINECOUNT])&0x3F));
			StartBatMeasure();				// start battery measurement
			StartTimers();
		}
		PCHANGED;
		while (!bInterrupt)
                {
                        if (nDbgState > DBG_OFF)                // debugger active
                        {
                                Debugger();
                                // if suspended skip next opcode execution
                                if (nDbgState == DBG_SUSPEND)
                                {
                                        if (Chipset.Shutdn) break;
                                        continue;
                                }
                        }
                        EvalOpcode(FASTPTR(Chipset.pc)); // execute opcode
                        // check for display update in BW mode
                        if (!bGrayscale) CheckDisp(!Chipset.Shutdn);
                        AdjustSpeed();                                  // adjust emulation speed

#ifdef __EMSCRIPTEN__
                        {
                                extern void web_yield_to_browser(void);
                                static unsigned int s_emcc_yield_ctr = 0;
                                static int s_logged_first_yield = 0;
                                if (!s_logged_first_yield) {
                                        s_logged_first_yield = 1;
                                        LOGD( "engine.c: first opcode executed, pc=0x%05X", (unsigned)Chipset.pc);

                                        // Read 8 bytes at current PC to see what the CPU is executing
                                        BYTE *p = FASTPTR(Chipset.pc);
                                        if (p) {
                                                LOGD(
                                                        "  -> mem[pc..pc+7] = %02X %02X %02X %02X %02X %02X %02X %02X",
                                                        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                                        } else {
                                                LOGD( "  -> FASTPTR returned NULL!");
                                        }

                                        {
                                                extern LPBYTE pbyRom;
                                                extern DWORD dwRomSize;
                                                LOGD(
                                                        "  -> pbyRom=%p dwRomSize=%u  FASTPTR(pc)=%p (delta=%ld)",
                                                        pbyRom, dwRomSize, p, p ? (long)(p - pbyRom) : -1);
                                                if (pbyRom) {
                                                        LOGD(
                                                                "  -> pbyRom[0..7]   = %02X %02X %02X %02X %02X %02X %02X %02X",
                                                                pbyRom[0],pbyRom[1],pbyRom[2],pbyRom[3],
                                                                pbyRom[4],pbyRom[5],pbyRom[6],pbyRom[7]);
                                                }
                                        }
                                }

                                // PC trace ring buffer (last 32 PCs visited)
                                #define PC_TRACE_SIZE 32
                                static DWORD s_pc_trace[PC_TRACE_SIZE] = {0};
                                static unsigned int s_pc_trace_idx = 0;
                                s_pc_trace[s_pc_trace_idx] = Chipset.pc;
                                s_pc_trace_idx = (s_pc_trace_idx + 1) % PC_TRACE_SIZE;
                                // yield every 16384 opcodes for faster UI response
                                if ((++s_emcc_yield_ctr & 0x3FFF) == 0) {
                                        web_yield_to_browser();
                                }

                                {
                                        static unsigned int s_opc_ctr = 0;
                                        if ((++s_opc_ctr & 0xFFFFF) == 0) {
                                                extern LPBYTE RMap[];
                                                extern LPBYTE pbyRom;
                                                LOGD(
                                                        "CPU progress: opcodes=%uM, pc=0x%05X, t1=%X, t2=%08X, IO[2F]=%02X, IO[BITOFFSET]=%02X",
                                                        (s_opc_ctr >> 20), (unsigned)Chipset.pc,
                                                        Chipset.t1, Chipset.t2,
                                                        Chipset.IORam[0x2F],
                                                        Chipset.IORam[0]);
                                                DWORD page = Chipset.pc >> 12;
                                                LOGD(
                                                        "  -> RMap[%u]=%p (delta from pbyRom=%ld)",
                                                        (unsigned)page, RMap[page],
                                                        RMap[page] ? (long)(RMap[page] - pbyRom) : -1);
                                        BYTE *opcptr = FASTPTR(Chipset.pc);
                                        LOGD(
                                                "  -> at pc=0x%05X: nibbles = %X %X %X %X %X %X %X %X | inte=%d Shutdn=%d IORam[0x00..0x07]=%02X %02X %02X %02X %02X %02X %02X %02X",
                                                (unsigned)Chipset.pc,
                                                opcptr[0]&0xF, opcptr[1]&0xF, opcptr[2]&0xF, opcptr[3]&0xF,
                                                opcptr[4]&0xF, opcptr[5]&0xF, opcptr[6]&0xF, opcptr[7]&0xF,
                                                (int)Chipset.inte, (int)Chipset.Shutdn,
                                                Chipset.IORam[0], Chipset.IORam[1], Chipset.IORam[2], Chipset.IORam[3],
                                                Chipset.IORam[4], Chipset.IORam[5], Chipset.IORam[6], Chipset.IORam[7]);
                                        // Dump PC trace ring buffer (oldest -> newest)
                                        {
                                            char trace_buf[512];
                                            char *tb = trace_buf;
                                            for (int i = 0; i < PC_TRACE_SIZE; i++) {
                                                int idx = (s_pc_trace_idx + i) % PC_TRACE_SIZE;
                                                tb += sprintf(tb, "%05X ", (unsigned)s_pc_trace[idx]);
                                            }
                                            LOGD( "  -> last %d PCs: %s", PC_TRACE_SIZE, trace_buf);
                                        }

                                        // Dump CPU registers (top nibble of each working register field)
                                        LOGD(
                                            "  -> regs: D0=%05X D1=%05X P=%X carry=%d HST=%X ST=%04X RSTK[top]=%05X mode=%c",
                                            (unsigned)Chipset.d0, (unsigned)Chipset.d1,
                                            (unsigned)Chipset.P, (int)Chipset.carry,
                                            (unsigned)Chipset.HST, (unsigned)Chipset.ST,
                                            (unsigned)(Chipset.rstkp > 0 ? Chipset.rstk[Chipset.rstkp - 1] : 0xFFFFF),
                                            Chipset.mode_dec ? 'D' : 'H');

                                        // Dump nibbles 0..15 of registers A, B, C, D (low to high)
                                        LOGD(
                                            "  -> A=%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X  B=%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X",
                                            Chipset.A[15],Chipset.A[14],Chipset.A[13],Chipset.A[12],
                                            Chipset.A[11],Chipset.A[10],Chipset.A[9],Chipset.A[8],
                                            Chipset.A[7],Chipset.A[6],Chipset.A[5],Chipset.A[4],
                                            Chipset.A[3],Chipset.A[2],Chipset.A[1],Chipset.A[0],
                                            Chipset.B[15],Chipset.B[14],Chipset.B[13],Chipset.B[12],
                                            Chipset.B[11],Chipset.B[10],Chipset.B[9],Chipset.B[8],
                                            Chipset.B[7],Chipset.B[6],Chipset.B[5],Chipset.B[4],
                                            Chipset.B[3],Chipset.B[2],Chipset.B[1],Chipset.B[0]);
                                        LOGD(
                                            "  -> C=%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X  D=%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X%X",
                                            Chipset.C[15],Chipset.C[14],Chipset.C[13],Chipset.C[12],
                                            Chipset.C[11],Chipset.C[10],Chipset.C[9],Chipset.C[8],
                                            Chipset.C[7],Chipset.C[6],Chipset.C[5],Chipset.C[4],
                                            Chipset.C[3],Chipset.C[2],Chipset.C[1],Chipset.C[0],
                                            Chipset.D[15],Chipset.D[14],Chipset.D[13],Chipset.D[12],
                                            Chipset.D[11],Chipset.D[10],Chipset.D[9],Chipset.D[8],
                                            Chipset.D[7],Chipset.D[6],Chipset.D[5],Chipset.D[4],
                                            Chipset.D[3],Chipset.D[2],Chipset.D[1],Chipset.D[0]);
                                        }
                                }
                        }
#endif                
                }
		bInterrupt = FALSE;					// be sure to reenter opcode loop

// enter SHUTDN handler only in RUN mode
if (Chipset.Shutdn && !(nDbgState == DBG_STEPINTO || nDbgState == DBG_STEPOVER))
                {
#ifdef __EMSCRIPTEN__
                        static int s_shutdn_enter_ctr = 0;
                        static int s_auto_on_done    = 0;
                        s_shutdn_enter_ctr++;

                        LOGD(
                            "engine.c: entering Shutdn #%d (SoftInt=%d, IO[TIMER1_CTRL]=%02X, IO[TIMER2_CTRL]=%02X, IO[BITOFFSET]=%02X)",
                            s_shutdn_enter_ctr, (int)Chipset.SoftInt,
                            Chipset.IORam[0x2E], Chipset.IORam[0x2F], Chipset.IORam[0]);

                        if (!s_auto_on_done && s_shutdn_enter_ctr == 1) {
                            s_auto_on_done = 1;
                            LOGD(
                                "engine.c: AUTO-ON: setting bShutdnWake=TRUE on first Shutdn");
                            Chipset.bShutdnWake = TRUE;
                        }

                        if (!Chipset.SoftInt) {
                            extern void web_yield_to_browser(void);
                            int poll_iters = 0;
                            while (!Chipset.bShutdnWake && nNextState == SM_RUN) {
                                web_yield_to_browser();
                                emscripten_sleep(10);
                                poll_iters++;
                            }
                        } else {
                            Chipset.bShutdnWake = TRUE;
                        }
#else
                        if (!Chipset.SoftInt)
                                WaitForSingleObject(hEventShutdn,INFINITE);
else
Chipset.bShutdnWake = TRUE;
#endif
			if (Chipset.bShutdnWake)		// waked up by timer, keyboard or serial
			{
				Chipset.bShutdnWake = FALSE;
				Chipset.Shutdn = FALSE;
				// init speed reference
				dwOldCyc = (DWORD) (Chipset.cycles & 0xFFFFFFFF);
				QueryPerformanceCounter(&lDummyInt);
				dwSpeedRef = lDummyInt.LowPart;
				nOpcSlow = 0;				// no opcodes to slow down
			}
		}
		if (Chipset.SoftInt)
		{
			Chipset.SoftInt = FALSE;
			if (Chipset.inte)
			{
				Chipset.inte = FALSE;
				rstkpush(Chipset.pc);
				Chipset.pc = 0xf;
			}
		}
	}
	_ASSERT(nNextState != SM_RUN);

	StopDisplay();							// stop display counter/update
	StopBatMeasure();						// stop battery measurement
	StopTimers();

	while (nNextState == SM_SLEEP)			// go into sleep state
	{
		nState = SM_SLEEP;					// in sleep state
		#ifdef __EMSCRIPTEN__
                                Chipset.bShutdnWake = TRUE;
#else
                                WaitForSingleObject(hEventShutdn,INFINITE);
#endif
	}
	goto loop;
	UNREFERENCED_PARAMETER(pParam);
}









