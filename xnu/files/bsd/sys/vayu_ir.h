/*
 * The IR blaster of the POCO X3 Pro (vayu, SM8150-AC): QUP wrapper 0 SE1, spi@884000
 * (registers at physical 0x00884000, 0x4000 bytes, vayu.dtb), driven as a raw MOSI bit
 * stream at 1.92 MHz with 32-bit words, bsd/dev/vayu_ir.c.
 *
 *   vayu_ir=1      map the SE, a read-only probe, then /dev/vayu-ir (0666)
 *
 * The loader owns the pins (GPIO 114-117, qup1, TLMM exception 7) and the SE clock
 * (GCC exception 7a, vayu_ir_clk=1, 19.2 MHz TCXO); XNU writes neither TLMM nor GCC.
 * A write() to /dev/vayu-ir is one SPI TX-only transfer: the bytes ARE the waveform.
 */
#ifndef _SYS_VAYU_IR_H_
#define _SYS_VAYU_IR_H_

void vayu_ir_init(void);                /* boot-arg vayu_ir=1; nothing at all without it */
void vayu_ir_quiesce(void);             /* at the run's end and on a panic: cancel/abort a transfer in flight */

#endif /* _SYS_VAYU_IR_H_ */
