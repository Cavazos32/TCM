#pragma once

#include <Arduino.h>
#include "driver/gpio.h"
#include "esp_idf_version.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "driver/pulse_cnt.h"
#define ENC_PCNT_IDF5 1
#else
#include "driver/pcnt.h"
#include "soc/pcnt_struct.h"
#define ENC_PCNT_IDF5 0
#endif

// ====================== OM Encoder (PCNT) — dos unidades físicas ======================
static const uint16_t ENC_PPR = 2000;
static const uint8_t  ENC_QUAD = 1;
static const uint16_t ENC_CPR = ENC_PPR * ENC_QUAD;
static const float    ENC_PULLEY_DIAM_MM = 50.0f;
static const int32_t  ENC_COUNTS_PER_100MM = 1273;

// Lado R (OM derecho) — hardware actual cableado
static const bool       ENC_R_HW_INSTALLED = true;
static const gpio_num_t ENC_R_PIN_A = GPIO_NUM_18;
static const gpio_num_t ENC_R_PIN_B = GPIO_NUM_19;
static const gpio_num_t ENC_R_PIN_Z = GPIO_NUM_21;

// Lado L (OM izquierdo) — poner ENC_L_HW_INSTALLED=true al cablear 2.º encoder
static const bool       ENC_L_HW_INSTALLED = false;
static const gpio_num_t ENC_L_PIN_A = GPIO_NUM_22;
static const gpio_num_t ENC_L_PIN_B = GPIO_NUM_23;
static const gpio_num_t ENC_L_PIN_Z = GPIO_NUM_25;

// Compat nombres legado (web / un solo encoder en UI)
#define ENC_PIN_A ENC_R_PIN_A
#define ENC_PIN_B ENC_R_PIN_B
#define ENC_PIN_Z ENC_R_PIN_Z

static const int16_t ENC_PCNT_HIGH_LIM = 30000;
static const int16_t ENC_PCNT_LOW_LIM  = -30000;

static const uint32_t ENC_GLITCH_NS        = 12000;
static const uint16_t ENC_FILTER_APB_TICKS = 1023;

static const float    ENC_MOVE_MMS   = 20.0f;
static const uint32_t ENC_SETTLE_MS  = 250;

static const float    OM_OFFSET_MM_MIN = -5.0f;
static const float    OM_OFFSET_MM_MAX =  5.0f;

#if !ENC_PCNT_IDF5
static const pcnt_unit_t ENC_PCNT_UNIT_R = PCNT_UNIT_0;
static const pcnt_unit_t ENC_PCNT_UNIT_L = PCNT_UNIT_1;
#define ENC_PCNT_UNIT ENC_PCNT_UNIT_R
#endif

enum EncSideIx : uint8_t { ENC_IX_L = 0, ENC_IX_R = 1 };

static inline bool encSideIsR(uint8_t ix) { return ix == ENC_IX_R; }
static inline uint8_t encIxFromSideR(bool sideR) { return sideR ? ENC_IX_R : ENC_IX_L; }

// ====================== Protocolo TCP maestro (HMI) ======================
enum EncTcpCmd : uint8_t {
  ENC_CMD_MEASURE_R = 0x0F,
  ENC_CMD_SET0_R    = 0x10,
  ENC_TX_ERROR      = 0x11,
  ENC_CMD_MEASURE_L = 0x17,
  ENC_CMD_SET0_L    = 0x18,
};

#define ENC_CMD_SET0 ENC_CMD_SET0_R
