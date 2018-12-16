/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

/*
 * SIO functions.
 */

#include <sys/stat.h>

#include "core/sio.h"

// Status Flags
#define TX_RDY 0x0001
#define RX_RDY 0x0002
#define TX_EMPTY 0x0004
#define PARITY_ERR 0x0008
#define RX_OVERRUN 0x0010
#define FRAMING_ERR 0x0020
#define SYNC_DETECT 0x0040
#define DSR 0x0080
#define CTS 0x0100
#define IRQ 0x0200

// Control Flags
#define TX_PERM 0x0001
#define DTR 0x0002
#define RX_PERM 0x0004
#define BREAK 0x0008
#define RESET_ERR 0x0010
#define RTS 0x0020
#define SIO_RESET 0x0040

// MCD flags
#define MCDST_CHANGED 0x08

// *** FOR WORKS ON PADS AND MEMORY CARDS *****

void LoadDongle(const char *str);
void SaveDongle(const char *str);

#define BUFFER_SIZE 0x1010

static unsigned char s_buf[BUFFER_SIZE];

//[0] -> dummy
//[1] -> memory card status flag
//[2] -> card 1 id, 0x5a->plugged, any other not plugged
//[3] -> card 2 id, 0x5d->plugged, any other not plugged
unsigned char s_cardh[4] = {0x00, 0x08, 0x5a, 0x5d};

// Transfer Ready and the Buffer is Empty
// static unsigned short s_statReg = 0x002b;
static unsigned short s_statReg = TX_RDY | TX_EMPTY;
static unsigned short s_modeReg;
static unsigned short s_ctrlReg;
static unsigned short s_baudReg;

static unsigned int s_bufcount;
static unsigned int s_parp;
static unsigned int s_mcdst, s_rdwr;
static unsigned char s_adrH, s_adrL;
static unsigned int s_padst;
static unsigned int s_gsdonglest;

char g_mcd1Data[MCD_SIZE], g_mcd2Data[MCD_SIZE];

#define DONGLE_SIZE 0x40 * 0x1000

unsigned int s_dongleBank;
unsigned char s_dongleData[DONGLE_SIZE];
static int s_dongleInit;

#if 0
// Breaks Twisted Metal 2 intro
#define SIO_INT(eCycle)                                          \
    {                                                            \
        if (!g_config.SioIrq) {                                    \
            g_psxRegs.interrupt |= (1 << PSXINT_SIO);              \
            g_psxRegs.intCycle[PSXINT_SIO].cycle = eCycle;         \
            g_psxRegs.intCycle[PSXINT_SIO].sCycle = g_psxRegs.cycle; \
        }                                                        \
                                                                 \
        s_statReg &= ~RX_RDY;                                      \
        s_statReg &= ~TX_RDY;                                      \
    }
#endif

#define SIO_INT(eCycle)                                          \
    {                                                            \
        if (!g_config.SioIrq) {                                    \
            g_psxRegs.interrupt |= (1 << PSXINT_SIO);              \
            g_psxRegs.intCycle[PSXINT_SIO].cycle = eCycle;         \
            g_psxRegs.intCycle[PSXINT_SIO].sCycle = g_psxRegs.cycle; \
        }                                                        \
    }

// clk cycle byte
// 4us * 8bits = (PSXCLK / 1000000) * 32; (linuzappz)
// TODO: add SioModePrescaler
#define SIO_CYCLES (s_baudReg * 8)

// rely on this for now - someone's actual testing
//#define SIO_CYCLES (PSXCLK / 57600)
// PCSX 1.9.91
//#define SIO_CYCLES 200
// PCSX 1.9.91
//#define SIO_CYCLES 270
// ePSXe 1.6.0
//#define SIO_CYCLES		535
// ePSXe 1.7.0
//#define SIO_CYCLES 635

unsigned char reverse_8(unsigned char bits) {
    unsigned char tmp;
    int lcv;

    tmp = 0;
    for (lcv = 0; lcv < 8; lcv++) {
        tmp >>= 1;
        tmp |= (bits & 0x80);

        bits <<= 1;
    }

    return tmp;
}

void sioWrite8(unsigned char value) {
#ifdef PAD_LOG
    PAD_LOG("sio write8 %x (PAR:%x PAD:%x MCDL%x)\n", value, s_parp, s_padst, s_mcdst);
#endif
    switch (s_padst) {
        case 1:
            SIO_INT(SIO_CYCLES);
            /*
            $41-4F
            $41 = Find bits in poll respones
            $42 = Polling command
            $43 = Config mode (Dual shock?)
            $44 = Digital / Analog (after $F3)
            $45 = Get status info (Dual shock?)

            ID:
            $41 = Digital
            $73 = Analogue Red LED
            $53 = Analogue Green LED

            $23 = NegCon
            $12 = Mouse
            */

            if ((value & 0x40) == 0x40) {
                s_padst = 2;
                s_parp = 1;
                if (!g_config.UseNet) {
                    switch (s_ctrlReg & 0x2002) {
                        case 0x0002:
                            s_buf[s_parp] = PAD1_poll(value);
                            break;
                        case 0x2002:
                            s_buf[s_parp] = PAD2_poll(value);
                            break;
                    }
                } /* else {
//					SysPrintf("%x: %x, %x, %x, %x\n", s_ctrlReg&0x2002, s_buf[2], s_buf[3], s_buf[4],
s_buf[5]);
                 }*/

                if (!(s_buf[s_parp] & 0x0f)) {
                    s_bufcount = 2 + 32;
                } else {
                    s_bufcount = 2 + (s_buf[s_parp] & 0x0f) * 2;
                }

                // Digital / Dual Shock Controller
                if (s_buf[s_parp] == 0x41) {
                    switch (value) {
                        // enter config mode
                        case 0x43:
                            s_buf[1] = 0x43;
                            break;

                        // get status
                        case 0x45:
                            s_buf[1] = 0xf3;
                            break;
                    }
                }

                // NegCon - Wipeout 3
                if (s_buf[s_parp] == 0x23) {
                    switch (value) {
                        // enter config mode
                        case 0x43:
                            s_buf[1] = 0x79;
                            break;

                        // get status
                        case 0x45:
                            s_buf[1] = 0xf3;
                            break;
                    }
                }
            } else
                s_padst = 0;
            return;
        case 2:
            s_parp++;
            /*			if (s_buf[1] == 0x45) {
                                            s_buf[s_parp] = 0;
                                            SIO_INT(SIO_CYCLES);
                                            return;
                                    }*/
            if (!g_config.UseNet) {
                switch (s_ctrlReg & 0x2002) {
                    case 0x0002:
                        s_buf[s_parp] = PAD1_poll(value);
                        break;
                    case 0x2002:
                        s_buf[s_parp] = PAD2_poll(value);
                        break;
                }
            }

            if (s_parp == s_bufcount) {
                s_padst = 0;
                return;
            }
            SIO_INT(SIO_CYCLES);
            return;
    }

    switch (s_mcdst) {
        case 1:
            SIO_INT(SIO_CYCLES);
            if (s_rdwr) {
                s_parp++;
                return;
            }
            s_parp = 1;
            switch (value) {
                case 0x52:
                    s_rdwr = 1;
                    break;
                case 0x57:
                    s_rdwr = 2;
                    break;
                default:
                    s_mcdst = 0;
            }
            return;
        case 2:  // address H
            SIO_INT(SIO_CYCLES);
            s_adrH = value;
            *s_buf = 0;
            s_parp = 0;
            s_bufcount = 1;
            s_mcdst = 3;
            return;
        case 3:  // address L
            SIO_INT(SIO_CYCLES);
            s_adrL = value;
            *s_buf = s_adrH;
            s_parp = 0;
            s_bufcount = 1;
            s_mcdst = 4;
            return;
        case 4:
            SIO_INT(SIO_CYCLES);
            s_parp = 0;
            switch (s_rdwr) {
                case 1:  // read
                    s_buf[0] = 0x5c;
                    s_buf[1] = 0x5d;
                    s_buf[2] = s_adrH;
                    s_buf[3] = s_adrL;
                    switch (s_ctrlReg & 0x2002) {
                        case 0x0002:
                            memcpy(&s_buf[4], g_mcd1Data + (s_adrL | (s_adrH << 8)) * 128, 128);
                            break;
                        case 0x2002:
                            memcpy(&s_buf[4], g_mcd2Data + (s_adrL | (s_adrH << 8)) * 128, 128);
                            break;
                    }
                    {
                        char xorsum = 0;
                        int i;
                        for (i = 2; i < 128 + 4; i++) xorsum ^= s_buf[i];
                        s_buf[132] = xorsum;
                    }
                    s_buf[133] = 0x47;
                    s_bufcount = 133;
                    break;
                case 2:  // write
                    s_buf[0] = s_adrL;
                    s_buf[1] = value;
                    s_buf[129] = 0x5c;
                    s_buf[130] = 0x5d;
                    s_buf[131] = 0x47;
                    s_bufcount = 131;
                    s_cardh[1] &= ~MCDST_CHANGED;
                    break;
            }
            s_mcdst = 5;
            return;
        case 5:
            s_parp++;
            if (s_rdwr == 2) {
                if (s_parp < 128) s_buf[s_parp + 1] = value;
            }
            SIO_INT(SIO_CYCLES);
            return;
    }

    /*
    GameShark CDX

    ae - be - ef - 04 + [00]
    ae - be - ef - 01 + 00 + [00] * $1000
    ae - be - ef - 01 + 42 + [00] * $1000
    ae - be - ef - 03 + 01,01,1f,e3,85,ae,d1,28 + [00] * 4
    */
    switch (s_gsdonglest) {
        // main command loop
        case 1:
            SIO_INT(SIO_CYCLES);

            // GS CDX
            // - unknown output

            // reset device when fail?
            if (value == 0xae) {
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;
            }

            // GS CDX
            else if (value == 0xbe) {
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;

                s_buf[0] = reverse_8(0xde);
            }

            // GS CDX
            else if (value == 0xef) {
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;

                s_buf[0] = reverse_8(0xad);
            }

            // GS CDX [1 in + $1000 out + $1 out]
            else if (value == 0x01) {
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;

                // $00 = 0000 0000
                // - (reverse) 0000 0000
                s_buf[0] = 0x00;
                s_gsdonglest = 2;
            }

            // GS CDX [1 in + $1000 in + $1 out]
            else if (value == 0x02) {
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;

                // $00 = 0000 0000
                // - (reverse) 0000 0000
                s_buf[0] = 0x00;
                s_gsdonglest = 3;
            }

            // GS CDX [8 in, 4 out]
            else if (value == 0x03) {
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;

                // $00 = 0000 0000
                // - (reverse) 0000 0000
                s_buf[0] = 0x00;

                s_gsdonglest = 4;
            }

            // GS CDX [out 1]
            else if (value == 0x04) {
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;

                // $00 = 0000 0000
                // - (reverse) 0000 0000
                s_buf[0] = 0x00;
                s_gsdonglest = 5;
            } else {
                // ERROR!!
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;
                s_buf[0] = 0xff;

                s_gsdonglest = 0;
            }

            return;

        // be - ef - 01
        case 2: {
            unsigned char checksum;
            unsigned int lcv;

            SIO_INT(SIO_CYCLES);
            s_statReg |= RX_RDY;

            // read 1 byte
            s_dongleBank = s_buf[0];

            // write data + checksum
            checksum = 0;
            for (lcv = 0; lcv < 0x1000; lcv++) {
                unsigned char data;

                data = s_dongleData[s_dongleBank * 0x1000 + lcv];

                s_buf[lcv + 1] = reverse_8(data);
                checksum += data;
            }

            s_parp = 0;
            s_bufcount = 0x1001;
            s_buf[0x1001] = reverse_8(checksum);

            s_gsdonglest = 255;
            return;
        }

        // be - ef - 02
        case 3:
            SIO_INT(SIO_CYCLES);
            s_statReg |= RX_RDY;

            // command start
            if (s_parp < 0x1000 + 1) {
                // read 1 byte
                s_buf[s_parp] = value;
                s_parp++;
            }

            if (s_parp == 0x1001) {
                unsigned char checksum;
                unsigned int lcv;

                s_dongleBank = s_buf[0];
                memcpy(s_dongleData + s_dongleBank * 0x1000, s_buf + 1, 0x1000);

                // save to file
                SaveDongle("memcards/CDX_Dongle.bin");

                // write 8-bit checksum
                checksum = 0;
                for (lcv = 1; lcv < 0x1001; lcv++) {
                    checksum += s_buf[lcv];
                }

                s_parp = 0;
                s_bufcount = 1;
                s_buf[1] = reverse_8(checksum);

                // flush result
                s_gsdonglest = 255;
            }
            return;

        // be - ef - 03
        case 4:
            SIO_INT(SIO_CYCLES);
            s_statReg |= RX_RDY;

            // command start
            if (s_parp < 8) {
                // read 2 (?,?) + 4 (DATA?) + 2 (CRC?)
                s_buf[s_parp] = value;
                s_parp++;
            }

            if (s_parp == 8) {
                // now write 4 bytes via -FOUR- $00 writes
                s_parp = 8;
                s_bufcount = 12;

                // TODO: Solve CDX algorithm

                // GS CDX [magic key]
                if (s_buf[2] == 0x12 && s_buf[3] == 0x34 && s_buf[4] == 0x56 && s_buf[5] == 0x78) {
                    s_buf[9] = reverse_8(0x3e);
                    s_buf[10] = reverse_8(0xa0);
                    s_buf[11] = reverse_8(0x40);
                    s_buf[12] = reverse_8(0x29);
                }

                // GS CDX [address key #2 = 6ec]
                else if (s_buf[2] == 0x1f && s_buf[3] == 0xe3 && s_buf[4] == 0x45 && s_buf[5] == 0x60) {
                    s_buf[9] = reverse_8(0xee);
                    s_buf[10] = reverse_8(0xdd);
                    s_buf[11] = reverse_8(0x71);
                    s_buf[12] = reverse_8(0xa8);
                }

                // GS CDX [address key #3 = ???]
                else if (s_buf[2] == 0x1f && s_buf[3] == 0xe3 && s_buf[4] == 0x72 && s_buf[5] == 0xe3) {
                    // unsolved!!

                    // Used here: 80090348 / 80090498

                    // dummy value - MSB
                    s_buf[9] = reverse_8(0xfa);
                    s_buf[10] = reverse_8(0xde);
                    s_buf[11] = reverse_8(0x21);
                    s_buf[12] = reverse_8(0x97);
                }

                // GS CDX [address key #4 = a00]
                else if (s_buf[2] == 0x1f && s_buf[3] == 0xe3 && s_buf[4] == 0x85 && s_buf[5] == 0xae) {
                    s_buf[9] = reverse_8(0xee);
                    s_buf[10] = reverse_8(0xdd);
                    s_buf[11] = reverse_8(0x7d);
                    s_buf[12] = reverse_8(0x44);
                }

                // GS CDX [address key #5 = 9ec]
                else if (s_buf[2] == 0x17 && s_buf[3] == 0xe3 && s_buf[4] == 0xb5 && s_buf[5] == 0x60) {
                    s_buf[9] = reverse_8(0xee);
                    s_buf[10] = reverse_8(0xdd);
                    s_buf[11] = reverse_8(0x7e);
                    s_buf[12] = reverse_8(0xa8);
                }

                else {
                    // dummy value - MSB
                    s_buf[9] = reverse_8(0xfa);
                    s_buf[10] = reverse_8(0xde);
                    s_buf[11] = reverse_8(0x21);
                    s_buf[12] = reverse_8(0x97);
                }

                // flush bytes -> done
                s_gsdonglest = 255;
            }
            return;

        // be - ef - 04
        case 5:
            if (value == 0x00) {
                SIO_INT(SIO_CYCLES);
                s_statReg |= RX_RDY;

                // read 1 byte
                s_parp = 0;
                s_bufcount = s_parp;

                // size of dongle card?
                s_buf[0] = reverse_8(DONGLE_SIZE / 0x1000);

                // done already
                s_gsdonglest = 0;
            }
            return;

        // flush bytes -> done
        case 255:
            if (value == 0x00) {
                // SIO_INT( SIO_CYCLES );
                SIO_INT(1);
                s_statReg |= RX_RDY;

                s_parp++;
                if (s_parp == s_bufcount) {
                    s_gsdonglest = 0;

#ifdef GSDONGLE_LOG
                    PAD_LOG("(gameshark dongle) DONE!!\n");
#endif
                }
            } else {
                // ERROR!!
                s_statReg |= RX_RDY;

                s_parp = 0;
                s_bufcount = s_parp;
                s_buf[0] = 0xff;

                s_gsdonglest = 0;
            }
            return;
    }

    switch (value) {
        case 0x01:              // start pad
            s_statReg |= RX_RDY;  // Transfer is Ready

            if (!g_config.UseNet) {
                switch (s_ctrlReg & 0x2002) {
                    case 0x0002:
                        s_buf[0] = PAD1_startPoll(1);
                        break;
                    case 0x2002:
                        s_buf[0] = PAD2_startPoll(2);
                        break;
                }
            } else {
                if ((s_ctrlReg & 0x2002) == 0x0002) {
                    int i, j;

                    PAD1_startPoll(1);
                    s_buf[0] = 0;
                    s_buf[1] = PAD1_poll(0x42);
                    if (!(s_buf[1] & 0x0f)) {
                        s_bufcount = 32;
                    } else {
                        s_bufcount = (s_buf[1] & 0x0f) * 2;
                    }
                    s_buf[2] = PAD1_poll(0);
                    i = 3;
                    j = s_bufcount;
                    while (j--) {
                        s_buf[i++] = PAD1_poll(0);
                    }
                    s_bufcount += 3;

                    if (NET_sendPadData(s_buf, s_bufcount) == -1) netError();

                    if (NET_recvPadData(s_buf, 1) == -1) netError();
                    if (NET_recvPadData(s_buf + 128, 2) == -1) netError();
                } else {
                    memcpy(s_buf, s_buf + 128, 32);
                }
            }

            s_bufcount = 2;
            s_parp = 0;
            s_padst = 1;
            SIO_INT(SIO_CYCLES);
            return;
        case 0x81:  // start memcard
                    // case 0x82: case 0x83: case 0x84: // Multitap memcard access
            s_statReg |= RX_RDY;

            // Chronicles of the Sword - no memcard = password options
            if (g_config.NoMemcard || (!g_config.Mcd1[0] && !g_config.Mcd2[0])) {
                memset(s_buf, 0x00, 4);
            } else {
                memcpy(s_buf, s_cardh, 4);
                if (!g_config.Mcd1[0]) s_buf[2] = 0;  // is card 1 plugged? (Codename Tenka)
                if (!g_config.Mcd2[0]) s_buf[3] = 0;  // is card 2 plugged?
            }

            s_parp = 0;
            s_bufcount = 3;
            s_mcdst = 1;
            s_rdwr = 0;
            SIO_INT(SIO_CYCLES);
            return;
        case 0xae:  // GameShark CDX - start dongle
            s_statReg |= RX_RDY;
            s_gsdonglest = 1;

            s_parp = 0;
            s_bufcount = s_parp;

            if (!s_dongleInit) {
                LoadDongle("memcards/CDX_Dongle.bin");

                s_dongleInit = 1;
            }

            SIO_INT(SIO_CYCLES);
            return;

        default:  // no hardware found
            s_statReg |= RX_RDY;
            return;
    }
}

void sioWriteStat16(unsigned short value) {}

void sioWriteMode16(unsigned short value) { s_modeReg = value; }

void sioWriteCtrl16(unsigned short value) {
#ifdef PAD_LOG
    PAD_LOG("sio ctrlwrite16 %x (PAR:%x PAD:%x MCD:%x)\n", value, s_parp, s_padst, s_mcdst);
#endif
    s_ctrlReg = value & ~RESET_ERR;
    if (value & RESET_ERR) s_statReg &= ~IRQ;
    if ((s_ctrlReg & SIO_RESET) || (!s_ctrlReg)) {
        s_padst = 0;
        s_mcdst = 0;
        s_parp = 0;
        s_statReg = TX_RDY | TX_EMPTY;
        g_psxRegs.interrupt &= ~(1 << PSXINT_SIO);
    }
}

void sioWriteBaud16(unsigned short value) { s_baudReg = value; }

unsigned char sioRead8() {
    unsigned char ret = 0;

    if ((s_statReg & RX_RDY) /* && (s_ctrlReg & RX_PERM)*/) {
        //		s_statReg &= ~RX_OVERRUN;
        ret = s_buf[s_parp];
        if (s_parp == s_bufcount) {
            s_statReg &= ~RX_RDY;  // Receive is not Ready now
            if (s_mcdst == 5) {
                s_mcdst = 0;
                if (s_rdwr == 2) {
                    switch (s_ctrlReg & 0x2002) {
                        case 0x0002:
                            memcpy(g_mcd1Data + (s_adrL | (s_adrH << 8)) * 128, &s_buf[1], 128);
                            SaveMcd(g_config.Mcd1, g_mcd1Data, (s_adrL | (s_adrH << 8)) * 128, 128);
                            break;
                        case 0x2002:
                            memcpy(g_mcd2Data + (s_adrL | (s_adrH << 8)) * 128, &s_buf[1], 128);
                            SaveMcd(g_config.Mcd2, g_mcd2Data, (s_adrL | (s_adrH << 8)) * 128, 128);
                            break;
                    }
                }
            }
            if (s_padst == 2) s_padst = 0;
            if (s_mcdst == 1) {
                s_mcdst = 2;
                s_statReg |= RX_RDY;
            }
        }
    }

#ifdef PAD_LOG
    PAD_LOG("sio read8 ;ret = %x (I:%x ST:%x BUF:(%x %x %x))\n", ret, s_parp, s_statReg, s_buf[s_parp > 0 ? s_parp - 1 : 0],
            s_buf[s_parp], s_buf[s_parp < BUFFER_SIZE - 1 ? s_parp + 1 : BUFFER_SIZE - 1]);
#endif
    return ret;
}

unsigned short sioReadStat16() {
    u16 hard;

    hard = s_statReg;

#if 0
	// wait for IRQ first
	if( g_psxRegs.interrupt & (1 << PSXINT_SIO) )
	{
		hard &= ~TX_RDY;
		hard &= ~RX_RDY;
		hard &= ~TX_EMPTY;
	}
#endif

    return hard;
}

unsigned short sioReadMode16() { return s_modeReg; }

unsigned short sioReadCtrl16() { return s_ctrlReg; }

unsigned short sioReadBaud16() { return s_baudReg; }

void netError() {
    // ClosePlugins();
    SysMessage("%s", _("Connection closed!\n"));

    g_cdromId[0] = '\0';
    g_cdromLabel[0] = '\0';

    SysRunGui();
}

void sioInterrupt() {
#ifdef PAD_LOG
    PAD_LOG("Sio Interrupt (CP0.Status = %x)\n", g_psxRegs.CP0.n.Status);
#endif
    //	SysPrintf("Sio Interrupt\n");
    s_statReg |= IRQ;
    psxHu32ref(0x1070) |= SWAPu32(0x80);

#if 0
	// Rhapsody: fixes input problems
	// Twisted Metal 2: breaks intro
	s_statReg |= TX_RDY;
	s_statReg |= RX_RDY;
#endif
}

void LoadMcd(int mcd, const char *str) {
    FILE *f;
    char *data = NULL;
    char filepath[MAXPATHLEN] = {'\0'};
    const char *apppath = GetAppPath();

    if (mcd == 1) data = g_mcd1Data;
    if (mcd == 2) data = g_mcd2Data;

    if (*str == 0) {
        SysPrintf(_("No memory card value was specified - card %i is not plugged.\n"), mcd);
        return;
    }

    // Getting full application path.
    memmove(filepath, apppath, strlen(apppath));
    strcat(filepath, str);

    f = fopen(filepath, "rb");
    if (f == NULL) {
        SysPrintf(_("The memory card %s doesn't exist - creating it\n"), filepath);
        CreateMcd(filepath);
        f = fopen(filepath, "rb");
        if (f != NULL) {
            struct stat buf;

            if (stat(filepath, &buf) != -1) {
                if (buf.st_size == MCD_SIZE + 64)
                    fseek(f, 64, SEEK_SET);
                else if (buf.st_size == MCD_SIZE + 3904)
                    fseek(f, 3904, SEEK_SET);
            }
            fread(data, 1, MCD_SIZE, f);
            fclose(f);
        } else
            SysMessage(_("Memory card %s failed to load!\n"), filepath);
    } else {
        struct stat buf;
        SysPrintf(_("Loading memory card %s\n"), filepath);
        if (stat(filepath, &buf) != -1) {
            if (buf.st_size == MCD_SIZE + 64)
                fseek(f, 64, SEEK_SET);
            else if (buf.st_size == MCD_SIZE + 3904)
                fseek(f, 3904, SEEK_SET);
        }
        fread(data, 1, MCD_SIZE, f);
        fclose(f);
    }

    // flag indicating entries have not yet been read (i.e. new card plugged)
    s_cardh[1] |= MCDST_CHANGED;
}

void LoadMcds(const char *mcd1, const char *mcd2) {
    LoadMcd(1, mcd1);
    LoadMcd(2, mcd2);
}

void SaveMcd(const char *mcd, const char *data, uint32_t adr, int size) {
    FILE *f;

    f = fopen(mcd, "r+b");
    if (f != NULL) {
        struct stat buf;

        if (stat(mcd, &buf) != -1) {
            if (buf.st_size == MCD_SIZE + 64)
                fseek(f, adr + 64, SEEK_SET);
            else if (buf.st_size == MCD_SIZE + 3904)
                fseek(f, adr + 3904, SEEK_SET);
            else
                fseek(f, adr, SEEK_SET);
        } else
            fseek(f, adr, SEEK_SET);

        fwrite(data + adr, 1, size, f);
        fclose(f);
        SysPrintf(_("Saving memory card %s\n"), mcd);
        return;
    }

#if 0
	// try to create it again if we can't open it
	f = fopen(mcd, "wb");
	if (f != NULL) {
		fwrite(data, 1, MCD_SIZE, f);
		fclose(f);
	}
#endif

    ConvertMcd(mcd, data);
}

void CreateMcd(const char *mcd) {
    FILE *f;
    struct stat buf;
    int s = MCD_SIZE;
    int i = 0, j;

    f = fopen(mcd, "wb");
    if (f == NULL) return;

    if (stat(mcd, &buf) != -1) {
        if ((buf.st_size == MCD_SIZE + 3904) || strstr(mcd, ".gme")) {
            s = s + 3904;
            fputc('1', f);
            s--;
            fputc('2', f);
            s--;
            fputc('3', f);
            s--;
            fputc('-', f);
            s--;
            fputc('4', f);
            s--;
            fputc('5', f);
            s--;
            fputc('6', f);
            s--;
            fputc('-', f);
            s--;
            fputc('S', f);
            s--;
            fputc('T', f);
            s--;
            fputc('D', f);
            s--;
            for (i = 0; i < 7; i++) {
                fputc(0, f);
                s--;
            }
            fputc(1, f);
            s--;
            fputc(0, f);
            s--;
            fputc(1, f);
            s--;
            fputc('M', f);
            s--;
            fputc('Q', f);
            s--;
            for (i = 0; i < 14; i++) {
                fputc(0xa0, f);
                s--;
            }
            fputc(0, f);
            s--;
            fputc(0xff, f);
            while (s-- > (MCD_SIZE + 1)) fputc(0, f);
        } else if ((buf.st_size == MCD_SIZE + 64) || strstr(mcd, ".mem") || strstr(mcd, ".vgs")) {
            s = s + 64;
            fputc('V', f);
            s--;
            fputc('g', f);
            s--;
            fputc('s', f);
            s--;
            fputc('M', f);
            s--;
            for (i = 0; i < 3; i++) {
                fputc(1, f);
                s--;
                fputc(0, f);
                s--;
                fputc(0, f);
                s--;
                fputc(0, f);
                s--;
            }
            fputc(0, f);
            s--;
            fputc(2, f);
            while (s-- > (MCD_SIZE + 1)) fputc(0, f);
        }
    }
    fputc('M', f);
    s--;
    fputc('C', f);
    s--;
    while (s-- > (MCD_SIZE - 127)) fputc(0, f);
    fputc(0xe, f);
    s--;

    for (i = 0; i < 15; i++) {  // 15 blocks
        fputc(0xa0, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        for (j = 0; j < 117; j++) {
            fputc(0x00, f);
            s--;
        }
        fputc(0xa0, f);
        s--;
    }

    for (i = 0; i < 20; i++) {
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0x00, f);
        s--;
        fputc(0xff, f);
        s--;
        fputc(0xff, f);
        s--;
        for (j = 0; j < 118; j++) {
            fputc(0x00, f);
            s--;
        }
    }

    while ((s--) >= 0) fputc(0, f);

    fclose(f);
}

void ConvertMcd(const char *mcd, const char *data) {
    FILE *f;
    int i = 0;
    int s = MCD_SIZE;

    if (strstr(mcd, ".gme")) {
        f = fopen(mcd, "wb");
        if (f != NULL) {
            fwrite(data - 3904, 1, MCD_SIZE + 3904, f);
            fclose(f);
        }
        f = fopen(mcd, "r+");
        s = s + 3904;
        fputc('1', f);
        s--;
        fputc('2', f);
        s--;
        fputc('3', f);
        s--;
        fputc('-', f);
        s--;
        fputc('4', f);
        s--;
        fputc('5', f);
        s--;
        fputc('6', f);
        s--;
        fputc('-', f);
        s--;
        fputc('S', f);
        s--;
        fputc('T', f);
        s--;
        fputc('D', f);
        s--;
        for (i = 0; i < 7; i++) {
            fputc(0, f);
            s--;
        }
        fputc(1, f);
        s--;
        fputc(0, f);
        s--;
        fputc(1, f);
        s--;
        fputc('M', f);
        s--;
        fputc('Q', f);
        s--;
        for (i = 0; i < 14; i++) {
            fputc(0xa0, f);
            s--;
        }
        fputc(0, f);
        s--;
        fputc(0xff, f);
        while (s-- > (MCD_SIZE + 1)) fputc(0, f);
        fclose(f);
    } else if (strstr(mcd, ".mem") || strstr(mcd, ".vgs")) {
        f = fopen(mcd, "wb");
        if (f != NULL) {
            fwrite(data - 64, 1, MCD_SIZE + 64, f);
            fclose(f);
        }
        f = fopen(mcd, "r+");
        s = s + 64;
        fputc('V', f);
        s--;
        fputc('g', f);
        s--;
        fputc('s', f);
        s--;
        fputc('M', f);
        s--;
        for (i = 0; i < 3; i++) {
            fputc(1, f);
            s--;
            fputc(0, f);
            s--;
            fputc(0, f);
            s--;
            fputc(0, f);
            s--;
        }
        fputc(0, f);
        s--;
        fputc(2, f);
        while (s-- > (MCD_SIZE + 1)) fputc(0, f);
        fclose(f);
    } else {
        f = fopen(mcd, "wb");
        if (f != NULL) {
            fwrite(data, 1, MCD_SIZE, f);
            fclose(f);
        }
    }
}

void GetMcdBlockInfo(int mcd, int block, McdBlock *Info) {
    char *data = NULL, *ptr, *str, *sstr;
    unsigned short clut[16];
    unsigned short c;
    int i, x;

    memset(Info, 0, sizeof(McdBlock));

    if (mcd == 1) data = g_mcd1Data;
    if (mcd == 2) data = g_mcd2Data;

    ptr = data + block * 8192 + 2;

    Info->IconCount = *ptr & 0x3;

    ptr += 2;

    x = 0;

    str = Info->Title;
    sstr = Info->sTitle;

    for (i = 0; i < 48; i++) {
        c = *(ptr) << 8;
        c |= *(ptr + 1);
        if (!c) break;

        // Convert ASCII characters to half-width
        if (c >= 0x8281 && c <= 0x829A)
            c = (c - 0x8281) + 'a';
        else if (c >= 0x824F && c <= 0x827A)
            c = (c - 0x824F) + '0';
        else if (c == 0x8140)
            c = ' ';
        else if (c == 0x8143)
            c = ',';
        else if (c == 0x8144)
            c = '.';
        else if (c == 0x8146)
            c = ':';
        else if (c == 0x8147)
            c = ';';
        else if (c == 0x8148)
            c = '?';
        else if (c == 0x8149)
            c = '!';
        else if (c == 0x815E)
            c = '/';
        else if (c == 0x8168)
            c = '"';
        else if (c == 0x8169)
            c = '(';
        else if (c == 0x816A)
            c = ')';
        else if (c == 0x816D)
            c = '[';
        else if (c == 0x816E)
            c = ']';
        else if (c == 0x817C)
            c = '-';
        else {
            str[i] = ' ';
            sstr[x++] = *ptr++;
            sstr[x++] = *ptr++;
            continue;
        }

        str[i] = sstr[x++] = c;
        ptr += 2;
    }

    trim(str);
    trim(sstr);

    ptr = data + block * 8192 + 0x60;  // icon palette data

    for (i = 0; i < 16; i++) {
        clut[i] = *((unsigned short *)ptr);
        ptr += 2;
    }

    for (i = 0; i < Info->IconCount; i++) {
        short *icon = &Info->Icon[i * 16 * 16];

        ptr = data + block * 8192 + 128 + 128 * i;  // icon data

        for (x = 0; x < 16 * 16; x++) {
            icon[x++] = clut[*ptr & 0xf];
            icon[x] = clut[*ptr >> 4];
            ptr++;
        }
    }

    ptr = data + block * 128;

    Info->Flags = *ptr;

    ptr += 0xa;
    strncpy(Info->ID, ptr, 12);
    ptr += 12;
    strncpy(Info->Name, ptr, 16);
}

int sioFreeze(gzFile f, int Mode) {
    gzfreeze(s_buf, sizeof(s_buf));
    gzfreeze(&s_statReg, sizeof(s_statReg));
    gzfreeze(&s_modeReg, sizeof(s_modeReg));
    gzfreeze(&s_ctrlReg, sizeof(s_ctrlReg));
    gzfreeze(&s_baudReg, sizeof(s_baudReg));
    gzfreeze(&s_bufcount, sizeof(s_bufcount));
    gzfreeze(&s_parp, sizeof(s_parp));
    gzfreeze(&s_mcdst, sizeof(s_mcdst));
    gzfreeze(&s_rdwr, sizeof(s_rdwr));
    gzfreeze(&s_adrH, sizeof(s_adrH));
    gzfreeze(&s_adrL, sizeof(s_adrL));
    gzfreeze(&s_padst, sizeof(s_padst));

    return 0;
}

void LoadDongle(const char *str) {
    FILE *f;

    f = fopen(str, "r+b");
    if (f != NULL) {
        fread(s_dongleData, 1, DONGLE_SIZE, f);
        fclose(f);
    } else {
        u32 *ptr, lcv;

        ptr = (unsigned int *)s_dongleData;

        // create temp data
        ptr[0] = (u32)0x02015447;
        ptr[1] = (u32)7;
        ptr[2] = (u32)1;
        ptr[3] = (u32)0;

        for (lcv = 4; lcv < 0x6c / 4; lcv++) {
            ptr[lcv] = 0;
        }

        ptr[lcv] = (u32)0x02000100;
        lcv++;

        while (lcv < 0x1000 / 4) {
            ptr[lcv] = (u32)0xffffffff;
            lcv++;
        }
    }
}

void SaveDongle(const char *str) {
    FILE *f;

    f = fopen(str, "wb");
    if (f != NULL) {
        fwrite(s_dongleData, 1, DONGLE_SIZE, f);
        fclose(f);
    }
}

void CALLBACK SIO1irq(void) { psxHu32ref(0x1070) |= SWAPu32(0x100); }