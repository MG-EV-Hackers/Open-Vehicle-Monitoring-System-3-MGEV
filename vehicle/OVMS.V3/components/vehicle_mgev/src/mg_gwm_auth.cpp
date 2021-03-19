/*
;    Project:       Open Vehicle Monitor System
;    Date:          25th February 2021
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2021       Chris Staite
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include "ovms_log.h"
static const char *TAG = "v-mgev";

#include "vehicle_mgev.h"
#include "mg_obd_pids.h"

namespace {

uint32_t umul_lsr45(uint32_t a, uint32_t b)
{
    uint32_t i = a & 0xffffu;
    a >>= 16u;
    uint32_t j = b & 0xffffu;
    b >>= 16u;
    return (((((i * j) >> 16u) + (i * b + j * a)) >> 16u) + (a * b)) >> 13u;
}

uint32_t pass1(uint32_t seed)
{
    uint32_t i = seed & 0xffffu;
    uint32_t i2 = 1u;
    uint32_t i3 = 0x12e5u;
    while (i3)
    {
        if (i3 & 1u)
        {
            uint32_t tmp = i2 * i;
            i2 = (tmp - (umul_lsr45(tmp, 0x82b87f05u) * 0x3eabu));
        }
        uint32_t tmp2 = i * i;
        i = (tmp2 - (umul_lsr45(tmp2, 0x82b87f05u) * 0x3eabu));
        i3 >>= 1u;
    }
    uint32_t i5 = ((i2 >> 8u) + i2) ^ 0x0fu;
    uint32_t i6 = (i2 ^ (i5 << 8u)) & 0xff00u;
    uint32_t i7 = ((i2 ^ i5) & 0xffu) | i6;
    return (i7 | i7 << 16u) ^ 0xad0779e2u;
}

uint32_t iterate(uint32_t seed, uint32_t count)
{
    while (count--)
    {
        seed = (seed << 1u) | ((((((((seed >> 6u) ^ seed) >> 12u) ^ seed) >> 10u) ^ seed) >> 2u) & 1u);
    }
    return seed;
}

uint32_t pass2(uint32_t seed)
{
    uint32_t count = 0x25u + (((seed >> 0x18u) & 0x1cu) ^ 0x08u);
    return iterate(seed, count) ^ 0xdc8fe1aeu;
}

}  // anon namespace

bool OvmsVehicleMgEv::StartAuthentication(canbus* currentBus)
{
    ESP_LOGI(TAG, "Starting GWM authentication");
    CAN_frame_t authStart = {
        currentBus,
        nullptr,
        { .B = { 8, 0, CAN_no_RTR, CAN_frame_std, 0 } },
        gwmId,
        { .u8 = {
            (ISOTP_FT_SINGLE << 4) + 2, VEHICLE_POLL_TYPE_OBDIISESSION, 1, 0, 0, 0, 0, 0
        } }
    };
    return currentBus->Write(&authStart) != ESP_FAIL;
}

void OvmsVehicleMgEv::GwmAuthentication(canbus* currentBus, uint8_t serviceId, uint8_t* data)
{
    CAN_frame_t nextFrame = {
        currentBus,
        nullptr,
        { .B = { 8, 0, CAN_no_RTR, CAN_frame_std, 0 } },
        gwmId,
        { .u8 = {
            0, 0, 0, 0, 0, 0, 0, 0
        } }
    };

    bool known = false;

    if (serviceId == VEHICLE_POLL_TYPE_OBDIISESSION)
    {
        if (data[1] == 0x01)
        {
            // First session start, start another
            ESP_LOGV(TAG, "GWM auth: sending 1003");
            nextFrame.data.u8[0] = (ISOTP_FT_SINGLE << 4) | 2;
            nextFrame.data.u8[1] = VEHICLE_POLL_TYPE_OBDIISESSION;
            nextFrame.data.u8[2] = 3;
            known = true;
        }
        else if (data[1] == 0x03)
        {
            // Request seed1
            ESP_LOGV(TAG, "GWM auth: requesting seed1");
            nextFrame.data.u8[0] = (ISOTP_FT_SINGLE << 4) | 6;
            nextFrame.data.u8[1] = VEHICLE_POLL_TYPE_SECACCESS;
            nextFrame.data.u8[2] = 0x41u;
            nextFrame.data.u8[3] = 0x3eu;
            nextFrame.data.u8[4] = 0xabu;
            nextFrame.data.u8[5] = 0x00u;
            nextFrame.data.u8[6] = 0x0du;
            known = true;
        }
    }
    else if (serviceId == VEHICLE_POLL_TYPE_SECACCESS)
    {
        if (data[1] == 0x41)
        {
            // Seed1 response
            uint32_t seed = (data[2] << 24) | (data[3] << 16) | (data[4] << 8) | data[5];
            uint32_t key = pass1(seed);
            ESP_LOGV(TAG, "GWM auth: seed1 received %x. Replying with key1 %x", seed, key);
            nextFrame.data.u8[0] = (ISOTP_FT_SINGLE << 4) | 6;
            nextFrame.data.u8[1] = VEHICLE_POLL_TYPE_SECACCESS;            
            nextFrame.data.u8[2] = 0x42u;
            nextFrame.data.u8[3] = key >> 24u;
            nextFrame.data.u8[4] = key >> 16u;
            nextFrame.data.u8[5] = key >> 8u;
            nextFrame.data.u8[6] = key;
            known = true;
        }
        else if (data[1] == 0x42)
        {
            // Seed1 accept, request seed2
            ESP_LOGV(TAG, "GWM auth: key1 accepted, requesting seed2");
            nextFrame.data.u8[0] = (ISOTP_FT_SINGLE << 4) | 2;
            nextFrame.data.u8[1] = VEHICLE_POLL_TYPE_SECACCESS;
            nextFrame.data.u8[2] = 0x01u;
            known = true;
        }
        else if (data[1] == 0x01)
        {
            // Seed 2 response
            uint32_t seed = (data[2] << 24) | (data[3] << 16) | (data[4] << 8) | data[5];
            uint32_t key = pass2(seed);
            ESP_LOGV(TAG, "GWM auth: seed2 received %x. Replying with key2 %x", seed, key);
            nextFrame.data.u8[0] = (ISOTP_FT_SINGLE << 4) | 6;
            nextFrame.data.u8[1] = VEHICLE_POLL_TYPE_SECACCESS;            
            nextFrame.data.u8[2] = 0x02u;
            nextFrame.data.u8[3] = key >> 24u;
            nextFrame.data.u8[4] = key >> 16u;
            nextFrame.data.u8[5] = key >> 8u;
            nextFrame.data.u8[6] = key;
            known = true;
        }
        else if (data[1] == 0x02)
        {
            // Seed 2 accept, end session 1
            ESP_LOGV(TAG, "GWM auth: key2 accepted, ending session 1");
            nextFrame.data.u8[0] = (ISOTP_FT_SINGLE << 4) | 5;
            nextFrame.data.u8[1] = VEHICLE_POLL_TYPE_ROUTINECONTROL;
            nextFrame.data.u8[2] = 0x01u;
            nextFrame.data.u8[3] = 0xaau;
            nextFrame.data.u8[4] = 0xffu;
            nextFrame.data.u8[5] = 0x00u;
            known = true;
        }
    }
    else if (serviceId == VEHICLE_POLL_TYPE_ROUTINECONTROL)
    {
        if (data[1] == 0x01)
        {
            // Ack end session 1, end session 3
            ESP_LOGV(TAG, "GWM auth: session 1 ended, ending session 3");
            nextFrame.data.u8[0] = (ISOTP_FT_SINGLE << 4) | 4;
            nextFrame.data.u8[1] = VEHICLE_POLL_TYPE_ROUTINECONTROL;
            nextFrame.data.u8[2] = 0x03u;
            nextFrame.data.u8[3] = 0xaau;
            nextFrame.data.u8[4] = 0xffu;
            known = true;
        }
        else if (data[1] == 0x03)
        {
            // Ack session 3
            ESP_LOGV(TAG, "GWM auth: session 3 ended.");
            ESP_LOGI(TAG, "Gateway authentication complete");
        }
    }

    if (known)
    {
        if (currentBus->Write(&nextFrame) == ESP_FAIL) {
            ESP_LOGE(TAG, "Error writing authentication frame");
        }
    }
}
