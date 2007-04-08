/* Monitor WinCE exceptions.
 *
 * (C) Copyright 2006 Kevin O'Connor <kevin@koconnor.net>
 *
 * This file may be distributed under the terms of the GNU GPL license.
 */

#include <windows.h> // for pkfuncs.h
#include "pkfuncs.h" // AllocPhysMem
#include <time.h> // time
#include <string.h> // memcpy

#include "xtypes.h"
#include "watch.h" // memcheck
#include "output.h" // Output
#include "memory.h" // memPhysMap
#include "cpu.h" // DEF_GETCPRATTR
#include "script.h" // REG_CMD
#include "cbitmap.h" // BITMAPSIZE
#include "machines.h" // getIrqNames
#include "arch-pxa.h" // MachinePXA
#include "lateload.h" // LATE_LOAD

LATE_LOAD(AllocPhysMem, "coredll")
LATE_LOAD(FreePhysMem, "coredll")


/****************************************************************
 * Shared storage between irq handlers and reporting code
 ****************************************************************/

struct traceitem;
typedef void (*tracereporter)(uint32 msecs, traceitem *);

// The layout of each item in the "trace buffer" shared between the
// exception handlers and the monitoring code.
struct traceitem {
    // Event specific reporter
    tracereporter reporter;
    // Data
    uint32 d0, d1, d2, d3, d4;
};

static const uint32 MAX_IRQ = 32 + 2 + 120;
static const uint32 MAX_IGNOREADDR = 64;

// Maximum number of irq/trace level memory polls available.
#define MAX_MEMCHECK 32

// Number of items to place in the trace buffer.  MUST be a power of
// 2.
#define NR_TRACE 8192
#if (NR_TRACE & (NR_TRACE-1)) != 0
#error NR_TRACE must be a power of 2
#endif

// Persistent data accessible by both exception handlers and regular
// code.
struct irqData {
    // Trace buffer.
    uint32 overflows, errors;
    uint32 writePos, readPos;
    struct traceitem traces[NR_TRACE];

    // Intel PXA based chip?
    int isPXA;

    // Irq information.
    uint8 *irq_ctrl, *gpio_ctrl;
    uint32 ignoredIrqs[BITMAPSIZE(MAX_IRQ)];
    uint32 demuxGPIOirq;
    uint32 irqpollcount;
    memcheck irqpolls[MAX_MEMCHECK];

    // Debug information.
    uint32 ignoreAddr[MAX_IGNOREADDR];
    uint32 traceForWatch;

    // Instruction trace information.
    struct insn_s { uint32 addr1, addr2, reg1, reg2; } insns[2];
    uint32 dbr0, dbr1, dbcon;
    uint32 tracepollcount;
    memcheck tracepolls[MAX_MEMCHECK];

    // Summary counters.
    uint32 irqCount, abortCount, prefetchCount;
};

// Add an item to the trace buffer.
static inline int __irq
add_trace(struct irqData *data, tracereporter reporter
          , uint32 d0=0, uint32 d1=0, uint32 d2=0, uint32 d3=0, uint32 d4=0)
{
    if (data->writePos - data->readPos >= NR_TRACE) {
        // No more space in trace buffer.
        data->overflows++;
        return -1;
    }
    struct traceitem *pos = &data->traces[data->writePos % NR_TRACE];
    pos->reporter = reporter;
    pos->d0 = d0;
    pos->d1 = d1;
    pos->d2 = d2;
    pos->d3 = d3;
    pos->d4 = d4;
    data->writePos++;
    return 0;
}

static uint32 LastOverflowReport;

// Pull a trace event from the trace buffer and print it out.  Returns
// 0 if nothing was available.
static int
printTrace(uint32 msecs, struct irqData *data)
{
    uint32 writePos = data->writePos;
    if (data->readPos == writePos)
        return 0;
    uint32 tmpoverflow = data->overflows;
    if (tmpoverflow != LastOverflowReport) {
        Output("overflowed %d traces"
               , tmpoverflow - LastOverflowReport);
        LastOverflowReport = tmpoverflow;
    }
    struct traceitem *cur = &data->traces[data->readPos % NR_TRACE];
    cur->reporter(msecs, cur);
    data->readPos++;
    return 1;
}


/****************************************************************
 * ARM Register manipulation
 ****************************************************************/

// Contents of register description passed into the exception
// handlers.  This layout corresponds with the assembler code in
// irqchain.S.  If one changes a register it _will_ alter the machine
// state after the exception handler exits.
struct irqregs {
    union {
        struct {
            uint32 r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
        };
        uint32 regs[13];
    };
    uint32 old_pc;
};

// Create a set of CPU coprocessor accessor functions for irq handlers
#define DEF_GETIRQCPR(Name, Cpr, Op1, CRn, CRm, Op2)     \
    DEF_GETCPRATTR(Name, Cpr, Op1, CRn, CRm, Op2, __irq)
#define DEF_SETIRQCPR(Name, Cpr, Op1, CRn, CRm, Op2)     \
    DEF_SETCPRATTR(Name, Cpr, Op1, CRn, CRm, Op2, __irq)

// Get pid - can't use haret's because it isn't in this irq section.
DEF_GETIRQCPR(get_PID, p15, 0, c13, c0, 0)

// Return the Modified Virtual Address (MVA) of a given PC.
static inline uint32 __irq transPC(uint32 pc) {
    if (pc <= 0x01ffffff)
        // Need to turn virtual address in to modified virtual address.
        pc |= (get_PID() & 0xfe000000);
    return pc;
}

// Get the SPSR register
static inline uint32 __irq get_SPSR(void) {
    uint32 val;
    asm volatile("mrs %0, spsr" : "=r" (val));
    return val;
}

struct extraregs {
    union {
        struct {
            uint32 r13, r14;
        };
        uint32 regs[2];
    };
    uint32 didfetch;
};

// Return the value of a given register.
static uint32 __irq
getReg(struct irqregs *regs, struct extraregs *er, uint32 nr)
{
    if (nr < 13)
        return regs->regs[nr];
    if (nr >= 15)
        return regs->old_pc;
    if (!er->didfetch) {
        // In order to access the r13/r14 registers, it is necessary
        // to switch contexts, copy the registers to low order
        // registers, and then switch context back.
        uint32 newContext = get_SPSR();
        newContext &= 0x1f; // Extract mode bits.
        newContext |= (1<<6)|(1<<7); // Disable interrupts
        uint32 temp;
        asm volatile("mrs %0, cpsr @ Get current cpsr\n"
                     "msr cpsr, %3 @ Change processor mode\n"
                     "mov %1, r13  @ Get r13\n"
                     "mov %2, r14  @ Get r14\n"
                     "msr cpsr, %0 @ Restore processor mode"
                     : "=&r" (temp), "=r" (er->r13), "=r" (er->r14)
                     : "r" (newContext));
        er->didfetch = 1;
    }
    return er->regs[nr-13];
}

#define mask_Rn(insn) (((insn)>>16) & 0xf)
#define mask_Rd(insn) (((insn)>>12) & 0xf)

// Lookup an assembler name for an instruction - this is incomplete.
static const char *
getInsnName(uint32 insn)
{
    const char *iname = "?";
    int isLoad = insn & 0x00100000;
    if ((insn & 0x0C000000) == 0x04000000) {
        if (isLoad) {
            if (insn & (1<<22))
                iname = "ldrb";
            else
                iname = "ldr";
        } else {
            if (insn & (1<<22))
                iname = "strb";
            else
                iname = "str";
        }
    } else if ((insn & 0x0E000000) == 0x00000000) {
        int lowbyte = insn & 0xF0;
        if (isLoad) {
            if (lowbyte == 0xb0)
                iname = "ldrh";
            else if (lowbyte == 0xd0)
                iname = "ldrsb";
            else if (lowbyte == 0xf0)
                iname = "ldrsh";
        } else {
            if (lowbyte == 0xb0)
                iname = "strh";
            else if (lowbyte == 0x90)
                iname = "swp?";
        }
    }

    return iname;
}


/****************************************************************
 * C part of exception handlers
 ****************************************************************/

static void
report_memPoll(uint32 msecs, traceitem *item)
{
    memcheck *mc = (memcheck*)item->d0;
    uint32 clock=item->d1, val=item->d2, mask=item->d3;
    mc->reporter(msecs, clock, mc, val, mask);
}

// Perform a set of memory polls and add to trace buffer.
static int __irq
checkPolls(struct irqData *data, uint32 clock, memcheck *list, uint32 count)
{
    int foundcount = 0;
    for (uint i=0; i<count; i++) {
        memcheck *mc = &list[i];
        uint32 val, maskval;
        int ret = testMem(mc, &val, &maskval);
        if (!ret)
            continue;
        foundcount++;
        ret = add_trace(data, report_memPoll, (uint32)mc, clock, val, maskval);
        if (ret)
            // Couldn't add trace - reset compare function.
            mc->trySuppress = 0;
    }
    return foundcount;
}

static void __irq PXA_irq_handler(struct irqData *data, struct irqregs *regs);
static int __irq PXA_abort_handler(struct irqData *data, struct irqregs *regs);
static int __irq PXA_prefetch_handler(struct irqData *data, struct irqregs *regs);

// Handler for interrupt events.  Note that this is running in
// "Modified Virtual Address" mode, so avoid reading any global
// variables or calling any non local functions.
extern "C" void __irq
irq_handler(struct irqData *data, struct irqregs *regs)
{
    data->irqCount++;
    if (data->isPXA) {
        // Separate routine for PXA chips
        PXA_irq_handler(data, regs);
        return;
    }

    // Irq time memory polling.
    checkPolls(data, 0, data->irqpolls, data->irqpollcount);
    // Trace time memory polling.
    checkPolls(data, 0, data->tracepolls, data->tracepollcount);
}

extern "C" int __irq
abort_handler(struct irqData *data, struct irqregs *regs)
{
    data->abortCount++;
    if (data->isPXA)
        // Separate routine for PXA chips
        return PXA_abort_handler(data, regs);
    return 0;
}

extern "C" int __irq
prefetch_handler(struct irqData *data, struct irqregs *regs)
{
    data->prefetchCount++;
    if (data->isPXA)
        // Separate routine for PXA chips
        return PXA_prefetch_handler(data, regs);
    return 0;
}


/****************************************************************
 * Standard interface commands and variables
 ****************************************************************/

// Commands and variables are only applicable if AllocPhysMem is
// available.
static int testAvail() {
    return late_AllocPhysMem && late_FreePhysMem;
}

static uint32 watchirqcount;
static memcheck watchirqpolls[16];

static void
cmd_addirqwatch(const char *cmd, const char *args)
{
    watchCmdHelper(watchirqpolls, ARRAY_SIZE(watchirqpolls), &watchirqcount
                   , cmd, args);
}
REG_CMD(testAvail, "ADDIRQWATCH", cmd_addirqwatch,
        "ADDIRQWATCH <addr> [<mask> <32|16|8> <cmpValue>]\n"
        "  Setup an address to be polled when an irq hits\n"
        "  See ADDWATCH for syntax.  <CLEAR|LS>IRQWATCH is also available.")
REG_CMD_ALT(testAvail, "CLEARIRQWATCH", cmd_addirqwatch, clear, 0)
REG_CMD_ALT(testAvail, "LSIRQWATCH", cmd_addirqwatch, list, 0)

static uint32 watchtracecount;
static memcheck watchtracepolls[16];

static void
cmd_addtracewatch(const char *cmd, const char *args)
{
    watchCmdHelper(watchtracepolls, ARRAY_SIZE(watchtracepolls), &watchtracecount
                   , cmd, args);
}
REG_CMD(testAvail, "ADDTRACEWATCH", cmd_addtracewatch,
        "ADDTRACEWATCH <addr> [<mask> <32|16|8> <cmpValue>]\n"
        "  Setup an address to be polled when an irq hits\n"
        "  See ADDWATCH for syntax.  <CLEAR|LS>TRACEWATCH is also available.")
REG_CMD_ALT(testAvail, "CLEARTRACEWATCH", cmd_addtracewatch, clear, 0)
REG_CMD_ALT(testAvail, "LSTRACEWATCH", cmd_addtracewatch, list, 0)


/****************************************************************
 * Code to report feedback from exception handlers
 ****************************************************************/

// Called before exceptions are taken over.
static void
preLoop(struct irqData *data)
{
    LastOverflowReport = 0;

    // Setup memory tracing.
    memcpy(data->irqpolls, watchirqpolls, sizeof(data->irqpolls));
    data->irqpollcount = watchirqcount;
    memcpy(data->tracepolls, watchtracepolls, sizeof(data->tracepolls));
    data->tracepollcount = watchtracecount;
}

// Code called while exceptions are rerouted - should return after
// 'seconds' amount of time has passed.  This is called from normal
// process context, and the full range of input/output functions and
// variables are available.
static void
mainLoop(struct irqData *data, int seconds)
{
    uint32 start_time = GetTickCount();
    uint32 cur_time = start_time;
    uint32 fin_time = cur_time + seconds * 1000;
    int tmpcount = 0;
    while (cur_time <= fin_time) {
        int ret = printTrace(cur_time - start_time, data);
        if (ret) {
            // Processed a trace - try to process another without
            // sleeping.
            tmpcount++;
            if (tmpcount < 100)
                continue;
            // Hrmm.  Recheck the current time so that we don't run
            // away reporting traces.
        } else
            // Nothing to report; yield the cpu.
            late_SleepTillTick();
        cur_time = GetTickCount();
        tmpcount = 0;
    }
}

// Called after exceptions are restored to wince - may be used to
// report additional data or cleanup structures.
static void
postLoop(struct irqData *data)
{
    // Flush trace buffer.
    for (;;) {
        int ret = printTrace(0, data);
        if (! ret)
            break;
    }
    Output("Handled %d irq, %d abort, %d prefetch, %d lost, %d errors"
           , data->irqCount, data->abortCount, data->prefetchCount
           , data->overflows, data->errors);
}


/****************************************************************
 * Intel PXA specific memory tracing
 ****************************************************************/

enum {
    ICHP_VAL_IRQ = 1<<31,

    START_GPIO_IRQS = 34,
    NR_GPIOx_IRQ = 10,

    IRQ_OFFSET = 0x40D00000,

    IRQ_ICHP_OFFSET = 0x18,
    IRQ_ICMR_OFFSET = 0x04,
    IRQ_ICIP_OFFSET = 0x00,
    IRQ_ICMR2_OFFSET = 0xA0,
    IRQ_ICIP2_OFFSET = 0x9c,

    GPIO_OFFSET = 0x40E00000,

    GPIO_GEDR0_OFFSET = 0x48,
    GPIO_GEDR1_OFFSET = 0x4c,
    GPIO_GEDR2_OFFSET = 0x50,
    GPIO_GEDR3_OFFSET = 0x148,
};

#define mask_ICHP_IRQ(ichp) (((ichp)>>16) & ((1<<6)-1))

// The CCNT performance monitoring register
DEF_GETIRQCPR(get_CCNT, p14, 0, c1, c1, 0)
// The DBCON software debug register
DEF_GETIRQCPR(get_DBCON, p15, 0, c14, c4, 0)
DEF_SETIRQCPR(set_DBCON, p15, 0, c14, c4, 0)
// Interrupt status register
DEF_GETIRQCPR(get_ICHP, p6, 0, c5, c0, 0)
// Set the IBCR0 software debug register
DEF_SETIRQCPR(set_IBCR0, p15, 0, c14, c8, 0)
// Set the IBCR1 software debug register
DEF_SETIRQCPR(set_IBCR1, p15, 0, c14, c9, 0)
// Set the EVTSEL performance monitoring register
DEF_SETIRQCPR(set_EVTSEL, p14, 0, c8, c1, 0)
// Set the INTEN performance monitoring register
DEF_SETIRQCPR(set_INTEN, p14, 0, c4, c1, 0)
// Set the PMNC performance monitoring register
DEF_SETIRQCPR(set_PMNC, p14, 0, c0, c1, 0)
// Set the DBR0 software debug register
DEF_SETIRQCPR(set_DBR0, p15, 0, c14, c0, 0)
// Set the DBR1 software debug register
DEF_SETIRQCPR(set_DBR1, p15, 0, c14, c3, 0)
// Set the DCSR software debug register
DEF_SETIRQCPR(set_DCSR, p14, 0, c10, c0, 0)
// Get the FSR software debug register
DEF_GETIRQCPR(get_FSR, p15, 0, c5, c0, 0)

// Enable CPU registers to catch insns and memory accesses
static void __irq
startPXAtraps(struct irqData *data)
{
    if (! data->isPXA)
        return;
    // Enable performance monitor
    set_EVTSEL(0xffffffff);  // Disable explicit event counts
    set_INTEN(0);  // Don't use interrupts
    set_PMNC(0xf);  // Enable performance monitor; clear counter
    // Enable software debug
    if (data->dbcon || data->insns[0].addr1 != 0xFFFFFFFF) {
        set_DBCON(0);  // Clear DBCON
        set_DBR0(data->dbr0);
        set_DBR1(data->dbr1);
        set_DBCON(data->dbcon);
        set_DCSR(1<<31); // Global enable bit
        if (data->insns[0].addr1 != 0xFFFFFFFF)
            set_IBCR0(data->insns[0].addr1 | 0x01);
        if (data->insns[1].addr1 != 0xFFFFFFFF)
            set_IBCR1(data->insns[1].addr1 | 0x01);
    }
}

static void
report_winceResume(uint32 msecs, traceitem *item)
{
    Output("%06d: %08x: cpu resumed", msecs, 0);
}

static void
report_irq(uint32 msecs, traceitem *item)
{
    uint32 clock = item->d0, irq = item->d1;
    if (irq >= START_GPIO_IRQS)
        Output("%06d: %08x: irq %d(gpio %d)"
               , msecs, clock, irq, irq-START_GPIO_IRQS);
    else
        Output("%06d: %08x: irq %d(%s)"
               , msecs, clock, irq, Mach->getIrqName(irq));
}

static void __irq
PXA_irq_handler(struct irqData *data, struct irqregs *regs)
{
    uint32 clock = get_CCNT();

    if (get_DBCON() != data->dbcon) {
        // Performance counter not running - reenable.
        add_trace(data, report_winceResume);
        startPXAtraps(data);
        clock = 0;
    }

    set_DBCON(0);
    uint32 irqs[2] = {
        (*(uint32*)&data->irq_ctrl[IRQ_ICIP_OFFSET]
         & *(uint32*)&data->irq_ctrl[IRQ_ICMR_OFFSET]),
        (*(uint32*)&data->irq_ctrl[IRQ_ICIP2_OFFSET]
         & *(uint32*)&data->irq_ctrl[IRQ_ICMR2_OFFSET])};
    for (int i=0; i<START_GPIO_IRQS; i++)
        if (TESTBIT(irqs, i) && !TESTBIT(data->ignoredIrqs, i))
            add_trace(data, report_irq, clock, i);
    if (irqs[0] & 0x400 && data->demuxGPIOirq) {
        // Gpio activity
        uint32 gpio_irqs[4] = {
            *(uint32*)&data->gpio_ctrl[GPIO_GEDR0_OFFSET],
            *(uint32*)&data->gpio_ctrl[GPIO_GEDR1_OFFSET],
            *(uint32*)&data->gpio_ctrl[GPIO_GEDR2_OFFSET],
            *(uint32*)&data->gpio_ctrl[GPIO_GEDR3_OFFSET]};
        for (int i=0; i<120; i++)
            if (TESTBIT(gpio_irqs, i) && !TESTBIT(data->ignoredIrqs
                                                  , START_GPIO_IRQS+i))
                add_trace(data, report_irq, clock, START_GPIO_IRQS+i);
    }

    // Irq time memory polling.
    checkPolls(data, clock, data->irqpolls, data->irqpollcount);
    // Trace time memory polling.
    checkPolls(data, clock, data->tracepolls, data->tracepollcount);
    set_DBCON(data->dbcon);
}


static void
report_memAccess(uint32 msecs, traceitem *item)
{
    uint32 clock=item->d0, pc=item->d1, insn=item->d2, Rd=item->d3, Rn=item->d4;
    Output("%06d: %08x: debug %08x: %08x(%s) %08x %08x"
           , msecs, clock, pc, insn, getInsnName(insn), Rd, Rn);
}

// Code that handles memory access events.
static int __irq
PXA_abort_handler(struct irqData *data, struct irqregs *regs)
{
    if ((get_FSR() & (1<<9)) == 0)
        // Not a debug trace event
        return 0;

    uint32 clock = get_CCNT();
    data->abortCount++;

    // Trace time memory polling.
    set_DBCON(0);
    int count = checkPolls(data, clock, data->tracepolls, data->tracepollcount);
    set_DBCON(data->dbcon);

    if (data->traceForWatch && !count)
        return 1;

    uint32 old_pc = transPC(regs->old_pc - 8);
    uint32 ignoreCount = data->ignoreAddr[0];
    for (uint32 i=1; i<ignoreCount; i++) {
        if (old_pc == data->ignoreAddr[i])
            // This address is being ignored.
            return 1;
    }

    extraregs er;
    er.didfetch = 0;
    uint32 insn = *(uint32*)old_pc;
    add_trace(data, report_memAccess, clock, old_pc, insn
              , getReg(regs, &er, mask_Rd(insn))
              , getReg(regs, &er, mask_Rn(insn)));
    return 1;
}

static void
report_insnTrace(uint32 msecs, traceitem *item)
{
    uint32 clock=item->d0, pc=item->d1, reg1=item->d2, reg2=item->d3;
    Output("%06d: %08x: insn %08x: %08x %08x"
           , msecs, clock, pc, reg1, reg2);
}

// Code that handles instruction breakpoint events.
static int __irq
PXA_prefetch_handler(struct irqData *data, struct irqregs *regs)
{
    if ((get_FSR() & (1<<9)) == 0)
        // Not a debug trace event
        return 0;

    uint32 clock = get_CCNT();
    data->prefetchCount++;

    uint32 old_pc = transPC(regs->old_pc-4);
    struct irqData::insn_s *idata = &data->insns[0];
    if (idata->addr1 == old_pc) {
        // Match on breakpoint.  Setup to single step next time.
        set_IBCR0(idata->addr2 | 0x01);
    } else if (idata->addr2 == old_pc) {
        // Called after single stepping - reset breakpoint.
        set_IBCR0(idata->addr1 | 0x01);
    } else {
        idata = &data->insns[1];
        if (idata->addr1 == old_pc) {
            // Match on breakpoint.  Setup to single step next time.
            set_IBCR1(idata->addr2 | 0x01);
        } else if (idata->addr2 == old_pc) {
            // Called after single stepping - reset breakpoint.
            set_IBCR1(idata->addr1 | 0x01);
        } else {
            // Huh?!  Got breakpoint for address not watched.
            data->errors++;
            set_IBCR0(0);
            set_IBCR1(0);
        }
    }
    extraregs er;
    er.didfetch = 0;
    add_trace(data, report_insnTrace, clock, old_pc
              , getReg(regs, &er, idata->reg1)
              , getReg(regs, &er, idata->reg2));

    // Trace time memory polling.
    set_DBCON(0);
    checkPolls(data, clock, data->tracepolls, data->tracepollcount);
    set_DBCON(data->dbcon);
    return 1;
}

// Reset CPU registers that conrol software debug / performance monitoring
static void
stopPXAtraps(struct irqData *data)
{
    if (! data->isPXA)
        return;
    // Disable software debug
    set_IBCR0(0);
    set_IBCR1(0);
    set_DBCON(0);
    set_DCSR(0);
    // Disable performance monitor
    set_PMNC(0);
}

// Commands and variables are only applicable if AllocPhysMem is
// available and if this is a PXA based pda.
static int testPXAAvail() {
    return testAvail() && testPXA();
}

// Mask of ignored interrupts (set in script.cpp)
static uint32 irqIgnore[BITMAPSIZE(MAX_IRQ)];
static uint32 irqDemuxGPIO = 1;
static uint32 irqIgnoreAddr[MAX_IGNOREADDR];
static uint32 traceForWatch;

REG_VAR_BITSET(testPXAAvail, "II", irqIgnore, MAX_IRQ,
               "The list of interrupts to ignore during WI")
REG_VAR_INT(testPXAAvail, "IRQGPIO", irqDemuxGPIO,
            "Turns on/off interrupt handler gpio irq demuxing")
REG_VAR_INTLIST(testPXAAvail, "TRACEIGNORE", irqIgnoreAddr, MAX_IGNOREADDR,
                "List of pc addresses to ignore when tracing")
REG_VAR_INT(testPXAAvail, "TRACEFORWATCH", traceForWatch,
            "Only report memory trace if ADDTRACEWATCH poll succeeds")

// Externally modifiable settings for software debug
static uint32 irqTrace = 0xFFFFFFFF;
static uint32 irqTraceMask = 0;
static uint32 irqTrace2 = 0xFFFFFFFF;
static uint32 irqTraceType = 2;
static uint32 irqTrace2Type = 2;

REG_VAR_INT(testPXAAvail, "TRACE", irqTrace,
            "Memory location to trace during WI")
REG_VAR_INT(testPXAAvail, "TRACEMASK", irqTraceMask,
            "Memory location mask to apply to TRACE during WI")
REG_VAR_INT(testPXAAvail, "TRACE2", irqTrace2,
            "Second memory location to trace during WI (only if no mask)")
REG_VAR_INT(testPXAAvail, "TRACETYPE", irqTraceType,
            "1=store only, 2=loads or stores, 3=loads only")
REG_VAR_INT(testPXAAvail, "TRACE2TYPE", irqTrace2Type,
            "1=store only, 2=loads or stores, 3=loads only")

// Externally modifiable settings for software tracing
static uint32 insnTrace = 0xFFFFFFFF, insnTraceReenable = 0xFFFFFFFF;
static uint32 insnTraceReg1 = 0, insnTraceReg2 = 1;
static uint32 insnTrace2 = 0xFFFFFFFF, insnTrace2Reenable = 0xFFFFFFFF;
static uint32 insnTrace2Reg1 = 0, insnTrace2Reg2 = 1;

REG_VAR_INT(testPXAAvail, "INSN", insnTrace,
            "Instruction address to monitor during WI")
REG_VAR_INT(testPXAAvail, "INSNREENABLE", insnTraceReenable,
            "Instruction address to reenable breakpoint after INSN")
REG_VAR_INT(testPXAAvail, "INSNREG1", insnTraceReg1,
            "Register to report during INSN breakpoint")
REG_VAR_INT(testPXAAvail, "INSNREG2", insnTraceReg2,
            "Second register to report during INSN breakpoint")
REG_VAR_INT(testPXAAvail, "INSN2", insnTrace2,
            "Second instruction address to monitor during WI")
REG_VAR_INT(testPXAAvail, "INSN2REENABLE", insnTrace2Reenable,
            "Instruction address to reenable breakpoint after INSN2")
REG_VAR_INT(testPXAAvail, "INSN2REG1", insnTrace2Reg1,
            "Register to report during INSN2 breakpoint")
REG_VAR_INT(testPXAAvail, "INSN2REG2", insnTrace2Reg2,
            "Second register to report during INSN2 breakpoint")

#define mask_DBCON_E0(val) (((val) & (0x3))<<0)
#define mask_DBCON_E1(val) (((val) & (0x3))<<2)
#define DBCON_MASKBIT (1<<8)

static void
prepPXAtraps(struct irqData *data)
{
    data->isPXA = testPXA();
    if (! data->isPXA)
        return;
    // Check for software debug data watch points.
    if (irqTrace != 0xFFFFFFFF) {
        data->dbr0 = irqTrace;
        data->dbcon |= mask_DBCON_E0(irqTraceType);
        if (irqTraceMask) {
            data->dbr1 = irqTraceMask;
            data->dbcon |= DBCON_MASKBIT;
        } else if (irqTrace2 != 0xFFFFFFFF) {
            data->dbr1 = irqTrace2;
            data->dbcon |= mask_DBCON_E1(irqTrace2Type);
        }
    }

    // Setup instruction trace registers
    data->insns[0].addr1 = insnTrace;
    if (insnTraceReenable == 0xffffffff)
        data->insns[0].addr2 = insnTrace+4;
    else
        data->insns[0].addr2 = insnTraceReenable;
    data->insns[0].reg1 = insnTraceReg1;
    data->insns[0].reg2 = insnTraceReg2;
    data->insns[1].addr1 = insnTrace2;
    if (insnTrace2Reenable == 0xffffffff)
        data->insns[1].addr2 = insnTrace2+4;
    else
        data->insns[1].addr2 = insnTrace2Reenable;
    data->insns[1].reg1 = insnTrace2Reg1;
    data->insns[1].reg2 = insnTrace2Reg2;

    if (insnTrace != 0xFFFFFFFF || irqTrace != 0xFFFFFFFF) {
        Output("Will set memory tracing to:%08x %08x %08x %08x %08x"
               , data->dbr0, data->dbr1, data->dbcon
               , irqTrace, irqTrace2);
        Output("Will set software debug to:%08x->%08x %08x->%08x"
               , data->insns[0].addr1, data->insns[0].addr2
               , data->insns[1].addr1, data->insns[1].addr2);
    }

    data->gpio_ctrl = memPhysMap(GPIO_OFFSET);
    data->irq_ctrl = memPhysMap(IRQ_OFFSET);
    memcpy(data->ignoredIrqs, irqIgnore, sizeof(irqIgnore));
    data->demuxGPIOirq = irqDemuxGPIO;
    memcpy(data->ignoreAddr, irqIgnoreAddr, sizeof(data->ignoreAddr));
    data->traceForWatch = traceForWatch;
}


/****************************************************************
 * Binding of "chained" irq handler
 ****************************************************************/

// Layout of memory in physically continuous ram.
static const int IRQ_STACK_SIZE = 4096;
struct irqChainCode {
    // Stack for C prefetch code.
    char stack_prefetch[IRQ_STACK_SIZE];
    // Stack for C abort code.
    char stack_abort[IRQ_STACK_SIZE];
    // Stack for C irq code.
    char stack_irq[IRQ_STACK_SIZE];
    // Data for C code.
    struct irqData data;

    // Variable length array storing asm/C exception handler code.
    char cCode[1] PAGE_ALIGNED;
};

// Low level information to be passed to the assembler part of the
// chained exception handler.  DO NOT CHANGE HERE without also
// upgrading the assembler code.
struct irqAsmVars {
    // Modified Virtual Address of irqData data.
    uint32 dataMVA;
    // Standard WinCE interrupt handler.
    uint32 winceIrqHandler;
    // Standard WinCE abort handler.
    uint32 winceAbortHandler;
    // Standard WinCE prefetch handler.
    uint32 wincePrefetchHandler;
};

extern "C" {
    // Symbols added by linker.
    extern char irq_start;
    extern char irq_end;

    // Assembler linkage.
    extern char asmIrqVars;
    extern void irq_chained_handler();
    extern void abort_chained_handler();
    extern void prefetch_chained_handler();
}
#define offset_asmIrqVars() (&asmIrqVars - &irq_start)
#define offset_asmIrqHandler() ((char *)irq_chained_handler - &irq_start)
#define offset_asmAbortHandler() ((char *)abort_chained_handler - &irq_start)
#define offset_asmPrefetchHandler() ((char *)prefetch_chained_handler - &irq_start)
#define size_cHandlers() (&irq_end - &irq_start)
#define size_handlerCode() (uint)(&((irqChainCode*)0)->cCode[size_cHandlers()])

// The virtual address of the irq vector
static const uint32 VADDR_IRQTABLE=0xffff0000;
static const uint32 VADDR_PREFETCHOFFSET=0x0C;
static const uint32 VADDR_ABORTOFFSET=0x10;
static const uint32 VADDR_IRQOFFSET=0x18;

// Locate a WinCE exception handler.  This assumes the handler is
// setup in a manor that wince has been observed to do in the past.
static uint32 *
findWinCEirq(uint8 *irq_table, uint32 offset)
{
    uint32 irq_ins = *(uint32*)&irq_table[offset];
    if ((irq_ins & 0xfffff000) != 0xe59ff000) {
        // We only know how to handle LDR PC, #XXX instructions.
        Output(C_INFO "Unknown irq instruction %08x", irq_ins);
        return NULL;
    }
    uint32 ins_offset = (irq_ins & 0xFFF) + 8;
    //Output("Ins=%08x ins_offset=%08x", irq_ins, ins_offset);
    return (uint32 *)(&irq_table[offset + ins_offset]);
}

// Main "watch irq" command entry point.
static void
cmd_wirq(const char *cmd, const char *args)
{
    uint32 seconds;
    if (!get_expression(&args, &seconds)) {
        Output(C_ERROR "line %d: Expected <seconds>", ScriptLine);
        return;
    }

    // Map in the IRQ vector tables in a place that we can be sure
    // there is full read/write access to it.
    uint8 *irq_table = memPhysMap(memVirtToPhys(VADDR_IRQTABLE));

    // Locate position of wince exception handlers.
    uint32 *irq_loc = findWinCEirq(irq_table, VADDR_IRQOFFSET);
    if (!irq_loc)
        return;
    uint32 *abort_loc = findWinCEirq(irq_table, VADDR_ABORTOFFSET);
    if (!abort_loc)
        return;
    uint32 *prefetch_loc = findWinCEirq(irq_table, VADDR_PREFETCHOFFSET);
    if (!prefetch_loc)
        return;
    uint32 newIrqHandler, newAbortHandler, newPrefetchHandler;

    // Allocate space for the irq handlers in physically continuous ram.
    void *rawCode = 0;
    ulong dummy;
    rawCode = late_AllocPhysMem(size_handlerCode()
                                , PAGE_EXECUTE_READWRITE, 0, 0, &dummy);
    irqChainCode *code = (irqChainCode *)cachedMVA(rawCode);
    struct irqData *data = &code->data;
    struct irqAsmVars *asmVars = (irqAsmVars*)&code->cCode[offset_asmIrqVars()];
    if (!rawCode) {
        Output(C_INFO "Can't allocate memory for irq code");
        goto abort;
    }
    if (!code) {
        Output(C_INFO "Can't find vm addr of alloc'd physical ram.");
        goto abort;
    }
    memset(code, 0, size_handlerCode());

    // Copy the C handlers to alloc'd space.
    memcpy(code->cCode, &irq_start, size_cHandlers());

    // Setup the assembler links
    asmVars->dataMVA = (uint32)data;
    asmVars->winceIrqHandler = *irq_loc;
    asmVars->winceAbortHandler = *abort_loc;
    asmVars->wincePrefetchHandler = *prefetch_loc;
    newIrqHandler = (uint32)&code->cCode[offset_asmIrqHandler()];
    newAbortHandler = (uint32)&code->cCode[offset_asmAbortHandler()];
    newPrefetchHandler = (uint32)&code->cCode[offset_asmPrefetchHandler()];

    Output("irq:%08x@%p=%08x abort:%08x@%p=%08x"
           " prefetch:%08x@%p=%08x"
           " data=%08x sizes=c:%08x,t:%08x"
           , asmVars->winceIrqHandler, irq_loc, newIrqHandler
           , asmVars->winceAbortHandler, abort_loc, newAbortHandler
           , asmVars->wincePrefetchHandler, prefetch_loc, newPrefetchHandler
           , asmVars->dataMVA
           , size_cHandlers(), size_handlerCode());

    prepPXAtraps(data);

    preLoop(data);

    // Replace old handler with new handler.
    Output("Replacing windows exception handlers...");
    take_control();
    startPXAtraps(data);
    Mach->flushCache();
    *irq_loc = newIrqHandler;
    *abort_loc = newAbortHandler;
    *prefetch_loc = newPrefetchHandler;
    return_control();
    Output("Finished installing exception handlers.");

    // Loop till time up.
    mainLoop(data, seconds);

    // Restore wince handler.
    Output("Restoring windows exception handlers...");
    take_control();
    stopPXAtraps(data);
    Mach->flushCache();
    *irq_loc = asmVars->winceIrqHandler;
    *abort_loc = asmVars->winceAbortHandler;
    *prefetch_loc = asmVars->wincePrefetchHandler;
    return_control();
    Output("Finished restoring windows exception handlers.");

    postLoop(data);
abort:
    if (rawCode)
        late_FreePhysMem(rawCode);
}
REG_CMD(testAvail, "WI|RQ", cmd_wirq,
        "WIRQ <seconds>\n"
        "  Watch which IRQ occurs for some period of time and report them.")
