#include <stdio.h>
#include <stdlib.h>

#include <proto/exec.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/interrupts.h>

#include <hardware/intbits.h>

#define DEBUG			1

#define CLOCKPORT_BASE		0xD80001
#define CLOCKPORT_STRIDE	4

#define I2CSTA			0
#define I2CTO			0
#define I2CDAT			2
#define I2CADR			1
#define I2CCON			3

#define I2CCON_CR0		(1 << 0)
#define I2CCON_CR1		(1 << 1)
#define I2CCON_CR2		(1 << 2)
#define I2CCON_CR_88KHZ		(0x4)
#define I2CCON_CR_59KHZ		(0x5)
#define I2CCON_CR_MASK		(0x7)
#define I2CCON_SI		(1 << 3)
#define I2CCON_STO		(1 << 4)
#define I2CCON_STA		(1 << 5)
#define I2CCON_ENSIO		(1 << 6)
#define I2CCON_AA		(1 << 7)

#define I2CSTA_START_SENT	0x08
#define I2CSTA_SLAR_TX_ACK_RX	0x40
#define I2CSTA_SLAR_TX_NACK_RX	0x48
#define I2CSTA_DATA_RX_ACK_TX	0x50
#define I2CSTA_DATA_RX_NACK_TX	0x58

#define I2CSTA_IDLE		0xF8

#pragma dontwarn 113

typedef enum { 
	OP_NOP,	
	OP_READ 
} op_t;

/* glorious god object that holds the state of everything in this program; tldr */
typedef struct {
	op_t cur_op; 

	UBYTE *cp;

	BYTE sig_intr;
	LONG sigmask_intr;
	struct Task *MainTask;

	UBYTE slave_addr; 
	UBYTE bytes_left;
#ifdef DEBUG
	int isr_called; /* how may times ISR was called */
	BOOL in_isr;
#endif /* DEBUG */
} pca9564_state_t;

UBYTE clockport_read(pca9564_state_t *, UBYTE);
void clockport_write(pca9564_state_t *, UBYTE, UBYTE);
__amigainterrupt void pca9564_isr(pca9564_state_t *);
void pca9564_dump_state(pca9564_state_t *);
void pca9564_send_start(pca9564_state_t *);
/*void pca9564_send_stop(void); XXX */
void pca9564_read_1(pca9564_state_t *, UBYTE);

UBYTE
clockport_read(pca9564_state_t *sp, UBYTE reg) 
{
	UBYTE v;
	UBYTE *ptr;

	ptr = sp->cp + (reg * CLOCKPORT_STRIDE);
	v = *ptr;
#ifdef DEBUG 
	if (!(sp->in_isr))
		printf("DEBUG: read %x from %p\n", (int) v, (void*) ptr);
#endif /* DEBUG */

	return v;
}

void
clockport_write(pca9564_state_t *sp, UBYTE reg, UBYTE value)
{
	UBYTE *ptr;

	ptr = (sp->cp) + (reg * CLOCKPORT_STRIDE);
#ifdef DEBUG 
	if (!(sp->in_isr))
		printf("DEBUG: write %x to %p\n", (int) value, (void*) ptr);
#endif /* DEBUG */

	*ptr = value;
}

int main(void)
{
	pca9564_state_t sc;
	struct Interrupt *int6;

	UBYTE ctrl;

#ifdef DEBUG
	sc.in_isr = FALSE;
	sc.isr_called = 0;
#endif /* DEBUG */
	sc.cp = CLOCKPORT_BASE;
	sc.cur_op = OP_NOP;

	sc.sig_intr = -1;
	if ((sc.sig_intr = AllocSignal(-1)) == -1) {
		printf("Couldn't allocate signal\n");
		return 1;
	}
	sc.sigmask_intr = 1L << sc.sig_intr;

	sc.MainTask = FindTask(NULL);

	if (int6 = AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC|MEMF_CLEAR)) {
		int6->is_Node.ln_Type = NT_INTERRUPT;
		int6->is_Node.ln_Pri = -60;
		int6->is_Node.ln_Name = "PCA9564";
		int6->is_Data = (APTR)&sc;
		int6->is_Code = pca9564_isr;

		AddIntServer(INTB_EXTER, int6); 
	} else {
		printf("Can't allocate memory for interrupt node\n");
		FreeSignal(sc.sig_intr);
		return 1;
	}

	/* init the host controller */
	ctrl = I2CCON_CR_59KHZ | I2CCON_ENSIO;
	clockport_write(&sc, I2CCON, ctrl);
	Delay(50);

	pca9564_read_1(&sc, 0x48); 	/* XXX */

	ctrl = 0;
	clockport_write(&sc, I2CCON, ctrl);

	RemIntServer(INTB_EXTER, int6);
	FreeMem(int6, sizeof(struct Interrupt));
	FreeSignal(sc.sig_intr);

	printf("ISR was called %d times\n", sc.isr_called);
    
    return 0;
}

void
pca9564_read_1(pca9564_state_t *sp, UBYTE address)
{
	/*assert(cur_op == OP_NOP);
	assert(address > 1);*/

	sp->cur_op = OP_READ;
	sp->slave_addr = address;
	sp->bytes_left = 1;

	printf("gonna send start\n");
	pca9564_send_start(sp);

	Wait(sp->sigmask_intr);

	Delay(10);
	pca9564_dump_state(sp);
/*
	printf("gonna send stop\n");
	pca9564_send_stop();
	pca9564_dump_state();
*/

	sp->slave_addr = 0;
	sp->cur_op = OP_NOP;
}

/*void
pca9564_send_stop(void)
{
	UBYTE c;

	c = clockport_read(I2CCON);
	c |= I2CCON_STO;
	c &= (I2CCON_STA);
	clockport_write(I2CCON, c);	
}*/

void
pca9564_send_start(pca9564_state_t *sp) 
{
	UBYTE c;

	c = clockport_read(sp, I2CCON);
	c |= I2CCON_STA|I2CCON_AA;
	clockport_write(sp, I2CCON, c);	/* send START condition */

}

void
pca9564_dump_state(pca9564_state_t *sp)
{
	UBYTE c, s, d;

	c = clockport_read(sp, I2CCON);
	s = clockport_read(sp, I2CSTA);
	d = clockport_read(sp, I2CDAT);
	printf("I2CCON: %x, I2CSTA: %x, I2CDAT: %x\n", c, s, d);

}

/* Interrupt service routine. */
__amigainterrupt void
pca9564_isr(__reg("a1") pca9564_state_t *sp) 
{
	UBYTE v;

	sp->in_isr = TRUE;
	sp->isr_called++; 

	if (!(clockport_read(sp, I2CCON) & I2CCON_SI)) {
		sp->in_isr = FALSE;
		return;
	}

	switch (sp->cur_op) {
	case OP_READ:
		switch (clockport_read(sp, I2CSTA)) {
		case I2CSTA_START_SENT:		/* 0x08 */
			clockport_write(sp, I2CDAT, (sp->slave_addr << 1) | 1);
			v = clockport_read(sp, I2CCON);
			v &= ~(I2CCON_SI|I2CCON_STA); 
			clockport_write(sp, I2CCON, v);
			break;
		case I2CSTA_SLAR_TX_ACK_RX:	/* 0x40 */
			v = clockport_read(sp, I2CCON);
			v &= ~(I2CCON_SI|I2CCON_AA); /*XXX: last byte */
			clockport_write(sp, I2CCON, v);
			break;
		case I2CSTA_DATA_RX_NACK_TX:	/* 0x58 */
			v = clockport_read(sp, I2CCON);
			v &= ~(I2CCON_SI);
			v |= (I2CCON_AA|I2CCON_STO);
			clockport_write(sp, I2CCON, v);
			Signal(sp->MainTask, sp->sigmask_intr);
			break;
		default:
			/* insert error handler here */
			clockport_write(sp, I2CCON, 0);
			break;
		}
		break;
	case OP_NOP:
		/* insert error handler here */
		clockport_write(sp, I2CCON, 0);
		break;
	}

	sp->in_isr = FALSE;
}

