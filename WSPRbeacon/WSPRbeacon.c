///////////////////////////////////////////////////////////////////////////////
//
//  Roman Piksaykin [piksaykin@gmail.com], R2BDY
//  https://www.qrz.com/db/r2bdy
//
///////////////////////////////////////////////////////////////////////////////
//
//
//  WSPRbeacon.c - WSPR beacon - related functions.
// 
//  DESCRIPTION
//      The pico-WSPR-tx project provides WSPR beacon function using only
//  Pi Pico board. *NO* additional hardware such as freq.synth required.
//
//  HOWTOSTART
//  .
//
//  PLATFORM
//      Raspberry Pi pico.
//
//  REVISION HISTORY
// 
//      Rev 0.1   18 Nov 2023
//  Initial release.
//
//  PROJECT PAGE
//      https://github.com/RPiks/pico-WSPR-tx
//
//  LICENCE
//      MIT License (http://www.opensource.org/licenses/mit-license.php)
//
//  Copyright (c) 2023 by Roman Piksaykin
//  
//  Permission is hereby granted, free of charge,to any person obtaining a copy
//  of this software and associated documentation files (the Software), to deal
//  in the Software without restriction,including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY,WHETHER IN AN ACTION OF CONTRACT,TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////
#include "WSPRbeacon.h"

#include <WSPRutility.h>

/// @brief Initializes a new WSPR beacon context.
/// @param pcallsign HAM radio callsign, 12 chr max.
/// @param pgridsquare Maidenhead locator, 7 chr max.
/// @param txpow_dbm TX power, db`mW.
/// @param pdco Ptr to working DCO.
/// @param dial_freq_hz The begin of working WSPR passband.
/// @param shift_freq_hz The shift of tx freq. relative to dial_freq_hz.
/// @param gpio Pico's GPIO pin of RF output.
/// @return Ptr to the new context.
WSPRbeaconContext *WSPRbeaconInit(const char *pcallsign, const char *pgridsquare, int txpow_dbm,
                                  PioDco *pdco, uint32_t dial_freq_hz, uint32_t shift_freq_hz,
                                  int gpio)
{
    assert_(pdco);

    WSPRbeaconContext *p = calloc(1, sizeof(WSPRbeaconContext));
    assert_(p);

    strncpy(p->_pu8_callsign, pcallsign, sizeof(p->_pu8_callsign));
    strncpy(p->_pu8_locator, pgridsquare, sizeof(p->_pu8_locator));
    p->_u8_txpower = txpow_dbm;
    

    p->_pTX = TxChannelInit(682667, 0, pdco);
    assert_(p->_pTX);
    p->_pTX->_u32_dialfreqhz = dial_freq_hz + shift_freq_hz;
    p->_pTX->_i_tx_gpio = gpio;

    return p;
}

/// @brief Sets dial (baseband minima) freq.
/// @param pctx Context.
/// @param freq_hz the freq., Hz.
void WSPRbeaconSetDialFreq(WSPRbeaconContext *pctx, uint32_t freq_hz)
{
    assert_(pctx);
    pctx->_pTX->_u32_dialfreqhz = freq_hz;
}

/// @brief Constructs a new WSPR packet using the data available.
/// @param pctx Context
/// @return 0 if OK.
int WSPRbeaconCreatePacket(WSPRbeaconContext *pctx)
{
    assert_(pctx);

    wspr_encode(pctx->_pu8_callsign, pctx->_pu8_locator, pctx->_u8_txpower, pctx->_pu8_outbuf);

    return 0;
}

/// @brief Sends a prepared WSPR packet using TxChannel.
/// @param pctx Context.
/// @return 0, if OK.
int WSPRbeaconSendPacket(const WSPRbeaconContext *pctx)
{
    assert_(pctx);
    assert_(pctx->_pTX);
    assert_(pctx->_pTX->_u32_dialfreqhz > 1100 * kHz);

    memcpy(pctx->_pTX->_pbyte_buffer, pctx->_pu8_outbuf, WSPR_SYMBOL_COUNT);
    pctx->_pTX->_ix_input += WSPR_SYMBOL_COUNT;

    return 0;
}

int WSPRbeaconTxScheduler(WSPRbeaconContext *pctx, int verbose)
{
    assert_(pctx);

    const uint64_t u64tmnow = GetUptime64();
    const uint32_t is_GPS_available = pctx->_pTX->_p_oscillator->_pGPStime->_time_data._u32_nmea_gprmc_count;
    const uint32_t is_GPS_active = pctx->_pTX->_p_oscillator->_pGPStime->_time_data._u8_is_solution_active;
    const uint32_t is_GPS_override = pctx->_txSched._u8_tx_GPS_past_time == YES;

    const uint64_t u64_GPS_last_age_sec 
        = (u64tmnow - pctx->_pTX->_p_oscillator->_pGPStime->_time_data._u64_sysclk_nmea_last) / 1000000ULL;

#if 1
    StampPrintf("%llu %lu %lu %lu %llu", u64tmnow, is_GPS_available, is_GPS_active, is_GPS_override, u64_GPS_last_age_sec);
    //return 0;
#endif

    if(!is_GPS_available)
    {
        StampPrintf("Waiting for GPS receiver...");
        return -1;
    }

    if(is_GPS_active || (is_GPS_override && u64_GPS_last_age_sec < 2 * HOUR))
    {
        const uint32_t u32_unixtime_now 
            = pctx->_pTX->_p_oscillator->_pGPStime->_time_data._u32_utime_nmea_last + u64_GPS_last_age_sec;
        
        const int isec_of_hour = u32_unixtime_now % HOUR;
        const int islot_number = isec_of_hour / (2 * MINUTE);
        const int islot_modulo = islot_number % pctx->_txSched._u8_tx_slot_skip;

        static int itx_trigger = 0;
        if(ZERO == islot_modulo)
        {
            if(!itx_trigger)
            {
                itx_trigger = 1;
                StampPrintf("Start transmission.");

                //WSPRbeaconSetDialFreq(pWB, WSPR_DIAL_FREQ_HZ + WSPR_SHIFT_FREQ_HZ);
                WSPRbeaconCreatePacket(pctx);
                
                PioDCOStart(pctx->_pTX->_p_oscillator);
                sleep_ms(100);
                WSPRbeaconSendPacket(pctx);
            }
        }
        else
        {
            itx_trigger = 0;
            StampPrintf("NO transmission slot.");
            PioDCOStop(pctx->_pTX->_p_oscillator);
        }
    }

    return 0;
}
